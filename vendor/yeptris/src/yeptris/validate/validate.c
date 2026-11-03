/* validate.c — the schema-family validation walk (#882 slice 1).
 *
 * One engine pass builds the DOM (the eager route: input-borrowed
 * spans, no tape), one walk checks it against the family:
 *
 *  - both families: explicit-tag consistency (the tag asserts a type
 *    the content cannot carry) and mapping-key uniqueness (spec
 *    3.2.1.3: keys compare by canonical form — 0o13 and 0xB are ONE
 *    key; strings byte-wise; collections structurally);
 *  - 12_JSON only: mapping keys must be string scalars; plain scalars
 *    must match a JSON production (spec 10.2.2's `*` -> Error row);
 *    explicit non-JSON tags rejected;
 *  - 12_FULL only: the core checks plus the full family's timestamp
 *    type — explicit !!timestamp content must re-resolve through the
 *    full12 classifier (spec 2.22's four forms), and untagged plains
 *    resolve timestamps at parse time (the resolver, not this walk).
 *    !!binary/!!merge/!!value/consumer tags ride as passthrough
 *    (spec 10.4: arbitrary explicit tags never invalidate a schema).
 *
 * 11_COMPAT is the parse-compat resolver, not a validation family —
 * the entry rejects it (ARG).
 *
 * MECE: parse/ owns the grammar (this walk only reads finished
 * nodes), resolve/ owns typing (the family resolvers), emit/ owns
 * bytes; diagnostics shape lives in validate.h. Allocation: the
 * report and the scratch (line index, path, dedup table) come from
 * the system allocator and are freed before return — nothing borrows
 * the caller's buffer beyond the call.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yeptris/dom.h>
#include <yeptris/validate.h>

#include "../common/error.h"
#include "../doc.h"
#include "../memory/allocator.h"
#include "../parse/numbers.h"
#include "../resolve/resolver.h"
#include "../tape_in.h"
#include "internal.h"

/* ---- line/col resolution ---------------------------------------------- */

/* Builds the line-start index once, on the first positioned diagnostic;
 * valid documents never pay. Returns 0 ok, -1 OOM (positions unknown). */
static int line_index_build(yep_vctx* c) {
    if (c->line_off != NULL) {
        return 0;
    }
    const yep_allocator* sys = yep_system_allocator();
    uint32_t cap = 256;
    uint32_t n = 0;
    uint32_t* off = yep_alloc(sys, cap * sizeof(uint32_t));
    if (off == NULL) {
        return -1;
    }
    off[n++] = 0;
    const char* p = c->input;
    for (size_t i = 0; i < c->input_len; i++) {
        if (p[i] == '\n') {
            if (n == cap) {
                uint32_t ncap = cap * 2;
                uint32_t* no = yep_alloc(sys, (size_t)ncap * sizeof(uint32_t));
                if (no == NULL) {
                    yep_free(sys, off);
                    return -1;
                }
                memcpy(no, off, (size_t)n * sizeof(uint32_t));
                yep_free(sys, off);
                off = no;
                cap = ncap;
            }
            off[n++] = (uint32_t)(i + 1);
        }
    }
    c->line_off = off;
    c->line_cnt = n;
    c->line_cap = cap;
    return 0;
}

/* 1-based line/col of a byte offset; columns are code points (the
 * libyaml mark semantics — multibyte characters advance one). */
static void line_col_of(yep_vctx* c, uint32_t off, uint32_t* line, uint32_t* col) {
    *line = 0;
    *col = 0;
    if (off == UINT32_MAX || off >= c->input_len) {
        return;
    }
    if (line_index_build(c) != 0) {
        return;
    }
    uint32_t lo = 0, hi = c->line_cnt; /* find the last line start <= off */
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (c->line_off[mid] <= off) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    uint32_t start = c->line_off[lo];
    uint32_t chars = 0;
    for (uint32_t i = start; i <= off; i++) {
        chars += ((((unsigned char)c->input[i]) & 0xC0) != 0x80);
    }
    *line = lo + 1;
    *col = chars;
}

/* ---- path scratch ------------------------------------------------------ */

static int path_reserve(yep_vctx* c, size_t need) {
    if (c->path_len + need <= c->path_cap) {
        return 0;
    }
    const yep_allocator* sys = yep_system_allocator();
    size_t ncap = c->path_cap ? c->path_cap * 2 : 256;
    while (ncap < c->path_len + need) {
        ncap *= 2;
    }
    char* np = yep_alloc(sys, ncap);
    if (np == NULL) {
        c->oom = 1;
        return -1;
    }
    if (c->path != NULL) {
        memcpy(np, c->path, c->path_len);
        yep_free(sys, c->path);
    }
    c->path = np;
    c->path_cap = ncap;
    return 0;
}

int yep_validate_path_push(yep_vctx* c, const char* seg, size_t seg_len) {
    /* JSON-pointer escapes: ~ -> ~0, / -> ~1 */
    size_t need = 1 + seg_len + 2; /* worst case: every byte escapes */
    for (size_t i = 0; i < seg_len; i++) {
        if (seg[i] == '~' || seg[i] == '/') {
            need++;
        }
    }
    if (path_reserve(c, need) != 0) {
        return -1;
    }
    c->path[c->path_len++] = '/';
    for (size_t i = 0; i < seg_len; i++) {
        char b = seg[i];
        if (b == '~') {
            c->path[c->path_len++] = '~';
            c->path[c->path_len++] = '0';
        } else if (b == '/') {
            c->path[c->path_len++] = '~';
            c->path[c->path_len++] = '1';
        } else {
            c->path[c->path_len++] = b;
        }
    }
    return 0;
}

