/* jsonschema.c — the JSON Schema draft 2020-12 keyword subset
 * (#882 slice 2; #885 item 2 sweep).
 *
 * The schema and the target are both parsed with the same engine; the
 * walk applies the schema tree to the instance tree and emits
 * diagnostics at instance JSON-pointer paths. Subset: type, enum,
 * const, pattern, minimum/maximum/exclusive*, multipleOf (libm-free
 * quotient integrality), minLength/maxLength (code points),
 * minItems/maxItems, uniqueItems, contains (+ minContains/maxContains
 * via suppressed probes), properties, required, patternProperties
 * (the pattern kernel over KEY strings), propertyNames (the schema
 * applies to the key string itself), minProperties/maxProperties,
 * additionalProperties (bool or schema; excludes properties AND
 * patternProperties matches), dependentRequired, dependentSchemas,
 * if/then/else (the if-schema rides a suppressed probe), items
 * (+ prefixItems; the draft-07 array form rides as prefixItems),
 * allOf/anyOf/oneOf/not, $ref (LOCAL "#/..." pointers — $defs and
 * definitions both resolve), boolean schemas. Unknown keywords, and
 * known keywords whose values cannot be read, are IGNORED (2020-12's
 * posture) — an unreadable constraint never guesses. Carried out of
 * the subset (documented in test/conformance/jsonschema/MATRIX.md):
 * unevaluatedItems/unevaluatedProperties (annotation plumbing),
 * $anchor/$id/$dynamicRef (pointer/base-URI machinery), remote $ref,
 * contentMediaType/contentSchema (annotation-only in 2020-12).
 *
 * Instance types ride the core-schema resolution (the YAML->JSON data
 * model): "y" is boolean, 1.0 is integer (2020-12: zero fractional
 * part), floats and ints are both numbers.
 *
 * Guards: the walk depth is the DOM's own parse-bounded depth; a
 * consecutive-$ref chain (schema-only cycles that consume no instance
 * structure) caps at 64 expansions and reports DIAG_REF. Every array
 * is bounded; allocation failure flags the context and unwinds.
 */

#include <stdio.h>
#include <string.h>

#include <yeptris/validate.h>

#include "../memory/allocator.h"
#include "../parse/numbers.h"
#include "../resolve/resolver.h"
#include "internal.h"

#define JS_MAX_REF_CHAIN 64
#define JS_MAX_TOKENS 256
#define JS_MAX_UNIQUE 4096 /* uniqueItems' hash table cap; over: O(n^2) probe */

typedef struct {
    yep_vctx v;        /* instance-side context (path, report, guards) */
    const yep_dom* sd; /* the schema document's tree */
    uint32_t sroot;    /* the schema's document root */
    /* the compiled schema's retained pattern programs (NULL: the
     * one-shot face — every search compiles fresh, identical verdicts) */
    const yep_re_cache* cache;
} js_ctx;

/* ---- schema-side facts -------------------------------------------------- */

static const yep_dnode* snode(const js_ctx* j, uint32_t id) {
    return yep_dom_node(j->sd, id);
}

/* First pair of the mapping whose key bytes equal `name`; 1 + *val_out,
 * 0 absent. Only scalar keys match (collection keys never will). */
static int kw_get(const js_ctx* j, uint32_t map_id, const char* name, uint32_t* val_out) {
    const yep_dnode* m = snode(j, map_id);
    if (m == NULL || m->kind != YEP_DOM_MAPPING) {
        return 0;
    }
    size_t nl = strlen(name);
    uint32_t k = m->first_child;
    while (k != UINT32_MAX) {
        uint32_t v = snode(j, k) ? snode(j, k)->next_sibling : UINT32_MAX;
        const yep_dnode* kn = snode(j, k);
        if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
            yep_view kv = yep_dom_view(j->sd, kn->value);
            if (kv.len == nl && (nl == 0 || memcmp(kv.p, name, nl) == 0)) {
                if (val_out != NULL) {
                    *val_out = v;
                }
                return 1;
            }
        }
        k = v != UINT32_MAX && snode(j, v) ? snode(j, v)->next_sibling : UINT32_MAX;
    }
    return 0;
}

/* A schema-valued keyword: a mapping, or the boolean schemas. */
static int schema_truth(const js_ctx* j, uint32_t id, int* is_false) {
    const yep_dnode* n = snode(j, id);
    if (n == NULL) {
        *is_false = 0;
        return 0;
    }
    if (n->kind == YEP_DOM_MAPPING) {
        *is_false = 0;
        return 1;
    }
    if (n->kind == YEP_DOM_SCALAR && yep_validate_scalar_tag(n) == YEPTRIS_TAG_BOOL) {
        yep_view v = yep_dom_view(j->sd, n->value);
        *is_false = (v.len == 5 && memcmp(v.p, "false", 5) == 0);
        return 1; /* true/false both carry meaning */
    }
    *is_false = 0;
    return 0; /* an unreadable schema: ignored */
}

static int kw_schema(const js_ctx* j, uint32_t map_id, const char* name, uint32_t* schema_out,
                     int* is_false_out) {
    uint32_t val;
    if (!kw_get(j, map_id, name, &val) || val == UINT32_MAX) {
        return 0;
    }
    int is_false = 0;
    if (!schema_truth(j, val, &is_false)) {
        return 0;
    }
    *schema_out = val;
    *is_false_out = is_false;
    return 1;
}

/* ---- instance-side facts ------------------------------------------------ */

/* Resolves an alias to the real node id (one hop; the DOM's targets
 * are real nodes). */
static uint32_t inst_real(const yep_dom* d, uint32_t id) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n != NULL && n->kind == YEP_DOM_ALIAS && n->target != UINT32_MAX) {
        return n->target;
    }
    return id;
}

/* The instance's JSON type word (7 bytes max); *integer receives 1 when
 * a float also satisfies 2020-12's zero-fraction rule. */
static void inst_type(const yep_dom* d, uint32_t id, const char** word, int* integer) {
    *integer = 0;
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        *word = "string";
        return;
    }
    if (n->kind == YEP_DOM_ALIAS) {
        if (n->target != UINT32_MAX) {
            inst_type(d, n->target, word, integer);
        } else {
            *word = "string";
        }
        return;
    }
    if (n->kind == YEP_DOM_SEQUENCE) {
        *word = "array";
        return;
    }
    if (n->kind == YEP_DOM_MAPPING) {
        *word = "object";
        return;
    }
    switch (yep_validate_scalar_tag(n)) {
    case YEPTRIS_TAG_NULL:
        *word = "null";
        break;
    case YEPTRIS_TAG_BOOL:
        *word = "boolean";
        break;
    case YEPTRIS_TAG_INT:
        *word = "integer";
        *integer = 1;
        break;
    case YEPTRIS_TAG_FLOAT: {
        yep_view v = yep_dom_view(d, n->value);
        double dv;
        /* 2020-12: "integer" means a zero fractional part. The range
         * guard must read the PARSED value (it once sat before the
         * parse, saw dv==0, folded constant-true, and left the cast
         * UB on out-of-range/NaN doubles). No libm: in int64 range a
         * cast round-trips iff the fractional part is zero; outside
         * it every finite double is already integral; NaN is not. */
        if (yep_num_f64(v.p, v.len, &dv) == 0) {
            const int in_i64 = dv >= -9223372036854775808.0 && dv < 9223372036854775808.0;
            if (in_i64 ? dv == (double)(int64_t)dv : dv == dv) {
                *word = "integer";
                *integer = 1;
            } else {
                *word = "number";
            }
        } else {
            *word = "number";
        }
        break;
    }
    default:
        *word = "string";
        break;
    }
}

