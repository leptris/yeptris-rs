/* jsonschema.c — the JSON Schema draft 2020-12 keyword subset
 * (#882 slice 2; #885 item 2 sweep; the 2020-12 sota sweep).
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
 * allOf/anyOf/oneOf/not, unevaluatedProperties/unevaluatedItems (the
 * annotation model, below), $ref, $dynamicRef (below), boolean
 * schemas. Unknown keywords, and known keywords whose values cannot
 * be read, are IGNORED (2020-12's posture) — an unreadable constraint
 * never guesses.
 *
 * THE ANNOTATION MODEL (2020-12's unevaluated*): every application of
 * a schema object to an instance location returns (a) its validity —
 * one fail_mark comparison around the application, unsuppressed emits
 * increment the mark — and (b) the properties/items it EVALUATED.
 * Annotations collect upward only: a schema object's set is its own
 * adjacent keywords' appends (properties, patternProperties,
 * additionalProperties, contains, items, prefixItems,
 * unevaluatedProperties/unevaluatedItems themselves) plus whatever its
 * in-place applicators brought back (allOf branches always;
 * anyOf/oneOf SUCCESSFUL branches only — a failed branch's annotations
 * are released; the if-probe when it holds, plus the branch taken;
 * $ref/$dynamicRef targets; dependentSchemas when triggered). `not`
 * and the probes never annotate outward. Applications to CHILD
 * locations (properties' values, items, propertyNames' keys, the
 * unevaluated* subschemas) release their sets — an annotation never
 * crosses an instance-location boundary. unevaluatedProperties /
 * unevaluatedItems run LAST in their schema object, whatever the
 * document order, and flag the properties/items absent from the
 * collected set.
 *
 * REFERENCES (the sota sweep): resolution rides the compiled schema's
 * identifier map (resources.c): $id base-URI composition (RFC 3986
 * path merge, fragment-free canonical forms), $anchor and
 * $dynamicAnchor plain-name fragments scoped to their nearest $id
 * resource, JSON Pointer fragments with the percent-encoding layer
 * (%25 -> '%', then ~0/~1), and $ref targets anywhere in the schema's
 * own identifier space. No network: a target outside that space is
 * unresolvable (DIAG_REF), never fetched. $dynamicRef rides the
 * draft's runtime dynamic scope (core spec 8.2.3.2 + appendix C): a
 * plain-name fragment that statically resolves to a $dynamicAnchor is
 * replaced by the identically-named $dynamicAnchor of the OUTERMOST
 * schema resource in the dynamic scope — the stack of resources
 * entered during evaluation; probes (anyOf/oneOf/not/if trials) enter
 * no scope, exactly as their annotations are discarded.
 *
 * Carried out of the subset (documented in
 * test/conformance/jsonschema/MATRIX.md): remote targets (the
 * metaschema and the suite's HTTP fixtures — no network corpus is
 * vendored), contentMediaType/contentSchema (annotation-only in
 * 2020-12).
 *
 * Instance types ride the core-schema resolution (the YAML->JSON data
 * model): "y" is boolean, 1.0 is integer (2020-12: zero fractional
 * part), floats and ints are both numbers.
 *
 * Guards: the walk depth is the DOM's own parse-bounded depth; a
 * consecutive-$ref/$dynamicRef chain (schema-only cycles that consume
 * no instance structure) caps at 64 expansions and reports DIAG_REF.
 * Every array is bounded; allocation failure flags the context and
 * unwinds.
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
#define JS_MAX_EVALS 16384 /* the annotation arena's entry cap; over: OOM */

/* One application's result: validity plus the annotation window it
 * owns — [pmark, plen) of the evaluated-property key-node arena and
 * [imark, ilen) of the evaluated-item index arena. Absorbing a result
 * into the caller's set is a NO-OP (the window simply stays); dropping
 * it is two assignments (the release). Zero-copy in both directions. */
typedef struct {
    int valid;
    size_t pmark, plen;
    size_t imark, ilen;
} js_eval;

