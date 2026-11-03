/* marshal.c — Ruby Marshal 4.8 emission (TODO.restructure/21).
 *
 * Two passes over the value records (the emitter discipline): sizing
 * computes the exact buffer, then emission is linear writes — both
 * runs execute the SAME walker with the writer no-op'd, so they
 * cannot drift. Per-open element counts come from one linear
 * pre-pass; registration ids are assigned deterministically in
 * record order.
 *
 * Format facts are empirical, pinned against Ruby 3.4 Marshal.dump /
 * Marshal.load byte streams:
 *   - w_long small form: x==0 -> 0x00; |x| <= 122 -> sign*(|x|+5);
 *     size form: tag +-k (k = minimal magnitude bytes), payload =
 *     LE(m) for positives, LE(2^(8k)-m) for negatives.
 *   - integer 'i' while |x| < 2^30 (the dumper's boundary; the loader
 *     accepts wider, but byte-matching the dumper keeps streams
 *     diffable). Beyond: bignum 'l' + sign + w_long(limbs) + LE
 *     16-bit limbs.
 *   - float 'f' + w_long(repr_len) + repr; inf/nan spell out.
 *   - UTF-8 string: 'I' '"' w_long(len) bytes 0x06 ':' 0x06 'E' 'T'.
 *   - object links '@' + w_long(index): the loader registers arrays,
 *     hashes, strings and FLOATS (0-based, stream order); integers,
 *     bignums, symbols and immediates never register.
 *
 * Semantics mirror the binding record walks exactly (the ':sym' plain
 * scan, single-char y/n, dot-required floats); aliases to registered
 * constructs become object links (identity preserved), aliases to
 * immediates re-emit the value. Merge keys ('<<') and timestamps
 * return YEPTRIS_ERROR_UNSUPPORTED — the host falls back to the walk.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "doc.h"
#include "emit/float/api.h"
#include "events/values_priv.h"

#include <yeptris/error.h>
#include <yeptris/marshal.h>
#include <yeptris/resolve.h> /* YEPTRIS_TAG_BINARY (#168) */

typedef struct {
    const char* name; /* into the value arena */
    uint32_t len;
    int has_obj; /* 1: registered construct -> obj_id */
    uint64_t obj_id;
    int is_scalar; /* copy for immediate re-emits */
    YeptrisValue scalar;
} yep_anchor;

typedef struct {
    const yep_value_ctx* c;
    int compat; /* Psych float quirks apply ONLY under 1.1 compat
                 * (TODO.restructure/32): the core schema's float
                 * regexp has an OPTIONAL dot - 1e3 IS a core float */
    char* buf;
    size_t len;
    size_t nreg;
    int sizing; /* 1: count only (writer no-op) */
    int oom;
    int unsupported;
    uint64_t last_reg; /* id of the most recently completed value's
                        * registration, if any */
    int last_was_reg;
    /* anchors: rebuilt identically each pass (deterministic order) */
    yep_anchor* anchors;
    size_t nanchors, anchors_cap;
    /* per-open element counts (document order of OPEN records) */
    const uint64_t* counts;
    size_t open_seq;
} E;

static void e_put(E* e, const void* p, size_t n) {
    if (!e->sizing && n != 0) {
        memcpy(e->buf + e->len, p, n);
    }
    e->len += n;
}

static void e_byte(E* e, unsigned char b) {
    e_put(e, &b, 1);
}

static void e_long(E* e, int64_t x) {
    if (x == 0) {
        e_byte(e, 0);
        return;
    }
    int64_t m = x < 0 ? -x : x;
    if (m <= 122) {
        e_byte(e, (unsigned char)(x < 0 ? -(m + 5) : m + 5));
        return;
    }
    /* width types: unsigned long is 32-bit on Windows (LLP64) and
       truncates every magnitude >= 2^32 (limb-count bug: a 19-digit
       int materialized as its low 32 bits). */
    uint64_t um = (uint64_t)m;
    int k = 0;
    while (um != 0) {
        um >>= 8;
        k++;
    }
    e_byte(e, (unsigned char)(x < 0 ? -k : k));
    uint64_t payload =
        x < 0 ? ((k == 8 ? UINT64_C(0) : UINT64_C(1) << (8 * k)) - (uint64_t)m) : (uint64_t)m;
    unsigned char b[8];
    for (int i = 0; i < k; i++) {
        b[i] = (unsigned char)(payload & 0xff);
        payload >>= 8;
    }
    e_put(e, b, (size_t)k);
}

