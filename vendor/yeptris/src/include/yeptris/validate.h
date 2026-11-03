/* validate.h — the validation face (issue #882).
 *
 * Two checks over one engine pass:
 *
 *  1. SCHEMA-FAMILY validation (`yeptris_validate`): parse + validate
 *     fused. Under YEPTRIS_SCHEMA_12_CORE the walk checks explicit-tag
 *     consistency (spec 10.3 resolution) and mapping-key uniqueness
 *     (spec 3.2.1.3: keys compare by canonical form — 0o13 and 0xB
 *     are the same key). Under YEPTRIS_SCHEMA_12_JSON the spec 10.2
 *     family applies: every PLAIN scalar must match a JSON production
 *     (the table's `*` -> Error row), mapping keys must be strings,
 *     non-JSON tags are rejected; core checks still run.
 *
 *  2. JSON SCHEMA validation (`yeptris_json_schema_validate`): the
 *     schema arrives as bytes (parsed with the same engine), the
 *     target against it — draft 2020-12 keyword subset (see the entry
 *     comment). Unknown keywords are ignored, per 2020-12.
 *
 * Every diagnostic carries a JSON-pointer instance path ("/a/b/0" —
 * ~0/~1 escaped), a YEPTRIS_DIAG_* code, a bounded expected/got
 * detail, and the offending node's 1-based line/column (0 = unknown:
 * scalars whose bytes the DOM owns in its arena have no input
 * position; the path still identifies them exactly).
 *
 * Results are WHOLE REPORTS: one call, no per-diagnostic callback —
 * the caller drains the flat array and frees the report once.
 */
#ifndef YEPTRIS_VALIDATE_H
#define YEPTRIS_VALIDATE_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>
#include <yeptris/parse.h> /* YeptrisDocument, YeptrisSchema */
#include <yeptris/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostic codes. Appending only — bindings hard-code the values
 * (test_abi pins the head). */
typedef enum {
    YEPTRIS_DIAG_NONE = 0,
    /* the schema-family walk */
    YEPTRIS_DIAG_TAG_MISMATCH = 1,   /* explicit tag contradicts content */
    YEPTRIS_DIAG_KEY_NOT_STRING = 2, /* json12: mapping key is not a string */
    YEPTRIS_DIAG_NON_JSON_TYPE = 3,  /* json12: plain scalar matches no
                                      * JSON production / carries a
                                      * non-JSON tag (spec 10.2.2 Error) */
    YEPTRIS_DIAG_DUPLICATE_KEY = 4,  /* equal keys in one mapping */
    /* the JSON Schema walk */
    YEPTRIS_DIAG_TYPE = 5,
    YEPTRIS_DIAG_REQUIRED = 6,
    YEPTRIS_DIAG_ENUM = 7,
    YEPTRIS_DIAG_CONST = 8,
    YEPTRIS_DIAG_PATTERN = 9,
    YEPTRIS_DIAG_MINIMUM = 10,
    YEPTRIS_DIAG_MAXIMUM = 11,
    YEPTRIS_DIAG_EXCLUSIVE_MINIMUM = 12,
    YEPTRIS_DIAG_EXCLUSIVE_MAXIMUM = 13,
    YEPTRIS_DIAG_MIN_LENGTH = 14,
    YEPTRIS_DIAG_MAX_LENGTH = 15,
    YEPTRIS_DIAG_MIN_ITEMS = 16,
    YEPTRIS_DIAG_MAX_ITEMS = 17,
    YEPTRIS_DIAG_UNIQUE_ITEMS = 18,
    YEPTRIS_DIAG_ADDITIONAL_PROPERTY = 19,
    YEPTRIS_DIAG_ANY_OF = 20,
    YEPTRIS_DIAG_ONE_OF = 21,
    YEPTRIS_DIAG_NOT = 22,
    YEPTRIS_DIAG_REF = 23,          /* $ref unresolvable / chain too deep */
    YEPTRIS_DIAG_FALSE_SCHEMA = 24, /* the schema is `false` here */
    YEPTRIS_DIAG_DEPTH = 25,        /* recursion guard tripped */
    YEPTRIS_DIAG_LIMIT = 26,        /* report truncated (the sentinel
                                     * rides the LAST slot; more
                                     * diagnostics were dropped) */
    /* the 2020-12 sweep (#885 item 2) */
    YEPTRIS_DIAG_MULTIPLE_OF = 27,
    YEPTRIS_DIAG_MIN_PROPERTIES = 28,
    YEPTRIS_DIAG_MAX_PROPERTIES = 29,
    YEPTRIS_DIAG_CONTAINS = 30,
    YEPTRIS_DIAG_MIN_CONTAINS = 31,
    YEPTRIS_DIAG_MAX_CONTAINS = 32,
    YEPTRIS_DIAG_DEPENDENT_REQUIRED = 33,
} YeptrisDiagCode;

/* Bounded diagnostic text; always NUL-terminated (over-long paths and
 * details truncate — the same discipline as the error channel). */
#define YEPTRIS_DIAG_PATH_MAX 256
#define YEPTRIS_DIAG_DETAIL_MAX 128

typedef struct yeptris_diagnostic {
    uint32_t code; /* YeptrisDiagCode */
    uint32_t line; /* 1-based; 0 = unknown */
    uint32_t col;  /* 1-based; 0 = unknown */
    uint32_t offset;
    char path[YEPTRIS_DIAG_PATH_MAX];
    char detail[YEPTRIS_DIAG_DETAIL_MAX];
} yeptris_diagnostic;
YEPTRIS_CT_ASSERT(sizeof(yeptris_diagnostic) ==
                  16 + YEPTRIS_DIAG_PATH_MAX + YEPTRIS_DIAG_DETAIL_MAX);

