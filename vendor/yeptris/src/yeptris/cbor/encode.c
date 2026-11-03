/* encode.c — CBOR (RFC 8949) encode over the shared DOM (TODO.cbor/02).
 *
 * DOM -> bytes, one data item for the document's first root (CBOR
 * Sequences are item 03). The item-13 discipline: one exact-sizing
 * walk, one allocation, linear writes. Every argument is minimal
 * (shortest form, s4.2.1); floats use the preferred width (s4.2.2:
 * smallest of half/single/double that round-trips; NaN canonicalizes
 * to f9 7e00).
 *
 * Canonical mode (YEPTRIS_CBOR_CANONICAL): the core deterministic
 * profile — minimal lengths, definite lengths, map keys sorted by the
 * bytewise lexicographic order of their full encoded items. Stability:
 * encode(decode(x)) is byte-identical across repeated encodes.
 *
 * Mapping ledger (mirrors decode.c's; pinned by tests):
 * - tag-INT/FLOAT text re-converts through the number kernel (SSOT);
 *   beyond-int64 shapes encode as preferred floats (the 01 ledger
 *   widened them — the integer width does not survive the DOM)
 * - tag-STR scalars encode as TEXT strings (major type 3): a byte
 *   string that decoded through the DOM re-encodes as text; both
 *   sides compare equal as DOM trees
 * - the node's tag chain ("2 55799", outermost first) splits back
 *   into tag heads verbatim; any other tag shape (a YAML !tag) is
 *   YEPTRIS_ERROR_UNSUPPORTED — CBOR tag numbers are decimal
 * - aliases (YAML input) are UNSUPPORTED: the CBOR model has none
 */
#include "cbor.h"
#include "encode_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <yeptris/cbor.h>
#include <yeptris/resolve.h>

#include "../common/error.h"
#include "../doc.h"
#include "../encoding/encoding.h"
#include "../scan/json.h"

/* ---- heads: minimal argument encoding (RFC 8949 s3) ----------------- */

size_t yep_cbor_arg_bytes(uint64_t arg) {
    if (arg < 24)
        return 1;
    if (arg <= 0xFFull)
        return 2;
    if (arg <= 0xFFFFull)
        return 3;
    if (arg <= 0xFFFFFFFFull)
        return 5;
    return 9;
}

void yep_cbor_put_head(uint8_t* out, size_t* pos, uint8_t mt, uint64_t arg) {
    /* payload bytes -> additional info: 1->24, 2->25, 4->26, 8->27 */
    static const uint8_t ai_of[9] = {0, 24, 25, 25, 26, 26, 26, 26, 27};
    size_t n = yep_cbor_arg_bytes(arg) - 1;
    out[(*pos)++] = (uint8_t)((mt << 5) | (n == 0 ? (uint8_t)arg : ai_of[n]));
    for (size_t k = 0; k < n; k++) { /* network byte order */
        out[(*pos)++] = (uint8_t)(arg >> (8 * (n - 1 - k)));
    }
}

/* ---- half floats, both directions ----------------------------------- */

static double cbor_h2d(uint16_t h) {
    unsigned exp = (h >> 10) & 0x1fu;
    unsigned mant = h & 0x3ffu;
    double val;
    if (exp == 0) {
        val = (double)mant;
        for (int k = 0; k < 24; k++) {
            val *= 0.5;
        }
    } else if (exp != 31) {
        val = (double)(mant + 1024);
        for (int k = 0; k < (int)exp - 25; k++) { /* halves above 2048 */
            val *= 2.0;
        }
        for (int k = 0; k < 25 - (int)exp; k++) {
            val *= 0.5;
        }
    } else {
        val = mant == 0 ? (double)INFINITY : (double)NAN;
    }
    if (h & 0x8000u) {
        val = -val;
    }
    return val;
}

