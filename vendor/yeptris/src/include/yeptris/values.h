/* values.h — the typed value stream (TODO.impl/15 phase F).
 *
 * The materialization fast path for FFI bindings: one parse, one
 * drain of PRE-CONVERTED typed values. The C side runs the same
 * engine the recorder drives, then converts every scalar by its
 * resolver tag — the host walks a flat array and builds containers,
 * never re-parsing text.
 *
 * The `is_key` bit carries the key/value alternation, so hosts need
 * no pending-key bookkeeping; `tag_id` rides along so hosts with
 * schema quirks (Psych's single-char y/n, PyYAML's dot-required
 * floats) can re-decide a scalar's type from its raw bytes without
 * re-running a conversion grammar.
 */
#ifndef YEPTRIS_VALUES_H
#define YEPTRIS_VALUES_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>
#include <yeptris/resolve.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YEP_V_DOC = 0, /* document boundary */
    YEP_V_NULL,
    YEP_V_BOOL,      /* b carries 0/1 */
    YEP_V_INT,       /* i */
    YEP_V_FLOAT,     /* d */
    YEP_V_STR,       /* off/len bytes */
    YEP_V_TIMESTAMP, /* off/len bytes (host constructs the date) */
    YEP_V_SEQ_OPEN,
    YEP_V_MAP_OPEN,
    YEP_V_CLOSE, /* closes the innermost open container */
    YEP_V_ALIAS, /* off/len: the alias name */
    /* decorates the entry that FOLLOWS it: the anchor name binds to
     * that value (identity for aliases). Emitted only when anchored. */
    YEP_V_ANCHOR, /* off/len: the name */
} YeptrisValueKind;

/* 24 bytes, ABI-pinned. Every scalar carries its raw bytes (off/len)
 * next to a typed payload; `p` holds the INT bits or the FLOAT bits
 * (reinterpreted by kind — hosts unpack it as ONE 64-bit field, so
 * the record costs 7 values, not 8). */
typedef struct {
    uint8_t kind;   /* YeptrisValueKind */
    uint8_t tag_id; /* the resolver's verdict for this scalar */
    uint8_t is_key; /* the entry lands in the inner map's key slot */
    uint8_t b;      /* BOOL payload; implicit-plain flag otherwise */
    uint32_t off;   /* STR/TIMESTAMP/ALIAS/anchor bytes */
    uint32_t len;
    uint64_t p; /* INT or FLOAT payload, by kind */
} YeptrisValue;
YEPTRIS_CT_ASSERT(sizeof(YeptrisValue) == 24);

/* Parses `yaml` and drains the typed value stream. On success sets
 * *vals / *count / *arena / *arena_len (both malloc'd; free with
 * yeptris_value_free exactly once). Returns YEPTRIS_OK,
 * YEPTRIS_ERROR_PARSE (details via yeptris_last_error), or
 * YEPTRIS_ERROR_MEMORY / YEPTRIS_ERROR_ARG. */
YEPTRIS_API YeptrisStatus yeptris_value_drain(const char* yaml, size_t len, YeptrisSchema schema,
                                              YeptrisValue** vals, size_t* count, char** arena,
                                              size_t* arena_len);

YEPTRIS_API void yeptris_value_free(YeptrisValue* vals, char* arena);

/* Columnar projection of the same stream (task #42: the materializer
 * wall is per-record struct decoding in FFI hosts — parallel typed
 * buffers let bindings iterate C-speed arrays and unpack whole
 * columns in one call). Same walk, same semantics, same arena; one
 * carved allocation holds payloads/offs/lens/kinds/tags/is_keys/bools
 * (payloads is the block base — yeptris_value_free_columns frees it
 * and the arena; both are safe on a zero-entry drain). */
typedef struct {
    size_t count;
    size_t arena_len;
    int64_t* payloads; /* INT or FLOAT bits, by kind */
    uint32_t* offs;    /* STR/TIMESTAMP/ALIAS/ANCHOR bytes */
    uint32_t* lens;
    uint8_t* kinds; /* YeptrisValueKind */
    uint8_t* tags;  /* resolver verdict per scalar */
    uint8_t* is_keys;
    uint8_t* bools; /* BOOL payload; implicit-plain flag otherwise */
    char* arena;
} YeptrisValueColumns;

YEPTRIS_API YeptrisStatus yeptris_value_drain_columns(const char* yaml, size_t len,
                                                      YeptrisSchema schema,
                                                      YeptrisValueColumns* cols);

YEPTRIS_API void yeptris_value_free_columns(YeptrisValueColumns* cols);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_VALUES_H */
