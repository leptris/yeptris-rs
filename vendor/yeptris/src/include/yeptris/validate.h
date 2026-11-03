/* validate.h — the validation face (issue #882).
 *
 * Two checks over one engine pass:
 *
 *  1. SCHEMA-FAMILY validation (`yeptris_validate`): parse + validate
 *     fused. Under YEPTRIS_SCHEMA_12_CORE the walk checks explicit-tag
 *     consistency (spec 10.3 resolution) and mapping-key uniqueness
 *     (spec 3.2.1.3: keys compare by canonical form — 0o13 and 0xB
 *     are the same key). Under YEPTRIS_SCHEMA_12_JSON the spec 10.2
 *     family applies: implicit resolution uses ONLY the JSON
 *     productions (the core extensions — 0o/0x radix, "+12.3",
 *     "True"/"~" casings — stay strings, per example 10.8's Invalid
 *     list), mapping keys must be strings (a key resolving to a
 *     non-string production is KEY_NOT_STRING; a no-production plain
 *     key IS a string, per example 10.8), explicit non-JSON tags are
 *     rejected; core checks still run. Under YEPTRIS_SCHEMA_12_FULL
 *     the spec 10.4 "other schema" family applies — the core schema
 *     "extended with additional tag resolution rules": untagged plains
 *     also resolve RFC3339-era timestamps (spec example 2.22's
 *     canonical/iso8601/spaced/date forms, classified by PyYAML's
 *     timestamp regex as the practice oracle) and the merge/value
 *     words, while explicit !!timestamp content must re-resolve
 *     through the same classifier (TAG_MISMATCH otherwise). !!binary
 *     rides as passthrough (base64 shaping is a construction concern);
 *     core checks still run. The 1.1 numeric quirks (underscores,
 *     sexagesimal, leading-0 octal, y/n booleans) stay with
 *     11_COMPAT — full is spec-refereed, not psych-refereed.
 *
 *  2. JSON SCHEMA validation (`yeptris_json_schema_validate`): the
 *     schema arrives as bytes (parsed with the same engine), the
 *     target against it — draft 2020-12 keyword subset (see the entry
 *     comment). Unknown keywords are ignored, per 2020-12.
 *
 * The COMPILED-schema economy (`yeptris_schema_*`) retains one parsed
 * schema for many instance streams; the TAG-DRIVEN registry
 * (`yeptris_schema_registry_*`, #906 slice 4) binds compiled schemas
 * to user tags over the family walk (`yeptris_validate_ex`). The
 * report converts to the published JSON-Schema output shape
 * (`yeptris_validate_report_write`, #906 slice 5).
 *
 * Every diagnostic carries a JSON-pointer instance path ("/a/b/0" —
 * ~0/~1 escaped), a YEPTRIS_DIAG_* code, a bounded expected/got
 * detail, and the offending node's 1-based line/column (0 = unknown:
 * scalars whose bytes the DOM owns in its arena have no input
 * position; the path still identifies them exactly).
 *
 * STREAMS (#885 item 4): a validated stream is addressed as an array
 * of documents. When the input holds MORE THAN ONE document, every
 * diagnostic path is prefixed with its document's index segment —
 * "/1/a/b" is a/b of document 1 (0-based), so diagnostics name their
 * document even when the miss is unpositioned (an empty document's
 * Error-row scalar has no line/col). Line/col/offset stay
 * input-absolute. Streams with a single document keep the bare
 * instance pointers — existing consumers' paths are unchanged.
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
    /* the 2020-12 unevaluated* annotation model (the sota sweep) */
    YEPTRIS_DIAG_UNEVALUATED_PROPERTY = 34, /* no sibling/applicator evaluated it */
    YEPTRIS_DIAG_UNEVALUATED_ITEM = 35,
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
 * against `schema` — the strict lanes: explicit-tag consistency,
 * canonical-form key uniqueness, (12_JSON) string keys over the JSON
 * productions, and (12_FULL) timestamp content against the full
 * family's classifier. Paths are root-relative per document; a
 * multi-document stream prefixes them with the document index (see
 * STREAMS above), so diagnostics name their document. The parse's own
 * guards compose: a document past the engine's depth cap fails the
 * parse (YEPTRIS_ERROR_DEPTH) before any validation runs.
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