int yep_validate_path_push_index(yep_vctx* c, uint32_t idx) {
    char buf[12];
    int n = snprintf(buf, sizeof(buf), "%u", idx);
    return yep_validate_path_push(c, buf, (size_t)n);
}

void yep_validate_path_pop(yep_vctx* c, size_t mark) {
    c->path_len = mark;
}

size_t yep_validate_doc_prefix(yep_vctx* c, int qualified, uint32_t index) {
    size_t mark = c->path_len;
    if (qualified && yep_validate_path_push_index(c, index) != 0) {
        c->oom = 1; /* the path scratch failed: the walk unwinds */
    }
    return mark;
}

/* ---- the report --------------------------------------------------------- */

/* The report-path writer (the cap law's one home). */
void yep_validate_report_put(yep_vctx* c, const yeptris_diagnostic* d) {
    yeptris_validate_report* r = c->rep;
    if (r->count >= YEP_VALIDATE_MAX_DIAGS) {
        /* the cap: the LAST slot becomes the LIMIT sentinel */
        r->truncated = 1;
        yeptris_diagnostic* last = &r->diags[r->count - 1];
        memset(last, 0, sizeof(*last));
        last->code = YEPTRIS_DIAG_LIMIT;
        snprintf(last->detail, sizeof(last->detail), "diagnostics truncated at %d",
                 YEP_VALIDATE_MAX_DIAGS);
        return;
    }
    r->diags[r->count++] = *d;
}

void yep_validate_emit(yep_vctx* c, int code, uint32_t off, const char* detail_fmt, ...) {
    if (c->suppress > 0) {
        c->trial_fail++; /* a probe (anyOf/oneOf/not/if): swallow, fail its own extent */
        return;
    }
    c->fail_mark++; /* the application that owns this extent is invalid */
    yeptris_diagnostic d;
    memset(&d, 0, sizeof(d));
    d.code = (uint32_t)code;
    if (off != UINT32_MAX && off < c->input_len) {
        d.offset = off;
        line_col_of(c, off, &d.line, &d.col);
    }
    size_t plen = c->path_len < YEPTRIS_DIAG_PATH_MAX - 1 ? c->path_len : YEPTRIS_DIAG_PATH_MAX - 1;
    if (c->path != NULL) {
        memcpy(d.path, c->path, plen);
    }
    va_list ap;
    va_start(ap, detail_fmt);
    vsnprintf(d.detail, sizeof(d.detail), detail_fmt, ap);
    va_end(ap);
    if (c->stage_put != NULL) {
        /* the event route's staging (#902): the diagnostic joins the
         * OPEN mapping's stage — the flush replays [own tag][duplicates]
         * [pair content] so the report order stays the DOM walk's.
         * NOTHING is dropped here: the walk position is unknown until
         * the flush order is fixed (a mapping's duplicates precede its
         * content), so the cap law applies at the replay, through
         * yep_validate_report_put — the staging holds O(violations),
         * the same input-bounded class as the DOM route's tree. */
        c->staged++;
        c->stage_put(c->stage, &d);
        return;
    }
    yep_validate_report_put(c, &d);
}

/* ---- node facts --------------------------------------------------------- */

uint32_t yep_validate_node_off(const yep_dom* d, uint32_t id) {
    uint32_t guard = 0;
    while (id != UINT32_MAX && guard++ < YEP_DOM_MAX_DEPTH) {
        const yep_dnode* n = yep_dom_node(d, id);
        if (n == NULL) {
            return UINT32_MAX;
        }
        if (n->kind == YEP_DOM_SCALAR || n->kind == YEP_DOM_ALIAS) {
            if (n->value.len > 0 && (n->value.off & YEP_SV_INPUT) == 0) {
                return n->value.off & YEP_SV_OFF; /* borrowed from the input */
            }
            return UINT32_MAX; /* arena-owned bytes: no input position */
        }
        id = n->first_child; /* collections: position at their first leaf */
    }
    return UINT32_MAX;
}

/* ---- canonical key/value facts (shared with jsonschema.c) ---------------- */

/* The scalar tag law (internal.h's fact core; the DOM wrapper below
 * and the event route's facts both land here). */
uint8_t yep_validate_tag_of(uint8_t tag_id, int has_tag, int plainish) {
    if (has_tag) {
        return tag_id;
    }
    /* implicit marks the resolver's verdict even at style ANY (the
     * document-level empty scalar carries no token to style) */
    if (plainish) {
        return tag_id;
    }
    return YEPTRIS_TAG_STR;
}

/* The resolved tag id of a scalar node (explicit tag wins, then the
 * plain verdict already on the node, then str for quoted/block). */
uint8_t yep_validate_scalar_tag(const yep_dnode* n) {
    return yep_validate_tag_of(n->tag_id, n->tag.len > 0,
                               n->style == YEP_STYLE_PLAIN || n->implicit);
}