typedef struct {
    yep_vctx v;        /* instance-side context (path, report, guards) */
    const yep_dom* sd; /* the schema document's tree */
    uint32_t sroot;    /* the schema's document root */
    /* the compiled schema's retained pattern programs (NULL: the
     * one-shot face — every search compiles fresh, identical verdicts) */
    const yep_re_cache* cache;
    /* the compiled schema's identifier map (NULL: only bare "#/..."
     * pointers against the document root resolve) */
    const js_resources* res;
    /* the dynamic scope: resource indices, outermost first — pushed
     * as evaluation enters each schema object's resource (dedup'd
     * against the top), popped on exit; probes push nothing */
    uint32_t scope[YEP_JS_MAX_SCOPE];
    int scope_n;
    /* the annotation arenas: evaluated property KEY node ids and item
     * indices, appended during the walk, released per application */
    uint32_t* ep;
    size_t ep_len, ep_cap;
    uint32_t* ei;
    size_t ei_len, ei_cap;
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

/* ---- the annotation arenas ---------------------------------------------- */

/* Appends an evaluated property key node; -1 over cap (the walk's OOM
 * flag fires — bounded memory is the law, never a silent guess). */
static int ep_append(js_ctx* j, uint32_t key_node) {
    if (j->ep_len == j->ep_cap) {
        const yep_allocator* sys = yep_system_allocator();
        size_t ncap = j->ep_cap ? j->ep_cap * 2 : 64;
        uint32_t* np = yep_alloc(sys, ncap * sizeof(uint32_t));
        if (np == NULL) {
            j->v.oom = 1;
            return -1;
        }
        if (j->ep != NULL) {
            memcpy(np, j->ep, j->ep_len * sizeof(uint32_t));
            yep_free(sys, j->ep);
        }
        j->ep = np;
        j->ep_cap = ncap;
    }
    if (j->ep_len >= JS_MAX_EVALS) {
        j->v.oom = 1;
        return -1;
    }
    j->ep[j->ep_len++] = key_node;
    return 0;
}

static int ei_append(js_ctx* j, uint32_t index) {
    if (j->ei_len == j->ei_cap) {
        const yep_allocator* sys = yep_system_allocator();
        size_t ncap = j->ei_cap ? j->ei_cap * 2 : 64;
        uint32_t* np = yep_alloc(sys, ncap * sizeof(uint32_t));
        if (np == NULL) {
            j->v.oom = 1;
            return -1;
        }
        if (j->ei != NULL) {
            memcpy(np, j->ei, j->ei_len * sizeof(uint32_t));
            yep_free(sys, j->ei);
        }
        j->ei = np;
        j->ei_cap = ncap;
    }
    if (j->ei_len >= JS_MAX_EVALS) {
        j->v.oom = 1;
        return -1;
    }
    j->ei[j->ei_len++] = index;
    return 0;
}

/* 1 when key_node's name is in the window [pmark, ep_len). */
static int eval_has_prop(const js_ctx* j, size_t pmark, uint32_t key_node) {
    const yep_dnode* kn = yep_dom_node(j->v.dom, key_node);
    if (kn == NULL) {
        return 0;
    }
    yep_view kv = yep_dom_view(j->v.dom, kn->value);
    for (size_t i = pmark; i < j->ep_len; i++) {
        const yep_dnode* en = yep_dom_node(j->v.dom, j->ep[i]);
        if (en == NULL) {
            continue;
        }
        yep_view ev = yep_dom_view(j->v.dom, en->value);
        if (ev.len == kv.len && (kv.len == 0 || memcmp(ev.p, kv.p, kv.len) == 0)) {
            return 1;
        }
    }
    return 0;
}

static int eval_has_item(const js_ctx* j, size_t imark, uint32_t index) {
    for (size_t i = imark; i < j->ei_len; i++) {
        if (j->ei[i] == index) {
            return 1;
        }
    }
    return 0;
}

/* ---- the walk ----------------------------------------------------------- */

static js_eval js_apply(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth);
static void js_apply_body(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth, size_t pmark,
                          size_t imark);

/* the object-keyword walkers (defined after js_apply) */
static void check_pattern_properties(js_ctx* j, uint32_t pp_id, uint32_t inst_id, int depth);
static void check_property_names(js_ctx* j, uint32_t pn_id, uint32_t inst_id, int depth);
static void check_dependent_required(js_ctx* j, uint32_t dr_id, uint32_t inst_id, int depth);
static void check_dependent_schemas(js_ctx* j, uint32_t ds_id, uint32_t inst_id, int depth);
static void check_unevaluated_properties(js_ctx* j, uint32_t upv, int upf, uint32_t inst_id,
                                         int depth, size_t pmark);
static void check_unevaluated_items(js_ctx* j, uint32_t uv, int uf, uint32_t inst_id, int depth,
                                    size_t imark);

/* Applies a schema to a CHILD instance location: the child's own
 * annotations stay at the child (released here) — an annotation never
 * crosses an instance-location boundary. */
static void js_apply_child(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth) {
    size_t pm = j->ep_len, im = j->ei_len;
    js_apply(j, schema_id, inst_id, depth);
    j->ep_len = pm;
    j->ei_len = im;
}

/* A suppressed probe: does `schema_id` hold at `inst_id`? Diagnostics
 * during the trial are swallowed (and the trial enters no dynamic
 * scope — its anchors are as discarded as its annotations). The
 * probe's validity is its OWN trial_fail delta — a suppressed emit
 * never touches the enclosing application's fail_mark. The CALLER
 * decides the window's fate: absorbed when the probe's annotations
 * feed the parent (a held if / a matching anyOf branch), released
 * otherwise. */
static js_eval js_probe(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth) {
    int tf = j->v.trial_fail;
    j->v.suppress++;
    js_eval r = js_apply(j, schema_id, inst_id, depth);
    j->v.suppress--;
    /* the extent's own delta only: nested probes restored their share,
     * so what remains is what THIS trial swallowed directly */
    int my = j->v.trial_fail - tf;
    j->v.trial_fail = tf;
    if (my != 0) {
        r.valid = 0;
    }
    return r;
}

/* ---- reference resolution ------------------------------------------------ */

/* JSON Pointer walk from `start` with the URI percent layer decoded
 * before the ~0/~1 escapes (%25 -> '%', then the RFC 6901 unescape).
 * `p` spans the raw fragment text ("/$defs/foo"); the loop consumes a
 * '/' before each token. */
static int pointer_walk(js_ctx* j, uint32_t start, const char* p, size_t plen, uint32_t* out) {
    uint32_t cur = start;
    size_t i = 0;
    int tokens = 0;
    while (i < plen) {
        if (p[i] != '/') {
            return 0;
        }
        i++;
        char token[256];
        size_t tl = 0;
        while (i < plen && p[i] != '/') {
            char b = p[i];
            if (b == '%' && i + 2 < plen) {
                int hi = (p[i + 1] >= '0' && p[i + 1] <= '9') ? p[i + 1] - '0'
                                                              : ((p[i + 1] | 0x20) - 'a' + 10);
                int lo = (p[i + 2] >= '0' && p[i + 2] <= '9') ? p[i + 2] - '0'
                                                              : ((p[i + 2] | 0x20) - 'a' + 10);
                if ((unsigned)hi < 16 && (unsigned)lo < 16) {
                    b = (char)((hi << 4) | lo);
                    i += 2;
                }
            } else if (b == '~' && i + 1 < plen && (p[i + 1] == '0' || p[i + 1] == '1')) {
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

/* The plain-name anchor lookup: canonical base URI + '#' + name. When
 * `dynamic` and the static anchor is a $dynamicAnchor, the draft's
 * runtime rule applies (core spec 8.2.3.2): the OUTERMOST schema
 * resource in the dynamic scope defining an identically-named
 * $dynamicAnchor wins; with nothing in scope, the static target
 * stands (the behavior is "$ref-like" — the bookending requirement is
 * the schema author's). */
static int anchor_lookup(js_ctx* j, const char* base, const char* name, size_t nl, int dynamic,
                         uint32_t* out) {
    if (j->res == NULL) {
        return 0;
    }
    char key[YEP_JS_URI_MAX];
    size_t bn = strlen(base);
    if (bn + 1 + nl + 1 > sizeof(key)) {
        return 0;
    }
    memcpy(key, base, bn);
    key[bn] = '#';
    memcpy(key + bn + 1, name, nl);
    key[bn + 1 + nl] = '\0';
    uint32_t node = 0;
    uint8_t dyn = 0;
    if (!js_anchor_find(j->res, key, bn + 1 + nl, &node, &dyn)) {
        return 0;
    }
    if (dynamic && dyn == 1) {
        uint32_t found = UINT32_MAX;
        for (int i = 0; i < j->scope_n && found == UINT32_MAX; i++) {
            uint32_t anode = 0;
            if (js_anchor_dynamic_in_res(j->res, j->scope[i], name, nl, &anode)) {
                found = anode;
            }
        }
        if (found != UINT32_MAX) {
            *out = found;
            return 1;
        }
    }
    *out = node;
    return 1;
}

/* Resolves the URI-reference at `site` (the schema object carrying
 * $ref/$dynamicRef) against the site's compiled base. `dynamic`
 * enables the $dynamicRef runtime scope. 1 + *out, 0 unresolvable. */
static int resolve_ref_ext(js_ctx* j, uint32_t site, const char* rp, uint32_t rl, int dynamic,
                           uint32_t* out) {
    const char* base = "";
    uint32_t base_root = j->sroot;
    if (j->res != NULL) {
        js_refsite_find(j->res, site, &base, &base_root);
    }
    if (rl == 0) {
        *out = base_root; /* "" : the base resource root */
        return 1;
    }
    if (rp[0] == '#') {
        const char* frag = rp + 1;
        size_t fl = rl - 1;
        if (fl == 0) {
            *out = base_root; /* "#" : the base resource root */
            return 1;
        }
        if (frag[0] == '/') {
            return pointer_walk(j, base_root, frag, fl, out);
        }
        return anchor_lookup(j, base, frag, fl, dynamic, out);
    }
    /* a URI-reference with its own (possibly empty) fragment */
    const char* hash = memchr(rp, '#', rl);
    size_t ul = hash != NULL ? (size_t)(hash - rp) : rl;
    char canon[YEP_JS_URI_MAX];
    if (js_uri_resolve(base, rp, ul, canon, sizeof(canon)) != 0) {
        return 0;
    }
    if (hash == NULL) {
        /* no fragment: the resource root itself */
        uint32_t ri = 0;
        if (j->res != NULL && js_res_find(j->res, canon, &ri)) {
            *out = j->res->res[ri].root;
            return 1;
        }
        if (strcmp(canon, base) == 0) {
            *out = base_root;
            return 1;
        }
        return 0; /* outside the schema's identifier space: never fetched */
    }
    const char* frag = hash + 1;
    size_t fl = (size_t)(rp + rl - frag);
    if (frag[0] == '/') {
        uint32_t start = UINT32_MAX;
        uint32_t ri = 0;
        if (j->res != NULL && js_res_find(j->res, canon, &ri)) {
            start = j->res->res[ri].root;
        } else if (strcmp(canon, base) == 0) {
            start = base_root;
        }
        if (start == UINT32_MAX) {
            return 0;
        }
        return pointer_walk(j, start, frag, fl, out);
    }
    if (fl == 0) {
        uint32_t ri = 0;
        if (j->res != NULL && js_res_find(j->res, canon, &ri)) {
            *out = j->res->res[ri].root;
            return 1;
        }
        if (strcmp(canon, base) == 0) {
            *out = base_root;
            return 1;
        }
        return 0;
    }
    return anchor_lookup(j, canon, frag, fl, dynamic, out);
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
    const yep_dom* dd = d;
    const yep_dnode* n = yep_dom_node(dd, id);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == YEP_DOM_ALIAS) {
        return n->target != UINT32_MAX && inst_str_len(dd, n->target, out);
    }
    if (n->kind != YEP_DOM_SCALAR || yep_validate_scalar_tag(n) != YEPTRIS_TAG_STR) {
        return 0;
    }
    yep_view v = yep_dom_view(dd, n->value);
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

/* The dynamic scope: entering a schema object whose resource differs
 * from the current top pushes that resource (outermost first); probes
 * (suppress > 0) enter no scope — their anchors are as discarded as
 * their annotations. 1 pushed, 0 not. */
static int js_scope_push(js_ctx* j, uint32_t schema_id) {
    if (j->res == NULL || j->v.suppress > 0 || schema_id >= j->res->rindex_n) {
        return 0;
    }
    uint32_t ri = j->res->rindex[schema_id];
    if (ri == UINT32_MAX || j->scope_n >= YEP_JS_MAX_SCOPE) {
        return 0;
    }
    if (j->scope_n > 0 && j->scope[j->scope_n - 1] == ri) {
        return 0;
    }
    j->scope[j->scope_n++] = ri;
    return 1;
}

/* One application: brackets the body with the validity mark and the
 * annotation-window marks, pushes the dynamic scope, and hands the
 * whole window to the caller (absorb = keep, drop = release). */
static js_eval js_apply(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth) {
    int pushed = js_scope_push(j, schema_id);
    int fm = j->v.fail_mark;
    js_eval r;
    r.pmark = j->ep_len;
    r.imark = j->ei_len;
    js_apply_body(j, schema_id, inst_id, depth, r.pmark, r.imark);
    r.valid = (fm == j->v.fail_mark && !j->v.oom);
    r.plen = j->ep_len;
    r.ilen = j->ei_len;
    if (pushed) {
        j->scope_n--;
    }
    return r;
}

static void js_apply_body(js_ctx* j, uint32_t schema_id, uint32_t inst_id, int depth, size_t pmark,
                          size_t imark) {
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

    /* $ref / $dynamicRef compose with their siblings (2020-12); the
     * chain guard kills schema-only cycles that consume no instance
     * structure. Same-location application: the target's annotations
     * are absorbed into this object's set. */
    {
        int is_dyn = 0;
        uint32_t ref_val = UINT32_MAX;
        if (!kw_get(j, schema_id, "$ref", &ref_val) || ref_val == UINT32_MAX) {
            is_dyn = 1;
            if (!kw_get(j, schema_id, "$dynamicRef", &ref_val) || ref_val == UINT32_MAX) {
                is_dyn = 0;
                ref_val = UINT32_MAX;
            }
        }
        if (ref_val != UINT32_MAX) {
            const char* rp = NULL;
            uint32_t rl = 0;
            if (schema_word(j, ref_val, &rp, &rl)) {
                if (++j->v.ref_depth > JS_MAX_REF_CHAIN) {
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_REF,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "$ref chain exceeds %d (a cycle?)", JS_MAX_REF_CHAIN);
                    j->v.ref_depth = 0;
                    return;
                }
                uint32_t target = UINT32_MAX;
                if (!resolve_ref_ext(j, schema_id, rp, rl, is_dyn, &target)) {
                    yep_validate_emit(
                        &j->v, YEPTRIS_DIAG_REF, yep_validate_node_off(j->v.dom, inst_id),
                        "$ref \"%.*s\" does not resolve", (int)(rl > 64 ? 64 : rl), rp);
                    j->v.ref_depth = 0;
                    return;
                }
                js_apply(j, target, inst_id, depth); /* absorbed */
                j->v.ref_depth = 0;
            }
            /* an unreadable $ref value: ignored like any unknown keyword */
        } else {
            j->v.ref_depth = 0;
        }
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
             * without contains are annotations — ignored. The matching
             * indices are this object's ANNOTATION — kept when contains
             * holds, released when it fails. */
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
                        js_eval pr = js_probe(j, ctv, ich, depth);
                        j->ep_len = pr.pmark; /* the item probe annotates nothing outward */
                        j->ei_len = pr.imark;
                        if (pr.valid) {
                            matches++;
                            ei_append(j, i);
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
                        j->ei_len = imark; /* a failed contains annotates nothing */
                        yep_validate_emit(
                            &j->v, have_minc ? YEPTRIS_DIAG_MIN_CONTAINS : YEPTRIS_DIAG_CONTAINS,
                            yep_validate_node_off(j->v.dom, inst_id),
                            "%u of %u items match the contains schema (need %g)", matches, cnt,
                            minc);
                    } else if (maxc >= 0.0 && (double)matches > maxc) {
                        j->ei_len = imark;
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
                            js_apply_child(j, ch, ich, depth);
                            yep_validate_path_pop(&j->v, mark);
                        }
                        ei_append(j, i);
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
                            js_apply_child(j, ch, ich, depth);
                            yep_validate_path_pop(&j->v, mark);
                        }
                        ei_append(j, i);
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
                            js_apply_child(j, pv, ich, depth);
                            yep_validate_path_pop(&j->v, mark);
                        }
                        ei_append(j, i);
                        ich = ni;
                    }
                }
            }
        }
    }

    /* allOf / anyOf / oneOf / not: same-location applicators — a
     * branch's annotations join this object's set when the branch
     * holds (allOf unconditionally: its verdict is this object's), a
     * failed probe's window is released, `not` never annotates. */
    {
        uint32_t cv;
        if (kw_get(j, schema_id, "allOf", &cv) && cv != UINT32_MAX) {
            const yep_dnode* cn = snode(j, cv);
            if (cn != NULL && cn->kind == YEP_DOM_SEQUENCE) {
                uint32_t ch = cn->first_child;
                while (ch != UINT32_MAX) {
                    uint32_t nc = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                    js_apply(j, ch, inst_id, depth); /* absorbed */
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
                    js_eval br = js_probe(j, ch, inst_id, depth);
                    if (br.valid) {
                        any = 1; /* absorbed: the held branch annotates */
                    } else {
                        j->ep_len = br.pmark;
                        j->ei_len = br.imark;
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
                    js_eval br = js_probe(j, ch, inst_id, depth);
                    if (br.valid) {
                        matches++; /* absorbed */
                    } else {
                        j->ep_len = br.pmark;
                        j->ei_len = br.imark;
                    }
                    ch = snode(j, ch) ? snode(j, ch)->next_sibling : UINT32_MAX;
                }
                if (matches != 1) {
                    j->ep_len = pmark; /* no single branch: no annotations */
                    j->ei_len = imark;
                    yep_validate_emit(&j->v, YEPTRIS_DIAG_ONE_OF,
                                      yep_validate_node_off(j->v.dom, inst_id),
                                      "%u of the %u oneOf branches matched (exactly one "
                                      "required)",
                                      matches, branches);
                }
            }
        }
        if (kw_get(j, schema_id, "not", &cv) && cv != UINT32_MAX) {
            js_eval nr = js_probe(j, cv, inst_id, depth);
            j->ep_len = nr.pmark; /* the not-schema never annotates outward */
            j->ei_len = nr.imark;
            if (nr.valid) {
                yep_validate_emit(&j->v, YEPTRIS_DIAG_NOT, yep_validate_node_off(j->v.dom, inst_id),
                                  "the not-schema matched");
            }
        }
    }

    /* if/then/else: the if-schema rides a suppressed probe; exactly one
     * of then/else applies. A HOLDING if annotates (the probe's window
     * is absorbed); a failed one is released. then/else are
     * same-location applications — absorbed. An unreadable if (or
     * then/else) is ignored. */
    {
        uint32_t ifv;
        if (kw_get(j, schema_id, "if", &ifv) && ifv != UINT32_MAX) {
            int iff = 0;
            if (schema_truth(j, ifv, &iff)) {
                uint32_t tv = UINT32_MAX, ev = UINT32_MAX;
                int tf = 0, ef = 0;
                int have_t = kw_schema(j, schema_id, "then", &tv, &tf);
                int have_e = kw_schema(j, schema_id, "else", &ev, &ef);
                js_eval ir = js_probe(j, ifv, inst_id, depth);
                if (ir.valid) { /* absorbed: the if's annotations count */
                    if (have_t) {
                        js_apply(j, tv, inst_id, depth);
                    }
                } else {
                    j->ep_len = ir.pmark;
                    j->ei_len = ir.imark;
                    if (have_e) {
                        js_apply(j, ev, inst_id, depth);
                    }
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
            /* properties walk (values through their schemas): the
             * PRESENT keys are this object's annotation */
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
                                        js_apply_child(j, v, cv, depth);
                                        yep_validate_path_pop(&j->v, mark);
                                    }
                                    ep_append(j, ck);
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
            /* propertyNames: the schema applies to each KEY string
             * (a different instance location: annotates nothing here) */
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
             * schema applies to the whole object (same location: the
             * inner annotations join this object's set) */
            {
                uint32_t dsv;
                if (kw_get(j, schema_id, "dependentSchemas", &dsv) && dsv != UINT32_MAX) {
                    check_dependent_schemas(j, dsv, inst_id, depth);
                }
            }
            /* additionalProperties: the keys properties does not name —
             * the keys it APPLIED to are this object's annotation */
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
                                uint32_t pv2 =
                                    snode(j, pk) ? snode(j, pk)->next_sibling : UINT32_MAX;
                                if (pkn != NULL && pkn->kind == YEP_DOM_SCALAR) {
                                    yep_view pw = yep_dom_view(j->sd, pkn->value);
                                    known = pw.len == kv.len &&
                                            (kv.len == 0 || memcmp(pw.p, kv.p, kv.len) == 0);
                                }
                                pk = pv2 != UINT32_MAX && snode(j, pv2)
                                         ? snode(j, pv2)->next_sibling
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
                                uint32_t pv2 =
                                    snode(j, pk) ? snode(j, pk)->next_sibling : UINT32_MAX;
                                const char* pat = NULL;
                                uint32_t pl = 0;
                                if (pkn != NULL && pkn->kind == YEP_DOM_SCALAR &&
                                    schema_word(j, pk, &pat, &pl) &&
                                    yep_re_cache_search(j->cache, pat, pl, kv.p != NULL ? kv.p : "",
                                                        kv.len) == 1) {
                                    known = 1;
                                }
                                pk = pv2 != UINT32_MAX && snode(j, pv2)
                                         ? snode(j, pv2)->next_sibling
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
                                    js_apply_child(j, ap, v, depth);
                                    yep_validate_path_pop(&j->v, mark);
                                }
                                ep_append(j, k);
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

    /* the unevaluated* keywords ride LAST in their schema object,
     * whatever the document order: their verdict reads every sibling
     * keyword's and every in-place applicator's annotations for THIS
     * location — [pmark, ep_len) / [imark, ei_len) */
    {
        uint32_t uv;
        int uf = 0;
        if (kw_schema(j, schema_id, "unevaluatedProperties", &uv, &uf)) {
            check_unevaluated_properties(j, uv, uf, inst_id, depth, pmark);
        }
        if (kw_schema(j, schema_id, "unevaluatedItems", &uv, &uf)) {
            check_unevaluated_items(j, uv, uf, inst_id, depth, imark);
        }
    }
}

/* ---- the object-keyword walkers ------------------------------------------ */

/* patternProperties: pattern -> schema over the KEY strings; every
 * matching pattern applies its schema to the key's value (a false
 * schema rides js_apply's FALSE_SCHEMA diagnostic). The matched keys
 * are this object's annotation. */
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
                        js_apply_child(j, pv, v, depth);
                        yep_validate_path_pop(&j->v, mark);
                    }
                    ep_append(j, k);
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
                js_apply_child(j, pn_id, k, depth);
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
 * to the whole object (same location: its annotations join this
 * object's set). */
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
                js_apply(j, sv, inst_id, depth); /* absorbed */
            }
        }
        sk = ns;
    }
}