/* The EVENT-ROUTE twin of yeptris_validate (#902): the same parse
 * (front-end, depth/encoding guards, statuses — the DOM route's
 * sink-entry seam) feeding a streaming validator state machine. The
 * family lanes ride the event walk itself; NO node tree is ever
 * built. Everything observable is the DOM route's, by construction
 * and by the differential gate:
 *
 *  - diagnostics: the same code rail (0-35 after the sweep), the same report shape and
 *    cap, the same JSON-pointer paths and the stream face's
 *    per-document attribution — emitted in the DOM walk's order (a
 *    mapping's own tag check, its duplicate-key misses, then its
 *    pairs' content);
 *  - positions: the DOM route's region law — a diagnostic sits at the
 *    node's input-borrowed value bytes (0/0 when the engine pooled
 *    them), so line/col/offset match bit for bit;
 *  - statuses: OK / SCHEMA / PARSE / DEPTH / ENCODING / MEMORY / ARG,
 *    byte-identical argument gates (NULL with length, 11_COMPAT).
 *    All three families (12_CORE / 12_JSON / 12_FULL) ride both
 *    routes with identical verdicts.
 *
 * The compiled JSON Schema lane (yeptris_schema_validate) keeps the
 * DOM route: its evaluator needs random access (uniqueItems, contains,
 * required); a streaming twin waits for #901's annotation-model seam.
 *
 * Memory: the caller owns *out; free it with
 * yeptris_validate_report_free. */
YEPTRIS_API YeptrisStatus yeptris_validate_stream(const char* buf, size_t len, YeptrisSchema schema,
                                                  yeptris_validate_report** out);

/* JSON Schema draft 2020-12 subset (#882 slice 2; #885 item 2; the
 * 2020-12 sota sweep). `schema` is parsed with the same engine (JSON
 * is YAML flow); document 0 of the stream is the schema. Keyword
 * subset: type, enum, const, pattern, minimum, maximum,
 * exclusiveMinimum, exclusiveMaximum (numeric, 2020-12 form),
 * multipleOf (libm-free quotient test),
 * minLength/maxLength (code points), minItems/maxItems, uniqueItems,
 * contains (+ the minContains/maxContains qualifiers), items
 * (+ prefixItems; the draft-07 array form rides as prefixItems),
 * properties, required, patternProperties, propertyNames,
 * minProperties/maxProperties, additionalProperties (bool or schema;
 * excludes patternProperties matches), dependentRequired,
 * dependentSchemas, if/then/else, allOf/anyOf/oneOf/not,
 * unevaluatedProperties/unevaluatedItems (the 2020-12 annotation
 * model: evaluations collect upward through the in-place applicators,
 * a failed branch annotates nothing, and nothing crosses an
 * instance-location boundary), $ref, $dynamicRef, boolean schemas.
 * REFERENCES: $id base-URI composition (RFC 3986 path merge),
 * $anchor/$dynamicAnchor plain-name fragments scoped to their nearest
 * $id resource, JSON Pointer fragments with the percent layer
 * (%25 -> '%', then ~0/~1), and $dynamicRef's runtime dynamic scope
 * (the OUTERMOST schema resource in scope defining the anchor). No
 * network: a reference resolves only against the schema's own
 * identifier space — an external target (the metaschema, an HTTP
 * fixture) is a DIAG_REF, never a fetch. Boolean schemas
 * (`true`/`false`) apply. Unknown keywords and known keywords with
 * unreadable values are ignored (2020-12). Instance types ride the
 * core-schema resolution (the YAML->JSON data model): "y" is boolean,
 * 1.0 is integer. The IETF WG's next-edition draft
 * (draft-ietf-jsonschema-json-schema-00) carries these same keyword
 * semantics — no evaluator delta; the diff record is
 * docs/schema/ietf-wg-draft-delta.md.
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
 * every document of the instance stream — the stream face's document
 * attribution applies (see STREAMS above): a multi-document stream's
 * paths carry the document index. yeptris_schema_free releases
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

/* ---- the TAG-DRIVEN SCHEMA registry (#906 slice 4) -----------------------
 *
 * The dialect table's reserved slot: a compiled schema binds to a user
 * tag, so a document node carrying `!shape` (its EXPLICIT tag, exactly
 * the resolved bytes the DOM exposes — `!shape`, `!<tag:...>`, or a
 * `%TAG`-resolved URI; `!!x` arrives as `tag:yaml.org,2002:x`) has its
 * SUBTREE validated against the bound schema. Registration is the only
 * extension point (OCP): the family walk reads the registry, it never
 * grows schema branches.
 *
 * Matching is byte-exact on the tag's resolved form and fires only on
 * an explicit tag — implicit (plain) typing never triggers a binding.
 * An unregistered tag passes untouched: no diagnostic, no lane. The
 * hook rides the DOM family walk (yeptris_validate_ex /
 * yeptris_validate_document_ex): roots, sequence items and mapping
 * VALUES (a tagged mapping KEY is not registry-validated — its
 * instance path is undefined in the walk's model). Diagnostics from a
 * bound subtree land at the tag site: the tag node's own path + the
 * schema walk's instance pointers, with the offending node's
 * line/col/offset; the report cap and codes are the shared rail.
 * Bindings COMPOSE: a node inside a bound subtree that carries its own
 * registered tag is validated by both schemas.
 *
 * THE REGISTRY LAW (the document-ownership law, no refcounting): the
 * registry BORROWS the compiled schemas it binds — it owns nothing
 * they own. A bound schema must OUTLIVE the registry: free the
 * registry first (yeptris_schema_registry_free), then the schemas.
 * Freeing a bound schema while its registry lives leaves the registry
 * dead (a later lookup is undefined), exactly like a freed document.
 * NULL is accepted everywhere a handle is (a NULL registry is simply
 * no bindings; yeptris_schema_registry_free(NULL) is a no-op).
 *
 * Thread law: one registry per thread, like documents and schemas —
 * it is mutable under register and read-only during validation;
 * read-only sharing across threads (no register in flight) is safe. */