/* Canonical hash of a node (the key-uniqueness fingerprint): tags and
 * canonical forms, numeric values for int/float. */
uint64_t yep_validate_hash_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

/* The scalar arm (internal.h's law): base + tag; int/float fold the
 * numeric VALUE — one bucket for both tags (1 and 1.0 are the same
 * canonical number) — exact for i64-representable magnitudes up to
 * 2^53, and beyond that two distinct ints may collide (equality is
 * the authority, a collision only costs a probe); everything else
 * folds the raw bytes. */
uint64_t yep_validate_scalar_digest(uint8_t tag, yep_view v) {
    uint64_t h = YEP_VAL_DIGEST_SCALAR + tag;
    if (tag == YEPTRIS_TAG_INT || tag == YEPTRIS_TAG_FLOAT) {
        double dv = 0;
        int64_t iv = 0;
        if (yep_num_i64(v.p, v.len, &iv) == 0) {
            dv = (double)iv;
        } else {
            yep_num_f64(v.p, v.len, &dv);
        }
        uint64_t bits;
        memcpy(&bits, &dv, sizeof(bits));
        return yep_validate_hash_mix(h, bits);
    }
    for (uint32_t i = 0; i < v.len; i++) {
        h = yep_validate_hash_mix(h, (unsigned char)v.p[i]);
    }
    return h;
}

uint64_t yep_validate_node_hash(const yep_dom* d, uint32_t id, int depth) {
    if (depth > 64) {
        return 0xDA7A;
    }
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == YEP_DOM_ALIAS) {
        return n->target != UINT32_MAX ? yep_validate_node_hash(d, n->target, depth + 1)
                                       : yep_validate_hash_mix(YEP_VAL_DIGEST_ALIAS, n->value.len);
    }
    if (n->kind == YEP_DOM_SEQUENCE) {
        uint64_t h = YEP_VAL_DIGEST_SEQ_SEED;
        uint32_t ch = n->first_child;
        while (ch != UINT32_MAX) {
            h = yep_validate_hash_mix(h, yep_validate_node_hash(d, ch, depth + 1));
            ch = yep_dom_node(d, ch) ? yep_dom_node(d, ch)->next_sibling : UINT32_MAX;
        }
        return h;
    }
    if (n->kind == YEP_DOM_MAPPING) {
        /* mappings are UNORDERED (spec 3.2.1.3 canonical form): the
         * fold is commutative (XOR of per-pair mixes) so key order
         * cannot change the fingerprint */
        uint64_t h = YEP_VAL_DIGEST_MAP_SEED;
        uint32_t k = n->first_child;
        while (k != UINT32_MAX) {
            uint32_t v = yep_dom_node(d, k) ? yep_dom_node(d, k)->next_sibling : UINT32_MAX;
            uint64_t pair = yep_validate_hash_mix(YEP_VAL_DIGEST_PAIR_SEED,
                                                  yep_validate_node_hash(d, k, depth + 1));
            pair = yep_validate_hash_mix(pair, yep_validate_node_hash(d, v, depth + 1));
            h ^= pair;
            k = v != UINT32_MAX && yep_dom_node(d, v) ? yep_dom_node(d, v)->next_sibling
                                                      : UINT32_MAX;
        }
        return h;
    }
    /* scalar: tag + canonical form */
    return yep_validate_scalar_digest(yep_validate_scalar_tag(n), yep_dom_view(d, n->value));
}

/* Canonical equality (spec 3.2.1.3): same tag and canonical form.
 * Collections compare structurally; numbers within one tag compare
 * numerically. The two sides may live in different DOMs (the JSON
 * Schema face compares instances against a schema's enum/const).
 * Depth-bounded like the hashes. */
