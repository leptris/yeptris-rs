/* validate/internal.h — the validation face's shared internals (#882).
 *
 * Private to src/yeptris/validate/: the report shape, the diagnostic
 * emitter (path capture + line/col resolution), and the JSON-pointer
 * scratch path. The two walks (validate.c's schema family and
 * jsonschema.c's keyword subset) share only this — MECE keeps the
 * checks themselves in their own TUs.
 */
#ifndef YEP_VALIDATE_INTERNAL_H
#define YEP_VALIDATE_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <yeptris/validate.h>

#include "../dom/dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The report cap: a hostile document can carry O(nodes) violations, so
 * the walk stops here and the LIMIT sentinel replaces the last slot. */
#define YEP_VALIDATE_MAX_DIAGS 256

struct yeptris_validate_report {
    yeptris_diagnostic* diags;
    uint32_t count;
    int truncated;
};

typedef struct yep_vctx {
    const yep_dom* dom;
    yeptris_validate_report* rep;
    /* line/col resolution over the node bytes (lazy: built on the
     * first positioned diagnostic; valid documents never pay) */
    const char* input;
    size_t input_len;
    uint32_t* line_off; /* byte offset of every line start */
    uint32_t line_cnt, line_cap;
    /* JSON-pointer path under construction (DFS scratch) */
    char* path;
    size_t path_len, path_cap;
    int depth; /* the walk's nesting guard (never exceeds the DOM's) */
    int oom;   /* an allocation failed: stop diagnosing */
    /* JSON Schema trial suppression (anyOf/oneOf/not probes): emits
     * inside a probe never reach the report and never touch
     * fail_mark — the probe's own validity is ITS trial_fail delta
     * (zero delta = held), the enclosing application's is its
     * fail_mark delta */
    int suppress;
    /* the walks' invalid-application counters: every emit increments
     * exactly one of them (suppressed -> trial_fail, otherwise
     * fail_mark), so an application's validity is one comparison (the
     * unevaluated* annotation model reads fail_mark) */
    int fail_mark;
    int trial_fail;
    int ref_depth; /* consecutive $ref/$dynamicRef expansions */
    /* the event route's staging hook (#902): when set, composed
     * diagnostics join the OPEN mapping's stage (the flush replays
     * [own tag][duplicates][pair content] so report order stays the
     * DOM walk's); the cap counts the walk position (rep count +
     * staged). NULL on the DOM route — the report path, unchanged. */
    void (*stage_put)(void* stage, const yeptris_diagnostic* d);
    void* stage;
    size_t staged;
    /* the tag-driven registry (#906 slice 4): NULL = no bindings —
     * the DOM route only (the event route keeps its lanes). */
    const yeptris_schema_registry* reg;
} yep_vctx;

/* Offset of a node's first positioned byte (the value span of the node
 * itself, or of its first scalar leaf for collections; UINT32_MAX when
 * everything reachable lives in the arena — line/col unknown). */
uint32_t yep_validate_node_off(const yep_dom* d, uint32_t id);

/* ---- the fact cores (shared by the DOM walk and the event route) -------
 *
 * The family checks read five facts per node: kind (yep_dom_kind),
 * explicit-tag presence, the resolved tag id, the plainness law
 * (PLAIN style or the implicit flag), and the value bytes. The DOM
 * walk extracts them from yep_dnode; the event route (#902) extracts
 * the same five from the engine's events — both call THESE cores, so
 * a verdict change lands once and both routes move together. `off` is
 * the caller's position fact (the DOM's node_off law / the event
 * route's borrowed-view law); UINT32_MAX = unpositioned (line/col 0).
 * A zero-length value decodes to {NULL, 0} (the yep_dom_view law). */

/* The scalar tag law: explicit tag wins, then the resolver verdict for
 * plain/implicit scalars, then str for quoted/block styles. */
uint8_t yep_validate_tag_of(uint8_t tag_id, int has_tag, int plainish);

/* Explicit-tag consistency (both families). */
struct yep_resolver;
void yep_check_tag_facts(yep_vctx* c, int kind, int has_tag, uint8_t tag_id, yep_view value,
                         uint32_t off, const struct yep_resolver* fam, int json_family);

/* json12: a plain scalar must match a JSON production; explicit
 * non-JSON tags rejected. */
