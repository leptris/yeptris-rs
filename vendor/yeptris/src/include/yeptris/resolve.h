/* resolve.h — schema resolvers and typed scalar access (TODO.impl/10).
 *
 * Typing is pluggable and outside the grammar: a resolver decides,
 * per plain scalar, which implicit tag it carries. YAML 1.2 core
 * correctness and Psych/libyaml 1.1 parity are two resolvers over one
 * interface; the parse options select one per document.
 *
 * Resolution assigns a dense tag id at parse time (stored on DOM
 * nodes); typed VALUE conversion happens on accessor demand — parse
 * speed never pays for typing the caller never reads.
 */
#ifndef YEPTRIS_RESOLVE_H
#define YEPTRIS_RESOLVE_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>
#include <yeptris/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tag ids. The core set is preallocated and stable (ABI-pinned);
 * custom tags resolve to YEPTRIS_TAG_CUSTOM and keep their string. */
typedef enum {
    YEPTRIS_TAG_STR = 0,
    YEPTRIS_TAG_INT = 1,
    YEPTRIS_TAG_FLOAT = 2,
    YEPTRIS_TAG_BOOL = 3,
    YEPTRIS_TAG_NULL = 4,
    YEPTRIS_TAG_TIMESTAMP = 5,
    YEPTRIS_TAG_SEQ = 6,
    YEPTRIS_TAG_MAP = 7,
    YEPTRIS_TAG_BINARY = 8,
    YEPTRIS_TAG_MERGE = 9,
    YEPTRIS_TAG_VALUE = 10,
    YEPTRIS_TAG_CUSTOM = 11,
} YeptrisTagId;

/* Schemas. Appending only — bindings hard-code the values (test_abi). */
typedef enum {
    YEPTRIS_SCHEMA_12_CORE = 0, /* default: the yaml-test-suite target */
    YEPTRIS_SCHEMA_11_COMPAT,   /* libyaml/Psych implicit typing */
    YEPTRIS_SCHEMA_12_JSON,     /* spec 10.2: JSON productions; plain
                                 * scalars matching none are errors (#882) */
    YEPTRIS_SCHEMA_12_FULL,     /* the full 1.2 family (#906 slice 3):
                                 * core typing + RFC3339-era timestamps
                                 * (spec 2.22's four forms; the yaml.org
                                 * type repository carried forward) and
                                 * the merge/value words; !!binary rides
                                 * as passthrough. A validation family:
                                 * explicit !!timestamp content must
                                 * re-resolve through the classifier. */
} YeptrisSchema;

/* Canonical tag URI for a core id ("tag:yaml.org,2002:str"); NULL for
 * YEPTRIS_TAG_CUSTOM. */
YEPTRIS_API const char* yeptris_tag_uri(YeptrisTagId id);

/* The tag a scalar resolved to (PLAIN scalars carry their implicit
 * tag; quoted/other styles are always STR; explicit tags map to their
 * core id when they match one, else CUSTOM). Non-scalars: SEQ/MAP.
 * YEPTRIS_ERROR_ARG on a bad handle. */
YEPTRIS_API YeptrisTagId yeptris_node_tag_id(YeptrisNode node);

/* Typed access (lazy conversion from the raw view; resolution's tag
 * id decides eligibility, the conversion revalidates the bytes):
 * YEPTRIS_ERROR_PARSE when the scalar's tag does not match the
 * accessor (e.g. an INT accessor on a STR). */
YEPTRIS_API YeptrisStatus yeptris_node_int(YeptrisNode node, int64_t* out);
YEPTRIS_API YeptrisStatus yeptris_node_float(YeptrisNode node, double* out);
YEPTRIS_API YeptrisStatus yeptris_node_bool(YeptrisNode node, int* out);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_RESOLVE_H */