/* The instance's numeric value (int or float); 0 when not numeric. */
static int inst_number(const yep_dom* d, uint32_t id, double* out) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == YEP_DOM_ALIAS) {
        return n->target != UINT32_MAX && inst_number(d, n->target, out);
    }
    if (n->kind != YEP_DOM_SCALAR) {
        return 0;
    }
    uint8_t tag = yep_validate_scalar_tag(n);
    if (tag != YEPTRIS_TAG_INT && tag != YEPTRIS_TAG_FLOAT) {
        return 0;
    }
    yep_view v = yep_dom_view(d, n->value);
    int64_t iv = 0;
    if (yep_num_i64(v.p, v.len, &iv) == 0) {
        *out = (double)iv;
        return 1;
    }
    return yep_num_f64(v.p, v.len, out) == 0;
}

static int inst_seq_count(const yep_dom* d, uint32_t id, uint32_t* count) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL || n->kind != YEP_DOM_SEQUENCE) {
        return 0;
    }
    *count = n->count;
    return 1;
}

/* Finds an instance pair by key bytes (scalar keys only): the VALUE
 * node id, UINT32_MAX absent. *key_out receives the KEY node id when
 * non-NULL (patternProperties/propertyNames walk the key itself). */
static uint32_t inst_find_key(const yep_dom* d, uint32_t map_id, const char* name, size_t nl,
                              uint32_t* key_out) {
    const yep_dnode* n = yep_dom_node(d, map_id);
    if (n == NULL || n->kind != YEP_DOM_MAPPING) {
        return UINT32_MAX;
    }
    uint32_t k = n->first_child;
    while (k != UINT32_MAX) {
        uint32_t v = yep_dom_node(d, k) ? yep_dom_node(d, k)->next_sibling : UINT32_MAX;
        const yep_dnode* kn = yep_dom_node(d, k);
        if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
            yep_view kv = yep_dom_view(d, kn->value);
            if (kv.len == nl && (nl == 0 || memcmp(kv.p, name, nl) == 0)) {
                if (key_out != NULL) {
                    *key_out = k;
                }
                return v;
            }
        }
        k = v != UINT32_MAX && yep_dom_node(d, v) ? yep_dom_node(d, v)->next_sibling : UINT32_MAX;
    }
    return UINT32_MAX;
}

/* ---- the walk ----------------------------------------------------------- */

static void js_apply(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth);

/* the object-keyword walkers (defined after js_apply) */
static void check_pattern_properties(js_ctx* j, uint32_t pp_id, uint32_t inst_id, int depth);
static void check_property_names(js_ctx* j, uint32_t pn_id, uint32_t inst_id, int depth);
static void check_dependent_required(js_ctx* j, uint32_t dr_id, uint32_t inst_id, int depth);
static void check_dependent_schemas(js_ctx* j, uint32_t ds_id, uint32_t inst_id, int depth);

/* A suppressed probe: does `schema_id` hold at `inst_id`? Diagnostics
 * during the trial are swallowed. */
static int js_holds(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth) {
    int save = j->v.trial_hit;
    j->v.trial_hit = 0;
    j->v.suppress++;
    js_apply(j, schema_id, inst_id, depth);
    j->v.suppress--;
    int hit = j->v.trial_hit;
    j->v.trial_hit = save;
    return !hit;
}

/* Local "#/..." pointer resolution against the schema document. */
static int resolve_ref(js_ctx* j, const char* p, uint32_t plen, uint32_t* out) {
    if (plen == 0 || p[0] != '#') {
        return 0; /* remote/anchor forms are out of the subset */
    }
    uint32_t cur = j->sroot;
    size_t i = 1;
    if (i >= plen || p[i] != '/') {
        *out = cur; /* "#" alone is the root */
        return i == plen;
    }
    int tokens = 0;
    while (i < plen) {
        if (p[i] != '/') {
            return 0;
        }
        i++;
        char token[128];
        size_t tl = 0;
        while (i < plen && p[i] != '/') {
            char b = p[i];
            if (b == '~' && i + 1 < plen && (p[i + 1] == '0' || p[i + 1] == '1')) {
                b = p[i + 1] == '0' ? '~' : '/';
                i++;
            } else if (b == '~') {
                return 0; /* a dangling ~ escape */
            }
            if (tl + 1 >= sizeof(token)) {
                return 0;
            }
            token[tl++] = b;
            i++;
        }
        if (++tokens > JS_MAX_TOKENS) {
            return 0;
        }
        const yep_dnode* n = snode(j, cur);
        if (n == NULL) {
            return 0;
        }
        if (n->kind == YEP_DOM_SEQUENCE) {
            /* a decimal array index (JSON pointer: no leading zeros,
             * but be lenient — the pointer either resolves or not) */
            uint32_t idx = 0;
            if (tl == 0) {
                return 0;
            }
            for (size_t t = 0; t < tl; t++) {
                if (token[t] < '0' || token[t] > '9' || idx > 999999) {
                    return 0;
                }
                idx = idx * 10 + (uint32_t)(token[t] - '0');
            }
            uint32_t count = 0;
            if (!inst_seq_count(j->sd, cur, &count) || idx >= count) {
                return 0;
            }
            uint32_t ch = n->first_child;
            while (ch != UINT32_MAX && idx > 0) {
                ch = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                idx--;
            }
            cur = ch;
        } else if (n->kind == YEP_DOM_MAPPING) {
            uint32_t k = n->first_child;
            int found = 0;
            while (k != UINT32_MAX && !found) {
                uint32_t v = snode(j, k) ? snode(j, k)->next_sibling : UINT32_MAX;
                const yep_dnode* kn = snode(j, k);
                if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
                    yep_view kv = yep_dom_view(j->sd, kn->value);
                    if (kv.len == tl && (tl == 0 || memcmp(kv.p, token, tl) == 0)) {
                        cur = v;
                        found = 1;
                    }
                }
                k = v != UINT32_MAX && snode(j, v) ? snode(j, v)->next_sibling : UINT32_MAX;
            }
            if (!found) {
                return 0;
            }
        } else {
            return 0; /* a pointer into a scalar */
        }
    }
    *out = cur;
    return cur != UINT32_MAX;
}

/* uniqueItems: canonical hash buckets, canonical equality inside a
 * bucket; allocation failure skips the check (oom flags the walk). */
