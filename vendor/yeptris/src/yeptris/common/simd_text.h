/* simd_text.h — SIMD-accelerated text scan primitives (TODO.impl/04).
 *
 * AOT framework (the libleptris pattern): a scalar reference TU is always
 * compiled; AVX2 / NEON TUs are compiled separately with the right -m
 * flags; dispatch picks the best table once via cpu detection. Callers go
 * through yep_text_active() — never call an ISA table directly.
 *
 * Contract (every implementation, every ISA):
 *  - functions read AT MOST len bytes; no faults past the end
 *    (length-guarded chunking, never full-width loads beyond len);
 *  - len == 0 is valid: contains/count/find_not-style predicates return
 *    their empty answer, finds return -1;
 *  - results are bit-identical to the scalar reference — enforced by the
 *    differential suite (test_simd_text.cpp), which compares against naive
 *    test-local references, not against our own scalar TU.
 */
#ifndef YEP_SIMD_TEXT_H
#define YEP_SIMD_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "port.h" /* yep_ctz64: the SWAR walks' first-set-lane extraction */

#ifdef __cplusplus
extern "C" {
#endif

/* One-pass pre-scan statistics: the ten occurrence counts every
 * content-derived sizing rule needs (arena/node pre-reserve, engine
 * anchor pre-reserve) plus the ASCII/printable flags that gate the
 * UTF-8 validator. Fills the role of four separate full-buffer
 * passes (printable_validate's SWAR loop + three count3 calls). */
typedef struct yep_text_stats {
    size_t nl, comma, dash, colon, bracket, brace, dq, sq, pipe, amp;
    int nonascii;      /* any byte >= 0x80 present */
    int bad_printable; /* an ASCII byte violating c-printable (not 9/10/13/0x20..0x7E) */
} yep_text_stats;

/* A byte-class stop set. The 256-bit bitmap is the truth (the scalar
 * fallback reads it); lo/hi are derived nibble-class tables the vector
 * kernels classify lanes with — member #k of group g sets bit k in
 * lo[g][b&15] and hi[g][b>>4], so a lane matches iff some member has
 * both nibbles of the lane byte: exact, no false positives (the
 * SIMD/scalar differential suite pins it). Constant sets embed
 * prebuilt literals (pinned against this init by StopsetLiterals). */
#define YEP_STOPSET_GROUPS 4 /* 32 members — every YAML stop class fits */

typedef struct yep_stopset {
    unsigned char bitmap[32];
    uint8_t groups; /* nibble-class groups in use; >GROUPS = vector kernels
                       decline (scalar bitmap walk stays correct) */
    unsigned char lo[YEP_STOPSET_GROUPS][16];
    unsigned char hi[YEP_STOPSET_GROUPS][16];
} yep_stopset;

static inline void yep_stopset_init(yep_stopset* ss, const unsigned char bitmap[32]) {
    for (size_t i = 0; i < sizeof(*ss); i++) {
        ((unsigned char*)ss)[i] = 0;
    }
    unsigned n = 0;
    for (unsigned b = 0; b < 256; b++) {
        if ((bitmap[b >> 3] >> (b & 7)) & 1) {
            ss->bitmap[b >> 3] |= (unsigned char)(1u << (b & 7));
            if (n < YEP_STOPSET_GROUPS * 8) {
                ss->lo[n >> 3][b & 15] |= (unsigned char)(1u << (n & 7));
                ss->hi[n >> 3][b >> 4] |= (unsigned char)(1u << (n & 7));
            }
            n++;
        }
    }
    ss->groups = (uint8_t)((n + 7) >> 3);
}

/* The line-facts sweep's output (one `key: value` line's positions).
 * end is the first \n/\r at/after pos (or len); indent the first
 * byte != ' ' (spaces only — tabs are the caller's rule, as in
 * yep_scan_line); stop the first block-plain stop byte at/after the
 * indent and strictly before end, stop_set=0 when none exists. */
typedef struct yep_line_facts {
    uint32_t end;
    uint32_t indent;
    uint32_t stop;
    uint32_t stop_set;
} yep_line_facts;

/* The flow-kernel chunk classifier's output (one <=32-byte chunk):
 * bit i of each mask = byte i of the chunk. The five classes — quote,
 * backslash, structural ({,},[,],,:), value-start (0-9,-,t,f,n), and
 * raw C0 — give the token walk every class it dispatches on; valid
 * marks the bytes that exist (partial tail chunk). Bit-identical
 * across ISAs. */
typedef struct yep_chunk_masks {
    uint32_t quote;
    uint32_t bs;
    uint32_t structurals;
    uint32_t valstart; /* 0-9, '-', 't', 'f', 'n' — scalar-token starts */
    uint32_t c0;       /* bytes < 0x20 (raw controls) */
    uint32_t valid;    /* 1-bits for the bytes that exist (tails) */
} yep_chunk_masks;

/* one 64-byte block's classification (json_stage1_masks) */
struct yep_s1_block {
    uint64_t q;      /* raw quote bytes */
    uint64_t bs;     /* backslash bytes */
    uint64_t op;     /* []{}:, */
    uint64_t c0;     /* bytes < 0x20 (ws included) */
    uint64_t tokens; /* the resolver's verdict: ops + string-open quotes */
};

/* The kernel table. One struct = one dispatch point (OCP: a new ISA is a
 * new TU exporting a new table; nothing else changes). */
typedef struct yep_text_kernels {
    /* nonzero iff c occurs in [s, s+len) */
    int (*contains)(const char* s, size_t len, char c);

    /* offset of the first c, or -1 */
    ptrdiff_t (*find)(const char* s, size_t len, char c);

    /* offset of the first c0c1c2 run, or -1; len < 3 -> -1 */
    ptrdiff_t (*find3)(const char* s, size_t len, char c0, char c1, char c2);

    /* occurrences of c */
    size_t (*count_char)(const char* s, size_t len, char c);

    /* three counts in one memory pass (arena sizing pre-scan, 06) */
    void (*count3)(const char* s, size_t len, char c0, char c1, char c2, size_t* n0, size_t* n1,
                   size_t* n2);

    /* memcpy + count3 fused: one traversal (TODO 188 pattern) */
    void (*copy_count3)(char* dst, const char* src, size_t len, char c0, char c1, char c2,
                        size_t* n0, size_t* n1, size_t* n2);

    /* offset of the first byte != c, or -1 (indentation column scan) */
    ptrdiff_t (*find_not)(const char* s, size_t len, char c);

    /* offset of the first byte in the stop set, or -1 (plain-scalar
     * end detection: ": " / " #" / line-break / flow stops) */
    ptrdiff_t (*stopset_find)(const yep_stopset* ss, const char* s, size_t len);

    /* Given the content AFTER an opening quote: offset of the closing
     * quote q honoring backslash escapes, or -1 if unterminated.
     * *has_escape is set to 1 iff a backslash occurred (0 otherwise). */
    ptrdiff_t (*quote_scan)(const char* s, size_t len, char q, int* has_escape);

    /* the fused pre-scan above (results bit-identical across ISAs) */
    void (*scan_stats)(const char* s, size_t len, yep_text_stats* out);

    /* offset of the first byte that is a double quote, a backslash,
     * or a C0 control — the JSON string-stop set (scan/json.c owns
     * the grammar; simd_text owns the finding). One SIMD pass,
     * three compares OR'd; bit-identical across ISAs. */
    ptrdiff_t (*qbc_find)(const char* s, size_t len);

    /* 0 iff EVERY byte is gate-safe (0x20-0x7E, 09, 0A, 0D) — the
     * encoding gate's fast answer; 1 means the authoritative validator
     * must rule (non-ASCII may still be valid UTF-8). Replaces the
     * multi-class stats sweep at parse entry (TODO.restructure/66).
     * Bit-identical across ISAs. */
    int (*gate_scan)(const char* s, size_t len);

    /* The fused line-facts sweep (TODO.restructure/76): one pass
     * yields the four positions a block line needs — first break,
     * first non-space (indent), and the first block-plain stop byte
     * ({\n \r # :}) at or after the indent. scan.c derives every
     * line/shape fact from these; the old paths re-walked the same
     * bytes per fact. Bit-identical across ISAs. */
    void (*line_facts)(const char* s, size_t len, size_t pos, yep_line_facts* out);

    /* The flow-kernel chunk classifier (the JSON tape's stage 1
     * foundation): five byte-class masks for one <=32-byte chunk
     * (quote, backslash, structurals, value-starts, C0). n must be
     * <= 32; reads exactly n bytes. Bit-identical across ISAs. */
    yep_chunk_masks (*json_chunk)(const char* p, size_t n);

    /* The JSON structural indexer (the simdjson stage-1 shape, the
     * token-contract front): walks the whole buffer in 64-byte blocks
     * and writes, in position order, the u32 offsets of every token
     * stage 2 dispatches on — operators ([]{}:,), string OPEN quotes
     * (escape-aware, the borrow-trick scanner), and scalar-run starts
     * (a non-operator non-whitespace byte whose predecessor was not
     * one). String interiors and close quotes never appear; ws bytes
     * are never touched by stage 2. No grammar validation: the only
     * hard error is an unterminated string (odd final parity) ->
     * return 0. idx must hold len+2 entries. Bit-identical across
     * ISAs (the differential suite pins it); scan/json.c owns the
     * grammar contract. flags (never NULL) accumulates YEP_S1_* bits:
     * the doc-level facts stage 2 needs to skip content scanning. */
    int (*json_stage1)(const char* p, size_t len, uint32_t* idx, size_t* nidx, unsigned* flags);

    /* The structural indexer's classification half, without the
     * emission: per 64-byte block, the four u64 masks the resolver
     * consumes (QUOTE/BS/OP/C0; ws is dead since the token contract
     * dropped scalar-run starts). blocks receives len/64+1 records;
     * *nblocks returns how many were written. Same reject/flag
     * contract as json_stage1. The single-pass tape walk consumes
     * this — no idx array, no emission loop. */
    int (*json_stage1_masks)(const char* p, size_t len, struct yep_s1_block* blocks,
                             size_t* nblocks, unsigned* flags);
} yep_text_kernels;

/* json_stage1 flag bits (accumulated across blocks; cleared by caller) */
#define YEP_S1_C0_IN_STRING 1u /* raw non-ws C0 inside a string: reject */
#define YEP_S1_HAS_ESCAPE 2u   /* a backslash byte exists: validate spans */

/* The structural indexer's mask resolver — the SSOT identity set for
 * every json_stage1 implementation (scalar in scan/json.c, the ISA
 * twins in the SIMD TUs). Given one 64-byte block's QUOTE/BS/OP/WS
 * masks (bit k = byte k, valid masking tail bytes) plus the three
 * carried bits (string parity, escape parity, scalar-follows), it
 * derives the token mask and appends positions to idx:
 *
 *   escaped    = the simdjson escape scanner's ODD_BITS borrow trick
 *                (real escapes + odd-run terminals ^ bs|carry)
 *   in_string  = prefix_xor(live quotes) ^ prev parity
 *   string_tail= in_string ^ live quotes  (interiors + closes)
 *   starts     = ~(op|ws) & ~follows      (scalar-run heads; quotes
 *                are scalars, a non-quote scalar extends a run)
 *   tokens     = (op | starts) & ~string_tail
 *
 * Returns the block's contribution; carries update in place. */
static inline uint64_t yep_json_stage1_tokens(uint64_t q, uint64_t bs, uint64_t op, uint64_t c0,
                                              uint64_t valid, uint64_t* prev_in_string,
                                              uint64_t* esc_carry, unsigned* flags) {
    uint64_t potential = bs & ~*esc_carry;
    uint64_t s = (((potential << 1) | 0xAAAAAAAAAAAAAAAAull) - potential) ^ 0xAAAAAAAAAAAAAAAAull;
    uint64_t escaped = s ^ (bs | *esc_carry);
    *esc_carry = (s & bs) >> 63;
    uint64_t quote_live = q & ~escaped;
    uint64_t ps = quote_live; /* sequential prefix-xor — a flat
                               * one-expression xor of shifts misses
                               * the cross terms (the naive caught it) */
    ps ^= ps << 1;
    ps ^= ps << 2;
    ps ^= ps << 4;
    ps ^= ps << 8;
    ps ^= ps << 16;
    ps ^= ps << 32;
    uint64_t in_string = ps ^ *prev_in_string;
    *prev_in_string = (uint64_t)((int64_t)in_string >> 63);
    uint64_t string_tail = (in_string ^ quote_live) & valid;
    if (bs != 0) {
        *flags |= YEP_S1_HAS_ESCAPE;
    }
    if ((c0 & in_string & valid) != 0) {
        *flags |= YEP_S1_C0_IN_STRING;
    }
    return (op | (quote_live & ~string_tail)) & ~string_tail & valid;
}

/* The full resolver: token mask derivation + the idx emission loop
 * (the json_stage1 implementations). */
static inline size_t yep_json_stage1_resolve(uint64_t q, uint64_t bs, uint64_t op, uint64_t ws,
                                             uint64_t c0, uint64_t valid, uint64_t* prev_in_string,
                                             uint64_t* esc_carry, size_t off, uint32_t* idx,
                                             size_t n, unsigned* flags) {
    uint64_t tokens =
        yep_json_stage1_tokens(q, bs, op, c0, valid, prev_in_string, esc_carry, flags);
    while (tokens != 0) {
        idx[n++] = (uint32_t)(off + (size_t)yep_ctz64(tokens));
        tokens &= tokens - 1;
    }
    return n;
}

/* The best table for this CPU (atomic-lazy, like yep_cpu_detect). */
const yep_text_kernels* yep_text_active(void);

/* Scalar reference implementations — callable directly for differential
 * testing and as fallbacks. The active() table never points here on CPUs
 * with a vector ISA we compiled. */
int yep_text_contains_scalar(const char* s, size_t len, char c);
ptrdiff_t yep_text_find_scalar(const char* s, size_t len, char c);
ptrdiff_t yep_text_find3_scalar(const char* s, size_t len, char c0, char c1, char c2);
size_t yep_text_count_char_scalar(const char* s, size_t len, char c);
void yep_text_count3_scalar(const char* s, size_t len, char c0, char c1, char c2, size_t* n0,
                            size_t* n1, size_t* n2);
void yep_text_copy_count3_scalar(char* dst, const char* src, size_t len, char c0, char c1, char c2,
                                 size_t* n0, size_t* n1, size_t* n2);
ptrdiff_t yep_text_qbc_find_scalar(const char* s, size_t len);
ptrdiff_t yep_text_find_not_scalar(const char* s, size_t len, char c);
ptrdiff_t yep_text_stopset_find_scalar(const yep_stopset* ss, const char* s, size_t len);
ptrdiff_t yep_text_quote_scan_scalar(const char* s, size_t len, char q, int* has_escape);
void yep_text_scan_stats_scalar(const char* s, size_t len, yep_text_stats* out);
int yep_text_gate_scan_scalar(const char* s, size_t len);
void yep_text_line_facts_scalar(const char* s, size_t len, size_t pos, yep_line_facts* out);
yep_chunk_masks yep_text_json_chunk_scalar(const char* p, size_t n);
/* The SWAR walk bounded: settles the facts when the line's break sits
 * within cap bytes (or the buffer ends) — 0 means a longer line, the
 * caller sweeps. The ISA kernels use it as BOTH the short-line test
 * and the short-line answer (no probe-then-rescan). */
int yep_text_line_facts_capped(const char* s, size_t len, size_t pos, size_t cap,
                               yep_line_facts* out);

/* SWAR byte flags: 0x80 at every byte equal to k (little-endian
 * lanes — every supported target is LE). The subtract-based zero
 * test is NOT exact: a 0x01 byte under a borrow chain from zero
 * bytes below false-flags ('!' = 0x21 ^ ' ' = 0x01 read as a space —
 * the corpus caught it); this form is carry-free per byte. */
#define YEP_SWAR_FLAGS 0x8080808080808080ull

static inline uint64_t yep_swar_eq8(uint64_t x, uint64_t k) {
    uint64_t v = x ^ k;
    uint64_t low = (v & 0x7F7F7F7F7F7F7F7Full) + 0x7F7F7F7F7F7F7F7Full;
    uint64_t nz = (low | v) & YEP_SWAR_FLAGS;
    return ~nz & YEP_SWAR_FLAGS;
}

/* Stopset bitmap helpers: a 256-bit bitmap is the wire form of a byte
 * class; build with yep_stopset_clear + yep_stopset_add. */
static inline void yep_stopset_clear(unsigned char set[32]) {
    for (int i = 0; i < 32; i++) {
        set[i] = 0;
    }
}

static inline void yep_stopset_add(unsigned char set[32], unsigned char c) {
    set[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static inline int yep_stopset_test(const unsigned char set[32], unsigned char c) {
    return (set[c >> 3] >> (c & 7)) & 1;
}

#ifdef __cplusplus
}
#endif

#endif /* YEP_SIMD_TEXT_H */