static void e_reg(E* e, uint64_t* id_out) {
    uint64_t id = e->nreg++;
    if (id_out != NULL) {
        *id_out = id;
    }
}

static void e_int(E* e, int64_t x) {
    e->last_was_reg = 0;
    if (x == INT64_MIN) { /* |x| overflows int64 arithmetic */
        e_byte(e, 'l');
        e_byte(e, '-');
        e_long(e, 4); /* 2^63: 64 bits -> 4 16-bit limbs */
        static const unsigned char limbs[8] = {0, 0, 0, 0, 0, 0, 0, 0x80};
        e_put(e, limbs, 8);
        return;
    }
    int64_t m = x < 0 ? -x : x;
    if (m < (int64_t)1 << 30) {
        e_byte(e, 'i');
        e_long(e, x);
        return;
    }
    /* bignum: sign + limb count + LE 16-bit limbs */
    e_byte(e, 'l');
    e_byte(e, x < 0 ? '-' : '+');
    uint64_t um = (uint64_t)m;
    int bits = 0;
    while (um != 0) {
        um >>= 1;
        bits++;
    }
    int limbs = (bits + 15) / 16;
    e_long(e, limbs);
    unsigned char b[8] = {0};
    uint64_t v = (uint64_t)m;
    for (int i = 0; i < limbs * 2; i += 2) {
        b[i] = (unsigned char)(v & 0xff);
        b[i + 1] = (unsigned char)((v >> 8) & 0xff);
        v >>= 16;
    }
    e_put(e, b, (size_t)(limbs * 2));
}

static void e_str(E* e, const char* p, uint32_t len) {
    uint64_t id;
    e_reg(e, &id); /* strings register at their '"' token */
    e_byte(e, 'I');
    e_byte(e, '"');
    e_long(e, (int64_t)len);
    e_put(e, p, len);
    e_byte(e, 0x06); /* 1 ivar */
    e_byte(e, ':');
    e_byte(e, 0x06); /* :E — 1-byte symbol */
    e_byte(e, 'E');
    e_byte(e, 'T'); /* true: UTF-8 */
    e->last_reg = id;
    e->last_was_reg = 1;
}

static void e_sym(E* e, const char* p, uint32_t len) {
    e->last_was_reg = 0;
    e_byte(e, ':');
    e_long(e, (int64_t)len);
    e_put(e, p, len);
}

static void e_float(E* e, double d) {
    uint64_t id;
    e_reg(e, &id); /* the loader registers floats (dump links them) */
    char repr[40];
    const char* s = repr;
    uint32_t rl;
    if (isnan(d)) {
        s = "nan";
        rl = 3;
    } else if (isinf(d)) {
        s = d < 0 ? "-inf" : "inf";
        rl = d < 0 ? 4 : 3;
    } else {
        int n = yep_d2s_shortest(d, repr);
        repr[n] = '\0';
        rl = (uint32_t)n;
    }
    e_byte(e, 'f');
    e_long(e, (int64_t)rl);
    e_put(e, s, rl);
    e->last_reg = id;
    e->last_was_reg = 1;
}

static void e_null(E* e) {
    e->last_was_reg = 0;
    e_byte(e, '0');
}

static void e_bool(E* e, int truthy) {
    e->last_was_reg = 0;
    e_byte(e, truthy ? 'T' : 'F');
}

/* Psych's plain-scalar symbol scan: implicit ':name' (not '::'). */
static int sym_scan(const char* p, uint32_t len) {
    return len > 1 && p[0] == ':' && p[1] != ':';
}

/* The float dot-quirk, exactly as the walks decide it: without '.',
 * ':' or a leading '.', Psych leaves the scalar a String. */
static int float_text_ok(const E* e, const char* p, uint32_t len) {
    if (!e->compat) {
        return 1; /* core_12: the resolver's verdict is the spec's */
    }
    if (len == 0 || p[0] == '.') {
        return len != 0;
    }
    return memchr(p, '.', len) != NULL || memchr(p, ':', len) != NULL;
}

static const char* e_text(const E* e, const YeptrisValue* v) {
    return v->len == 0 ? "" : e->c->arena + v->off;
}

static yep_anchor* anchor_find(E* e, const char* name, uint32_t len) {
    /* newest binding wins: YAML lets a later &anchor shadow an
     * earlier one of the same name (libyaml snapshot 3GZX) */
    for (size_t i = e->nanchors; i > 0; i--) {
        yep_anchor* a = &e->anchors[i - 1];
        if (a->len == len && memcmp(a->name, name, len) == 0) {
            return a;
        }
    }
    return NULL;
}