/* Round-to-nearest-even double -> half. */
static uint16_t cbor_d2h(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    uint32_t sign = (uint32_t)((bits >> 48) & 0x8000u);
    uint32_t exp = (uint32_t)((bits >> 52) & 0x7ffu);
    uint64_t man = bits & 0xfffffffffffffull;
    if (exp == 0x7ffu) {
        /* canonical NaN: f9 7e00, sign DROPPED — NaN sign is not
         * meaningful (and (double)NAN carries a SET sign bit on
         * arm64-MSVC, which turned the "canonical" form negative) */
        return man ? 0x7e00u : (uint16_t)(sign | 0x7c00u); /* inf keeps sign */
    }
    int e2 = (int)exp - 1023 + 15;
    if (e2 >= 0x1f) {
        return (uint16_t)(sign | 0x7c00u); /* overflow -> infinity */
    }
    if (e2 > 0) {
        uint32_t m10 = (uint32_t)(man >> 42);
        uint64_t round = (man >> 41) & 1;
        uint64_t sticky = man & ((1ull << 41) - 1);
        if (round && (sticky || (m10 & 1))) {
            m10++;
            if (m10 == 0x400u) {
                m10 = 0;
                e2++;
                if (e2 >= 0x1f) {
                    return (uint16_t)(sign | 0x7c00u);
                }
            }
        }
        return (uint16_t)(sign | ((uint32_t)e2 << 10) | m10);
    }
    if (e2 < -10) {
        return (uint16_t)sign; /* underflow to signed zero */
    }
    /* subnormal half: m10 = round_even((2^52+man) / 2^shift) with the
     * result carrying the implicit one (values 0x200..0x3ff here) */
    uint64_t full = man | (1ull << 52);
    uint32_t shift = (uint32_t)(43 - e2); /* m10 = full / 2^(43-e2): e2=0
                                             -> shift 43 (subnormal MSB) */
    uint32_t m10 = (uint32_t)(full >> shift);
    uint32_t round = (uint32_t)((full >> (shift - 1)) & 1);
    uint64_t sticky = full & ((1ull << (shift - 1)) - 1);
    if (round && (sticky || (m10 & 1))) {
        m10++;
        if (m10 == 0x400u) {
            return (uint16_t)(sign | (1u << 10)); /* promoted to min normal */
        }
    }
    return (uint16_t)(sign | m10);
}

uint8_t yep_cbor_float_width(double d) {
    if (d != d) {
        return 2; /* NaN: the canonical f9 7e00 (s4.2.2) */
    }
    if (cbor_h2d(cbor_d2h(d)) == d) {
        return 2;
    }
    float f = (float)d;
    if ((double)f == d) {
        return 4;
    }
    return 8;
}

void yep_cbor_put_float(uint8_t* out, size_t* pos, double d, uint8_t width) {
    out[(*pos)++] = (uint8_t)((7 << 5) | (width == 2 ? 25 : width == 4 ? 26 : 27));
    uint64_t bits = 0;
    if (width == 2) {
        bits = (uint64_t)cbor_d2h(d) << 48;
    } else if (width == 4) {
        float f = (float)d;
        uint32_t w;
        memcpy(&w, &f, sizeof(w));
        bits = (uint64_t)w << 32;
    } else {
        memcpy(&bits, &d, sizeof(bits));
    }
    for (uint8_t k = 0; k < width; k++) { /* bits holds the value top-aligned */
        out[(*pos)++] = (uint8_t)(bits >> (8 * (7 - k)));
    }
}

/* ---- scalar decoding of node text (the number-kernel SSOT) ---------- */

typedef struct {
    uint8_t mt; /* 0/1 integers, 7 floats */
    uint64_t uval;
    double dval;
    uint8_t fwidth;
} cbor_num;

static int cbor_scalar_number(const yep_view* v, cbor_num* out) {
    size_t i = 0;
    int shape = 0;
    int64_t iv = 0;
    double dv = 0;
    if (v->len == 0 || v->len > 32) { /* shortest text needed; d2s maxes ~24 */
        return 0;
    }
    /* non-finite words (the decode ledger's rendering) */
    if (v->len == 3 && memcmp(v->p, "NaN", 3) == 0) {
        out->mt = 7;
        out->dval = (double)NAN;
        out->fwidth = 2;
        return 1;
    }
    if ((v->len == 8 && memcmp(v->p, "Infinity", 8) == 0) ||
        (v->len == 9 && memcmp(v->p, "-Infinity", 9) == 0)) {
        out->mt = 7;
        out->dval = v->p[0] == '-' ? -(double)INFINITY : (double)INFINITY;
        out->fwidth = 2; /* half holds infinities exactly */
        return 1;
    }
    if (!yep_json_number_scan(v->p, v->len, &i, &shape, &iv, &dv) || i != v->len) {
        return 0;
    }
    if (shape == 0) {
        if (iv < 0) {
            out->mt = 1;
            out->uval = (uint64_t)(-(iv + 1));
        } else {
            out->mt = 0;
            out->uval = (uint64_t)iv;
        }
        return 1;
    }
    out->mt = 7;
    out->dval = dv;
    out->fwidth = yep_cbor_float_width(dv);
    return 1;
}

