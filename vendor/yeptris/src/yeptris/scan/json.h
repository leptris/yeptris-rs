/* json.h — the strict-JSON grammar interface (TODO.impl/08C/21).
 *
 * Kernels are exported (YEPTRIS_API) so host materializers (the Ruby
 * native extension's fused JSON→VALUE path) can call them without
 * pulling the whole static archive. */
#ifndef YEP_JSON_H
#define YEP_JSON_H

#include <stddef.h>
#include <yeptris/api.h>

#ifdef __cplusplus
extern "C" {
#endif

YEPTRIS_API int yep_json_ws(const char* p, size_t len, size_t* i, int* saw_tab);

/* Token-stream walker over a JSON-class flow span (TODO.restructure/53):
 * the ONE grammar walk, shared by the engine's validator and the DOM's
 * fused builder — validate and build ride the same state machine. */
#define YEP_JSON_WALK_DEPTH 256

typedef enum {
    YEP_JW_OK = 0,     /* token delivered in *t */
    YEP_JW_DONE = 1,   /* the span's matching close was consumed */
    YEP_JW_REJECT = 2, /* not JSON-class: the general kernel judges */
} yep_jw_status;

typedef struct yep_json_tok {
    size_t at;      /* token start (the quote/opener/digit) */
    size_t end;     /* token end (past the closing quote for strings) */
    char cls;       /* '"' string, '#' number, 'a' literal, '[', '{', ']', '}' */
    int has_escape; /* strings: a backslash escape is present */
    int is_float;   /* numbers (cls '#'): the text has '.' or an exponent */
    /* numbers: the ONE conversion (the walker's number arm runs
     * yep_json_number_scan — validate, advance, convert in a single
     * pass; consumers read the values instead of rescanning). nshape:
     * 0 int (ival exact), 1 float text (dval), 2 integer text beyond
     * int64 (dval approximate — the number-kernel contract). */
    int nshape;
    int64_t ival;
    double dval;
} yep_json_tok;

/* Walker states (the reference machine in scan/json.c and the tape's
 * fused specialization share one set — same grammar, same rejects). */
enum {
    JW_VALUE_OR_CLOSE = 0,
    JW_VALUE,
    JW_KEY_OR_CLOSE,
    JW_KEY,
    JW_COLON,
    JW_COMMA_OR_CLOSE,
};

typedef struct yep_json_walk {
    const char* p;
    size_t len;
    int max_depth; /* the engine's runtime nesting limit */
    size_t i;      /* cursor: one past the opener */
    size_t close;  /* valid once DONE */
    int long_key;  /* a map key over the 1024-char simple-key limit */
    int saw_tab;
    /* strict JSON (RFC 8259): map keys are STRINGS only, including
     * the first entry — the lenient flow class accepts YAML keys
     * (TODO.restructure/81 stage 2: the strict build's walk IS the
     * validator; reject falls to yep_json_document's error path). */
    int strict;
    uint8_t kind[YEP_JSON_WALK_DEPTH];   /* 0 seq, 1 map */
    uint8_t expect[YEP_JSON_WALK_DEPTH]; /* walker states */
    int depth;
} yep_json_walk;

/* Opens over p[open] ('[' or '{'); call _next until DONE/REJECT. */
void yep_json_walk_init(yep_json_walk* w, const char* p, size_t len, size_t open, int max_depth);
yep_jw_status yep_json_walk_next(yep_json_walk* w, yep_json_tok* t);
YEPTRIS_API int yep_json_number(const char* p, size_t len, size_t* i);
/* The grammar walk fused with conversion (TODO.restructure/26): one
 * scan validates and converts. *is_float reports the TEXT shape:
 * 0 = integer (*iv exact, INT64_MIN included), 1 = float text
 * (*dv converted via the number-kernel SSOT), 2 = integer text
 * beyond int64 (*dv approximate; exact hosts rebuild from the span
 * start..*i). Out params may be NULL. */
YEPTRIS_API int yep_json_number_scan(const char* p, size_t len, size_t* i, int* is_float,
                                     int64_t* iv, double* dv);
/* Same grammar, same rejects, same advance — NO conversion: reports
 * the text shape only (0 int / 1 float). The lazy tape's arm. */
YEPTRIS_API int yep_json_number_shape(const char* p, size_t len, size_t* i, int* is_float);
YEPTRIS_API int yep_json_literal(const char* p, size_t len, size_t* i, const char* word);
YEPTRIS_API int yep_json_string(const char* p, size_t len, size_t* i, size_t* close_out,
                                int* has_esc);
/* Stage 1 of the token-contract tape front: the structural indexer
 * (scalar reference; the kernels table's json_stage1 slot carries the
 * ISA twins). Writes the token positions stage 2 dispatches on —
 * operators, string OPEN quotes, scalar-run starts — and returns 0
 * only for an unterminated string. idx holds len+2 entries. flags
 * (never NULL) accumulates the YEP_S1_* doc-level facts. */
YEPTRIS_API int yep_json_stage1_scalar(const char* p, size_t len, uint32_t* idx, size_t* nidx,
                                       unsigned* flags);
/* The classification half (no emission) — the kernels table's
 * json_stage1_masks slot carries the ISA twins. */
struct yep_s1_block; /* defined in scan/json.c (the span blocks) */
YEPTRIS_API int yep_json_stage1_masks_scalar(const char* p, size_t len, struct yep_s1_block* blocks,
                                             size_t* nblocks, unsigned* flags);

/* The per-chunk byte-class classifier behind the flow kernels: the
 * kernels table's json_chunk slot (common/simd_text.h) — yep_chunk_masks
 * is defined there; the differential suite pins the ISAs to it. */

YEPTRIS_API int yep_json_document(const char* p, size_t len, size_t* err);

#ifdef __cplusplus
}
#endif

#endif /* YEP_JSON_H */