int yep_validate_node_equal(const yep_dom* da, uint32_t a, const yep_dom* db, uint32_t b,
                            int depth) {
    if (depth > 64) {
        return 0;
    }
    const yep_dnode* na = yep_dom_node(da, a);
    const yep_dnode* nb = yep_dom_node(db, b);
    if (na == NULL || nb == NULL) {
        return 0;
    }
    if (na->kind == YEP_DOM_ALIAS) {
        return na->target != UINT32_MAX &&
               yep_validate_node_equal(da, na->target, db, b, depth + 1);
    }
    if (nb->kind == YEP_DOM_ALIAS) {
        return nb->target != UINT32_MAX &&
               yep_validate_node_equal(da, a, db, nb->target, depth + 1);
    }
    if (na->kind != nb->kind) {
        return 0;
    }
    if (na->kind == YEP_DOM_SEQUENCE) {
        if (na->count != nb->count) {
            return 0;
        }
        uint32_t ca = na->first_child, cb = nb->first_child;
        while (ca != UINT32_MAX && cb != UINT32_MAX) {
            if (!yep_validate_node_equal(da, ca, db, cb, depth + 1)) {
                return 0;
            }
            const yep_dnode* da2 = yep_dom_node(da, ca);
            const yep_dnode* db2 = yep_dom_node(db, cb);
            ca = da2 ? da2->next_sibling : UINT32_MAX;
            cb = db2 ? db2->next_sibling : UINT32_MAX;
        }
        return ca == UINT32_MAX && cb == UINT32_MAX;
    }
    if (na->kind == YEP_DOM_MAPPING) {
        if (na->count != nb->count) {
            return 0;
        }
        uint32_t ka = na->first_child;
        while (ka != UINT32_MAX) {
            uint32_t va = yep_dom_node(da, ka) ? yep_dom_node(da, ka)->next_sibling : UINT32_MAX;
            /* find ka's key in b */
            uint32_t kb = nb->first_child;
            int found = 0;
            while (kb != UINT32_MAX && !found) {
                uint32_t vb =
                    yep_dom_node(db, kb) ? yep_dom_node(db, kb)->next_sibling : UINT32_MAX;
                if (yep_validate_node_equal(da, ka, db, kb, depth + 1)) {
                    if (!yep_validate_node_equal(da, va, db, vb, depth + 1)) {
                        return 0;
                    }
                    found = 1;
                }
                kb = vb != UINT32_MAX && yep_dom_node(db, vb) ? yep_dom_node(db, vb)->next_sibling
                                                              : UINT32_MAX;
            }
            if (!found) {
                return 0;
            }
            ka = va != UINT32_MAX && yep_dom_node(da, va) ? yep_dom_node(da, va)->next_sibling
                                                          : UINT32_MAX;
        }
        return 1;
    }
    /* scalars: numbers (int or float, either tag) compare by numeric
     * value (2020-12: 0 and 0.0 are one constant); everything else
     * requires the same tag */
    uint8_t ta = yep_validate_scalar_tag(na), tb = yep_validate_scalar_tag(nb);
    int numa = ta == YEPTRIS_TAG_INT || ta == YEPTRIS_TAG_FLOAT;
    int numb = tb == YEPTRIS_TAG_INT || tb == YEPTRIS_TAG_FLOAT;
    if (numa != numb) {
        return 0;
    }
    yep_view va = yep_dom_view(da, na->value);
    yep_view vb = yep_dom_view(db, nb->value);
    if (numa) {
        int64_t ia, ib;
        if (yep_num_i64(va.p, va.len, &ia) == 0 && yep_num_i64(vb.p, vb.len, &ib) == 0) {
            return ia == ib;
        }
        double fa, fb;
        if (yep_num_f64(va.p, va.len, &fa) == 0 && yep_num_f64(vb.p, vb.len, &fb) == 0) {
            return fa == fb;
        }
        return 0;
    }
    if (ta != tb) {
        return 0;
    }
    return va.len == vb.len && (va.len == 0 || memcmp(va.p, vb.p, va.len) == 0);
}

/* ---- the family walk ---------------------------------------------------- */

typedef struct {
    uint32_t hash;
    uint32_t node; /* 0 = empty slot (node 0 is the DOCUMENT root, never
                    * a key inside itself) */
} key_slot;

/* Duplicate-key detection over one mapping: open addressing, expected
 * O(pairs). An allocation failure silently skips (the walk's other
 * checks still run; oom flags the context). */
static void check_duplicates(yep_vctx* c, uint32_t map_id) {
    const yep_dom* d = c->dom;
    const yep_dnode* m = yep_dom_node(d, map_id);
    uint32_t pairs = m->count / 2;
    if (pairs < 2) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    uint32_t cap = 8;
    while (cap < pairs * 4) {
        cap *= 2;
    }
    key_slot* tbl = yep_alloc(sys, (size_t)cap * sizeof(key_slot));
    if (tbl == NULL) {
        c->oom = 1;
        return;
    }
    memset(tbl, 0, (size_t)cap * sizeof(key_slot));
    uint32_t k = m->first_child;
    while (k != UINT32_MAX) {
        uint32_t v = yep_dom_node(d, k) ? yep_dom_node(d, k)->next_sibling : UINT32_MAX;
        uint64_t h64 = yep_validate_node_hash(d, k, 0);
        uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
        uint32_t i = h & (cap - 1);
        while (tbl[i].node != 0) {
            if (tbl[i].hash == h && yep_validate_node_equal(d, tbl[i].node - 1, d, k, 0)) {
                /* enumerate EVERY miss: each key whose canonical form
                 * already appeared is its own diagnostic (the report
                 * cap bounds a hostile mapping) */
                uint32_t off = yep_validate_node_off(d, k);
                yep_view kv = yep_dom_view(d, yep_dom_node(d, k)->value);
                char key[40];
                size_t kl = kv.len < sizeof(key) - 8 ? kv.len : sizeof(key) - 8;
                if (kl > 0 && kv.p != NULL) {
                    memcpy(key, kv.p, kl);
                }
                key[kl] = '\0';
                yep_validate_emit(c, YEPTRIS_DIAG_DUPLICATE_KEY, off,
                                  "duplicate mapping key (\"%s\")", key);
                break;
            }
            i = (i + 1) & (cap - 1);
        }
        if (tbl[i].node == 0) {
            tbl[i].hash = h;
            tbl[i].node = k + 1; /* +1: node 0 means empty */
        }
        k = v != UINT32_MAX && yep_dom_node(d, v) ? yep_dom_node(d, v)->next_sibling : UINT32_MAX;
    }
    yep_free(sys, tbl);
}