static int cbor_bool_value(const yep_view* v) {
    /* first byte decides: t/y/on-class true, f/n/off-class false
     * (the YAML 1.1 resolver already typed the node as BOOL) */
    if (v->len == 0) {
        return 0;
    }
    switch (v->p[0]) {
    case 't':
    case 'T':
    case 'y':
    case 'Y':
    case 'o':
    case 'O':
        return 1;
    default:
        return 0;
    }
}

/* The diagnostic round-trip (the 01 ledger): decode renders
 * undefined/simple(N) as text; encode recognizes them back. The words
 * are reserved — a literal "undefined" string encodes as the simple. */
int yep_cbor_simple_of(const char* p, size_t len) {
    if (len == 9 && memcmp(p, "undefined", 9) == 0) {
        return 23;
    }
    if (len > 9 && memcmp(p, "simple(", 7) == 0 && p[len - 1] == ')') {
        uint64_t val = 0;
        for (size_t i = 7; i + 1 < len; i++) {
            if (p[i] < '0' || p[i] > '9') {
                return -1;
            }
            val = val * 10 + (uint64_t)(p[i] - '0');
            if (val > 255) {
                return -1;
            }
        }
        if (val >= 24 && val < 32) {
            return -1; /* 24..31 have no well-formed encoding */
        }
        return (int)val;
    }
    return -1;
}

/* ---- tag chains: "2 55799" (outermost first) -> numbers -------------- */

static int cbor_tag_chain(const yep_dom* d, const yep_dnode* n, uint64_t* out, int max) {
    if (n->tag.len == 0) {
        return 0;
    }
    yep_view v = yep_dom_view(d, n->tag);
    int count = 0;
    size_t i = 0;
    while (i < v.len) {
        if (count >= max || v.p[i] < '0' || v.p[i] > '9') {
            return -1;
        }
        uint64_t val = 0;
        while (i < v.len && v.p[i] >= '0' && v.p[i] <= '9') {
            if (val > (UINT64_MAX - (uint64_t)(v.p[i] - '0')) / 10) {
                return -1;
            }
            val = val * 10 + (uint64_t)(v.p[i] - '0');
            i++;
        }
        out[count++] = val;
        if (i < v.len) {
            if (v.p[i] != ' ' || i + 1 >= v.len) {
                return -1; /* separators are single spaces, never trailing */
            }
            i++;
        }
    }
    return count;
}

/* ---- canonical map ordering (s4.2.1: bytewise on encoded keys) ------ */

typedef struct {
    uint32_t map; /* map node id */
    uint32_t pairs;
    uint32_t* child; /* map children ids in chain order (k,v,k,v) */
    uint32_t* order; /* pair emit order (indices into child) */
} cmap;

typedef struct {
    const yep_dom* d;
    int canonical;
    int failed;
    YeptrisStatus status;
    cmap* maps;
    size_t nmaps, maps_cap;
    size_t next_map; /* write-order cursor: preps are consumed in the
                        same pre-order the sizing pass built them */
} cenc;

/* Encodes one KEY item (tags + scalar) into exactly-sized scratch. */
static uint8_t* cbor_encode_key(const yep_dom* d, const yep_dnode* key, size_t* out_len) {
    yep_view v = yep_dom_view(d, key->value);
    size_t cap = 16 * 9 + 9 + v.len; /* tags + head + text */
    uint8_t* buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    size_t pos = 0;
    uint64_t tags[16];
    int ntags = cbor_tag_chain(d, key, tags, 16);
    if (ntags < 0) {
        goto bad;
    }
    for (int t = 0; t < ntags; t++) {
        yep_cbor_put_head(buf, &pos, 6, tags[t]);
    }
    if (key->tag_id == YEPTRIS_TAG_INT || key->tag_id == YEPTRIS_TAG_FLOAT) {
        cbor_num num;
        if (!cbor_scalar_number(&v, &num)) {
            goto bad;
        }
        if (num.mt != 7) {
            yep_cbor_put_head(buf, &pos, num.mt, num.uval);
        } else {
            yep_cbor_put_float(buf, &pos, num.dval, num.fwidth);
        }
    } else { /* strings and diagnostics: a simple, else a text item */
        if (key->tag.len == 0) {
            int sv = yep_cbor_simple_of(v.p, v.len);
            if (sv >= 0) {
                if (sv < 24) {
                    buf[pos++] = (uint8_t)(0xE0 | (uint8_t)sv);
                } else {
                    buf[pos++] = 0xF8;
                    buf[pos++] = (uint8_t)sv;
                }
                *out_len = pos;
                return buf;
            }
        }
        yep_cbor_put_head(
            buf, &pos,
            v.len == 0 || yep_utf8_validate((const unsigned char*)v.p, v.len, NULL) ? 3 : 2, v.len);
        if (v.len > 0) {
            memcpy(buf + pos, v.p, v.len);
            pos += v.len;
        }
    }
    *out_len = pos;
    return buf;
bad:
    free(buf);
    return NULL;
}