void yep_check_json_scalar_facts(yep_vctx* c, int has_tag, uint8_t tag_id, int plainish,
                                 yep_view value, uint32_t off);

/* json12: a SCALAR mapping key's checks (the caller resolves aliases
 * and non-scalar kinds first — the DOM wrapper keeps that recursion). */
void yep_check_json_key_facts(yep_vctx* c, int has_tag, uint8_t tag_id, int plainish,
                              yep_view value, uint32_t off);

/* ---- the canonical digest law (spec 3.2.1.3) --------------------------- */

/* The 64-bit fold primitive behind every canonical fingerprint. */
uint64_t yep_validate_hash_mix(uint64_t h, uint64_t v);

/* Seed constants of the canonical fold (validate.c's DOM hash and the
 * event route's streaming fold must agree bit for bit). */
#define YEP_VAL_DIGEST_SEQ_SEED 0x5EED51ULL  /* sequences: ordered fold */
#define YEP_VAL_DIGEST_MAP_SEED 0x5EED4A9ULL /* mappings: XOR of pairs */
#define YEP_VAL_DIGEST_PAIR_SEED 0x51D1ULL   /* one (key, value) pair */
#define YEP_VAL_DIGEST_ALIAS 0xA11AULL       /* unbound alias fallback */
#define YEP_VAL_DIGEST_SCALAR 0x5CA1AULL     /* scalar base (tag added) */

/* The scalar arm of the canonical hash: base + tag, the numeric VALUE
 * for int/float (the double bits), the raw bytes otherwise. */
uint64_t yep_validate_scalar_digest(uint8_t tag, yep_view v);

/* Emits one diagnostic at the current path. line/col resolve from
 * `off` through the lazy line index; 0/0 when unknown. Detail is
 * printf-composed into the bounded slot. Never fails the caller: the
 * report cap turns the last slot into the LIMIT sentinel. */
void yep_validate_emit(yep_vctx* c, int code, uint32_t off, const char* detail_fmt, ...);

/* The report-path writer (the cap law's one home): appends a RESOLVED
 * diagnostic, or turns the last slot into the LIMIT sentinel once the
 * report is full. The event route's flush replays its staged
 * diagnostics through this — the compose time (and therefore the path
 * and position) stays the walk's. */
void yep_validate_report_put(yep_vctx* c, const yeptris_diagnostic* d);

/* JSON-pointer path scratch: push an escaped segment / a decimal
 * index, pop to a mark. 0 ok, -1 OOM (ctx.oom set; the walk unwinds). */
int yep_validate_path_push(yep_vctx* c, const char* seg, size_t seg_len);
int yep_validate_path_push_index(yep_vctx* c, uint32_t idx);
void yep_validate_path_pop(yep_vctx* c, size_t mark);

/* The stream face's document attribution (#885 item 4): a validated
 * stream is addressed as an array of documents — when it holds MORE
 * THAN ONE document, every diagnostic path is prefixed with the
 * document's index segment, so diagnostics name their document ("/1/k"
 * is key k of document 1; the line/col stay input-absolute). Streams
 * with a single document keep the bare instance pointers (the
 * JSON-Schema-Test-Suite's paths and every single-doc pin are
 * unchanged). Call before a document's walk and pop the returned mark
 * after it; OOM sets ctx.oom and the walk unwinds. */
size_t yep_validate_doc_prefix(yep_vctx* c, int qualified, uint32_t index);

/* The bounded ECMA-regex subset behind `pattern` (jsonschema.c):
 * literals, '.', classes, groups, alternation, the greedy/lazy
 * quantifiers, anchors, \d\w\s\D\W\S\b\B, \xHH, \cX and the
 * \p{Letter}/\p{digit} property escapes. Classes match WHOLE UTF-8
 * code points (the named classes carry range tables: ASCII \d/\w,
 * Unicode \s, code-point complements, generated Unicode properties);
 * literals match byte-wise. A Thompson NFA over per-thread positions —
 * O(len * m), no backtracking blowup, no recursion over the subject.
 * Patterns over the program budget are invalid. 1 match, 0 no match,
 * -1 invalid pattern. */
int yep_re_search(const char* pat, uint32_t pat_len, const char* s, uint32_t s_len);