/* unevaluatedProperties: the keys no sibling keyword and no in-place
 * applicator evaluated — [pmark, ep_len). A false schema flags each
 * one; a readable schema is APPLIED to the value (the applied keys
 * then annotate, so a nested unevaluatedProperties:true marks every
 * property evaluated for the parent). */
static void check_unevaluated_properties(js_ctx* j, uint32_t upv, int upf, uint32_t inst_id,
                                         int depth, size_t pmark) {
    const yep_dom* d = j->v.dom;
    const yep_dnode* n = yep_dom_node(d, inst_id);
    if (n == NULL || n->kind != YEP_DOM_MAPPING) {
        return;
    }
    uint32_t k = n->first_child;
    while (k != UINT32_MAX && !j->v.oom) {
        uint32_t v = yep_dom_node(d, k) ? yep_dom_node(d, k)->next_sibling : UINT32_MAX;
        uint32_t nk =
            v != UINT32_MAX && yep_dom_node(d, v) ? yep_dom_node(d, v)->next_sibling : UINT32_MAX;
        const yep_dnode* kn = yep_dom_node(d, k);
        if (kn != NULL && kn->kind == YEP_DOM_SCALAR && !eval_has_prop(j, pmark, k)) {
            yep_view kv = yep_dom_view(d, kn->value);
            char namebuf[64];
            size_t cl = name_copy(namebuf, sizeof(namebuf), kv.p, kv.len);
            if (upf) {
                size_t mark = j->v.path_len;
                int pushed = yep_validate_path_push(&j->v, namebuf, cl);
                yep_validate_emit(&j->v, YEPTRIS_DIAG_UNEVALUATED_PROPERTY,
                                  yep_validate_node_off(d, k), "property \"%s\" is not evaluated",
                                  namebuf);
                if (pushed == 0) {
                    yep_validate_path_pop(&j->v, mark);
                }
            } else {
                size_t mark = j->v.path_len;
                if (yep_validate_path_push(&j->v, namebuf, cl) == 0) {
                    js_apply_child(j, upv, v, depth);
                    yep_validate_path_pop(&j->v, mark);
                }
                ep_append(j, k); /* the applied property now counts as evaluated */
            }
        }
        k = nk;
    }
}