static const char* tag_word(uint8_t id) {
    switch (id) {
    case YEPTRIS_TAG_STR:
        return "string";
    case YEPTRIS_TAG_INT:
        return "int";
    case YEPTRIS_TAG_FLOAT:
        return "float";
    case YEPTRIS_TAG_BOOL:
        return "bool";
    case YEPTRIS_TAG_NULL:
        return "null";
    case YEPTRIS_TAG_TIMESTAMP:
        return "timestamp";
    case YEPTRIS_TAG_SEQ:
        return "sequence";
    case YEPTRIS_TAG_MAP:
        return "mapping";
    case YEPTRIS_TAG_BINARY:
        return "binary";
    case YEPTRIS_TAG_MERGE:
        return "merge";
    case YEPTRIS_TAG_VALUE:
        return "value";
    default:
        return "custom";
    }
}

/* Explicit-tag consistency (both families): the tag asserts a type the
 * content cannot carry. seq/map check the node KIND; the five core
 * scalar tags re-resolve the content through the family resolver.
 * internal.h's fact core — the event route feeds the same five facts. */
void yep_check_tag_facts(yep_vctx* c, int kind, int has_tag, uint8_t tag_id, yep_view value,
                         uint32_t off, const yep_resolver* fam, int json_family) {
    if (!has_tag) {
        return; /* no explicit tag */
    }
    uint8_t want = tag_id;
    const char* w = tag_word(want);
    if (want == YEPTRIS_TAG_SEQ || want == YEPTRIS_TAG_MAP || want == YEPTRIS_TAG_STR) {
        int ok = want == YEPTRIS_TAG_STR ? kind == YEP_DOM_SCALAR
                                         : (want == YEPTRIS_TAG_SEQ && kind == YEP_DOM_SEQUENCE) ||
                                               (want == YEPTRIS_TAG_MAP && kind == YEP_DOM_MAPPING);
        if (!ok) {
            yep_validate_emit(c, YEPTRIS_DIAG_TAG_MISMATCH, off, "explicit %s tag on a %s", w,
                              kind == YEP_DOM_SCALAR
                                  ? "scalar"
                                  : (kind == YEP_DOM_SEQUENCE ? "sequence" : "mapping"));
        }
        return;
    }
    if (want >= YEPTRIS_TAG_INT && want <= YEPTRIS_TAG_NULL) {
        if (kind != YEP_DOM_SCALAR) {
            yep_validate_emit(c, YEPTRIS_DIAG_TAG_MISMATCH, off, "explicit %s tag on a %s", w,
                              kind == YEP_DOM_SEQUENCE ? "sequence" : "mapping");
            return;
        }
        uint8_t got = fam->resolve(fam->ctx, value.p != NULL ? value.p : "", value.len);
        if (got != want) {
            if (json_family) {
                yep_validate_emit(c, YEPTRIS_DIAG_TAG_MISMATCH, off,
                                  "explicit %s tag; content is not a JSON %s", w, w);
            } else {
                yep_validate_emit(c, YEPTRIS_DIAG_TAG_MISMATCH, off,
                                  "explicit %s tag on %s content", w, tag_word(got));
            }
        }
        return;
    }
    if (want == YEPTRIS_TAG_TIMESTAMP && fam->timestamped) {
        /* the FULL family: timestamp is a family type — the explicit
         * tag asserts content the classifier accepts (the SAME
         * classifier the untagged plain form rides, so the two cannot
         * diverge). Core leaves the tag consumer-defined (spec 10.3's
         * core schema has no timestamp); the JSON family rejects it
         * wholesale (checked by the caller). */
        if (kind != YEP_DOM_SCALAR) {
            yep_validate_emit(c, YEPTRIS_DIAG_TAG_MISMATCH, off, "explicit timestamp tag on a %s",
                              kind == YEP_DOM_SEQUENCE ? "sequence" : "mapping");
            return;
        }
        uint8_t got = fam->resolve(fam->ctx, value.p != NULL ? value.p : "", value.len);
        if (got != want) {
            yep_validate_emit(c, YEPTRIS_DIAG_TAG_MISMATCH, off,
                              "explicit timestamp tag on %s content", tag_word(got));
        }
        return;
    }
    /* binary/merge/value/custom: consumer-defined under every family
     * (spec 10.4: arbitrary explicit tags ride). The binary
     * PASSTHROUGH policy: !!binary is never family-checked — PyYAML's
     * base64 decode is a CONSTRUCTION step, so resolution and this
     * walk keep their hands off the shaping. */
}

static void check_tag(yep_vctx* c, uint32_t id, const yep_resolver* fam, int json_family) {
    const yep_dom* d = c->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    if (n->tag.len == 0) {
        return;
    }
    yep_check_tag_facts(c, (int)n->kind, 1, n->tag_id, yep_dom_view(d, n->value),
                        yep_validate_node_off(d, id), fam, json_family);
}

/* json12: a SCALAR mapping key's checks (internal.h's fact core —
 * the DOM walk and the event route both land here, so the two routes
 * cannot diverge on a verdict). A plain key matching no JSON
 * production IS a string (spec 10.2 example 10.8's "A null"/
 * "Booleans" are plain block keys) — the string-key demand bars only
 * keys resolving to non-string productions. */
