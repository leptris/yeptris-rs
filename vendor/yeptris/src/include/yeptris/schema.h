/* schema.h — the fused schema-descriptor materialization API
 * (issue #238, TODO.restructure/83).
 *
 * The descriptor is the ABI, NOT a schema language: XSD/JSON-Schema
 * cannot express consumer semantics (two-names-one-slot merges, value
 * maps, key renames, raw capture) — the CONSUMER compiles those into
 * this flat positional plan, the same way lutaml-model compiles its
 * rule plans. The engine materializes against it in ONE native pass;
 * the intermediate generic document (the Hash/Array the bindings used
 * to build) never exists.
 *
 * Design contract (from the committed consumer, co-designed):
 *  - kind CALLBACK is the escape hatch: the raw value span plus the
 *    node's byte position come back as records; the host finishes
 *    just those fields (custom procs, polymorphism, delegates).
 *  - Results are WHOLE DOCUMENTS: this call never crosses the FFI
 *    boundary per value — the caller passes output buffers sized by
 *    expectation and receives per-node counts.
 *  - The descriptor ABI is versioned in lockstep with the bindings
 *    ({c-semver}.{binding-patch}); desc_abi changes only with a C
 *    minor bump.
 *  - Sequences of a plan run the SAME node plan for every element
 *    (flags ELEMENT); nested mappings run their child plan slice.
 */
#ifndef YEPTRIS_SCHEMA_H
#define YEPTRIS_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>
#include <yeptris/parse.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The descriptor ABI version of this header. */
#define YEPTRIS_DESC_ABI 1u

/* node kinds */
enum {
    YEP_SK_SCALAR = 0, /* typed leaf: type_tag decides the column */
    YEP_SK_SEQUENCE,   /* sequence: children[0] is the ELEMENT plan */
    YEP_SK_MAPPING,    /* mapping: children are the key plans */
    YEP_SK_CALLBACK,   /* escape hatch: raw span + position records */
};

/* scalar type tags (the columns' element types) */
enum {
    YEP_ST_STR = 0, /* off/len into the input */
    YEP_ST_INT,     /* int64_t */
    YEP_ST_FLOAT,   /* double */
    YEP_ST_BOOL,    /* uint8_t */
    YEP_ST_NULL,    /* no payload (count only) */
    YEP_ST_ANY,     /* SCALAR: keep the resolver's verdict — the
                       column is a YeptrisValue-like record */
};

/* node flags */
enum {
    YEP_SF_REQUIRED = 1u << 0,   /* missing at its mapping -> error */
    YEP_SF_FIRST_WINS = 1u << 1, /* duplicates: first match kept
                                    (default: last, as at parse) */
};

/* One plan node. ABI-pinned: a pointer plus 16 fixed-width bytes —
 * 24 on LP64, 20 on 32-bit (the hosts' FFI mirrors size :pointer
 * per-process, so the layout contract is the POINTER plus the tail). */
typedef struct yeptris_desc_node {
    const char* wire_name; /* mapping key to match (NULL: the root) */
    uint8_t kind;          /* YEP_SK_* */
    uint8_t type_tag;      /* YEP_ST_* (SCALAR) */
    uint16_t flags;        /* YEP_SF_* */
    uint32_t child_index;  /* SEQUENCE: the element plan; MAPPING: the
                              first child. Unused otherwise (0). */
    uint32_t child_count;  /* MAPPING: children from child_index. */
    uint32_t reserved;     /* must be 0 (ABI headroom) */
} yeptris_desc_node;

YEPTRIS_CT_ASSERT(sizeof(yeptris_desc_node) == sizeof(const char*) + 16);

/* One column's output. Caller allocates the payload array (sized by
 * expectation); the call fills it and sets count. No per-value FFI:
 * hosts drain whole columns after the call. */
typedef struct yeptris_schema_column {
    void* data;        /* element type from the plan's type_tag */
    uint32_t capacity; /* elements the buffer holds */
    uint32_t count;    /* elements materialized (out) */
} yeptris_schema_column;

/* STR/CALLBACK payload: off/len into the INPUT buffer (zero-copy to
 * the host). CALLBACK also carries the node's byte position. */
typedef struct yeptris_span2 {
    uint32_t off;
    uint32_t len;
    uint32_t node_off; /* the node's first byte (callbacks) */
    uint32_t pad;
} yeptris_span2;

YEPTRIS_CT_ASSERT(sizeof(yeptris_span2) == 16);

/* Parses `source` (YAML or strict JSON — the same front door as
 * yeptris_parse_ex) and materializes against the descriptor in one
 * pass: node i's matches land in cols[i]. The input buffer must
 * outlive the string/callback spans (they borrow it).
 *
 * Status: YEPTRIS_OK; YEPTRIS_ERROR_PARSE (yeptris_last_error);
 * YEPTRIS_ERROR_SCHEMA (a REQUIRED node missing — the error carries
 * the node index; detail via yeptris_last_error); YEPTRIS_ERROR_MEMORY
 * / ARG (NULL descriptor, desc_len 0, desc_abi mismatch). */
YEPTRIS_API YeptrisStatus yeptris_schema_load(const char* source, size_t len, YeptrisSchema schema,
                                              const yeptris_desc_node* desc, uint32_t desc_len,
                                              uint32_t desc_abi, yeptris_schema_column* cols);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_SCHEMA_H */