static int anchor_add(E* e, const char* name, uint32_t len) {
    if (e->nanchors == e->anchors_cap) {
        size_t cap = e->anchors_cap ? e->anchors_cap * 2 : 16;
        yep_anchor* na = realloc(e->anchors, cap * sizeof(*na));
        if (na == NULL) {
            return -1;
        }
        e->anchors = na;
        e->anchors_cap = cap;
    }
    yep_anchor* a = &e->anchors[e->nanchors++];
    memset(a, 0, sizeof(*a));
    a->name = name;
    a->len = len;
    return 0;
}

/* Psych's integer shape (scalar_scanner): optional sign, no
 * leading zero, digits with '_' / ',' separators only BETWEEN
 * digits. Ints within int64 arrive as V_INT; this rebuilds the
 * BEYOND-int64 texts Psych materializes as Integer (issue #31,
 * TODO.restructure/43). */
static int psych_int_text(const char* p, uint32_t len) {
    if (len == 0)
        return 0;
    if (p[0] == '-' || p[0] == '+') {
        p++;
        len--;
    }
    if (len == 0 || p[0] == '0')
        return 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = p[i];
        if (c >= '0' && c <= '9')
            continue;
        if ((c == '_' || c == ',') && i + 1 < len && p[i + 1] >= '0' && p[i + 1] <= '9')
            continue;
        return 0;
    }
    return 1;
}

/* decimal text -> bignum limbs (base 2^16 LE), then Marshal's 'l':
 * sign + w_long(limb count) + limbs. The gate (marshal_node: text
 * under 1300 chars) bounds the digits at 1299 -> ceil(1299*log2(10)/16)
 * = 270 limbs; the arrays are sized from that bound so no carry is
 * ever dropped (the old 128-limb cap silently truncated 617..1299-digit
 * values into wrong numbers). */
#define E_BIGNUM_LIMBS_MAX 272

static void e_bignum_text(E* e, const char* p, uint32_t len) {
    int neg = 0;
    if (p[0] == '-' || p[0] == '+') {
        neg = p[0] == '-';
        p++;
        len--;
    }
    uint16_t limbs[E_BIGNUM_LIMBS_MAX];
    memset(limbs, 0, sizeof(limbs));
    int nl = 1;
    for (uint32_t i = 0; i < len; i++) {
        char c = p[i];
        if (c == '_' || c == ',')
            continue;
        uint32_t carry = (uint32_t)(c - '0');
        for (int j = 0; j < nl; j++) {
            uint32_t t = (uint32_t)limbs[j] * 10u + carry;
            limbs[j] = (uint16_t)(t & 0xffffu);
            carry = t >> 16;
        }
        while (carry != 0 && nl < E_BIGNUM_LIMBS_MAX) {
            limbs[nl++] = (uint16_t)(carry & 0xffffu);
            carry >>= 16;
        }
    }
    while (nl > 1 && limbs[nl - 1] == 0)
        nl--;
    e->last_was_reg = 0;
    e_byte(e, 'l');
    e_byte(e, neg ? '-' : '+');
    e_long(e, nl);
    unsigned char b[E_BIGNUM_LIMBS_MAX * 2];
    for (int i = 0; i < nl; i++) {
        b[2 * i] = (unsigned char)(limbs[i] & 0xff);
        b[2 * i + 1] = (unsigned char)(limbs[i] >> 8);
    }
    e_put(e, b, (size_t)nl * 2);
}

static int e_scalar(E* e, const YeptrisValue* v) {
    const char* p = e_text(e, v);
    switch (v->kind) {
    case YEP_V_NULL:
        e_null(e);
        return 0;
    case YEP_V_BOOL:
        if (v->len == 1) { /* Psych: single-char y/n stay Strings */
            e_str(e, p, v->len);
        } else {
            e_bool(e, v->b);
        }
        return 0;
    case YEP_V_INT:
        e_int(e, (int64_t)v->p);
        return 0;
    case YEP_V_FLOAT:
        if (float_text_ok(e, p, v->len)) {
            double d;
            memcpy(&d, &v->p, sizeof(d));
            e_float(e, d);
        } else {
            e_str(e, p, v->len);
        }
        return 0;
    case YEP_V_STR:
        if (v->b == 1 && v->len < 1300 && v->len > 18 && psych_int_text(p, v->len)) {
            e_bignum_text(e, p, v->len);
            return 0;
        }
        if (v->b == 1 && sym_scan(p, v->len)) {
            e_sym(e, p + 1, v->len - 1);
        } else {
            e_str(e, p, v->len);
        }
        return 0;
    default:
        return -1; /* unreachable: the pre-scan rejects timestamps */
    }
}