static void check_unique(js_ctx* j, uint32_t seq_id, uint32_t inst_off) {
    const yep_dom* d = j->v.dom;
    const yep_dnode* s = yep_dom_node(d, seq_id);
    uint32_t n = s->count;
    if (n < 2) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    uint32_t cap = 8;
    while (cap < n * 2 && cap < JS_MAX_UNIQUE) {
        cap *= 2;
    }
    /* cap never reaches n when n exceeds JS_MAX_UNIQUE: elements beyond
     * a FULL table are unchecked below (a 100%%-full table cannot probe
     * — the linear scan would spin forever). Bounded memory, bounded
     * time; the check degrades to the first `cap` distinct entries. */
    struct {
        uint32_t hash;
        uint32_t node; /* +1 encoded */
    }* tbl = yep_alloc(sys, (size_t)cap * sizeof(*tbl));
    if (tbl == NULL) {
        j->v.oom = 1;
        return;
    }
    memset(tbl, 0, (size_t)cap * sizeof(*tbl));
    uint32_t ch = s->first_child;
    int idx = 0;
    int reported = 0;
    uint32_t inserted = 0;
    while (ch != UINT32_MAX && !reported) {
        if (inserted == cap) {
            break; /* the table is full: stop checking (see the cap note) */
        }
        uint64_t h64 = yep_validate_node_hash(d, ch, 0);
        uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
        uint32_t i = h & (cap - 1);
        int dup = 0;
        while (tbl[i].node != 0) {
            if (tbl[i].hash == h && yep_validate_node_equal(d, tbl[i].node - 1, d, ch, 0)) {
                dup = 1;
                break;
            }
            i = (i + 1) & (cap - 1);
        }
        if (dup) {
            reported = 1;
            yep_validate_emit(&j->v, YEPTRIS_DIAG_UNIQUE_ITEMS, inst_off,
                              "elements %u and %u are equal", (uint32_t)idx, (uint32_t)(idx - 1));
        } else {
            tbl[i].hash = h;
            tbl[i].node = ch + 1;
            inserted++;
        }
        ch = yep_dom_node(d, ch) ? yep_dom_node(d, ch)->next_sibling : UINT32_MAX;
        idx++;
    }
    yep_free(sys, tbl);
}

/* The instance's string length in code points (2020-12 minLength/
 * maxLength). 0 when the instance is not a string. */
static int inst_str_len(const yep_dom* d, uint32_t id, uint32_t* out) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == YEP_DOM_ALIAS) {
        return n->target != UINT32_MAX && inst_str_len(d, n->target, out);
    }
    if (n->kind != YEP_DOM_SCALAR || yep_validate_scalar_tag(n) != YEPTRIS_TAG_STR) {
        return 0;
    }
    yep_view v = yep_dom_view(d, n->value);
    uint32_t chars = 0;
    for (uint32_t i = 0; i < v.len; i++) {
        chars += ((((unsigned char)v.p[i]) & 0xC0) != 0x80);
    }
    *out = chars;
    return 1;
}

/* Reads a keyword's numeric value (int or float scalar); 0 unreadable. */
static int schema_number(const js_ctx* j, uint32_t id, double* out) {
    const yep_dnode* n = snode(j, id);
    if (n == NULL || n->kind != YEP_DOM_SCALAR) {
        return 0;
    }
    uint8_t tag = yep_validate_scalar_tag(n);
    if (tag != YEPTRIS_TAG_INT && tag != YEPTRIS_TAG_FLOAT) {
        return 0;
    }
    yep_view v = yep_dom_view(j->sd, n->value);
    int64_t iv = 0;
    if (yep_num_i64(v.p, v.len, &iv) == 0) {
        *out = (double)iv;
        return 1;
    }
    return yep_num_f64(v.p, v.len, out) == 0;
}

/* Applies one keyword value that must be a string (the type names,
 * pattern sources); 0 when unreadable. */
static int schema_word(const js_ctx* j, uint32_t id, const char** p, uint32_t* len) {
    const yep_dnode* n = snode(j, id);
    if (n == NULL || n->kind != YEP_DOM_SCALAR || yep_validate_scalar_tag(n) != YEPTRIS_TAG_STR) {
        return 0;
    }
    yep_view v = yep_dom_view(j->sd, n->value);
    if (v.p == NULL) {
        return 0;
    }
    *p = v.p;
    *len = v.len;
    return 1;
}