void yep_check_json_key_facts(yep_vctx* c, int has_tag, uint8_t tag_id, int plainish,
                              yep_view value, uint32_t off) {
    if (has_tag) {
        uint8_t want = tag_id;
        if (want == YEPTRIS_TAG_STR) {
            return;
        }
        if (want >= YEPTRIS_TAG_INT && want <= YEPTRIS_TAG_NULL) {
            yep_validate_emit(c, YEPTRIS_DIAG_KEY_NOT_STRING, off,
                              "mapping key is %s (expected a "
                              "string)",
                              tag_word(want));
        } else {
            yep_validate_emit(c, YEPTRIS_DIAG_NON_JSON_TYPE, off,
                              "mapping key carries a non-JSON "
                              "tag");
        }
        return;
    }
    if (!plainish) {
        return; /* quoted/block keys are strings */
    }
    int cls = yep_json12_plain(value.p != NULL ? value.p : "", value.len);
    if (cls != 0 && cls != (int)YEPTRIS_TAG_STR) {
        yep_validate_emit(c, YEPTRIS_DIAG_KEY_NOT_STRING, off,
                          "mapping key is %s (expected a "
                          "string)",
                          tag_word((uint8_t)cls));
    }
}

/* json12: a mapping key must be a string scalar. */
static void check_json_key(yep_vctx* c, uint32_t key_id) {
    const yep_dom* d = c->dom;
    const yep_dnode* n = yep_dom_node(d, key_id);
    if (n->kind == YEP_DOM_ALIAS) {
        if (n->target != UINT32_MAX) {
            check_json_key(c, n->target);
        }
        return;
    }
    if (n->kind != YEP_DOM_SCALAR) {
        uint32_t off = yep_validate_node_off(d, key_id);
        yep_validate_emit(c, YEPTRIS_DIAG_KEY_NOT_STRING, off, "mapping key is a %s",
                          n->kind == YEP_DOM_SEQUENCE ? "sequence" : "mapping");
        return;
    }
    yep_check_json_key_facts(c, n->tag.len > 0, n->tag_id,
                             n->implicit || n->style == YEP_STYLE_PLAIN, yep_dom_view(d, n->value),
                             yep_validate_node_off(d, key_id));
}

/* json12 scalar facts: only explicit non-JSON tags diagnose — example
 * 10.8 resolves implicit no-production plains as strings (keys AND
 * the "Invalid" list), overriding the table's error note. */
void yep_check_json_scalar_facts(yep_vctx* c, int has_tag, uint8_t tag_id, int plainish,
                                 yep_view value, uint32_t off) {
    (void)plainish;
    (void)value;
    if (has_tag) {
        uint8_t want = tag_id;
        if (want == YEPTRIS_TAG_TIMESTAMP || want == YEPTRIS_TAG_BINARY ||
            want == YEPTRIS_TAG_MERGE || want == YEPTRIS_TAG_VALUE || want == YEPTRIS_TAG_CUSTOM) {
            yep_validate_emit(c, YEPTRIS_DIAG_NON_JSON_TYPE, off,
                              "explicit %s tag is not a JSON type", tag_word(want));
        }
    }
}

static void check_json_scalar(yep_vctx* c, uint32_t id) {
    const yep_dom* d = c->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    yep_check_json_scalar_facts(c, n->tag.len > 0, n->tag_id,
                                n->implicit || n->style == YEP_STYLE_PLAIN,
                                yep_dom_view(d, n->value), yep_validate_node_off(d, id));
}

static void walk_family(yep_vctx* c, uint32_t id, const yep_resolver* fam, int json_family);

/* The tag-driven schema hook (#906 slice 4): a node whose EXPLICIT tag
 * matches a registry entry has its subtree validated against the bound
 * compiled schema. The walk context carries the tag site's path — the
 * subtree's misses extend it — and the shared report (one cap law).
 * Unregistered tags pass untouched; tagged mapping KEYS never reach
 * here (the walk checks keys in place, no instance path of their
 * own). Registration is the only extension: this reads the registry,
 * it never grows a schema branch (OCP). */
static void check_tag_schema(yep_vctx* c, uint32_t id, const yep_dnode* n) {
    if (c->oom || c->reg == NULL || n->tag.len == 0) {
        return;
    }
    yep_view tv = yep_dom_view(c->dom, n->tag);
    if (tv.p == NULL) {
        return;
    }
    const yeptris_schema* bound = yeptris_schema_registry_lookup(c->reg, tv.p, tv.len);
    if (bound == NULL) {
        return; /* an unregistered tag is nobody's schema */
    }
    yep_schema_view v;
    yep_schema_view_of(bound, &v);
    if (v.dom == NULL) {
        return;
    }
    yep_jsonschema_apply_subtree(v.dom, v.sroot, c, id, v.cache, v.res);
}

static void walk_children(yep_vctx* c, uint32_t first, const yep_resolver* fam, int json_family) {
    uint32_t ch = first;
    int guard = 0;
    while (ch != UINT32_MAX && !c->oom) {
        if (++guard > (int)c->dom->ncount) {
            break; /* defensive: the sibling list is parse-built */
        }
        const yep_dnode* n = yep_dom_node(c->dom, ch);
        uint32_t next = n ? n->next_sibling : UINT32_MAX;
        walk_family(c, ch, fam, json_family);
        ch = next;
    }
}