static int e_alias(E* e, const YeptrisValue* v) {
    const char* p = e_text(e, v);
    yep_anchor* a = anchor_find(e, p, v->len);
    if (a == NULL) {
        e->unsupported = 1; /* forward reference: impossible in YAML */
        return -1;
    }
    if (a->has_obj) {
        e->last_was_reg = 0;
        e_byte(e, '@');
        e_long(e, (int64_t)a->obj_id);
        return 0;
    }
    /* immediate target: re-emit the value (floats re-register, which
     * matches the loader's own registration discipline) */
    return e_scalar(e, &a->scalar);
}

/* One value at cursor i: scalar, alias, or OPEN..CLOSE subtree.
 * Returns the cursor after the value, or -1 on bail. */
long e_value(E* e, size_t i, size_t end);

static long e_body(E* e, size_t i, size_t end) {
    const YeptrisValue* v = &e->c->vals[i];
    switch (v->kind) {
    case YEP_V_SEQ_OPEN:
    case YEP_V_MAP_OPEN: {
        uint64_t id;
        e_reg(e, &id);
        e_byte(e, v->kind == YEP_V_SEQ_OPEN ? '[' : '{');
        e_long(e, (int64_t)e->counts[e->open_seq++]); /* pre-computed */
        size_t j = i + 1;
        while (j < end) {
            if (e->c->vals[j].kind == YEP_V_CLOSE) {
                break;
            }
            long nx = e_value(e, j, end);
            if (nx < 0) {
                return nx;
            }
            j = (size_t)nx;
        }
        if (j >= end) {
            return -1; /* unterminated container */
        }
        e->last_reg = id;
        e->last_was_reg = 1;
        return (long)j + 1;
    }
    case YEP_V_CLOSE:
    case YEP_V_DOC:
        return -1; /* only the driver emits around these */
    case YEP_V_ALIAS:
        return e_alias(e, v) == 0 ? (long)i + 1 : -1;
    default:
        return e_scalar(e, v) == 0 ? (long)i + 1 : -1;
    }
}

/* ANCHOR decorates the value that follows: bind after it completes. */
long e_value(E* e, size_t i, size_t end) {
    const YeptrisValue* v = &e->c->vals[i];
    if (v->kind != YEP_V_ANCHOR) {
        return e_body(e, i, end);
    }
    if (anchor_add(e, e->c->arena + v->off, v->len) != 0) {
        e->oom = 1;
        return -1;
    }
    /* the index is a LOCAL: the decorated value can itself contain
     * anchors (e.g. &a [&b x]), and the recursion's own bindings must
     * not clobber ours (ASAN: anchors[-1] write on the snapshots) */
    size_t pend_idx = e->nanchors - 1;
    long nx = e_value(e, i + 1, end);
    if (nx >= 0) {
        yep_anchor* a = &e->anchors[pend_idx];
        if (e->last_was_reg) {
            a->has_obj = 1;
            a->obj_id = e->last_reg;
        } else {
            a->is_scalar = 1;
            a->scalar = e->c->vals[i + 1];
        }
    }
    e->last_was_reg = 0;
    return nx;
}

/* ---- the driver ---- */

typedef struct {
    size_t from, to;
} yep_seg;

/* One linear scan: rejection scan (timestamps, merge keys), the
 * per-open counts, and the DOC segmentation. Returns 0 ok, -1 oom,
 * -2 unsupported. */