/* The diagnostics list. Opaque; free it with
 * yeptris_validate_report_free (one call, everything). */
typedef struct yeptris_validate_report yeptris_validate_report;

/* Diagnostic count (0 = the document is valid). */
YEPTRIS_API size_t yeptris_validate_report_count(const yeptris_validate_report* report);

/* Diagnostic i (0-based); NULL out of range. Memory: borrowed from the
 * report — valid until yeptris_validate_report_free. */
YEPTRIS_API const yeptris_diagnostic*
yeptris_validate_report_at(const yeptris_validate_report* report, size_t index);

YEPTRIS_API void yeptris_validate_report_free(yeptris_validate_report* report);

/* Parses `buf` (the same front door as yeptris_parse_ex: encodings,
 * BOM, NEL normalization) and validates every document of the stream
 * against `schema`. Paths are root-relative per document.
 *
 * Status: YEPTRIS_OK (valid; *out is a report — possibly with zero
 * diagnostics for an empty stream); YEPTRIS_ERROR_SCHEMA (invalid:
 * drain *out); YEPTRIS_ERROR_PARSE / DEPTH / ENCODING / MEMORY (the
 * parse itself failed; *out stays NULL); YEPTRIS_ERROR_ARG.
 *
 * Memory: the caller owns *out; free it with
 * yeptris_validate_report_free. */
YEPTRIS_API YeptrisStatus yeptris_validate(const char* buf, size_t len, YeptrisSchema schema,
                                           yeptris_validate_report** out);

/* Validates an already-parsed document (or stream). The document is
 * not modified; the report borrows nothing from it. */
YEPTRIS_API YeptrisStatus yeptris_validate_document(YeptrisDocument doc, YeptrisSchema schema,
                                                    yeptris_validate_report** out);

/* JSON Schema draft 2020-12 subset (#882 slice 2; #885 item 2). `schema`
 * is parsed with the same engine (JSON is YAML flow); document 0 of the
 * stream is the schema. Keyword subset: type, enum, const, pattern,
 * minimum/maximum/exclusiveMinimum/exclusiveMaximum (numeric, 2020-12
 * form), multipleOf (libm-free quotient test), minLength/maxLength
 * (code points), minItems/maxItems, uniqueItems, contains (+ the
 * minContains/maxContains qualifiers), items (+ prefixItems; the
 * draft-07 array form rides as prefixItems), properties, required,
 * patternProperties, propertyNames, minProperties/maxProperties,
 * additionalProperties (bool or schema; excludes patternProperties
 * matches), dependentRequired, dependentSchemas, if/then/else,
 * allOf/anyOf/oneOf/not, $ref (LOCAL "#/..." pointers only — $defs and
 * definitions both resolve). Boolean schemas (`true`/`false`) apply.
 * Unknown keywords and known keywords with unreadable values are
 * ignored (2020-12). Instance types ride the core-schema resolution
 * (the YAML->JSON data model): "y" is boolean, 1.0 is integer.
 *
 * Status: as yeptris_validate, plus YEPTRIS_ERROR_ARG when the schema
 * bytes hold no document, and YEPTRIS_ERROR_PARSE when they do not
 * parse. A $ref that resolves nowhere is a DIAG_REF diagnostic, not a
 * status. Recursion is depth-guarded (the engine's guard): a
 * self-referential $ref over a deep or cyclic chain reports DIAG_DEPTH
 * / DIAG_REF — never a crash. */
YEPTRIS_API YeptrisStatus yeptris_json_schema_validate(const char* schema, size_t schema_len,
                                                       const char* buf, size_t len,
                                                       yeptris_validate_report** out);

/* The COMPILED SCHEMA — the leptris-XSD economy (#885 item 3): parse
 * the schema ONCE, validate MANY instance streams against the retained
 * model. The handle owns everything it reads: the schema bytes (copied
 * — the retained DOM's views borrow them), the parsed document, and
 * the pattern kernel's compiled programs (the XSD content-model
 * analog: `(a+)+$` compiles once here, not once per validated string
 * the way the one-shot face pays). yeptris_schema_compile returns NULL
 * on failure; yeptris_last_error() carries the why (bad arguments,
 * bytes that hold no document, a schema that does not parse,
 * allocation failure).
 *
 * yeptris_schema_validate keeps yeptris_json_schema_validate's exact
 * status/report contract; diagnostics enumerate every miss across
 * every document of the instance stream. yeptris_schema_free releases
 * the handle and everything it owns in ONE call (no refcounting) —
 * afterwards the handle is DEAD: passing it anywhere is undefined,
 * exactly like a freed document. NULL is accepted (a no-op).
 *
 * Thread law: one schema handle per thread, like documents — the
 * handle is immutable after compile (the pattern programs are built at
 * compile time), so read-only sharing across threads is safe; reports
 * are per-call and caller-owned. */
typedef struct yeptris_schema yeptris_schema;

YEPTRIS_API yeptris_schema* yeptris_schema_compile(const char* bytes, size_t n);

YEPTRIS_API YeptrisStatus yeptris_schema_validate(yeptris_schema* s, const char* buf, size_t len,
                                                  yeptris_validate_report** out);

YEPTRIS_API void yeptris_schema_free(yeptris_schema* s);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_VALIDATE_H */