/* unevaluatedItems: the mirror over array indices ([imark, ei_len)). */
static void check_unevaluated_items(js_ctx* j, uint32_t uv, int uf, uint32_t inst_id, int depth,
                                    size_t imark) {
    const yep_dom* d = j->v.dom;
    const yep_dnode* n = yep_dom_node(d, inst_id);
    if (n == NULL || n->kind != YEP_DOM_SEQUENCE) {
        return;
    }
    uint32_t ich = n->first_child;
    for (uint32_t i = 0; ich != UINT32_MAX && !j->v.oom; i++) {
        uint32_t ni = yep_dom_node(d, ich) ? yep_dom_node(d, ich)->next_sibling : UINT32_MAX;
        if (!eval_has_item(j, imark, i)) {
            if (uf) {
                size_t mark = j->v.path_len;
                int pushed = yep_validate_path_push_index(&j->v, i);
                yep_validate_emit(&j->v, YEPTRIS_DIAG_UNEVALUATED_ITEM,
                                  yep_validate_node_off(d, ich), "item %u is not evaluated", i);
                if (pushed == 0) {
                    yep_validate_path_pop(&j->v, mark);
                }
            } else {
                size_t mark = j->v.path_len;
                if (yep_validate_path_push_index(&j->v, i) == 0) {
                    js_apply_child(j, uv, ich, depth);
                    yep_validate_path_pop(&j->v, mark);
                }
                ei_append(j, i);
            }
        }
        ich = ni;
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
 * sides, build the report and hand over the schema DOM, the retained
 * pattern cache and the identifier map (the declarations are
 * internal.h's — the shared SSOT). */
int yep_jsonschema_run(const yep_dom* schema_dom, uint32_t schema_root, const yep_dom* instance_dom,
                       const uint32_t* doc_roots, uint32_t doc_count, yeptris_validate_report* rep,
                       const yep_re_cache* cache, const js_resources* res) {
    js_ctx j;
    memset(&j, 0, sizeof(j));
    j.sd = schema_dom;
    j.sroot = schema_root;
    j.cache = cache;
    j.res = res;
    j.v.dom = instance_dom;
    j.v.rep = rep;
    j.v.input = instance_dom->input_base;
    j.v.input_len = instance_dom->input_len;
    int rc = 0;
    for (uint32_t i = 0; i < doc_count; i++) {
        j.v.path_len = 0;
        /* the stream face's document attribution: a multi-document
         * stream addresses its documents as array elements */
        size_t mark = yep_validate_doc_prefix(&j.v, doc_count > 1, i);
        js_validate_doc(&j, doc_roots[i]);
        yep_validate_path_pop(&j.v, mark);
        if (j.v.oom) {
            rc = -1;
            break;
        }
    }
    /* the walk's scratch (the lazy line index, the path buffer, the
     * annotation arenas) dies here: the report carries only finished
     * diagnostics */
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, j.v.line_off);
    yep_free(sys, j.v.path);
    yep_free(sys, j.ep);
    yep_free(sys, j.ei);
    return rc;
}

/* The tag-driven hook's seam (internal.h, #906 slice 4): one bound
 * schema applied to ONE instance subtree through the CALLER'S walk
 * context. The context is copied in (its path prefix is the tag
 * site's — the subtree's instance pointers extend it), the schema
 * root is applied at depth 0 (the same walk, the same annotation
 * model), and the context is copied back: the report, the guards and
 * the scratch pointers stay the caller's, so the cap law and the
 * line/col resolution hold across the whole report. */
int yep_jsonschema_apply_subtree(const yep_dom* schema_dom, uint32_t schema_root, yep_vctx* inst,
                                 uint32_t inst_root, const yep_re_cache* cache,
                                 const js_resources* res) {
    if (schema_dom == NULL || inst == NULL) {
        return 0;
    }
    js_ctx j;
    memset(&j, 0, sizeof(j));
    j.sd = schema_dom;
    j.sroot = schema_root;
    j.cache = cache;
    j.res = res;
    j.v = *inst; /* the caller's prefix, report, line index, guards */
    js_apply(&j, j.sroot, inst_root, 0);
    /* the annotation arenas are THIS application's scratch (the full
     * driver's tail law) — they never join the caller's context */
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, j.ep);
    yep_free(sys, j.ei);
    *inst = j.v; /* scratch may have grown: the caller still frees it */
    return j.v.oom ? -1 : 0;
}