static int pre_scan(const yep_value_ctx* c, uint64_t** counts_out, size_t* nopens_out,
                    yep_seg** segs_out, size_t* nsegs_out) {
    const YeptrisValue* vals = c->vals;
    size_t n = c->n;
    size_t opens = 0;
    size_t ndocs = 0;
    for (size_t i = 0; i < n; i++) {
        switch (vals[i].kind) {
        case YEP_V_SEQ_OPEN:
        case YEP_V_MAP_OPEN:
            opens++;
            break;
        case YEP_V_DOC:
            ndocs++;
            break;
        case YEP_V_TIMESTAMP:
            return -2;
        case YEP_V_STR:
            if (vals[i].tag_id == YEPTRIS_TAG_BINARY) {
                return -2; /* !binary: the walk base64-decodes (#168) */
            }
            if (vals[i].is_key == 1 && vals[i].len == 2 && c->arena[vals[i].off] == '<' &&
                c->arena[vals[i].off + 1] == '<') {
                return -2; /* merge key: the walk resolves these */
            }
            break;
        default:
            break;
        }
    }
    uint64_t* counts = NULL;
    if (opens > 0) {
        counts = calloc(opens, sizeof(*counts));
        if (counts == NULL) {
            return -1;
        }
    }
    /* counts: one stack pass — every value record credits its
     * innermost open (sequences count elements, maps count pairs) */
    size_t* idxs = NULL;
    unsigned char* maps = NULL;
    size_t sp = 0, cap = 0;
    size_t oi = 0;
    /* documents: content AFTER each DOC marker ([d+1, next d); no
     * markers at all = one implicit document [0, n) — node subtrees
     * take that path too) */
    size_t* doc_pos = NULL;
    size_t ndoc_seen = 0;
    if (ndocs > 0) {
        doc_pos = calloc(ndocs, sizeof(*doc_pos));
        if (doc_pos == NULL) {
            free(counts);
            return -1;
        }
    }
    yep_seg* segs = NULL;
    size_t nsegs = 0;
    int bad = 0;
    {
        size_t scap = (ndocs > 0 ? ndocs : 1);
        segs = calloc(scap, sizeof(*segs));
        if (segs == NULL) {
            free(counts);
            free(doc_pos);
            return -1;
        }
    }
    for (size_t i = 0; i < n && !bad; i++) {
        unsigned char k = vals[i].kind;
        if (k == YEP_V_SEQ_OPEN || k == YEP_V_MAP_OPEN) {
            /* the open itself is a slot in its parent (complex keys
             * aside, this is how sequences nest) */
            if (sp > 0 && (maps[sp - 1] == 0 || vals[i].is_key == 1)) {
                counts[idxs[sp - 1]]++;
            }
            if (sp == cap) {
                size_t ncap = cap ? cap * 2 : 64;
                size_t* ni = realloc(idxs, ncap * sizeof(*ni));
                unsigned char* nm = realloc(maps, ncap);
                if (ni == NULL || nm == NULL) {
                    free(ni);
                    free(nm);
                    bad = 1;
                    break;
                }
                idxs = ni;
                maps = nm;
                cap = ncap;
            }
            maps[sp] = (k == YEP_V_MAP_OPEN);
            idxs[sp] = oi++;
            counts[idxs[sp]] = 0;
            sp++;
        } else if (k == YEP_V_CLOSE) {
            if (sp > 0) {
                sp--;
            }
        } else if (k == YEP_V_ANCHOR) {
            /* decorates the next record: no slot */
        } else if (k == YEP_V_DOC) {
            doc_pos[ndoc_seen++] = i;
        } else if (sp > 0) {
            if (maps[sp - 1] == 0 || vals[i].is_key == 1) {
                counts[idxs[sp - 1]]++;
            }
        }
    }
    free(idxs);
    free(maps);
    if (!bad) {
        if (ndocs == 0) {
            segs[nsegs].from = 0;
            segs[nsegs].to = n;
            nsegs = 1;
        } else {
            for (size_t i = 0; i < ndocs; i++) {
                segs[i].from = doc_pos[i] + 1;
                segs[i].to = (i + 1 < ndocs) ? doc_pos[i + 1] : n;
            }
            nsegs = ndocs;
        }
    }
    free(doc_pos);
    if (bad) {
        free(counts);
        free(segs);
        return -1;
    }
    *counts_out = counts;
    *nopens_out = opens;
    *segs_out = segs;
    *nsegs_out = nsegs;
    return 0;
}

static void e_init(E* e, const yep_value_ctx* c, int compat, const uint64_t* counts, char* buf,
                   int sizing) {
    memset(e, 0, sizeof(*e));
    e->c = c;
    e->compat = compat;
    e->counts = counts;
    e->buf = buf;
    e->sizing = sizing;
}

/* Runs the walker over one segment set per `mode`. Returns 0 ok,
 * -1 oom, -2 unsupported, -3 drift (a bug). */
