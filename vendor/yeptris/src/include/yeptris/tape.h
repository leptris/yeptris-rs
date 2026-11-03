/* tape.h — the compact JSON tape (issue #238's consumer shape,
 * TODO.restructure/85: the simdjson-class materialization).
 *
 * One fused strict walk appends ONE record per token — no DOM nodes,
 * no links, no views. Records are THREE parallel columns (kind byte,
 * off, len) carved from one block: bools encode in the kind, container
 * links ride the off column, and NUMBERS RECORD SPANS ONLY — parse
 * validates the grammar (the lean shape scan) and materialize converts
 * (yeptris_tape_convert; the C-API materializer converts inline).
 * Strings stay zero-copy spans into the caller's buffer.
 *
 * The tape is a document-level result (never per value across FFI);
 * the buffer the spans borrow must outlive it.
 */
#ifndef YEPTRIS_TAPE_H
#define YEPTRIS_TAPE_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record kinds (the kinds column). */
enum {
    YEP_T_DOC = 0, /* document boundary (records start after it) */
    YEP_T_NULL,
    YEP_T_TRUE,     /* the kind IS the value */
    YEP_T_FALSE,    /* the kind IS the value */
    YEP_T_INT,      /* off/len: signed digit span; convert at materialize */
    YEP_T_FLOAT,    /* off/len: float text span; convert at materialize */
    YEP_T_STR,      /* off/len: raw span (escapes untouched; the
                        qbc kernel found them) */
    YEP_T_SEQ_OPEN, /* off: index of the matching CLOSE */
    YEP_T_MAP_OPEN, /* off: index of the matching CLOSE; keys are
                        the STR records at even positions inside */
    YEP_T_CLOSE,    /* off: index of the matching OPEN */
    YEP_T_NUM,      /* lenient route only: off/len number span with
                        the grammar UNVALIDATED at parse —
                        yeptris_tape_convert is the authority (it
                        classifies int/float and rejects malformed
                        spans; simdjson's deferred contract) */
};

/* The interleaved record (TODO.max-perf/07): one 8-byte store per
 * token instead of three column stores. Layout: off:u32 | len:u23 |
 * kind:u8 — a span longer than 0xFEFFFF bytes carries len == 0xFFFFFF
 * with the true length stored as a CONT-terminated extension record
 * (kind YEP_T_CONT; unreachable for <16 MiB tokens). The columns
 * stay the compatibility ABI: yeptris_tape_columns materializes them
 * lazily from the records on first touch. */
#define YEP_T_CONT 8 /* record-length extension (not a token kind) */
typedef uint64_t yeptris_tape_rec;

typedef struct yeptris_json_tape {
    size_t count;
    uint8_t* kinds;         /* compat columns — materialized lazily (see
                               yeptris_tape_columns) when the interleaved
                               records are the primary storage */
    uint32_t* offs;         /* span starts (STR/INT/FLOAT); container links */
    uint32_t* lens;         /* span lengths (STR/INT/FLOAT) */
    yeptris_tape_rec* recs; /* primary storage (may be NULL on legacy
                               column-built tapes: the strict route) */
    int _cols_ready;        /* columns materialized from recs already */
    int _rec_primary;       /* the lenient route: recs carry the data and
                               the columns are lazy */
    int64_t int_min;        /* set by yeptris_tape_convert when an INT span
                               exceeds int64 (materialize-time discovery) */
    const void* _src;       /* the parsed buffer (spans borrow it; it must
                               outlive the tape — same contract as the spans) */
    size_t _srclen;
    void* _block; /* the carved allocation base */
    /* the >1024 simple-key verdict, settled DURING the lenient walk
     * (one compare per map key) — the post-scan re-walk of the whole
     * tape cost 16% of json-users parse (#739 profile, 2026-10-28) */
    unsigned char _long_key_known;
    unsigned char _long_key;
} yeptris_json_tape;

/* Parses strict JSON into the tape. Accepts scalar roots (a single
 * record). Returns YEPTRIS_OK, YEPTRIS_ERROR_PARSE (details via
 * yeptris_last_error), YEPTRIS_ERROR_MEMORY, YEPTRIS_ERROR_ARG. */
YEPTRIS_API YeptrisStatus yeptris_parse_json_tape(const char* source, size_t len,
                                                  yeptris_json_tape* tape);

/* The lenient route (simdjson's deferred contract, opt-in): every
 * STRUCTURAL check stays at parse (bracket matching, alternation,
 * comma/colon placement, literals, string closes/escapes) while
 * NUMBER grammar defers to yeptris_tape_convert — a number records
 * as a YEP_T_NUM charset run (so "1.2.3" is accepted here and
 * rejected at convert, exactly where simdjson's number parse would
 * fail; a run with non-number bytes like "12ab" still rejects
 * structurally at parse). Accepts a superset of the strict route's
 * inputs (tab whitespace becomes legal, RFC 8259); on everything
 * the strict route accepts, the tapes are structurally identical
 * (spans and containers equal; NUM records carry the same bytes the
 * strict INT/FLOAT records carry). Non-ASCII inputs take the strict
 * route unchanged. */
YEPTRIS_API YeptrisStatus yeptris_parse_json_tape_lenient(const char* source, size_t len,
                                                          yeptris_json_tape* tape);

YEPTRIS_API void yeptris_tape_free(yeptris_json_tape* tape);

/* Converts the INT/FLOAT records in [from, to): ivals/dvals are
 * parallel to the RECORD indices (slots for other kinds are left
 * untouched; either pointer may be NULL). Returns the number of
 * number records converted, or SIZE_MAX if a span does not re-scan
 * as a number (impossible for a tape this library produced).
 * Sets t->int_min when an INT span exceeds int64. */
/* Materializes the kinds/offs/lens columns from the interleaved
 * records (no-op when the tape was built column-primary or the
 * columns are already materialized). Consumers reading the column
 * pointers directly (the FFI binding) must call this once first.
 * Returns 0 on success, nonzero on allocation failure. */
YEPTRIS_API int yeptris_tape_columns(yeptris_json_tape* t);

YEPTRIS_API size_t yeptris_tape_convert(yeptris_json_tape* t, size_t from, size_t to,
                                        int64_t* ivals, double* dvals);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_TAPE_H */