typedef struct yeptris_schema_registry yeptris_schema_registry;

/* A fresh empty registry; NULL on allocation failure. */
YEPTRIS_API yeptris_schema_registry* yeptris_schema_registry_new(void);

/* Binds `tag` (copied — nothing borrows the caller's bytes beyond the
 * call) to the compiled `schema` (borrowed — see THE REGISTRY LAW).
 * Status: YEPTRIS_OK; YEPTRIS_ERROR_ARG (NULL handle, an empty tag,
 * or a NULL schema); YEPTRIS_ERROR_MEMORY. Re-registering a tag
 * REPLACES the binding (last registration wins). */
YEPTRIS_API YeptrisStatus yeptris_schema_registry_register(yeptris_schema_registry* reg,
                                                           const char* tag, size_t tag_len,
                                                           const yeptris_schema* schema);

/* The schema bound to `tag` (exact bytes), or NULL. Memory: borrowed
 * from the registry — valid until a register replaces the tag or the
 * registry is freed. */
YEPTRIS_API const yeptris_schema* yeptris_schema_registry_lookup(const yeptris_schema_registry* reg,
                                                                 const char* tag, size_t tag_len);

YEPTRIS_API void yeptris_schema_registry_free(yeptris_schema_registry* reg);

/* Validation options (size-versioned like yeptris_emit_options:
 * initialize with {sizeof(yeptris_validate_options)} so future fields
 * stay optional; NULL selects the defaults). */
typedef struct yeptris_validate_options {
    uint32_t size; /* sizeof(yeptris_validate_options) */
    /* The tag-driven registry (#906 slice 4): NULL = none. Only read
     * when the caller's struct is large enough (size-versioned). The
     * registry is borrowed for the call; see THE REGISTRY LAW above. */
    const yeptris_schema_registry* registry;
} yeptris_validate_options;

/* yeptris_validate / yeptris_validate_document with options: the
 * family walk plus, when a registry is attached, the tag-driven
 * schema applications described above. Everything else (statuses,
 * report shape, paths, positions) is the flat face's, unchanged. */
YEPTRIS_API YeptrisStatus yeptris_validate_ex(const char* buf, size_t len, YeptrisSchema schema,
                                              const yeptris_validate_options* opts,
                                              yeptris_validate_report** out);

YEPTRIS_API YeptrisStatus yeptris_validate_document_ex(YeptrisDocument doc, YeptrisSchema schema,
                                                       const yeptris_validate_options* opts,
                                                       yeptris_validate_report** out);

/* ---- the structured OUTPUT face (#906 slice 5) ---------------------------
 *
 * The report as the json-schema.org RECOMMENDED OUTPUT shape (core
 * spec section 12; the meta-schema at
 * https://json-schema.org/draft/2020-12/output/schema): one root
 * output unit — {"valid", "keywordLocation", "instanceLocation"} and,
 * when the report carries diagnostics, "errors" — with one child unit
 * per diagnostic {valid, keywordLocation, instanceLocation, error}.
 * This is the published BASIC form: the report is flat by the
 * enumerated-misses law, so the schema-structured DETAILED/VERBOSE
 * hierarchies (which need per-miss schema-side keyword paths the flat
 * report does not carry) are not derivable from it. keywordLocation
 * is derived from the diagnostic code — the keyword's name segment
 * ("/type", "/required", ...; the family codes and the bookkeeping
 * codes sit at the root pointer ""); it is a JSON Pointer, but NOT
 * necessarily resolvable in the schema (the flat report carries no
 * schema path). absoluteKeywordLocation is never emitted, and $ref
 * sites report the root pointer, so the output always satisfies the
 * published meta-schema. Strings are JSON-escaped (RFC 8259); the
 * bytes are UTF-8; the transform is pure and deterministic — the same
 * report always produces the same bytes.
 *
 * buf == NULL: returns the exact byte count needed, writes nothing.
 * Otherwise writes at most cap bytes (NUL-terminated when cap
 * allows) and returns the count written; when cap is too small
 * nothing is written and the needed size is returned. Returns 0 on a
 * NULL report (use the flat face for those — the transform never
 * invents a verdict).
 *
 * Memory: writes into the caller's buffer; borrows nothing. */
YEPTRIS_API size_t yeptris_validate_report_write(const yeptris_validate_report* report, char* buf,
                                                 size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_VALIDATE_H */