static int run_pass(E* e, YeptrisMarshalMode mode, int node_mode, const yep_seg* segs,
                    size_t nsegs) {
    static const unsigned char hdr[2] = {0x04, 0x08};
    e_put(e, hdr, 2);
    if (node_mode) {
        long nx = e_value(e, segs[0].from, segs[0].to);
        return nx < 0 ? (e->unsupported ? -2 : (e->oom ? -1 : -3)) : 0;
    }
    if (mode == YEPTRIS_MARSHAL_FIRST_DOC) {
        if (nsegs == 0 || segs[0].from == segs[0].to) {
            e_null(e); /* empty stream / empty first document */
            return 0;
        }
        long nx = e_value(e, segs[0].from, segs[0].to);
        return nx < 0 ? (e->unsupported ? -2 : (e->oom ? -1 : -3)) : 0;
    }
    /* ALL_DOCS: one array of every document's root */
    size_t ndocs = nsegs;
    uint64_t id;
    e_reg(e, &id);
    e_byte(e, '[');
    e_long(e, (int64_t)ndocs);
    for (size_t i = 0; i < nsegs; i++) {
        if (segs[i].from == segs[i].to) {
            e_null(e); /* an empty document */
            continue;
        }
        long nx = e_value(e, segs[i].from, segs[i].to);
        if (nx < 0) {
            return e->unsupported ? -2 : (e->oom ? -1 : -3);
        }
    }
    return 0;
}

static YeptrisStatus map_fail(int rc) {
    return rc == -1 ? YEPTRIS_ERROR_MEMORY : YEPTRIS_ERROR_UNSUPPORTED;
}

/* ---- strict duplicate-key pre-scan (yeptris-ruby#278) ----------------
 *
 * json 3 makes duplicate keys an error by DEFAULT; the marshal route
 * must raise before any bytes are written, exactly like the native
 * materializer's rb_hash_aref probe. One linear pass over the same
 * records the writer consumes — the writer itself stays untouched
 * (the dup law lives here, once). The JSON DOM keeps both pairs
 * ("{\"a\":1,\"a\":2}" map_count=2), so the records see them; the
 * non-strict Marshal stream stays last-wins, which is json 2.x's
 * contract.
 *
 * Key identity is the record's typed payload: span kinds compare the
 * arena bytes, the rest compare (kind, b, payload). Duplicates are
 * per-map by definition, so the scan keeps ONE open-addressing table
 * per open container on a frame stack: a key registers in the
 * innermost frame and frames pop on CLOSE. A single table cleared on
 * open cannot express this — a parent key equal to a key in an
 * earlier child object was a false duplicate, and a parent duplicate
 * after a nested object went unseen.
 */

typedef struct {
    uint64_t p; /* 0 = empty slot (payload 0 with kind 0 cannot be a key) */
    uint32_t off, len;
    uint8_t kind, b;
} dup_slot;

typedef struct {
    dup_slot* slots;
    size_t cap; /* power of two */
    size_t count;
} dup_set;

typedef struct {
    dup_set keys;
} dup_frame;

static uint64_t dup_hash(const dup_slot* k, const char* arena) {
    uint64_t h = 1469598103934665603ull;
    if (k->len != UINT32_MAX && (k->kind == YEP_V_STR || k->kind == YEP_V_TIMESTAMP ||
                                 k->kind == YEP_V_ALIAS || k->kind == YEP_V_ANCHOR)) {
        for (uint32_t i = 0; i < k->len; i++) {
            h ^= (unsigned char)arena[k->off + i];
            h *= 1099511628211ull;
        }
    }
    h ^= k->kind;
    h *= 1099511628211ull;
    h ^= k->b;
    h *= 1099511628211ull;
    h ^= k->p;
    h *= 1099511628211ull;
    return h;
}

static int dup_eq(const dup_slot* a, const dup_slot* b, const char* arena) {
    if (a->kind != b->kind || a->b != b->b || a->p != b->p) {
        return 0;
    }
    int spanned = (a->kind == YEP_V_STR || a->kind == YEP_V_TIMESTAMP || a->kind == YEP_V_ALIAS ||
                   a->kind == YEP_V_ANCHOR);
    if (!spanned) {
        return 1;
    }
    if (a->len != b->len) {
        return 0;
    }
    return a->len == 0 || memcmp(arena + a->off, arena + b->off, a->len) == 0;
}

static int dup_set_grow(dup_set* s, const char* arena) {
    size_t ncap = s->cap ? s->cap * 2 : 16;
    dup_slot* ns = calloc(ncap, sizeof(*ns));
    if (ns == NULL) {
        return -1;
    }
    for (size_t i = 0; i < s->cap; i++) {
        dup_slot* old = &s->slots[i];
        if (old->p == 0) {
            continue;
        }
        uint64_t h = dup_hash(old, arena);
        while (ns[h & (ncap - 1)].p != 0) {
            h++;
        }
        ns[h & (ncap - 1)] = *old;
    }
    free(s->slots);
    s->slots = ns;
    s->cap = ncap;
    return 0;
}

