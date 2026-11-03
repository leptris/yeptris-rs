/* plan.h — the compiled plan walk (TODO.restructure/87 slice one,
 * yeptris#293): a Descriptor-shaped plan compiled from a JSON spec,
 * applied to a parsed JSON tape in ONE C pass producing typed
 * COLUMNAR output — no Ruby/Python tree, no per-node host dispatch.
 *
 * Slice one covers the lutaml-model shape: a root collection (seq or
 * map-of-rows) whose rows are mappings of scalar leaves (int, float,
 * str, bool). Nested paths, the YAML leg, and partial descriptors
 * compose later per the board item.
 *
 * The plan borrows nothing; the result borrows the tape's source
 * buffer for string spans (free the result before the tape/source).
 */
#ifndef YEPTRIS_PLAN_H
#define YEPTRIS_PLAN_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>
#include <yeptris/tape.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Column kinds (mirrors the spec's leaf "kind"). */
enum {
    YEP_PLAN_INT = 0,
    YEP_PLAN_FLOAT,
    YEP_PLAN_STR,
    YEP_PLAN_BOOL,
};

typedef struct yeptris_plan yeptris_plan;
typedef struct yeptris_plan_result yeptris_plan_result;

/* Compiles a plan from a strict-JSON spec document:
 *
 *   {"kind":"seq","path":"items","children":[
 *      {"name":"id","kind":"int"}, {"name":"ok","kind":"bool"}, ...]}
 *
 * kind: "seq" or "map" — the ROWS container's shape at the root.
 * path: the root mapping's key holding the rows — a string, or an
 *       ARRAY of strings for a segmented (nested) path walked
 *       mapping by mapping. Empty/absent: the seq root itself.
 *       Leaf kinds: int, float, str, bool. Returns NULL with *st on
 *       a malformed spec (ARG/PARSE) or allocation failure. */
YEPTRIS_API yeptris_plan* yeptris_plan_compile(const char* spec, size_t len, YeptrisStatus* st);

YEPTRIS_API void yeptris_plan_free(yeptris_plan* plan);

YEPTRIS_API size_t yeptris_plan_column_count(const yeptris_plan* plan);

/* Applies the plan to a tape (from yeptris_parse_json_tape; the
 * LENIENT tape also works — NUM records convert at extraction).
 * Every row of the collection fills one slot per column; missing or
 * null leaves mark the slot null. Returns NULL with *st on
 * PARSE (document shape disagreement) or MEMORY. */
YEPTRIS_API yeptris_plan_result*
yeptris_tape_plan_walk(const yeptris_json_tape* tape, const yeptris_plan* plan, YeptrisStatus* st);

/* The YAML/DOM leg: the same compiled plan applied to a parsed
 * yeptris document (yeptris_parse — any schema). Typed extraction
 * rides the parse-time tag ids; string columns expose (ptr,len)
 * views into the document's regions (input or string arena), so the
 * RESULT BORROWS the document — free the result first. Same status
 * contract as the tape walk. */
YEPTRIS_API yeptris_plan_result*
yeptris_document_plan_walk(YeptrisDocument doc, const yeptris_plan* plan, YeptrisStatus* st);

YEPTRIS_API void yeptris_plan_result_free(yeptris_plan_result* r);

/* Result accessors: rows, and per-column kind + typed arrays. The
 * arrays hold `rows` entries; NULL markers are the per-column u8
 * bitmap (1 = null/missing). String columns expose spans into the
 * tape's source. */
YEPTRIS_API size_t yeptris_plan_result_rows(const yeptris_plan_result* r);
YEPTRIS_API int yeptris_plan_result_kind(const yeptris_plan_result* r, size_t col);
YEPTRIS_API const int64_t* yeptris_plan_result_ints(const yeptris_plan_result* r, size_t col);
YEPTRIS_API const double* yeptris_plan_result_floats(const yeptris_plan_result* r, size_t col);
YEPTRIS_API const uint32_t* yeptris_plan_result_str_offs(const yeptris_plan_result* r, size_t col);
YEPTRIS_API const uint32_t* yeptris_plan_result_str_lens(const yeptris_plan_result* r, size_t col);
/* The DOM leg's string column: rows (ptr,len) views into the
 * document (NULL on the tape leg — use the offs/lens pair there). */
typedef struct yeptris_plan_str {
    const char* p;
    size_t len;
} yeptris_plan_str;

YEPTRIS_API const yeptris_plan_str* yeptris_plan_result_strs(const yeptris_plan_result* r,
                                                             size_t col);
YEPTRIS_API const uint8_t* yeptris_plan_result_nulls(const yeptris_plan_result* r, size_t col);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_PLAN_H */