static void js_apply(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth) {
    if (j->v.oom) {
        return;
    }
    if (++depth > YEP_DOM_MAX_DEPTH) {
        if (depth == YEP_DOM_MAX_DEPTH + 1) {
            yep_validate_emit(&j->v, YEPTRIS_DIAG_DEPTH, UINT32_MAX,
                              "schema application exceeded the nesting limit");
        }
        return;
    }
    inst_id = inst_real(j->v.dom, inst_id);
    const yep_dnode* s = snode(j, schema_id);
    if (s == NULL) {
        return;
    }

    /* the boolean schemas */
    if (s->kind != YEP_DOM_MAPPING) {
        if (s->kind == YEP_DOM_SCALAR && yep_validate_scalar_tag(s) == YEPTRIS_TAG_BOOL) {
            yep_view v = yep_dom_view(j->sd, s->value);
            if (v.len == 5 && memcmp(v.p, "false", 5) == 0) {
                yep_validate_emit(&j->v, YEPTRIS_DIAG_FALSE_SCHEMA,
                                  yep_validate_node_off(j->v.dom, inst_id),
                                  "the schema is false here");
            }
        }
        return; /* true (or anything unreadable) constrains nothing */
    }

    /* $ref composes with its siblings (2020-12); the chain guard kills
     * schema-only cycles that consume no instance structure */
    uint32_t ref_val;
    if (kw_get(j, schema_id, "$ref", &ref_val) && ref_val != UINT32_MAX) {
        const char* rp = NULL;
        uint32_t rl = 0;
        if (schema_word(j, ref_val, &rp, &rl)) {
            if (++j->v.ref_depth > JS_MAX_REF_CHAIN) {
                yep_validate_emit(&j->v, YEPTRIS_DIAG_REF, yep_validate_node_off(j->v.dom, inst_id),
                                  "$ref chain exceeds %d (a cycle?)", JS_MAX_REF_CHAIN);
                j->v.ref_depth = 0;
                return;
            }
            uint32_t target = UINT32_MAX;
            if (!resolve_ref(j, rp, rl, &target)) {
                yep_validate_emit(&j->v, YEPTRIS_DIAG_REF, yep_validate_node_off(j->v.dom, inst_id),
                                  "$ref \"%.*s\" does not resolve", (int)(rl > 64 ? 64 : rl), rp);
                j->v.ref_depth = 0;
                return;
            }
            js_apply(j, target, inst_id, depth);
            j->v.ref_depth = 0;
        }
        /* an unreadable $ref value: ignored like any unknown keyword */
    } else {
        j->v.ref_depth = 0;
    }

    /* type: a single word or a list of words */
    {
        uint32_t tv;
        if (kw_get(j, schema_id, "type", &tv) && tv != UINT32_MAX) {
            const yep_dnode* tn = snode(j, tv);
            const char* word = NULL;
            uint32_t wl = 0;
            const char* it = NULL;
            int iis_int = 0;
            inst_type(j->v.dom, inst_id, &it, &iis_int);
            int matched = 0;
            int have = 0;
            if (tn != NULL && tn->kind == YEP_DOM_SCALAR && schema_word(j, tv, &word, &wl)) {
                have = 1;
                matched = (strlen(it) == wl && memcmp(it, word, wl) == 0);
                /* "number" also accepts integers */
                if (!matched && wl == 6 && memcmp(word, "number", 6) == 0 && iis_int) {
                    matched = 1;
                }
            } else if (tn != NULL && tn->kind == YEP_DOM_SEQUENCE) {
                have = 1;
                uint32_t ch = tn->first_child;
                while (ch != UINT32_MAX && !matched) {
                    const char* w2 = NULL;
                    uint32_t l2 = 0;
                    if (schema_word(j, ch, &w2, &l2)) {
                        matched = (strlen(it) == l2 && memcmp(it, w2, l2) == 0);
                        if (!matched && l2 == 6 && memcmp(w2, "number", 6) == 0 && iis_int) {
                            matched = 1;
                        }
                    }
                    ch = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                }
            }
            if (have && !matched) {
                char want[24];
                size_t cl = wl < sizeof(want) - 1 ? wl : sizeof(want) - 1;
                if (cl > 0 && word != NULL) {
                    memcpy(want, word, cl);
                }
                want[cl] = '\0';
                yep_validate_emit(&j->v, YEPTRIS_DIAG_TYPE,
                                  yep_validate_node_off(j->v.dom, inst_id), "expected %s, got %s",
                                  want, it);
            }
        }
    }

    /* const / enum */
    {
        uint32_t cv;
        if (kw_get(j, schema_id, "const", &cv) && cv != UINT32_MAX && snode(j, cv) != NULL) {
            if (!yep_validate_node_equal(j->sd, cv, j->v.dom, inst_id, 0)) {
                yep_validate_emit(&j->v, YEPTRIS_DIAG_CONST,
                                  yep_validate_node_off(j->v.dom, inst_id),
                                  "value is not the constant");
            }
        }
        uint32_t ev;
        if (kw_get(j, schema_id, "enum", &ev) && ev != UINT32_MAX) {
            const yep_dnode* en = snode(j, ev);
            if (en != NULL && en->kind == YEP_DOM_SEQUENCE) {
                int found = 0;
                uint32_t members = 0;
                uint32_t ch = en->first_child;
                while (ch != UINT32_MAX) {
                    members++;
                    if (!found && yep_validate_node_equal(j->sd, ch, j->v.dom, inst_id, 0)) {
                        found = 1;
                    }
                    ch = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                }
                if (!found) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_ENUM,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "value is not one of %u enum members", members);
                }
            }
        }
    }

    /* the numeric bounds (ignored on non-numeric instances) */
    {
        double iv = 0;
        int is_num = inst_number(j->v.dom, inst_id, &iv);
        if (is_num) {
            uint32_t mv;
            if (kw_get(j, schema_id, "minimum", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && iv < b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_MINIMUM,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%g is less than the minimum %g", iv, b);
                }
            }
            if (kw_get(j, schema_id, "maximum", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && iv > b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_MAXIMUM,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%g is greater than the maximum %g", iv, b);
                }
            }
            if (kw_get(j, schema_id, "exclusiveMinimum", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && iv <= b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_EXCLUSIVE_MINIMUM,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%g is not greater than %g", iv, b);
                }
            }
            if (kw_get(j, schema_id, "exclusiveMaximum", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && iv >= b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_EXCLUSIVE_MAXIMUM,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%g is not less than %g", iv, b);
                }
            }
            if (kw_get(j, schema_id, "multipleOf", &mv) && mv != UINT32_MAX) {
                double m;
                if (schema_number(j, mv, &m)) {
                    /* integrality of iv/m, libm-free: NaN fails (q==q),
                     * +-inf fails (q-q is NaN); every finite |q| >= 2^53
                     * is already an integer at double precision, and
                     * below that the int64 cast round-trips iff q is
                     * integral. A 0 divisor (never a legal schema) lands
                     * in inf/NaN and fails every instance. */
                    double q = iv / m;
                    int ok = (q == q) && (q - q == 0);
                    if (ok) {
                        double aq = q < 0 ? -q : q;
                        ok = aq >= 9007199254740992.0 /* 2^53 */ || q == (double)(int64_t)q;
                    }
                    if (!ok) {
                        yep_validate_emit(&j->v, YEPTRIS_DIAG_MULTIPLE_OF,
                                          yep_validate_node_off(j->v.dom, inst_id),
                                          "%g is not a multiple of %g", iv, m);
                    }
                }
            }
        }
    }

    /* string length + pattern (ignored on non-strings) */
    {
        uint32_t sl = 0;
        int is_str = inst_str_len(j->v.dom, inst_id, &sl);
        if (is_str) {
            uint32_t mv;
            if (kw_get(j, schema_id, "minLength", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && (double)sl < b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_MIN_LENGTH,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "length %u is shorter than %g", sl, b);
                }
            }
            if (kw_get(j, schema_id, "maxLength", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && (double)sl > b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_MAX_LENGTH,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "length %u is longer than %g", sl, b);
                }
            }
            uint32_t pv;
            if (kw_get(j, schema_id, "pattern", &pv) && pv != UINT32_MAX) {
                const char* pat = NULL;
                uint32_t pl = 0;
                if (schema_word(j, pv, &pat, &pl)) {
                    const yep_dnode* n = yep_dom_node(j->v.dom, inst_id);
                    yep_view v = yep_dom_view(j->v.dom, n->value);
                    int rc = yep_re_cache_search(j->cache, pat, pl, v.p != NULL ? v.p : "", v.len);
                    if (rc == 0) {
                        yep_validate_emit(&j->v, YEPTRIS_DIAG_PATTERN,
                                          yep_validate_node_off(j->v.dom, inst_id),
                                          "does not match the pattern");
                    }
                    /* rc < 0: the pattern is outside the subset — ignored */
                }
            }
        }
    }

    /* array bounds + uniqueness + items (ignored on non-arrays) */
    {
        uint32_t cnt = 0;
        if (inst_seq_count(j->v.dom, inst_id, &cnt)) {
            uint32_t mv;
            if (kw_get(j, schema_id, "minItems", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && (double)cnt < b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_MIN_ITEMS,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%u items is fewer than %g", cnt, b);
                }
            }
            if (kw_get(j, schema_id, "maxItems", &mv) && mv != UINT32_MAX) {
                double b;
                if (schema_number(j, mv, &b) && (double)cnt > b) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_MAX_ITEMS,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%u items is more than %g", cnt, b);
                }
            }
            uint32_t uv;
            int uf = 0;
            if (kw_schema(j, schema_id, "uniqueItems", &uv, &uf) && uf == 0) {
                check_unique(j, inst_id, yep_validate_node_off(j->v.dom, inst_id));
            }
            /* contains: count the items the schema holds on (suppressed
             * probes), then the minContains/maxContains qualifiers
             * (defaults: at least one; no maximum). min/maxContains
             * without contains are annotations — ignored. */
            uint32_t ctv;
            if (kw_get(j, schema_id, "contains", &ctv) && ctv != UINT32_MAX) {
                int ctf = 0;
                if (snode(j, ctv) != NULL && schema_truth(j, ctv, &ctf)) {
                    uint32_t matches = 0;
                    uint32_t ich = yep_dom_node(j->v.dom, inst_id)
                                       ? yep_dom_node(j->v.dom, inst_id)->first_child
                                       : UINT32_MAX;
                    for (uint32_t i = 0; i < cnt && ich != UINT32_MAX; i++) {
                        uint32_t ni = yep_dom_node(j->v.dom, ich)
                                          ? yep_dom_node(j->v.dom, ich)->next_sibling
                                          : UINT32_MAX;
                        if (js_holds(j, ctv, ich, depth)) {
                            matches++;
                        }
                        ich = ni;
                    }
                    double minc = 1.0;
                    double maxc = -1.0; /* unset */
                    uint32_t mc;
                    int have_minc = 0;
                    if (kw_get(j, schema_id, "minContains", &mc) && mc != UINT32_MAX) {
                        if (schema_number(j, mc, &minc)) {
                            have_minc = 1;
                        } else {
                            minc = 1.0; /* an unreadable qualifier: default */
                        }
                    }
                    if (kw_get(j, schema_id, "maxContains", &mc) && mc != UINT32_MAX) {
                        if (!schema_number(j, mc, &maxc)) {
                            maxc = -1.0;
                        }
                    }
                    if ((double)matches < minc) {
                        yep_validate_emit(
                            &j->v, have_minc ? YEPTRIS_DIAG_MIN_CONTAINS : YEPTRIS_DIAG_CONTAINS,
                            yep_validate_node_off(j->v.dom, inst_id),
                            "%u of %u items match the contains schema (need %g)", matches, cnt,
                            minc);
                    } else if (maxc >= 0.0 && (double)matches > maxc) {
                        yep_validate_emit(&j->v, YEPTRIS_DIAG_MAX_CONTAINS,
                                          yep_validate_node_off(j->v.dom, inst_id),
                                          "%u of %u items match the contains schema (max %g)",
                                          matches, cnt, maxc);
                    }
                }
            }
            /* prefixItems: per-index schemas; the draft-07 array form
             * of items rides the same semantics */
            uint32_t prefix = 0;
            uint32_t pv;
            if (kw_get(j, schema_id, "prefixItems", &pv) && pv != UINT32_MAX) {
                const yep_dnode* pn = snode(j, pv);
                if (pn != NULL && pn->kind == YEP_DOM_SEQUENCE) {
                    uint32_t i = 0;
                    uint32_t ch = pn->first_child;
                    uint32_t ich = yep_dom_node(j->v.dom, inst_id)->first_child;
                    while (ch != UINT32_MAX && ich != UINT32_MAX) {
                        uint32_t ns = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                        uint32_t ni = yep_dom_node(j->v.dom, ich)
                                          ? yep_dom_node(j->v.dom, ich)->next_sibling
                                          : UINT32_MAX;
                        size_t mark = j->v.path_len;
                        if (yep_validate_path_push_index(&j->v, i) == 0) {
                            js_apply(j, ch, ich, depth);
                            yep_validate_path_pop(&j->v, mark);
                        }
                        prefix++;
                        i++;
                        ch = ns;
                        ich = ni;
                    }
                }
            }
            /* items: a single schema over the remaining elements; the
             * draft-07 array form counts as the prefix */
            if (kw_get(j, schema_id, "items", &pv) && pv != UINT32_MAX) {
                const yep_dnode* pn = snode(j, pv);
                int is_false = 0;
                if (pn != NULL && pn->kind == YEP_DOM_SEQUENCE) {
                    /* the draft-07 tuple shim: per-index, from scratch */
                    uint32_t i = 0;
                    uint32_t ch = pn->first_child;
                    uint32_t ich = yep_dom_node(j->v.dom, inst_id)->first_child;
                    while (ch != UINT32_MAX && ich != UINT32_MAX) {
                        uint32_t ns = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                        uint32_t ni = yep_dom_node(j->v.dom, ich)
                                          ? yep_dom_node(j->v.dom, ich)->next_sibling
                                          : UINT32_MAX;
                        size_t mark = j->v.path_len;
                        if (yep_validate_path_push_index(&j->v, i) == 0) {
                            js_apply(j, ch, ich, depth);
                            yep_validate_path_pop(&j->v, mark);
                        }
                        i++;
                        ch = ns;
                        ich = ni;
                    }
                } else if (schema_truth(j, pv, &is_false)) {
                    /* items applies from prefixItems' count on */
                    uint32_t ich = yep_dom_node(j->v.dom, inst_id)->first_child;
                    for (uint32_t skip = 0; skip < prefix && ich != UINT32_MAX; skip++) {
                        ich = yep_dom_node(j->v.dom, ich)
                                  ? yep_dom_node(j->v.dom, ich)->next_sibling
                                  : UINT32_MAX;
                    }
                    for (uint32_t i = prefix; i < cnt && ich != UINT32_MAX; i++) {
                        uint32_t ni = yep_dom_node(j->v.dom, ich)
                                          ? yep_dom_node(j->v.dom, ich)->next_sibling
                                          : UINT32_MAX;
                        size_t mark = j->v.path_len;
                        if (yep_validate_path_push_index(&j->v, i) == 0) {
                            js_apply(j, pv, ich, depth);
                            yep_validate_path_pop(&j->v, mark);
                        }
                        ich = ni;
                    }
                }
            }
        }
    }

    /* allOf / anyOf / oneOf / not */
    {
        uint32_t cv;
        if (kw_get(j, schema_id, "allOf", &cv) && cv != UINT32_MAX) {
            const yep_dnode* cn = snode(j, cv);
            if (cn != NULL && cn->kind == YEP_DOM_SEQUENCE) {
                uint32_t ch = cn->first_child;
                while (ch != UINT32_MAX) {
                    uint32_t nc = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                    js_apply(j, ch, inst_id, depth);
                    ch = nc;
                }
            }
        }
        if (kw_get(j, schema_id, "anyOf", &cv) && cv != UINT32_MAX) {
            const yep_dnode* cn = snode(j, cv);
            if (cn != NULL && cn->kind == YEP_DOM_SEQUENCE) {
                int any = 0;
                uint32_t branches = 0;
                uint32_t ch = cn->first_child;
                while (ch != UINT32_MAX) {
                    branches++;
                    if (!any && js_holds(j, ch, inst_id, depth)) {
                        any = 1;
                    }
                    ch = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                }
                if (!any) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_ANY_OF,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "none of the %u anyOf branches matched", branches);
                }
            }
        }
        if (kw_get(j, schema_id, "oneOf", &cv) && cv != UINT32_MAX) {
            const yep_dnode* cn = snode(j, cv);
            if (cn != NULL && cn->kind == YEP_DOM_SEQUENCE) {
                uint32_t matches = 0;
                uint32_t branches = 0;
                uint32_t ch = cn->first_child;
                while (ch != UINT32_MAX) {
                    branches++;
                    if (js_holds(j, ch, inst_id, depth)) {
                        matches++;
                    }
                    ch = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                }
                if (matches != 1) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_ONE_OF,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%u of the %u oneOf branches matched (exactly one "
                                      "required)",
                                      matches, branches);
                }
            }
        }
        if (kw_get(j, schema_id, "not", &cv) && cv != UINT32_MAX) {
            if (js_holds(j, cv, inst_id, depth)) {
                yep_validate_emit(&j->v, YEPTRIS_DIAG_NOT, yep_validate_node_off(j->v.dom, inst_id),
                                  "the not-schema matched");
            }
        }
    }

    /* if/then/else: the if-schema rides a suppressed probe; exactly one
     * of then/else applies. An unreadable if (or then/else) is ignored. */
    {
        uint32_t ifv;
        if (kw_get(j, schema_id, "if", &ifv) && ifv != UINT32_MAX) {
            int iff = 0;
            if (schema_truth(j, ifv, &iff)) {
                uint32_t tv = UINT32_MAX, ev = UINT32_MAX;
                int tf = 0, ef = 0;
                int have_t = kw_schema(j, schema_id, "then", &tv, &tf);
                int have_e = kw_schema(j, schema_id, "else", &ev, &ef);
                if (js_holds(j, ifv, inst_id, depth)) {
                    if (have_t) {
                        js_apply(j, tv, inst_id, depth);
                    }
                } else if (have_e) {
                    js_apply(j, ev, inst_id, depth);
                }
            }
        }
    }

    /* the object keywords (ignored on non-objects) */
    {
        const yep_dnode* n = yep_dom_node(j->v.dom, inst_id);
        if (n != NULL && n->kind == YEP_DOM_MAPPING) {
            const uint32_t pairs = n->count / 2;
            /* properties: known keys */
            uint32_t props = UINT32_MAX;
            int have_props = kw_get(j, schema_id, "properties", &props) && props != UINT32_MAX &&
                             snode(j, props) != NULL && snode(j, props)->kind == YEP_DOM_MAPPING;
            /* patternProperties: pattern -> schema over the KEY strings
             * (readable once; additionalProperties must exclude its
             * matches too) */
            uint32_t pp_id = UINT32_MAX;
            {
                uint32_t ppid;
                if (kw_get(j, schema_id, "patternProperties", &ppid) && ppid != UINT32_MAX) {
                    const yep_dnode* pnm = snode(j, ppid);
                    if (pnm != NULL && pnm->kind == YEP_DOM_MAPPING) {
                        pp_id = ppid;
                    }
                }
            }
            uint32_t req;
            if (kw_get(j, schema_id, "required", &req) && req != UINT32_MAX) {
                const yep_dnode* rn = snode(j, req);
                if (rn != NULL && rn->kind == YEP_DOM_SEQUENCE) {
                    uint32_t ch = rn->first_child;
                    while (ch != UINT32_MAX) {
                        const char* name = NULL;
                        uint32_t nl = 0;
                        uint32_t nc = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                        if (schema_word(j, ch, &name, &nl) &&
                            inst_find_key(j->v.dom, inst_id, name, nl, NULL) == UINT32_MAX) {
                            char namebuf[64];
                            size_t cl = nl < sizeof(namebuf) - 1 ? nl : sizeof(namebuf) - 1;
                            memcpy(namebuf, name, cl);
                            namebuf[cl] = '\0';
                            /* the diagnostic points at the missing
                             * leaf: object path + /key (the ajv
                             * instancePath convention) */
                            size_t mark = j->v.path_len;
                            int pushed = yep_validate_path_push(&j->v, namebuf, cl);
                            yep_validate_emit(&j->v, YEPTRIS_DIAG_REQUIRED,
                                              yep_validate_node_off(j->v.dom, inst_id),
                                              "missing required property \"%s\"", namebuf);
                            if (pushed == 0) {
                                yep_validate_path_pop(&j->v, mark);
                            }
                        }
                        ch = nc;
                    }
                }
            }
            /* properties walk (values through their schemas) */
            if (have_props) {
                const yep_dnode* pn = snode(j, props);
                uint32_t k = pn->first_child;
                while (k != UINT32_MAX) {
                    uint32_t v = snode(j, k) ? snode(j, k)->next_sibling : UINT32_MAX;
                    const yep_dnode* kn = snode(j, k);
                    if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
                        yep_view kv = yep_dom_view(j->sd, kn->value);
                        /* find the child pair by key bytes */
                        uint32_t ck = n->first_child;
                        while (ck != UINT32_MAX) {
                            uint32_t cv = yep_dom_node(j->v.dom, ck)
                                              ? yep_dom_node(j->v.dom, ck)->next_sibling
                                              : UINT32_MAX;
                            const yep_dnode* ckn = yep_dom_node(j->v.dom, ck);
                            if (ckn != NULL && ckn->kind == YEP_DOM_SCALAR) {
                                yep_view ckv = yep_dom_view(j->v.dom, ckn->value);
                                if (ckv.len == kv.len &&
                                    (kv.len == 0 || memcmp(ckv.p, kv.p, kv.len) == 0)) {
                                    size_t mark = j->v.path_len;
                                    if (yep_validate_path_push(&j->v, kv.p != NULL ? kv.p : "",
                                                               kv.len) == 0) {
                                        js_apply(j, v, cv, depth);
                                        yep_validate_path_pop(&j->v, mark);
                                    }
                                    break;
                                }
                            }
                            ck = cv != UINT32_MAX && yep_dom_node(j->v.dom, cv)
                                     ? yep_dom_node(j->v.dom, cv)->next_sibling
                                     : UINT32_MAX;
                        }
                    }
                    k = v != UINT32_MAX && snode(j, v) ? snode(j, v)->next_sibling : UINT32_MAX;
                }
            }
            /* patternProperties: every pattern that matches a key
             * applies its schema to that key's value */
            if (pp_id != UINT32_MAX) {
                check_pattern_properties(j, pp_id, inst_id, depth);
            }
            /* propertyNames: the schema applies to each KEY string */
            {
                uint32_t pnv;
                if (kw_get(j, schema_id, "propertyNames", &pnv) && pnv != UINT32_MAX) {
                    int pnf = 0;
                    if (snode(j, pnv) != NULL && schema_truth(j, pnv, &pnf)) {
                        check_property_names(j, pnv, inst_id, depth);
                    }
                }
            }
            /* min/maxProperties */
            {
                uint32_t mv;
                if (kw_get(j, schema_id, "minProperties", &mv) && mv != UINT32_MAX) {
                    double b;
                    if (schema_number(j, mv, &b) && (double)pairs < b) {
                        yep_validate_emit(&j->v, YEPTRIS_DIAG_MIN_PROPERTIES,
                                          yep_validate_node_off(j->v.dom, inst_id),
                                          "%u properties is fewer than %g", pairs, b);
                    }
                }
                if (kw_get(j, schema_id, "maxProperties", &mv) && mv != UINT32_MAX) {
                    double b;
                    if (schema_number(j, mv, &b) && (double)pairs > b) {
                        yep_validate_emit(&j->v, YEPTRIS_DIAG_MAX_PROPERTIES,
                                          yep_validate_node_off(j->v.dom, inst_id),
                                          "%u properties is more than %g", pairs, b);
                    }
                }
            }
            /* dependentRequired: name -> the names that must ride along */
            {
                uint32_t drv;
                if (kw_get(j, schema_id, "dependentRequired", &drv) && drv != UINT32_MAX) {
                    check_dependent_required(j, drv, inst_id, depth);
                }
            }
            /* dependentSchemas: when the named key is present, its
             * schema applies to the whole object (walked directly, like
             * allOf: the inner diagnostics carry the failure) */
            {
                uint32_t dsv;
                if (kw_get(j, schema_id, "dependentSchemas", &dsv) && dsv != UINT32_MAX) {
                    check_dependent_schemas(j, dsv, inst_id, depth);
                }
            }
            /* additionalProperties: the keys properties does not name */
            uint32_t ap;
            int af = 0;
            if (kw_schema(j, schema_id, "additionalProperties", &ap, &af)) {
                /* k steps pair-to-pair: every visited node IS a key */
                uint32_t k = n->first_child;
                while (k != UINT32_MAX) {
                    uint32_t v = yep_dom_node(j->v.dom, k) ? yep_dom_node(j->v.dom, k)->next_sibling
                                                           : UINT32_MAX;
                    const yep_dnode* kn = yep_dom_node(j->v.dom, k);
                    if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
                        yep_view kv = yep_dom_view(j->v.dom, kn->value);
                        int known = 0;
                        if (have_props) {
                            uint32_t pk = snode(j, props)->first_child;
                            while (pk != UINT32_MAX && !known) {
                                const yep_dnode* pkn = snode(j, pk);
                                uint32_t pv =
                                    snode(j, pk) ? snode(j, pk)->next_sibling : UINT32_MAX;
                                if (pkn != NULL && pkn->kind == YEP_DOM_SCALAR) {
                                    yep_view pw = yep_dom_view(j->sd, pkn->value);
                                    known = pw.len == kv.len &&
                                            (kv.len == 0 || memcmp(pw.p, kv.p, kv.len) == 0);
                                }
                                pk = pv != UINT32_MAX && snode(j, pv) ? snode(j, pv)->next_sibling
                                                                      : UINT32_MAX;
                            }
                        }
                        /* patternProperties matches take the key out of
                         * additionalProperties' scope too (2020-12) */
                        if (!known && pp_id != UINT32_MAX) {
                            const yep_dnode* pnm = snode(j, pp_id);
                            uint32_t pk = pnm->first_child;
                            while (pk != UINT32_MAX && !known) {
                                const yep_dnode* pkn = snode(j, pk);
                                uint32_t pv =
                                    snode(j, pk) ? snode(j, pk)->next_sibling : UINT32_MAX;
                                const char* pat = NULL;
                                uint32_t pl = 0;
                                if (pkn != NULL && pkn->kind == YEP_DOM_SCALAR &&
                                    schema_word(j, pk, &pat, &pl) &&
                                    yep_re_cache_search(j->cache, pat, pl, kv.p != NULL ? kv.p : "",
                                                        kv.len) == 1) {
                                    known = 1;
                                }
                                pk = pv != UINT32_MAX && snode(j, pv) ? snode(j, pv)->next_sibling
                                                                      : UINT32_MAX;
                            }
                        }
                        if (!known) {
                            char namebuf[64];
                            size_t cl = kv.len < sizeof(namebuf) - 1 ? kv.len : sizeof(namebuf) - 1;
                            if (cl > 0 && kv.p != NULL) {
                                memcpy(namebuf, kv.p, cl);
                            }
                            namebuf[cl] = '\0';
                            if (af) {
                                yep_validate_emit(&j->v, YEPTRIS_DIAG_ADDITIONAL_PROPERTY,
                                                  yep_validate_node_off(j->v.dom, k),
                                                  "additional property \"%s\"", namebuf);
                            } else {
                                size_t mark = j->v.path_len;
                                if (yep_validate_path_push(&j->v, namebuf, cl) == 0) {
                                    js_apply(j, ap, v, depth);
                                    yep_validate_path_pop(&j->v, mark);
                                }
                            }
                        }
                    }
                    k = v != UINT32_MAX && yep_dom_node(j->v.dom, v)
                            ? yep_dom_node(j->v.dom, v)->next_sibling
                            : UINT32_MAX;
                }
            }
        }
    }
}

