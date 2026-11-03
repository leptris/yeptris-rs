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
    /* JSON Schema trial suppression (anyOf/oneOf/not probes) */
    int suppress;
    int trial_hit;
    int ref_depth; /* consecutive $ref expansions */
} yep_vctx;

/* Offset of a node's first positioned byte (the value span of the node
 * itself, or of its first scalar leaf for collections; UINT32_MAX when
 * everything reachable lives in the arena — line/col unknown). */
uint32_t yep_validate_node_off(const yep_dom* d, uint32_t id);

/* Emits one diagnostic at the current path. line/col resolve from
 * `off` through the lazy line index; 0/0 when unknown. Detail is
 * printf-composed into the bounded slot. Never fails the caller: the
 * report cap turns the last slot into the LIMIT sentinel. */
void yep_validate_emit(yep_vctx* c, int code, uint32_t off, const char* detail_fmt, ...);

/* JSON-pointer path scratch: push an escaped segment / a decimal
 * index, pop to a mark. 0 ok, -1 OOM (ctx.oom set; the walk unwinds). */
int yep_validate_path_push(yep_vctx* c, const char* seg, size_t seg_len);
int yep_validate_path_push_index(yep_vctx* c, uint32_t idx);
void yep_validate_path_pop(yep_vctx* c, size_t mark);

/* The bounded ECMA-regex subset behind `pattern` (jsonschema.c):
 * literals, '.', classes, groups, alternation, the greedy/lazy
 * quantifiers, anchors, \d\w\s\D\W\S\b\B and \xHH. Byte-oriented
 * (ASCII classes; UTF-8 literals match byte-wise). A Thompson NFA —
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

/* jsonschema.c's driver: applies the schema tree (its own DOM) to
 * every instance document root, emitting into `rep`. `cache` is the
 * compiled schema's retained pattern programs (NULL: none — the
 * one-shot face). Returns 0, -1 on OOM. */
int yep_jsonschema_run(const yep_dom* schema_dom, uint32_t schema_root, const yep_dom* instance_dom,
                       const uint32_t* doc_roots, uint32_t doc_count, yeptris_validate_report* rep,
                       const yep_re_cache* cache);

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

#ifdef __cplusplus
}
#endif

#endif /* YEP_VALIDATE_INTERNAL_H */