/* 1 = duplicate, 0 = inserted, -1 = oom. Load factor stays <= 1/2. */
static int dup_set_insert(dup_set* s, const dup_slot* k, const char* arena) {
    if ((s->count + 1) * 2 > s->cap && dup_set_grow(s, arena) != 0) {
        return -1;
    }
    uint64_t h = dup_hash(k, arena);
    for (;;) {
        dup_slot* slot = &s->slots[h & (s->cap - 1)];
        if (slot->p == 0) {
            *slot = *k;
            s->count++;
            return 0;
        }
        if (dup_eq(slot, k, arena)) {
            return 1;
        }
        h++;
    }
}

/* 1 = duplicate found (TLS error set), 0 = clean, -1 = oom */
static int strict_dup_scan(const yep_value_ctx* c) {
    dup_frame* frames = NULL;
    size_t depth = 0, cap = 0;
    int rc = 0;
    const YeptrisValue* vals = c->vals;
    for (size_t i = 0; i < c->n && rc == 0; i++) {
        const YeptrisValue* v = &vals[i];
        if (v->kind == YEP_V_MAP_OPEN || v->kind == YEP_V_SEQ_OPEN) {
            if (depth == cap) {
                size_t ncap = cap ? cap * 2 : 16;
                dup_frame* nf = realloc(frames, ncap * sizeof(*nf));
                if (nf == NULL) {
                    rc = -1;
                    break;
                }
                frames = nf;
                cap = ncap;
            }
            frames[depth].keys.slots = NULL;
            frames[depth].keys.cap = 0;
            frames[depth].keys.count = 0;
            depth++;
            continue;
        }
        if (v->kind == YEP_V_CLOSE) {
            if (depth > 0) {
                depth--;
                free(frames[depth].keys.slots);
            }
            continue;
        }
        if (v->is_key != 1 || depth == 0) {
            continue;
        }
        dup_slot k = {v->p, v->off, v->len, v->kind, v->b};
        if (k.p == 0) {
            k.p = 1; /* never aliases the empty-slot sentinel */
        }
        int irc = dup_set_insert(&frames[depth - 1].keys, &k, c->arena);
        if (irc != 1) {
            if (irc < 0) {
                rc = -1;
            }
            continue;
        }
        /* name the key like the tape walk's message (json 3 names it
         * too): the arena bytes are the text for the str-like kinds,
         * which is every JSON key */
        if (k.kind == YEP_V_STR || k.kind == YEP_V_TIMESTAMP || k.kind == YEP_V_ALIAS ||
            k.kind == YEP_V_ANCHOR) {
            yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                          "duplicate key \"%.*s\" in JSON object", (int)k.len,
                          k.len ? c->arena + k.off : "");
        } else {
            yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0, "duplicate key in object");
        }
        rc = 1;
    }
    for (size_t j = 0; j < depth; j++) {
        free(frames[j].keys.slots);
    }
    free(frames);
    return rc;
}

static int dom_subtree_tagged(const yep_dom* d, uint32_t id);
static YeptrisStatus marshal_records(const yep_value_ctx* c, int compat, YeptrisMarshalMode mode,
                                     int node_mode, char** out, size_t* out_len);

YEPTRIS_API YeptrisStatus yeptris_marshal_node_ex(YeptrisNode node, uint32_t flags, char** out,
                                                  size_t* out_len) {
    if (node == NULL || out == NULL || out_len == NULL ||
        (flags & ~YEPTRIS_MARSHAL_STRICT_DUP_KEYS) != 0) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    *out_len = 0;
    yeptris_node* h = (yeptris_node*)node;
    yep_dom* dom = yep_doc_dom(h->doc); /* #378: a lazy parse carries the
                                         * tape — the tree materializes here */
    if (dom == NULL) {
        return YEPTRIS_ERROR_MEMORY; /* the lazy route's retry contract */
    }
    if (dom_subtree_tagged(dom, h->id) != 0) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "marshal: explicitly tagged node not expressible; "
                      "fall back to the value walk");
        return YEPTRIS_ERROR_UNSUPPORTED;
    }
    yep_value_ctx* c = NULL;
    if (yep_values_from_dom(dom, h->id, 0, &c) != 0) {
        return YEPTRIS_ERROR_MEMORY;
    }
    if ((flags & YEPTRIS_MARSHAL_STRICT_DUP_KEYS) != 0) {
        int drc = strict_dup_scan(c);
        if (drc != 0) {
            yep_value_ctx_free(c);
            return drc < 0 ? YEPTRIS_ERROR_MEMORY : YEPTRIS_ERROR_PARSE;
        }
    }
    YeptrisStatus st = marshal_records(c, h->doc->schema == YEPTRIS_SCHEMA_11_COMPAT,
                                       YEPTRIS_MARSHAL_ALL_DOCS, 1, out, out_len);
    if (st == YEPTRIS_ERROR_UNSUPPORTED) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "marshal: merge key, timestamp or forward alias not expressible; "
                      "fall back to the value walk");
    }
    yep_value_ctx_free(c);
    return st;
}