/* ---- the object-keyword walkers ------------------------------------------ */

/* Copies a bounded name for diagnostics/detail text: the copied
 * length (the caller pushes paths with it — keys may hold NULs). */
static size_t name_copy(char* buf, size_t cap, const char* p, size_t len) {
    size_t cl = len < cap - 1 ? len : cap - 1;
    if (cl > 0 && p != NULL) {
        memcpy(buf, p, cl);
    }
    buf[cl] = '\0';
    return cl;
}

/* patternProperties: pattern -> schema over the KEY strings; every
 * matching pattern applies its schema to the key's value (a false
 * schema rides js_apply's FALSE_SCHEMA diagnostic). */
static void check_pattern_properties(js_ctx* j, uint32_t pp_id, uint32_t inst_id, int depth) {
    const yep_dom* d = j->v.dom;
    const yep_dnode* pnm = snode(j, pp_id);
    const yep_dnode* inst = yep_dom_node(d, inst_id);
    if (pnm == NULL || inst == NULL) {
        return;
    }
    uint32_t k = inst->first_child;
    while (k != UINT32_MAX && !j->v.oom) {
        uint32_t v = yep_dom_node(d, k) ? yep_dom_node(d, k)->next_sibling : UINT32_MAX;
        const yep_dnode* kn = yep_dom_node(d, k);
        uint32_t nk =
            v != UINT32_MAX && yep_dom_node(d, v) ? yep_dom_node(d, v)->next_sibling : UINT32_MAX;
        if (kn != NULL && kn->kind == YEP_DOM_SCALAR && v != UINT32_MAX) {
            yep_view kv = yep_dom_view(d, kn->value);
            uint32_t pk = pnm->first_child;
            while (pk != UINT32_MAX && !j->v.oom) {
                const yep_dnode* pkn = snode(j, pk);
                uint32_t pv = snode(j, pk) ? snode(j, pk)->next_sibling : UINT32_MAX;
                uint32_t np =
                    pv != UINT32_MAX && snode(j, pv) ? snode(j, pv)->next_sibling : UINT32_MAX;
                const char* pat = NULL;
                uint32_t pl = 0;
                if (pkn != NULL && pkn->kind == YEP_DOM_SCALAR && pv != UINT32_MAX &&
                    schema_word(j, pk, &pat, &pl) &&
                    yep_re_cache_search(j->cache, pat, pl, kv.p != NULL ? kv.p : "", kv.len) == 1) {
                    char namebuf[64];
                    size_t cl = name_copy(namebuf, sizeof(namebuf), kv.p, kv.len);
                    size_t mark = j->v.path_len;
                    if (yep_validate_path_push(&j->v, namebuf, cl) == 0) {
                        js_apply(j, pv, v, depth);
                        yep_validate_path_pop(&j->v, mark);
                    }
                }
                pk = np;
            }
        }
        k = nk;
    }
}