static void cenc_maps_free(cenc* e) {
    for (size_t m = 0; m < e->nmaps; m++) {
        free(e->maps[m].child);
        free(e->maps[m].order);
    }
    free(e->maps);
    e->maps = NULL;
    e->nmaps = 0;
    e->maps_cap = 0;
}

static cmap* cenc_map_add(cenc* e) {
    if (e->nmaps >= e->maps_cap) {
        size_t cap = e->maps_cap ? e->maps_cap * 2 : 16;
        cmap* nm = realloc(e->maps, cap * sizeof(*nm));
        if (nm == NULL) {
            e->failed = 1;
            e->status = YEPTRIS_ERROR_MEMORY;
            return NULL;
        }
        e->maps = nm;
        e->maps_cap = cap;
    }
    cmap* m = &e->maps[e->nmaps++];
    m->map = 0;
    m->pairs = 0;
    m->child = NULL;
    m->order = NULL;
    return m;
}

/* Builds the canonical permutation for one map (stable bytewise sort
 * of encoded keys). The child array also serves the write pass. */
static int cbor_canon_prepare(cenc* e, uint32_t id, uint32_t pairs) {
    const yep_dom* d = e->d;
    cmap* m = cenc_map_add(e);
    if (m == NULL) {
        return 0;
    }
    m->map = id;
    m->pairs = pairs;
    m->child = malloc((size_t)pairs * 2 * sizeof(uint32_t));
    m->order = malloc((size_t)pairs * sizeof(uint32_t));
    uint8_t** keys = malloc((size_t)pairs * sizeof(uint8_t*));
    size_t* klen = malloc((size_t)pairs * sizeof(size_t));
    if (m->child == NULL || m->order == NULL || keys == NULL || klen == NULL) {
        free(keys);
        free(klen);
        e->failed = 1;
        e->status = YEPTRIS_ERROR_MEMORY;
        return 0;
    }
    uint32_t c = d->nodes[id].first_child;
    for (uint32_t k = 0; k < pairs * 2; k++) {
        m->child[k] = c;
        c = d->nodes[c].next_sibling;
    }
    for (uint32_t p = 0; p < pairs; p++) {
        keys[p] = NULL;
        klen[p] = 0;
    }
    for (uint32_t p = 0; p < pairs; p++) {
        keys[p] = cbor_encode_key(d, &d->nodes[m->child[p * 2]], &klen[p]);
        if (keys[p] == NULL) {
            e->failed = 1;
            e->status = YEPTRIS_ERROR_UNSUPPORTED;
            for (uint32_t q = 0; q < pairs; q++) {
                free(keys[q]);
            }
            free(keys);
            free(klen);
            return 0;
        }
    }
    for (uint32_t p = 0; p < pairs; p++) {
        m->order[p] = p;
    }
    for (uint32_t p = 1; p < pairs; p++) { /* insertion sort: stable */
        uint32_t cur = m->order[p];
        size_t j = p;
        while (j > 0) {
            uint32_t other = m->order[j - 1];
            const uint8_t* a = keys[cur];
            const uint8_t* b = keys[other];
            size_t la = klen[cur];
            size_t lb = klen[other];
            size_t min = la < lb ? la : lb;
            int cmp = memcmp(a, b, min);
            if (cmp < 0 || (cmp == 0 && la < lb)) {
                m->order[j] = m->order[j - 1];
                j--;
            } else {
                break;
            }
        }
        m->order[j] = cur;
    }
    for (uint32_t p = 0; p < pairs; p++) {
        free(keys[p]);
    }
    free(keys);
    free(klen);
    return 1;
}