static YeptrisStatus marshal_records(const yep_value_ctx* c, int compat, YeptrisMarshalMode mode,
                                     int node_mode, char** out, size_t* out_len) {
    *out = NULL;
    *out_len = 0;
    uint64_t* counts = NULL;
    yep_seg* segs = NULL;
    size_t opens = 0, nsegs = 0;
    int prc = pre_scan(c, &counts, &opens, &segs, &nsegs);
    if (prc != 0) {
        return map_fail(prc);
    }
    E e;
    e_init(&e, c, compat, counts, NULL, 1);
    int rc = run_pass(&e, mode, node_mode, segs, nsegs);
    size_t total = e.len;
    free(e.anchors);
    if (rc != 0) {
        free(counts);
        free(segs);
        return map_fail(rc);
    }
    char* buf = malloc(total > 0 ? total : 1);
    if (buf == NULL) {
        free(counts);
        free(segs);
        return YEPTRIS_ERROR_MEMORY;
    }
    e_init(&e, c, compat, counts, buf, 0);
    rc = run_pass(&e, mode, node_mode, segs, nsegs);
    free(e.anchors);
    free(counts);
    free(segs);
    if (rc != 0 || e.len != total) { /* drift between passes is a bug */
        free(buf);
        return rc != 0 ? map_fail(rc) : YEPTRIS_ERROR_INTERNAL;
    }
    *out = buf;
    *out_len = total;
    return YEPTRIS_OK;
}

YEPTRIS_API YeptrisStatus yeptris_marshal(const char* data, size_t len, YeptrisSchema schema,
                                          YeptrisMarshalMode mode, char** out, size_t* out_len) {
    if (out == NULL || out_len == NULL || (data == NULL && len != 0) ||
        (mode != YEPTRIS_MARSHAL_ALL_DOCS && mode != YEPTRIS_MARSHAL_FIRST_DOC)) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    *out_len = 0;
    yep_value_ctx* c = NULL;
    int rc = yep_values_from_input(data, len, schema == YEPTRIS_SCHEMA_11_COMPAT, &c);
    if (rc != 0) {
        return rc == -1 ? YEPTRIS_ERROR_MEMORY : YEPTRIS_ERROR_PARSE;
    }
    YeptrisStatus st =
        marshal_records(c, schema == YEPTRIS_SCHEMA_11_COMPAT, mode, 0, out, out_len);
    if (st == YEPTRIS_ERROR_UNSUPPORTED) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "marshal: merge key, timestamp or forward alias not expressible; "
                      "fall back to the value walk");
    }
    yep_value_ctx_free(c);
    return st;
}

/* #178 (ruby #168's fast path): the value records carry the resolver's
 * verdict, NOT the source's explicit tag — a `!ruby/object` mapping
 * would marshal as a plain hash, silently dropping the tag (psych's
 * visitor revives it). One cheap pre-walk: any explicitly tagged node
 * in the subtree → UNSUPPORTED, the host walks. Explicit CORE tags
 * (!!str and friends) ride the same bail — correct, just conservative;
 * bulk data is untagged. */
static int dom_subtree_tagged(const yep_dom* d, uint32_t id) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return 0;
    }
    if (n->tag.len != 0) {
        return 1;
    }
    for (uint32_t c = n->first_child; c != UINT32_MAX;) {
        const yep_dnode* cn = yep_dom_node(d, c);
        if (cn == NULL) {
            break;
        }
        if (dom_subtree_tagged(d, c) != 0) {
            return 1;
        }
        c = cn->next_sibling;
    }
    return 0;
}

YEPTRIS_API YeptrisStatus yeptris_marshal_node(YeptrisNode node, char** out, size_t* out_len) {
    return yeptris_marshal_node_ex(node, 0, out, out_len);
}

YEPTRIS_API void yeptris_marshal_free(char* out) {
    free(out);
}