/* propertyNames: the schema applies to each KEY (the key string IS the
 * instance; a false propertyNames rides FALSE_SCHEMA at the key). */
static void check_property_names(js_ctx* j, uint32_t pn_id, uint32_t inst_id, int depth) {
    const yep_dom* d = j->v.dom;
    const yep_dnode* inst = yep_dom_node(d, inst_id);
    if (inst == NULL) {
        return;
    }
    uint32_t k = inst->first_child;
    while (k != UINT32_MAX && !j->v.oom) {
        uint32_t v = yep_dom_node(d, k) ? yep_dom_node(d, k)->next_sibling : UINT32_MAX;
        const yep_dnode* kn = yep_dom_node(d, k);
        uint32_t nk =
            v != UINT32_MAX && yep_dom_node(d, v) ? yep_dom_node(d, v)->next_sibling : UINT32_MAX;
        if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
            yep_view kv = yep_dom_view(d, kn->value);
            char namebuf[64];
            size_t cl = name_copy(namebuf, sizeof(namebuf), kv.p, kv.len);
            size_t mark = j->v.path_len;
            if (yep_validate_path_push(&j->v, namebuf, cl) == 0) {
                js_apply(j, pn_id, k, depth);
                yep_validate_path_pop(&j->v, mark);
            }
        }
        k = nk;
    }
}