/* ---- the two passes -------------------------------------------------- */

static size_t cbor_size_item(cenc* e, uint32_t id);

static size_t cbor_size_scalar(cenc* e, const yep_dnode* n) {
    const yep_dom* d = e->d;
    size_t sz = 0;
    uint64_t tags[16];
    int ntags = cbor_tag_chain(d, n, tags, 16);
    if (ntags < 0) {
        e->failed = 1;
        e->status = YEPTRIS_ERROR_UNSUPPORTED;
        return 0;
    }
    for (int t = 0; t < ntags; t++) {
        sz += yep_cbor_arg_bytes(tags[t]);
    }
    yep_view v = yep_dom_view(d, n->value);
    switch (n->tag_id) {
    case YEPTRIS_TAG_INT:
    case YEPTRIS_TAG_FLOAT: {
        cbor_num num;
        if (!cbor_scalar_number(&v, &num)) {
            e->failed = 1;
            e->status = YEPTRIS_ERROR_UNSUPPORTED;
            return 0;
        }
        return sz + (num.mt == 7 ? (size_t)(1 + num.fwidth) : yep_cbor_arg_bytes(num.uval));
    }
    case YEPTRIS_TAG_BOOL:
    case YEPTRIS_TAG_NULL:
        return sz + 1;
    default: /* str: a recognized diagnostic, else a text item */
        if (n->tag.len == 0) {
            int sv = yep_cbor_simple_of(v.p, v.len);
            if (sv >= 0) {
                return sz + ((uint32_t)sv < 24 ? 1 : 2);
            }
        }
        return sz + yep_cbor_arg_bytes(v.len) + v.len;
    }
}

static size_t cbor_size_item(cenc* e, uint32_t id) {
    const yep_dom* d = e->d;
    const yep_dnode* n = &d->nodes[id];
    if (n->kind == YEP_DOM_SCALAR) {
        return cbor_size_scalar(e, n); /* includes the node's own tags */
    }
    size_t sz = 0;
    uint64_t tags[16];
    int ntags = cbor_tag_chain(d, n, tags, 16);
    if (ntags < 0) {
        e->failed = 1;
        e->status = YEPTRIS_ERROR_UNSUPPORTED;
        return 0;
    }
    for (int t = 0; t < ntags; t++) {
        sz += yep_cbor_arg_bytes(tags[t]);
    }
    if (n->kind == YEP_DOM_ALIAS) {
        e->failed = 1;
        e->status = YEPTRIS_ERROR_UNSUPPORTED;
        return 0;
    }
    if (n->kind == YEP_DOM_SEQUENCE) {
        sz += yep_cbor_arg_bytes(n->count);
        uint32_t c = n->first_child;
        for (uint32_t k = 0; k < n->count && !e->failed; k++) {
            sz += cbor_size_item(e, c);
            c = d->nodes[c].next_sibling;
        }
        return e->failed ? 0 : sz;
    }
    /* mapping */
    uint32_t pairs = n->count / 2;
    sz += yep_cbor_arg_bytes(pairs);
    /* hold the INDEX, not the pointer: sizing a child recurses into
     * nested maps whose preps REALLOC e->maps (#152 — a held cmap*
     * dangled and the next pair read walked freed memory; n>=11 of
     * the depth-3 users shape crossed the 16->32 growth) */
    size_t canon_mi = 0;
    int canon = 0;
    if (e->canonical) {
        if (!cbor_canon_prepare(e, id, pairs)) {
            return 0;
        }
        canon_mi = e->nmaps - 1;
        canon = 1;
    }
    if (canon) {
        for (uint32_t p = 0; p < pairs && !e->failed; p++) {
            const cmap* cm = &e->maps[canon_mi];
            sz += cbor_size_item(e, cm->child[cm->order[p] * 2]);
            if (!e->failed) {
                cm = &e->maps[canon_mi];
                sz += cbor_size_item(e, cm->child[cm->order[p] * 2 + 1]);
            }
        }
    } else {
        uint32_t c = n->first_child; /* the chain IS the pair order */
        for (uint32_t p = 0; p < pairs && !e->failed; p++) {
            sz += cbor_size_item(e, c);
            c = d->nodes[c].next_sibling;
            if (!e->failed && c != UINT32_MAX) {
                sz += cbor_size_item(e, c);
                c = d->nodes[c].next_sibling;
            }
        }
    }
    return e->failed ? 0 : sz;
}