/* The retained-program cache behind the compiled schema (#885 item 3):
 * pattern source -> compiled NFA, populated at compile time, immutable
 * afterwards (read-only sharing, the document law). A miss — a NULL
 * cache included — compiles fresh with yep_re_search's exact verdicts;
 * cache_add returns 1 stored, 0 not retained (invalid pattern, empty,
 * table full — misses recompile), -1 allocation failure (loud). */
typedef struct yep_re_cache yep_re_cache;
yep_re_cache* yep_re_cache_new(void);
void yep_re_cache_free(yep_re_cache* t);
int yep_re_cache_add(yep_re_cache* t, const char* pat, uint32_t plen);
int yep_re_cache_search(const yep_re_cache* t, const char* pat, uint32_t plen, const char* s,
                        uint32_t s_len);

/* Canonical scalar tag of a DOM node (explicit tag, then the parse
 * resolver's plain verdict, then str): the value model both walks
 * compare under. */
uint8_t yep_validate_scalar_tag(const yep_dnode* n);

/* Canonical-form hash and equality over DOM subtrees (spec 3.2.1.3:
 * same tag + canonical form; numbers numerically within a tag). The
 * equality may cross documents — the schema's enum/const live in a
 * different DOM than the instance. Depth-bounded. */
uint64_t yep_validate_node_hash(const yep_dom* d, uint32_t id, int depth);
int yep_validate_node_equal(const yep_dom* da, uint32_t a, const yep_dom* db, uint32_t b,
                            int depth);

/* ---- the schema identifier map (resources.c) ----------------------------- */

/* Bounded identifier tables: an identifier past a cap (or a URI over
 * the length cap) is not registered — the reference then fails to
 * resolve at validation (DIAG_REF), exactly like any other unknown
 * target. Nothing loops, nothing guesses. */
#define YEP_JS_MAX_RESOURCES 1024
#define YEP_JS_MAX_ANCHORS 1024
#define YEP_JS_MAX_REFSITES 8192
#define YEP_JS_URI_MAX 512
#define YEP_JS_MAX_SCOPE 256 /* the runtime dynamic-scope stack */

/* One schema RESOURCE: a canonical (fragment-free) URI naming the
 * schema object that established it with "$id" — pointer fragments
 * walk from that node. The document root is resource 0 (the empty
 * base "" when the schema has no "$id"). */
typedef struct {
    char* uri;
    uint32_t root;
} js_resource;

/* A plain-name fragment: canonical URI + '#' + name -> the node.
 * `dynamic` distinguishes "$dynamicAnchor" (the $dynamicRef
 * extension points) from "$anchor". `res` is the owning resource
 * index (anchors belong to the resource of their nearest "$id"). */
typedef struct {
    char* key;
    uint32_t node;
    uint32_t res;
    uint8_t dynamic;
} js_anchor;

/* A "$ref"/"$dynamicRef" site: the canonical base URI it resolves
 * against, and that base resource's root (where fragment-only
 * pointers start walking). */
typedef struct {
    uint32_t site;
    char* base;
    uint32_t base_root;
} js_refsite;

/* The compiled schema's identifier map (built once by the crawl at
 * compile time, read-only afterwards — the document law). Everything
 * it owns dies with yep_js_resources_free. */
typedef struct {
    js_resource* res;
    uint32_t res_n;
    js_anchor* anc;
    uint32_t anc_n;
    js_refsite* site;
    uint32_t site_n;
    uint32_t* rindex; /* node id -> owning resource index (UINT32_MAX unvisited) */
    uint32_t rindex_n;
} js_resources;

/* The constructor: one bounded DFS over the schema DOM collecting
 * every $id/$anchor/$dynamicAnchor and $ref/$dynamicRef site. NULL on
 * allocation failure. */
js_resources* yep_js_resources_crawl(const yep_dom* d, uint32_t root);
void yep_js_resources_free(js_resources* r);

/* Lookups (linear over the bounded tables): resource by canonical
 * URI; anchor by key (URI + '#' + name); a resource's DYNAMIC anchor
 * by plain name (the dynamic-scope search's per-resource probe); a
 * ref site's base. Each returns 1 found, 0 absent. */
int js_res_find(const js_resources* r, const char* uri, uint32_t* idx_out);
int js_anchor_find(const js_resources* r, const char* key, size_t key_len, uint32_t* node_out,
                   uint8_t* dyn_out);