static void walk_family(yep_vctx* c, uint32_t id, const yep_resolver* fam, int json_family) {
    if (c->oom) {
        return;
    }
    if (++c->depth > YEP_DOM_MAX_DEPTH) {
        if (c->depth == YEP_DOM_MAX_DEPTH + 1) {
            yep_validate_emit(c, YEPTRIS_DIAG_DEPTH, UINT32_MAX,
                              "validation walk exceeded the nesting limit");
        }
        c->depth--;
        return;
    }
    const yep_dnode* n = yep_dom_node(c->dom, id);
    if (n == NULL) {
        c->depth--;
        return;
    }
    switch (n->kind) {
    case YEP_DOM_SCALAR:
        check_tag(c, id, fam, json_family);
        if (json_family) {
            check_json_scalar(c, id);
        }
        check_tag_schema(c, id, n);
        break;
    case YEP_DOM_ALIAS:
        break; /* the target is validated at its own place */
    case YEP_DOM_SEQUENCE: {
        check_tag(c, id, fam, json_family);
        check_tag_schema(c, id, n);
        size_t mark = c->path_len;
        uint32_t idx = 0;
        uint32_t ch = n->first_child;
        while (ch != UINT32_MAX && !c->oom) {
            const yep_dnode* cn = yep_dom_node(c->dom, ch);
            uint32_t next = cn ? cn->next_sibling : UINT32_MAX;
            if (yep_validate_path_push_index(c, idx++) == 0) {
                walk_family(c, ch, fam, json_family);
                yep_validate_path_pop(c, mark);
            }
            ch = next;
        }
        break;
    }
    case YEP_DOM_MAPPING: {
        check_tag(c, id, fam, json_family);
        size_t mark = c->path_len;
        check_duplicates(c, id);
        check_tag_schema(c, id, n);
        uint32_t k = n->first_child;
        while (k != UINT32_MAX && !c->oom) {
            uint32_t v =
                yep_dom_node(c->dom, k) ? yep_dom_node(c->dom, k)->next_sibling : UINT32_MAX;
            if (json_family) {
                check_json_key(c, k);
            }
            /* keys: tag-checked like any scalar; collection keys walk
             * under the mapping's path (their own path is undefined) */
            const yep_dnode* kn = yep_dom_node(c->dom, k);
            if (kn != NULL) {
                if (kn->kind == YEP_DOM_SCALAR) {
                    check_tag(c, k, fam, json_family);
                } else if (kn->kind != YEP_DOM_ALIAS) {
                    walk_children(c, kn->first_child, fam, json_family);
                }
            }
            if (v != UINT32_MAX) {
                yep_view kv = yep_dom_view(c->dom, kn ? kn->value : (yep_sview){0, 0});
                if (yep_validate_path_push(c, kv.p != NULL ? kv.p : "", kv.len) == 0) {
                    walk_family(c, v, fam, json_family);
                    yep_validate_path_pop(c, mark);
                }
            }
            k = v != UINT32_MAX && yep_dom_node(c->dom, v) ? yep_dom_node(c->dom, v)->next_sibling
                                                           : UINT32_MAX;
        }
        break;
    }
    default:
        break;
    }
    c->depth--;
}

/* ---- the public entries -------------------------------------------------- */

YEPTRIS_API size_t yeptris_validate_report_count(const yeptris_validate_report* report) {
    return report ? report->count : 0;
}

YEPTRIS_API const yeptris_diagnostic*
yeptris_validate_report_at(const yeptris_validate_report* report, size_t index) {
    if (report == NULL || index >= report->count) {
        return NULL;
    }
    return &report->diags[index];
}

YEPTRIS_API void yeptris_validate_report_free(yeptris_validate_report* report) {
    if (report == NULL) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, report->diags);
    yep_free(sys, report);
}