static void cbor_write_item(cenc* e, uint32_t id, uint8_t* out, size_t* pos);

static void cbor_write_scalar(cenc* e, const yep_dnode* n, uint8_t* out, size_t* pos) {
    /* the node's tag chain was already emitted by cbor_write_item */
    const yep_dom* d = e->d;
    yep_view v = yep_dom_view(d, n->value);
    switch (n->tag_id) {
    case YEPTRIS_TAG_INT:
    case YEPTRIS_TAG_FLOAT: {
        cbor_num num;
        if (!cbor_scalar_number(&v, &num)) {
            return; /* pass 1 already failed on this; unreachable */
        }
        if (num.mt == 7) {
            yep_cbor_put_float(out, pos, num.dval, num.fwidth);
        } else {
            yep_cbor_put_head(out, pos, num.mt, num.uval);
        }
        return;
    }
    case YEPTRIS_TAG_BOOL:
        out[(*pos)++] = cbor_bool_value(&v) ? 0xF5 : 0xF4;
        return;
    case YEPTRIS_TAG_NULL:
        out[(*pos)++] = 0xF6;
        return;
    default:
        if (n->tag.len == 0) {
            int sv = yep_cbor_simple_of(v.p, v.len);
            if (sv >= 0) {
                if (sv < 24) {
                    out[(*pos)++] = (uint8_t)(0xE0 | (uint8_t)sv);
                } else {
                    out[(*pos)++] = 0xF8;
                    out[(*pos)++] = (uint8_t)sv;
                }
                return;
            }
        }
        /* invalid UTF-8 can only have been a byte string (the 01
         * ledger: both decode to tag-str scalars) — emit mt2 so the
         * roundtrip re-decodes to the same bytes */
        yep_cbor_put_head(
            out, pos,
            v.len == 0 || yep_utf8_validate((const unsigned char*)v.p, v.len, NULL) ? 3 : 2, v.len);
        if (v.len > 0) { /* memcpy(_, NULL, 0) is UB (gcc UBSan) */
            memcpy(out + *pos, v.p, v.len);
            *pos += v.len;
        }
        return;
    }
}

static void cbor_write_item(cenc* e, uint32_t id, uint8_t* out, size_t* pos) {
    const yep_dom* d = e->d;
    const yep_dnode* n = &d->nodes[id];
    uint64_t tags[16];
    int ntags = cbor_tag_chain(d, n, tags, 16);
    for (int t = 0; t < ntags; t++) {
        yep_cbor_put_head(out, pos, 6, tags[t]);
    }
    if (n->kind == YEP_DOM_SCALAR) {
        cbor_write_scalar(e, n, out, pos);
        return;
    }
    if (n->kind == YEP_DOM_SEQUENCE) {
        yep_cbor_put_head(out, pos, 4, n->count);
        uint32_t c = n->first_child;
        for (uint32_t k = 0; k < n->count; k++) {
            cbor_write_item(e, c, out, pos);
            c = d->nodes[c].next_sibling;
        }
        return;
    }
    uint32_t pairs = n->count / 2;
    yep_cbor_put_head(out, pos, 5, pairs);
    cmap* cm = NULL;
    if (e->canonical && e->next_map < e->nmaps) {
        cm = &e->maps[e->next_map++]; /* walk order == build order */
    }
    if (cm != NULL) {
        for (uint32_t p = 0; p < pairs; p++) {
            cbor_write_item(e, cm->child[cm->order[p] * 2], out, pos);
            cbor_write_item(e, cm->child[cm->order[p] * 2 + 1], out, pos);
        }
    } else {
        uint32_t c = n->first_child;
        for (uint32_t p = 0; p < pairs && c != UINT32_MAX; p++) {
            cbor_write_item(e, c, out, pos);
            c = d->nodes[c].next_sibling;
            if (c != UINT32_MAX) {
                cbor_write_item(e, c, out, pos);
                c = d->nodes[c].next_sibling;
            }
        }
    }
}

/* the write pass consumes the canonical preps in walk order; drop each
 * map's arrays after its last use is not tracked — freed once at exit */

/* ONE sizing walk (it also builds the canonical map preparations);
 * writes only when out and cap allow — the needed size always comes
 * back through *out_len. *written flags whether bytes went out. */