int js_anchor_dynamic_in_res(const js_resources* r, uint32_t res_idx, const char* name,
                             size_t name_len, uint32_t* node_out);
int js_refsite_find(const js_resources* r, uint32_t site_node, const char** base,
                    uint32_t* base_root);

/* URI-reference resolution (RFC 3986 section 5.2, the exercised
 * subset: schemes, authorities, path merge with dot-segment removal,
 * query carry). Produces the canonical fragment-free form. 0 ok, -1
 * overlong (out holds ""). */
int js_uri_resolve(const char* base, const char* ref, size_t ref_len, char* out, size_t cap);

/* jsonschema.c's driver: applies the schema tree (its own DOM) to
 * every instance document root, emitting into `rep`. `cache` is the
 * compiled schema's retained pattern programs (NULL: none — the
 * one-shot face). `res` is the compiled schema's identifier map (the
 * resources/anchors/ref-site bases the reference machinery resolves
 * through; NULL: none — every reference beyond a bare "#/..." pointer
 * stays unresolvable). Returns 0, -1 on OOM. */
int yep_jsonschema_run(const yep_dom* schema_dom, uint32_t schema_root, const yep_dom* instance_dom,
                       const uint32_t* doc_roots, uint32_t doc_count, yeptris_validate_report* rep,
                       const yep_re_cache* cache, const js_resources* res);

/* The report constructor (validate.c owns the report machinery): a
 * fresh empty report, NULL on allocation failure. */
yeptris_validate_report* yep_validate_report_new(void);

/* compile.c's seam (#885 item 3): the compiled-schema constructor with
 * an explicit status — the one-shot face preserves its exact observable
 * statuses through it. NULL on failure with *st = YEPTRIS_ERROR_ARG
 * (NULL bytes with length, or bytes that hold no document; the thread
 * error channel carries the message), the failed parse's status, or
 * YEPTRIS_ERROR_MEMORY. */
struct yeptris_schema;
yeptris_schema* yep_schema_compile_st(const char* bytes, size_t n, YeptrisStatus* st);

/* The validation-family selector shared by both routes (#902): CORE_12
 * and JSON_12 resolve to their resolvers (json_family set for the
 * strict lane); every other schema is a parse-compat resolver, not a
 * family — 0 with the thread error channel set, exactly like the DOM
 * face's ARG rejection. */
int yep_validate_schema_family(YeptrisSchema schema, const struct yep_resolver** fam,
                               int* json_family);

/* ---- the tag-driven schema hook (#906 slice 4) --------------------------- */

/* The compiled schema's read-only evaluation view: everything the
 * keyword walk reads, borrowed from the handle (the REGISTRY LAW
 * applies — the handle must outlive every view taken from it). */
typedef struct {
    const yep_dom* dom;        /* the schema's retained tree */
    uint32_t sroot;            /* its document-0 root */
    const yep_re_cache* cache; /* retained pattern programs */
    const js_resources* res;   /* the identifier map */
} yep_schema_view;

/* compile.c: fills *out from the handle. NULL handle fills zeros. */
void yep_schema_view_of(const yeptris_schema* s, yep_schema_view* out);

/* jsonschema.c: applies the bound schema's root to ONE instance
 * subtree. `inst` is the CALLER'S walk context (the DOM route's
 * yep_vctx) — its path prefix (the tag site), report, line index and
 * guards are shared, so the subtree's misses land at the tag site's
 * path + the schema's instance pointers with the offending node's
 * position, and the cap law holds across the whole report. The
 * context's scratch pointers (path buffer, line index) may have been
 * reallocated on return — the caller frees them as before. Returns 0,
 * -1 on OOM (ctx.oom set; the walk unwinds). */
int yep_jsonschema_apply_subtree(const yep_dom* schema_dom, uint32_t schema_root, yep_vctx* inst,
                                 uint32_t inst_root, const yep_re_cache* cache,
                                 const js_resources* res);

/* ---- the structured output face (#906 slice 5) ---------------------------
 *
 * output.c: the report -> JSON transform (the published BASIC shape;
 * the byte contract lives on the public header). The keyword names
 * behind the code-derived keywordLocation pointers — the one table
 * (the output SSOT): a code without a keyword row writes the root
 * pointer "". */
const char* yep_validate_output_keyword(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* YEP_VALIDATE_INTERNAL_H */