/* dependentRequired: name -> the names required alongside it. A
 * present trigger with an absent dependent is DIAG_DEPENDENT_REQUIRED
 * at the object (the missing leaf's path, the required convention). */
static void check_dependent_required(js_ctx* j, uint32_t dr_id, uint32_t inst_id, int depth) {
    (void)depth;
    const yep_dom* d = j->v.dom;
    const yep_dnode* dm = snode(j, dr_id);
    if (dm == NULL || dm->kind != YEP_DOM_MAPPING) {
        return;
    }
    uint32_t sk = dm->first_child;
    while (sk != UINT32_MAX && !j->v.oom) {
        uint32_t sv = snode(j, sk) ? snode(j, sk)->next_sibling : UINT32_MAX;
        uint32_t ns = sv != UINT32_MAX && snode(j, sv) ? snode(j, sv)->next_sibling : UINT32_MAX;
        const yep_dnode* skn = snode(j, sk);
        if (skn != NULL && skn->kind == YEP_DOM_SCALAR && sv != UINT32_MAX) {
            yep_view trig = yep_dom_view(j->sd, skn->value);
            const yep_dnode* ln = snode(j, sv);
            if (trig.p != NULL && ln != NULL && ln->kind == YEP_DOM_SEQUENCE &&
                inst_find_key(d, inst_id, trig.p, trig.len, NULL) != UINT32_MAX) {
                char trigbuf[64];
                name_copy(trigbuf, sizeof(trigbuf), trig.p, trig.len);
                uint32_t rk = ln->first_child;
                while (rk != UINT32_MAX && !j->v.oom) {
                    uint32_t rn = snode(j, rk) ? snode(j, rk)->next_sibling : UINT32_MAX;
                    const char* name = NULL;
                    uint32_t nl = 0;
                    if (schema_word(j, rk, &name, &nl) &&
                        inst_find_key(d, inst_id, name, nl, NULL) == UINT32_MAX) {
                        /* the detail text is printf-composed: the view
                         * is NOT NUL-terminated — %s reads past the
                         * DOM allocation (ASan-caught); copy first */
                        char namebuf[64];
                        size_t cl = name_copy(namebuf, sizeof(namebuf), name, nl);
                        size_t mark = j->v.path_len;
                        int pushed = yep_validate_path_push(&j->v, namebuf, cl);
                        yep_validate_emit(&j->v, YEPTRIS_DIAG_DEPENDENT_REQUIRED,
                                          yep_validate_node_off(j->v.dom, inst_id),
                                          "\"%s\" requires \"%s\"", trigbuf, namebuf);
                        if (pushed == 0) {
                            yep_validate_path_pop(&j->v, mark);
                        }
                    }
                    rk = rn;
                }
            }
        }
        sk = ns;
    }
}