static int cbor_run(YeptrisDocument handle, uint32_t opts, uint8_t* out, size_t cap,
                    size_t* out_len, int* written) {
    yeptris_document* doc = (yeptris_document*)handle;
    yep_dom* d = yep_doc_dom(doc); /* #342 lazy: materialize on encode */
    if (d == NULL) {
        return YEPTRIS_ERROR_MEMORY;
    }
    if (d->dcount == 0) {
        return YEPTRIS_ERROR_ARG;
    }
    cenc e = {d, (opts & YEPTRIS_CBOR_CANONICAL) != 0, 0, YEPTRIS_OK, NULL, 0, 0, 0};
    size_t total = cbor_size_item(&e, d->docs[0]);
    if (e.failed) {
        cenc_maps_free(&e);
        return (int)e.status;
    }
    *out_len = total;
    if (out == NULL || cap < total) {
        cenc_maps_free(&e);
        *written = 0;
        return YEPTRIS_OK;
    }
    size_t pos = 0;
    cbor_write_item(&e, d->docs[0], out, &pos);
    cenc_maps_free(&e);
    if (pos != total) { /* the two passes diverged — a bug, refuse */
        return YEPTRIS_ERROR_INTERNAL;
    }
    *out_len = pos;
    *written = 1;
    return YEPTRIS_OK;
}

YEPTRIS_API size_t yeptris_cbor_encode_into(YeptrisDocument doc, uint32_t opts, void* buf,
                                            size_t cap) {
    if (doc == NULL) {
        return 0;
    }
    size_t out_len = 0;
    int wrote = 0;
    YeptrisStatus st = (YeptrisStatus)cbor_run(doc, opts, (uint8_t*)buf, cap, &out_len, &wrote);
    if (st != YEPTRIS_OK) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "cbor encode failed (status %d)", (int)st);
        return 0;
    }
    return out_len; /* the need when unwritten, the count when written */
}

YEPTRIS_API void* yeptris_cbor_encode(YeptrisDocument doc, uint32_t opts, size_t* len) {
    size_t need = yeptris_cbor_encode_into(doc, opts, NULL, 0);
    if (need == 0) {
        return NULL;
    }
    uint8_t* buf = malloc(need);
    if (buf == NULL) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0, "cbor encode: out of memory");
        return NULL;
    }
    size_t written = yeptris_cbor_encode_into(doc, opts, buf, need);
    if (written != need) {
        free(buf);
        return NULL;
    }
    if (len != NULL) {
        *len = written;
    }
    return buf;
}

YEPTRIS_API size_t yeptris_cbor_encode_sequence_into(YeptrisDocument* items, size_t n,
                                                     uint32_t opts, void* buf, size_t cap) {
    if (items == NULL && n != 0) {
        return 0;
    }
    size_t need = 0;
    size_t* sizes = malloc(n * sizeof(size_t));
    if (sizes == NULL) {
        return 0;
    }
    for (size_t k = 0; k < n; k++) {
        if (items[k] == NULL) {
            free(sizes);
            return 0;
        }
        size_t one = 0;
        int wrote = 0;
        if (cbor_run(items[k], opts, NULL, 0, &one, &wrote) != YEPTRIS_OK) {
            free(sizes);
            return 0;
        }
        sizes[k] = one;
        need += one;
    }
    if (buf == NULL || cap < need) {
        free(sizes);
        return need;
    }
    size_t pos = 0;
    for (size_t k = 0; k < n; k++) {
        size_t one = 0;
        int wrote = 0;
        if (cbor_run(items[k], opts, (uint8_t*)buf + pos, sizes[k], &one, &wrote) != YEPTRIS_OK ||
            !wrote || one != sizes[k]) {
            free(sizes);
            return 0;
        }
        pos += one;
    }
    free(sizes);
    return pos;
}

YEPTRIS_API void* yeptris_cbor_encode_sequence(YeptrisDocument* items, size_t n, uint32_t opts,
                                               size_t* len) {
    size_t need = yeptris_cbor_encode_sequence_into(items, n, opts, NULL, 0);
    if (need == 0) {
        return NULL;
    }
    uint8_t* buf = malloc(need);
    if (buf == NULL) {
        return NULL;
    }
    size_t wrote = yeptris_cbor_encode_sequence_into(items, n, opts, buf, need);
    if (wrote != need) {
        free(buf);
        return NULL;
    }
    if (len != NULL) {
        *len = wrote;
    }
    return buf;
}