/* The report constructor (internal.h's seam — one home for the shape). */
yeptris_validate_report* yep_validate_report_new(void) {
    const yep_allocator* sys = yep_system_allocator();
    yeptris_validate_report* r = yep_alloc(sys, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->diags = yep_alloc(sys, YEP_VALIDATE_MAX_DIAGS * sizeof(yeptris_diagnostic));
    if (r->diags == NULL) {
        yep_free(sys, r);
        return NULL;
    }
    r->count = 0;
    r->truncated = 0;
    return r;
}

/* The shared tail: walk every document of the DOM into a fresh report. */
static YeptrisStatus validate_dom(yep_dom* dom, const yep_resolver* fam, int json_family,
                                  const yeptris_schema_registry* reg,
                                  yeptris_validate_report** out) {
    const yep_allocator* sys = yep_system_allocator();
    yeptris_validate_report* r = yep_validate_report_new();
    if (r == NULL) {
        return YEPTRIS_ERROR_MEMORY;
    }

    yep_vctx c;
    memset(&c, 0, sizeof(c));
    c.dom = dom;
    c.rep = r;
    c.input = dom->input_base;
    c.input_len = dom->input_len;
    c.reg = reg;

    for (uint32_t i = 0; i < dom->dcount && !c.oom; i++) {
        c.path_len = 0; /* root-relative per document; a multi-document
                         * stream addresses its documents as array
                         * elements: "/i" + the instance pointer */
        size_t mark = yep_validate_doc_prefix(&c, dom->dcount > 1, i);
        walk_family(&c, dom->docs[i], fam, json_family);
        yep_validate_path_pop(&c, mark);
    }

    yep_free(sys, c.line_off);
    yep_free(sys, c.path);

    YeptrisStatus st = YEPTRIS_OK;
    if (c.oom) {
        yeptris_validate_report_free(r);
        return YEPTRIS_ERROR_MEMORY;
    }
    if (r->count > 0) {
        st = YEPTRIS_ERROR_SCHEMA;
    }
    *out = r;
    return st;
}

/* The family selector (internal.h's seam): 11_COMPAT is the
 * parse-compat resolver, not a validation family. */
int yep_validate_schema_family(YeptrisSchema schema, const yep_resolver** fam, int* json_family) {
    switch (schema) {
    case YEPTRIS_SCHEMA_12_CORE:
        *fam = yep_resolver_core12();
        *json_family = 0;
        return 1;
    case YEPTRIS_SCHEMA_12_JSON:
        *fam = yep_resolver_json12();
        *json_family = 1;
        return 1;
    case YEPTRIS_SCHEMA_12_FULL:
        *fam = yep_resolver_full12();
        *json_family = 0; /* full rides the core checks + timestamp */
        return 1;
    default:
        return 0;
    }
}

/* The options' registry field (size-versioned: an older/short struct
 * simply carries no registry — the emit-options law). */
static const yeptris_schema_registry* validate_registry(const yeptris_validate_options* opts) {
    if (opts == NULL || opts->size < (uint32_t)(offsetof(yeptris_validate_options, registry) +
                                                sizeof(opts->registry))) {
        return NULL;
    }
    return opts->registry;
}

YEPTRIS_API YeptrisStatus yeptris_validate_document_ex(YeptrisDocument doc, YeptrisSchema schema,
                                                       const yeptris_validate_options* opts,
                                                       yeptris_validate_report** out) {
    if (out == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    const yep_resolver* fam;
    int json_family;
    if (!yep_validate_schema_family(schema, &fam, &json_family)) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "schema %d is not a validation family", (int)schema);
        return YEPTRIS_ERROR_ARG;
    }
    yep_dom* dom = doc ? yep_doc_dom((yeptris_document*)doc) : NULL;
    if (doc != NULL && dom == NULL) {
        return YEPTRIS_ERROR_MEMORY; /* a lazy tree that failed to build */
    }
    if (dom == NULL) {
        /* an empty stream (or NULL doc): valid, empty report */
        yeptris_validate_report* r = yep_validate_report_new();
        if (r == NULL) {
            return YEPTRIS_ERROR_MEMORY;
        }
        *out = r;
        return YEPTRIS_OK;
    }
    return validate_dom(dom, fam, json_family, validate_registry(opts), out);
}

YEPTRIS_API YeptrisStatus yeptris_validate_document(YeptrisDocument doc, YeptrisSchema schema,
                                                    yeptris_validate_report** out) {
    return yeptris_validate_document_ex(doc, schema, NULL, out);
}

YEPTRIS_API YeptrisStatus yeptris_validate_ex(const char* buf, size_t len, YeptrisSchema schema,
                                              const yeptris_validate_options* opts,
                                              yeptris_validate_report** out) {
    if (out == NULL || (buf == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    const yep_resolver* fam;
    int json_family;
    if (!yep_validate_schema_family(schema, &fam, &json_family)) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "schema %d is not a validation family", (int)schema);
        return YEPTRIS_ERROR_ARG;
    }
    YeptrisParseOptions popts;
    memset(&popts, 0, sizeof(popts));
    popts.schema = schema; /* the parse resolves implicit types per family */
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse_eager_ex(buf, len, &popts, &st);
    if (doc == NULL) {
        if (st == YEPTRIS_OK) {
            /* an empty stream: valid, empty report */
            return yeptris_validate_document_ex(NULL, schema, opts, out);
        }
        return st; /* the parse's own error */
    }
    st = yeptris_validate_document_ex(doc, schema, opts, out);
    yeptris_document_free(doc);
    return st;
}

YEPTRIS_API YeptrisStatus yeptris_validate(const char* buf, size_t len, YeptrisSchema schema,
                                           yeptris_validate_report** out) {
    return yeptris_validate_ex(buf, len, schema, NULL, out);
}

YEPTRIS_API YeptrisStatus yeptris_json_schema_validate(const char* schema, size_t schema_len,
                                                       const char* buf, size_t len,
                                                       yeptris_validate_report** out) {
    if (out == NULL || (buf == NULL && len != 0) || (schema == NULL && schema_len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    /* the compiled-schema economy, one call deep (#885 item 3): the
     * schema parses ONCE here exactly as it always did, rides the SAME
     * walk and report paths as the retained handle, and dies at return
     * — the observable statuses and byte-for-byte verdicts are the
     * suite port's 1298 cases, unchanged */
    YeptrisStatus st = YEPTRIS_OK;
    yeptris_schema* s = yep_schema_compile_st(schema, schema_len, &st);
    if (s == NULL) {
        return st; /* the schema's parse/arg failure, verbatim */
    }
    st = yeptris_schema_validate(s, buf, len, out);
    yeptris_schema_free(s);
    return st;
}