/* dependentSchemas: when the named key is present, its schema applies
 * to the whole object. */
static void check_dependent_schemas(js_ctx* j, uint32_t ds_id, uint32_t inst_id, int depth) {
    const yep_dom* d = j->v.dom;
    const yep_dnode* dm = snode(j, ds_id);
    if (dm == NULL || dm->kind != YEP_DOM_MAPPING) {
        return;
    }
    uint32_t sk = dm->first_child;
    while (sk != UINT32_MAX && !j->v.oom) {
        uint32_t sv = snode(j, sk) ? snode(j, sk)->next_sibling : UINT32_MAX;
        uint32_t ns = sv != UINT32_MAX && snode(j, sv) ? snode(j, sv)->next_sibling : UINT32_MAX;
        const yep_dnode* skn = snode(j, sk);
        if (skn != NULL && skn->kind == YEP_DOM_SCALAR && sv != UINT32_MAX) {
            yep_view trig = yep_dom_view(j->sd, skn->value);
            if (trig.p != NULL && inst_find_key(d, inst_id, trig.p, trig.len, NULL) != UINT32_MAX) {
                js_apply(j, sv, inst_id, depth);
            }
        }
        sk = ns;
    }
}

/* ---- the entry ----------------------------------------------------------- */

/* Validates one instance document root against the schema root.
 * Returns 0 ok, -1 OOM. */
static int js_validate_doc(js_ctx* j, uint32_t inst_root) {
    js_apply(j, j->sroot, inst_root, 0);
    return j->v.oom ? -1 : 0;
}

/* The public entries live in validate.c/compile.c (yeptris_json_schema_
 * validate compiles+validates+frees through the compiled handle; the
 * compiled face drives this seam per instance stream): they parse the
 * sides, build the report and hand over the schema DOM + the retained
 * pattern cache (the declarations are internal.h's — the shared SSOT). */
int yep_jsonschema_run(const yep_dom* schema_dom, uint32_t schema_root, const yep_dom* instance_dom,
                       const uint32_t* doc_roots, uint32_t doc_count, yeptris_validate_report* rep,
                       const yep_re_cache* cache) {
    js_ctx j;
    memset(&j, 0, sizeof(j));
    j.sd = schema_dom;
    j.sroot = schema_root;
    j.cache = cache;
    j.v.dom = instance_dom;
    j.v.rep = rep;
    j.v.input = instance_dom->input_base;
    j.v.input_len = instance_dom->input_len;
    for (uint32_t i = 0; i < doc_count; i++) {
        j.v.path_len = 0;
        js_validate_doc(&j, doc_roots[i]);
        if (j.v.oom) {
            yep_free(yep_system_allocator(), j.v.line_off);
            yep_free(yep_system_allocator(), j.v.path);
            return -1;
        }
    }
    /* the walk's scratch (the lazy line index, the path buffer) dies
     * here: the report carries only finished diagnostics */
    yep_free(yep_system_allocator(), j.v.line_off);
    yep_free(yep_system_allocator(), j.v.path);
    return 0;
}
