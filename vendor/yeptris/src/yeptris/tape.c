/* tape.c — the compact JSON tape (TODO.restructure/85, v2 records).
 *
 * The strict fused walk drives: one THREE-column record per token
 * (kind byte, off, len) — bools encode in the kind, container links
 * ride the off column, numbers record spans only (the lean shape scan
 * validates; yeptris_tape_convert materializes). Strings stay
 * zero-copy spans. The record budget derives from the input length —
 * every token consumes at least one input byte — so len+2 slots
 * suffice with no pre-pass.
 *
 * Route shape mirrors yeptris_parse_json: a gate-clean container root
 * fuses validation into the walk; everything else validates through
 * yep_json_document first (the error precedence json-suite-strict
 * pins), then builds. The walk's DONE delivers the root close as a
 * token; the root OPEN is synthesized here (walk_init consumes the
 * opener without emitting it). */

#include <stdlib.h>
#include <string.h>

#include <yeptris/tape.h>

#if defined(__GNUC__)
#define YEP_UNUSED_FN __attribute__((unused))
#else
#define YEP_UNUSED_FN
#endif

#ifndef YEP_TOKEN_CONTRACT_ROUTE
#define YEP_TOKEN_CONTRACT_ROUTE 0
#endif

#include "common/error.h"
#include "common/simd_text.h"
#include "encoding/encoding.h"
#include "memory/allocator.h"
#include "parse/scalars.h"
#include "scan/json.h"

typedef struct {
    yeptris_json_tape* t;
    size_t cap;
    uint32_t open[YEP_JSON_WALK_DEPTH]; /* record indices */
    int depth;
    int oom;
} tape_ctx;

static int rec_put(tape_ctx* c, uint8_t kind, uint32_t off, uint32_t len) {
    if (c->t->count >= c->cap) {
        c->oom = 1;
        return 0;
    }
    size_t i = c->t->count++;
    c->t->kinds[i] = kind;
    c->t->offs[i] = off;
    c->t->lens[i] = len;
    return 1;
}

static YeptrisStatus tape_carve(yeptris_json_tape* t, size_t len) {
    size_t cap = len + 2;
    size_t off_o = (cap + 15) & ~(size_t)15;
    char* block = yep_alloc(yep_system_allocator(),
                            off_o + 2 * cap * sizeof(uint32_t) + cap * sizeof(yeptris_tape_rec));
    if (block == NULL) {
        return YEPTRIS_ERROR_MEMORY;
    }
    t->_block = block;
    t->kinds = (uint8_t*)block;
    t->offs = (uint32_t*)(void*)(block + off_o);
    t->lens = t->offs + cap;
    t->recs = (yeptris_tape_rec*)(void*)(t->lens + cap);
    t->count = 0;
    t->int_min = 0;
    t->_rec_primary = 0;
    t->_cols_ready = 0;
    t->_long_key_known = 0;
    t->_long_key = 0;
    return YEPTRIS_OK;
}

/* The lazy column materialization (TODO.max-perf/07): records are the
 * primary storage on the lenient route; the column ABI materializes
 * from them on first touch. */
YEPTRIS_API int yeptris_tape_columns(yeptris_json_tape* t) {
    if (t == NULL || !t->_rec_primary || t->recs == NULL || t->_cols_ready) {
        return 0; /* column-primary (the strict route) or ready */
    }
    for (size_t i = 0; i < t->count; i++) {
        yeptris_tape_rec r = t->recs[i];
        t->lens[i] = (uint32_t)((r >> 8) & 0xFFFFFFu);
        t->offs[i] = (uint32_t)(r >> 32);
        t->kinds[i] = (uint8_t)(r & 0xFFu);
    }
    t->_cols_ready = 1;
    return 0;
}

/* Scalar roots: the walk needs an opener, so a bare root value
 * converts through the same kernels. */
static int tape_put_root_scalar(tape_ctx* c, const char* p, size_t len, size_t at) {
    char ch = p[at];
    if (ch == '"') {
        size_t i = at;
        size_t close = 0;
        int esc = 0;
        if (!yep_json_string(p, len, &i, &close, &esc)) {
            return 0;
        }
        return rec_put(c, YEP_T_STR, (uint32_t)(at + 1), (uint32_t)(close - at - 1));
    }
    if (ch == 't' || ch == 'f' || ch == 'n') {
        const char* word = ch == 't' ? "true" : (ch == 'f' ? "false" : "null");
        size_t i = at;
        if (!yep_json_literal(p, len, &i, word)) {
            return 0;
        }
        uint8_t kind = ch == 'n' ? YEP_T_NULL : (ch == 't' ? YEP_T_TRUE : YEP_T_FALSE);
        return rec_put(c, kind, (uint32_t)at, (uint32_t)(i - at));
    }
    size_t i = 0;
    int flt = 0;
    if (yep_json_number_shape(p + at, len - at, &i, &flt) == 0) {
        return 0;
    }
    return rec_put(c, flt ? YEP_T_FLOAT : YEP_T_INT, (uint32_t)at, (uint32_t)i);
}

/* One strict walk over p[open] -> records. Returns OK, MEMORY
 * (tape zeroed), or PARSE (walk rejected; tape zeroed).
 * check_tail: the fast route has no document validation, so it
 * rejects trailing garbage here.
 *
 * FUSED specialization of yep_json_walk_next: same states, same
 * rejects, byte for byte — but the token never crosses a call
 * boundary or a tok struct. Cursor, record count, and the
 * top-of-stack (kind/expect/open_at) ride in locals; the depth
 * arrays are only touched on push/pop. Columns write directly
 * (capacity is structurally sound: every record consumes at least
 * one input byte, and the carve holds len+2). yep_json_walk stays
 * the reference machine for DOM/push/pull; tape-diff (corpus + the
 * 2M fuzz parity) pins the equivalence. */
/* The punctuation inline (the simdjson tape-builder lesson): after a
 * value or a key, the next token is a comma/colon or the close —
 * consume it inline instead of paying a full loop iteration for a
 * one-byte state flip. Anything else (close, garbage) falls through
 * with the cursor parked for the main loop to judge. Whitespace here
 * is RFC 8259's set — tab included. */
static inline uint8_t tape_inline_punct(const char* p, size_t len, size_t* i, uint8_t want) {
    size_t j = *i;
    while (j < len && (p[j] == ' ' || p[j] == '\t' || p[j] == '\n' || p[j] == '\r')) {
        j++;
    }
    if (j < len && (uint8_t)p[j] == want) {
        *i = j + 1;
        return 1;
    }
    *i = j;
    return 0;
}

static YeptrisStatus tape_walk(const char* p, size_t len, size_t open, yeptris_json_tape* t,
                               int check_tail) {
    if (tape_carve(t, len) != YEPTRIS_OK) {
        return YEPTRIS_ERROR_MEMORY;
    }
    uint8_t* kinds = t->kinds;
    uint32_t* offs = t->offs;
    uint32_t* lens = t->lens;
    size_t cap = len + 2;
    uint32_t open_at[YEP_JSON_WALK_DEPTH];
    uint8_t kind[YEP_JSON_WALK_DEPTH];

    size_t count = 0;
    kinds[0] = YEP_T_DOC;
    offs[0] = 0;
    lens[0] = 0;
    count = 1;

    uint8_t top_kind = p[open] == '[' ? 0 : 1;
    kinds[1] = top_kind ? YEP_T_MAP_OPEN : YEP_T_SEQ_OPEN;
    offs[1] = 0; /* the CLOSE arm writes the link */
    lens[1] = 0;
    uint32_t top_open = 1;
    count = 2;
    uint8_t top_expect = top_kind ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
    int depth = 1;
    open_at[0] = top_open;
    kind[0] = top_kind;

    size_t i = open + 1;
    for (;;) {
        while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) {
            i++;
        }
        if (i >= len) {
            goto reject;
        }
        size_t at = i;
        char c = p[i];

        /* walker order: punctuation states first so a value arm cannot
         * fire in JW_COMMA_OR_CLOSE / JW_COLON ("[1 2]" is a reject).
         * Close is legal in COMMA_OR_CLOSE, so it falls through. */
        if (top_expect == JW_COLON) {
            if (c != ':') {
                goto reject;
            }
            top_expect = JW_VALUE;
            i = at + 1;
            continue;
        }
        if (top_expect == JW_COMMA_OR_CLOSE) {
            if (c == ',') {
                top_expect = top_kind ? JW_KEY : JW_VALUE;
                i = at + 1;
                continue;
            }
            if (c != ']' && c != '}') {
                goto reject;
            }
            /* close falls through */
        }

        int key_slot = top_kind == 1 && (top_expect == JW_KEY_OR_CLOSE || top_expect == JW_KEY);

        if ((unsigned)(c - '0') <= 9u || c == '-') {
            if (key_slot) {
                goto reject;
            }
            int flt = 0;
            if (!yep_json_number_shape(p, len, &i, &flt)) {
                goto reject;
            }
            kinds[count] = flt ? YEP_T_FLOAT : YEP_T_INT;
            offs[count] = (uint32_t)at;
            lens[count] = (uint32_t)(i - at);
            count++;
            if (tape_inline_punct(p, len, &i, ',')) {
                top_expect = top_kind ? JW_KEY : JW_VALUE;
                key_slot = top_kind ? 1 : 0;
            } else {
                top_expect = JW_COMMA_OR_CLOSE;
            }
            continue;
        }
        if (c == '"') {
            size_t close = 0;
            int esc = 0;
            if (!yep_json_string(p, len, &i, &close, &esc)) {
                goto reject;
            }
            kinds[count] = YEP_T_STR;
            offs[count] = (uint32_t)(at + 1);
            lens[count] = (uint32_t)(close - at - 1);
            count++;
            if (key_slot) { /* the key's colon, inline */
                if (tape_inline_punct(p, len, &i, ':')) {
                    top_expect = JW_VALUE;
                    key_slot = 0;
                } else {
                    top_expect = JW_COLON;
                }
            } else if (tape_inline_punct(p, len, &i, ',')) {
                top_expect = top_kind ? JW_KEY : JW_VALUE;
                key_slot = top_kind ? 1 : 0;
            } else {
                top_expect = JW_COMMA_OR_CLOSE;
            }
            continue;
        }
        if (c == ']' || c == '}') {
            int want = c == ']' ? 0 : 1;
            if (top_kind != want ||
                (top_expect != JW_VALUE_OR_CLOSE && top_expect != JW_KEY_OR_CLOSE &&
                 top_expect != JW_COMMA_OR_CLOSE)) {
                goto reject;
            }
            kinds[count] = YEP_T_CLOSE;
            offs[count] = top_open; /* link to the OPEN */
            lens[count] = 0;
            offs[top_open] = (uint32_t)count; /* the OPEN's link back */
            count++;
            depth--;
            i = at + 1;
            if (depth == 0) {
                break;
            }
            top_kind = kind[depth - 1];
            top_expect = JW_COMMA_OR_CLOSE;
            top_open = open_at[depth - 1];
            key_slot = 0; /* never read before the comma resets it —
                             set anyway so the flag has one owner */
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth >= YEP_JSON_WALK_DEPTH) {
                goto reject;
            }
            if (key_slot) {
                goto reject;
            }
            /* spill current top, then push */
            kind[depth - 1] = top_kind;
            open_at[depth - 1] = top_open;
            top_kind = c == '[' ? 0 : 1;
            kinds[count] = c == '[' ? YEP_T_SEQ_OPEN : YEP_T_MAP_OPEN;
            offs[count] = 0; /* the matching CLOSE writes the link */
            lens[count] = 0;
            top_open = (uint32_t)count;
            count++;
            top_expect = top_kind ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
            kind[depth] = top_kind;
            open_at[depth] = top_open;
            depth++;
            i = at + 1;
            continue;
        }
        if (c == 't' || c == 'f' || c == 'n') {
            /* packed literal compare: bounds-checked span first, then
             * one u32 load (false adds its 5th byte) */
            size_t wl = c == 'f' ? 5 : 4;
            if (at + wl > len) {
                goto reject;
            }
            uint32_t got4;
            memcpy(&got4, p + at, 4);
            if (got4 != (c == 't'   ? 0x65757274u /* "true" */
                         : c == 'n' ? 0x6C6C756Eu /* "null" */
                                    : 0x736C6166u /* "fals" */) ||
                (c == 'f' && p[at + 4] != 'e')) {
                goto reject;
            }
            if (at + wl < len) {
                char z = p[at + wl];
                if (z != ' ' && z != '\t' && z != '\n' && z != '\r' && z != ',' && z != ']' &&
                    z != '}' && z != ':') {
                    goto reject;
                }
            }
            kinds[count] = c == 'n' ? YEP_T_NULL : (c == 't' ? YEP_T_TRUE : YEP_T_FALSE);
            offs[count] = (uint32_t)at;
            lens[count] = (uint32_t)wl;
            count++;
            i = at + wl;
            if (tape_inline_punct(p, len, &i, ',')) {
                top_expect = top_kind ? JW_KEY : JW_VALUE;
                key_slot = top_kind ? 1 : 0;
            } else {
                top_expect = JW_COMMA_OR_CLOSE;
            }
            continue;
        }
        goto reject;
    }
    if (check_tail) {
        size_t tail = i;
        while (tail < len &&
               (p[tail] == ' ' || p[tail] == '\t' || p[tail] == '\n' || p[tail] == '\r')) {
            tail++;
        }
        if (tail != len) {
            goto reject;
        }
    }
    if (count > cap) {
        goto mem;
    }
    t->count = count;
    t->_src = p;
    t->_srclen = len;
    return YEPTRIS_OK;

mem:
    yeptris_tape_free(t);
    return YEPTRIS_ERROR_MEMORY;
reject:
    yeptris_tape_free(t);
    return YEPTRIS_ERROR_PARSE;
}

/* The token-contract walk: consumes stage 1's positions — whitespace
 * is never scanned (a one-byte-typical gap check proves the residue
 * since the last token is whitespace), string interiors never appear
 * (yep_json_string stays the authority on closes, escapes, and raw
 * C0), numbers are span records via the shape scan. Same states and
 * rejects as the fused walk; tape-diff pins the equivalence. */
static YEP_UNUSED_FN YeptrisStatus tape_walk_idx(const char* p, size_t len, size_t open,
                                                 const uint32_t* idx, size_t nidx,
                                                 yeptris_json_tape* t) {
    if (tape_carve(t, len) != YEPTRIS_OK) {
        return YEPTRIS_ERROR_MEMORY;
    }
    uint8_t* kinds = t->kinds;
    uint32_t* offs = t->offs;
    uint32_t* lens = t->lens;
    uint32_t open_at[YEP_JSON_WALK_DEPTH];
    uint8_t kind[YEP_JSON_WALK_DEPTH];

    size_t count = 0;
    kinds[0] = YEP_T_DOC;
    offs[0] = 0;
    lens[0] = 0;
    count = 1;

    uint8_t top_kind = p[open] == '[' ? 0 : 1;
    kinds[1] = top_kind ? YEP_T_MAP_OPEN : YEP_T_SEQ_OPEN;
    offs[1] = 0;
    lens[1] = 0;
    uint32_t top_open = 1;
    count = 2;
    uint8_t top_expect = top_kind ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
    /* key_slot as a maintained flag: transitions update it, the loop
     * never recomputes (two compares per token saved) */
    int key_slot = top_kind ? 1 : 0;
    int depth = 1;
    open_at[0] = top_open;
    kind[0] = top_kind;

    size_t si = 0;
    while (si < nidx && idx[si] <= open) {
        si++; /* the root opener (and anything before it) is consumed */
    }
    size_t prev_end = open + 1;
    for (;;) {
        if (si >= nidx) {
            goto reject; /* EOF mid-structure */
        }
        size_t at = idx[si++];
        if (at >= len) {
            goto reject;
        }
        char c = p[at];
        /* no gap check: every byte class that could hide between
         * tokens is itself a token (dropped scalars are scalar-run
         * starts, raw C0 is a scalar — the state machine rejects
         * them); legal ws is simply invisible. Tabs are ws-class and
         * fall to the document path, exactly like the fused walk. */
        prev_end = at + 1;

        if (top_expect == JW_COLON) {
            if (c != ':') {
                goto reject;
            }
            top_expect = JW_VALUE;
            key_slot = 0; /* a value follows the colon */
            continue;
        }
        if (top_expect == JW_COMMA_OR_CLOSE) {
            if (c == ',') {
                top_expect = top_kind ? JW_KEY : JW_VALUE;
                key_slot = top_kind ? 1 : 0;
                continue;
            }
            if (c != ']' && c != '}') {
                goto reject;
            }
        } else if (key_slot) {
            /* KEY position: a string or the container's close — the
             * value arms are unreachable, the chain short-circuits */
            if (c != '"' && c != ']' && c != '}') {
                goto reject;
            }
        }

        if ((unsigned)(c - '0') <= 9u || c == '-') {
            size_t i = at;
            int flt = 0;
            if (!yep_json_number_shape(p, len, &i, &flt)) {
                goto reject;
            }
            kinds[count] = flt ? YEP_T_FLOAT : YEP_T_INT;
            offs[count] = (uint32_t)at;
            lens[count] = (uint32_t)(i - at);
            count++;
            prev_end = i;
            top_expect = JW_COMMA_OR_CLOSE;
            continue;
        }
        if (c == '"') {
            size_t i = at;
            size_t close = 0;
            int esc = 0;
            if (!yep_json_string(p, len, &i, &close, &esc)) {
                goto reject;
            }
            kinds[count] = YEP_T_STR;
            offs[count] = (uint32_t)(at + 1);
            lens[count] = (uint32_t)(close - at - 1);
            count++;
            prev_end = i;
            top_expect = key_slot ? JW_COLON : JW_COMMA_OR_CLOSE;
            /* key_slot stays 1 through COLON — the value arm clears it */
            continue;
        }
        if (c == ']' || c == '}') {
            int want = c == ']' ? 0 : 1;
            if (top_kind != want ||
                (top_expect != JW_VALUE_OR_CLOSE && top_expect != JW_KEY_OR_CLOSE &&
                 top_expect != JW_COMMA_OR_CLOSE)) {
                goto reject;
            }
            kinds[count] = YEP_T_CLOSE;
            offs[count] = top_open;
            lens[count] = 0;
            offs[top_open] = (uint32_t)count;
            count++;
            depth--;
            if (depth == 0) {
                break;
            }
            top_kind = kind[depth - 1];
            top_expect = JW_COMMA_OR_CLOSE;
            top_open = open_at[depth - 1];
            key_slot = 0; /* never read before the comma resets it —
                             set anyway so the flag has one owner */
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth >= YEP_JSON_WALK_DEPTH) {
                goto reject;
            }
            kind[depth - 1] = top_kind;
            open_at[depth - 1] = top_open;
            top_kind = c == '[' ? 0 : 1;
            kinds[count] = c == '[' ? YEP_T_SEQ_OPEN : YEP_T_MAP_OPEN;
            offs[count] = 0;
            lens[count] = 0;
            top_open = (uint32_t)count;
            count++;
            top_expect = top_kind ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
            key_slot = top_kind ? 1 : 0;
            kind[depth] = top_kind;
            open_at[depth] = top_open;
            depth++;
            continue;
        }
        if (c == 't' || c == 'f' || c == 'n') {
            size_t wl = c == 'f' ? 5 : 4;
            if (at + wl > len) {
                goto reject;
            }
            uint32_t got4;
            memcpy(&got4, p + at, 4);
            if (got4 != (c == 't'   ? 0x65757274u /* "true" */
                         : c == 'n' ? 0x6C6C756Eu /* "null" */
                                    : 0x736C6166u /* "fals" */) ||
                (c == 'f' && p[at + 4] != 'e')) {
                goto reject;
            }
            if (at + wl < len) {
                char z = p[at + wl];
                if (z != ' ' && z != '\t' && z != '\n' && z != '\r' && z != ',' && z != ']' &&
                    z != '}' && z != ':') {
                    goto reject;
                }
            }
            kinds[count] = c == 'n' ? YEP_T_NULL : (c == 't' ? YEP_T_TRUE : YEP_T_FALSE);
            offs[count] = (uint32_t)at;
            lens[count] = (uint32_t)wl;
            count++;
            prev_end = at + wl;
            top_expect = JW_COMMA_OR_CLOSE;
            key_slot = 0;
            continue;
        }
        goto reject;
    }
    /* trailing whitespace only */
    while (prev_end < len && (p[prev_end] == ' ' || p[prev_end] == '\t' || p[prev_end] == '\n' ||
                              p[prev_end] == '\r')) {
        prev_end++;
    }
    if (prev_end != len) {
        goto reject;
    }
    if (count > len + 2) {
        goto mem;
    }
    t->count = count;
    t->_src = p;
    t->_srclen = len;
    return YEPTRIS_OK;

mem:
    yeptris_tape_free(t);
    return YEPTRIS_ERROR_MEMORY;
reject:
    yeptris_tape_free(t);
    return YEPTRIS_ERROR_PARSE;
}

/* The LENIENT token walk (the simdjson deferred contract): stage-1
 * positions consumed with the grammar arms removed. Structural
 * alternation mirrors tape_walk_idx state for state — bracket
 * matching, key/value placement, literals (packed compares), and
 * string closes/escapes (yep_json_string) all stay parse-time
 * checks; NUMBER grammar does not — the arm records the scalar-run
 * span (start to the next token's position, trailing ws trimmed)
 * as YEP_T_NUM and yeptris_tape_convert owns validation and the
 * int/float split. A malformed number ("12ab") therefore rejects at
 * drain, not parse. */
/* The fused LENIENT walk: the strict walk's byte loop with the number
 * grammar deferred — the arm records the run to its natural delimiter
 * (the classify scan carries no accept/reject structure) and
 * yeptris_tape_convert owns validation. Everything else matches
 * tape_walk state for state. */
/* LNUM_RUN — the initial digit run of a number span: SWAR words, eight
 * bytes per pass, with the byte loop below as tail and classifier.
 * digit iff 0x30-0x39: a lane flags when (c+0x46) carries into bit7
 * (c > '9'), when ((c|0x80)-0xB0) goes negative (c < '0'), or when the
 * byte is non-ASCII — and since the all-digit prefix never borrows
 * across lanes, the flags are exact up to the first non-digit, where
 * the scan stops and the byte loop classifies what follows (the
 * consumed-only digits_only law is untouched: the run sets saw_digit,
 * nothing else). */
#define LNUM_RUN                                                                                   \
    do {                                                                                           \
        size_t ks_ = k;                                                                            \
        while (k + 8 <= len) {                                                                     \
            uint64_t w_;                                                                           \
            memcpy(&w_, p + k, 8);                                                                 \
            uint64_t nd_ =                                                                         \
                ((w_ + 0x4646464646464646ull) & 0x8080808080808080ull) |                           \
                (((w_ | 0x8080808080808080ull) - 0xB0B0B0B0B0B0B0B0ull) & 0x8080808080808080ull) | \
                (w_ & 0x8080808080808080ull);                                                      \
            if (nd_ != 0) {                                                                        \
                k += (size_t)yep_ctz64(nd_) >> 3;                                                  \
                break;                                                                             \
            }                                                                                      \
            k += 8;                                                                                \
        }                                                                                          \
        if (k > ks_) {                                                                             \
            saw_digit = 1;                                                                         \
        }                                                                                          \
    } while (0)

/* The strict RFC 8259 number check over one number-ish run — the
 * in-walk float/exp validator (the out-of-line number_shape call +
 * rescan was 13% of json-doc; the digits-only case never gets here).
 * One pass, no state machine words: sign, digits (leading-zero rule),
 * optional .digits, optional e[+-]digits, consume exactly len.
 * always_inline: the spans are 3-8 bytes and the out-of-line call was
 * the cost (the rescan itself is nothing). */
#if defined(__GNUC__)
#define TAPE_NUM_OK_INLINE __attribute__((always_inline)) inline
#else
#define TAPE_NUM_OK_INLINE
#endif
static TAPE_NUM_OK_INLINE int tape_num_ok(const char* p, size_t len) {
    size_t k = 0;
    if (k < len && p[k] == '-') {
        k++;
    }
    size_t int_start = k;
    while (k < len && (unsigned)(unsigned char)p[k] - '0' <= 9u) {
        k++;
    }
    if (k == int_start) {
        return 0; /* no integer part */
    }
    if (k - int_start > 1 && p[int_start] == '0') {
        return 0; /* leading zero */
    }
    if (k < len && p[k] == '.') {
        k++;
        size_t frac = k;
        while (k < len && (unsigned)(unsigned char)p[k] - '0' <= 9u) {
            k++;
        }
        if (k == frac) {
            return 0;
        }
    }
    if (k < len && (p[k] == 'e' || p[k] == 'E')) {
        k++;
        if (k < len && (p[k] == '+' || p[k] == '-')) {
            k++;
        }
        size_t exp = k;
        while (k < len && (unsigned)(unsigned char)p[k] - '0' <= 9u) {
            k++;
        }
        if (k == exp) {
            return 0;
        }
    }
    return k == len;
}

/* The >1024 simple-key law for the LENIENT walk's tape — the same
 * verdict yt_flow_scan reads for the record walk: a STR at a map's
 * key slot with a raw span past the limit (the law counts the RAW
 * token, so a quoted key's two quotes add back). The DOM route must
 * reject it BEFORE wrapping (strict JSON itself has no limit — the
 * tape API keeps accepting; only the YAML serialization cannot carry
 * a long simple key, so the fast parse defers to the engine's clean
 * error — the nightly's run 38143486175). */
int yep_tape_long_simple_key(const yeptris_json_tape* s) {
    if (s->_long_key_known) {
        return s->_long_key; /* settled during the lenient walk */
    }
    struct {
        uint8_t map;
        uint8_t key_next;
    } st[257];
    int d = 0;
    st[0].map = 0;
    st[0].key_next = 0;
    for (uint32_t i = 1; i < s->count; i++) { /* recs[0] is the DOC */
        uint64_t r = s->recs[i];
        uint8_t kind = (uint8_t)(r & 0xFFu);
        if (kind == YEP_T_MAP_OPEN || kind == YEP_T_SEQ_OPEN) {
            if (d >= 256) {
                return 1; /* deeper than the scratch stack: over by
                           * definition, the engine's depth law decides */
            }
            /* the container consumes the parent frame's pending slot;
             * after it closes the OTHER slot is next (yt_flow_scan's
             * round-108 lesson: a pinned key_next hid every key that
             * followed a container value) */
            if (d > 0 && st[d].map) {
                st[d].key_next = (uint8_t)!st[d].key_next;
            }
            d++;
            st[d].map = (uint8_t)(kind == YEP_T_MAP_OPEN);
            st[d].key_next = 0;
            continue;
        }
        if (kind == YEP_T_CLOSE) {
            if (d > 0) {
                d--;
            }
            continue;
        }
        if (st[d].map && st[d].key_next == 0) {
            uint32_t mlen = (uint32_t)(r >> 8) & 0xFFFFFFu;
            if (kind == YEP_T_STR && mlen + 2u > 1024u &&
                yep_simple_key_chars((const char*)s->_src + (size_t)((uint32_t)(r >> 32)), mlen) +
                        2 >
                    1024) {
                /* the budget counts characters, not bytes (the round-108
                 * shrink: a 2-byte code point at byte delta 1025 is
                 * legal); _src rides the walk that produced the tape */
                return 1;
            }
            st[d].key_next = 1;
        } else if (st[d].map) {
            st[d].key_next = 0;
        }
    }
    return 0;
}

YeptrisStatus yep_tape_walk_lenient_fused(const char* p, size_t len, size_t open,
                                          yeptris_json_tape* t, int strict_nums, int clean_only,
                                          int reuse_block) {
    /* reuse_block: the caller owns the struct and zeroed it (the flow
     * recorder's scratch) — a prior walk's block may be reused through
     * the _srclen watermark. Consumer structs carry untrustworthy
     * fields and always carve fresh. */
    if (reuse_block && t->_block != NULL && t->recs != NULL && t->_rec_primary &&
        t->_srclen >= len + 2) {
        t->_cols_ready = 0;
        t->_rec_primary = 1;
        t->_long_key_known = 0;
        t->_long_key = 0;
    } else if (tape_carve(t, len) != YEPTRIS_OK) {
        return YEPTRIS_ERROR_MEMORY;
    }
    t->_rec_primary = 1;
    /* the interleaved records are the primary storage (item 07); the
     * columns materialize lazily via yeptris_tape_columns */
    t->_rec_primary = 1;
    yeptris_tape_rec* recs = t->recs;
    uint32_t open_at[YEP_JSON_WALK_DEPTH];
    uint8_t kind[YEP_JSON_WALK_DEPTH];

    recs[0] = ((uint64_t)0 << 32) | ((uint64_t)0 << 8) | YEP_T_DOC;

    uint8_t top_kind = p[open] == '[' ? 0 : 1;
    recs[1] =
        ((uint64_t)0 << 32) | ((uint64_t)0 << 8) | (top_kind ? YEP_T_MAP_OPEN : YEP_T_SEQ_OPEN);
    size_t count = 2;
    int depth = 1;
    open_at[0] = 1;
    kind[0] = top_kind;

    size_t i = open + 1;
    /* clean_only (parse_json's route): tabs and non-ASCII bytes REJECT
     * — the caller falls to the strict sequence, which owns the pinned
     * error precedences those inputs carry (the tab reject, the UTF-8
     * gate). This replaces the entry's full-input gate_scan +
     * tab-memchr pre-passes: same routing by construction, zero scans
     * before the walk. */
    int cl = clean_only;
    if (top_kind) {
        goto lmap1;
    }
    goto lseq1;

    /* strict_nums: the parse_json route's contract is RFC 8259 AT
     * parse time, so its number arms validate the run in-place (the
     * settle pass disappears — the span is cache-warm here); the
     * standalone lenient entry keeps its deferred contract (0: the
     * run records unvalidated, yeptris_tape_convert owns it). */

    /* The specialized member loops (the #342 dispatch-chain cut): once
     * inside a container the grammar is a 2-state cycle — member or
     * separator — so the general chain's expect/key_slot machine and
     * its per-token re-dispatch are replaced by direct loops per kind.
     * Acceptance is IDENTICAL to the chain (the pinning suites hold:
     * Tape suite, LenientMatchesStrict, ErrorParity, the json corpora).
     *
     * lmap / lseq: positioned at a member (or the container's closer).
     * lvalue: scans one value (string/number/literal/container).
     * lafter: positioned after a complete value — ',' or the closer.
     * A close pops the frame; depth 0 ends the walk. */

#define LWS()                                                                                      \
    do {                                                                                           \
        while (i < len &&                                                                          \
               (p[i] == ' ' || p[i] == '\n' || p[i] == '\r' || (p[i] == '\t' && !cl))) {           \
            i++;                                                                                   \
        }                                                                                          \
        if (i >= len) {                                                                            \
            goto lreject;                                                                          \
        }                                                                                          \
    } while (0)

#define LPUSH(k)                                                                                   \
    do {                                                                                           \
        if (depth >= YEP_JSON_WALK_DEPTH) {                                                        \
            goto lreject;                                                                          \
        }                                                                                          \
        recs[count] = ((uint64_t)0 << 32) | ((uint64_t)0 << 8) |                                   \
                      (uint64_t)((k) ? YEP_T_MAP_OPEN : YEP_T_SEQ_OPEN);                           \
        kind[depth] = (k);                                                                         \
        open_at[depth] = (uint32_t)count;                                                          \
        count++;                                                                                   \
        depth++;                                                                                   \
        i++;                                                                                       \
    } while (0)

#define LCLOSE()                                                                                   \
    do {                                                                                           \
        uint32_t lo_ = open_at[depth - 1];                                                         \
        recs[count] = ((uint64_t)lo_ << 32) | ((uint64_t)0 << 8) | YEP_T_CLOSE;                    \
        recs[lo_] =                                                                                \
            (recs[lo_] & ~(uint64_t)0xFFFFFFFF00000000u) | ((uint64_t)(uint32_t)count << 32);      \
        count++;                                                                                   \
        i++;                                                                                       \
        depth--;                                                                                   \
        if (depth == 0) {                                                                          \
            goto ldone;                                                                            \
        }                                                                                          \
        if (kind[depth - 1]) {                                                                     \
            goto lmapafter;                                                                        \
        }                                                                                          \
        goto lseqafter;                                                                            \
    } while (0)

/* LSTR_SCAN(after) — the fused string arm, positioned at p[i] == '"'.
 * One SWAR round settles the common short span; quote_scan (the SIMD
 * close scan) settles longer no-escape spans, with a c0 sweep over the
 * content (quote_scan never tests control bytes); the kernel runs only
 * when escapes appear — its grammar authority covers the escape rules
 * quote_scan skips. `after` names the post-member label. */
#define LSTR_SCAN(after)                                                                           \
    do {                                                                                           \
        size_t j_ = i + 1;                                                                         \
        int esc_ = 0;                                                                              \
        if (j_ + 8 <= len) {                                                                       \
            uint64_t w_;                                                                           \
            memcpy(&w_, p + j_, 8);                                                                \
            uint64_t qm_ = (w_ ^ 0x2222222222222222ull);                                           \
            uint64_t bm_ = (w_ ^ 0x5C5C5C5C5C5C5C5Cull);                                           \
            qm_ = (qm_ - 0x0101010101010101ull) & ~qm_ & 0x8080808080808080ull;                    \
            bm_ = (bm_ - 0x0101010101010101ull) & ~bm_ & 0x8080808080808080ull;                    \
            uint64_t c0_ = (w_ - 0x2020202020202020ull) & ~w_ & 0x8080808080808080ull;             \
            uint64_t hi_ = cl ? (w_ & 0x8080808080808080ull) : 0;                                  \
            if (qm_ != 0 && ((bm_ | c0_ | hi_) & (qm_ - 1)) == 0) {                                \
                size_t cl_ = j_ + (size_t)yep_ctz64(qm_) / 8;                                      \
                recs[count] = ((uint64_t)((uint32_t)j_) << 32) |                                   \
                              ((uint64_t)((uint32_t)(cl_ - j_)) << 8) | (uint64_t)(YEP_T_STR);     \
                count++;                                                                           \
                i = cl_ + 1;                                                                       \
                goto after;                                                                        \
            }                                                                                      \
            /* rounds 2-4 (up to 32 bytes inline): the email-class strings                         \
             * close here without the quote_scan call and its redundant                            \
             * content re-sweep; reached only when round 1 had no close,                           \
             * so the short-string case pays nothing */                                            \
            if ((qm_ | bm_ | c0_ | hi_) == 0) {                                                    \
                size_t nr_ = (len - j_) >> 3;                                                      \
                if (nr_ > 4) {                                                                     \
                    nr_ = 4;                                                                       \
                }                                                                                  \
                for (size_t r_ = 1; r_ < nr_; r_++) {                                              \
                    size_t b2_ = j_ + (r_ << 3);                                                   \
                    memcpy(&w_, p + b2_, 8);                                                       \
                    qm_ = (w_ ^ 0x2222222222222222ull);                                            \
                    bm_ = (w_ ^ 0x5C5C5C5C5C5C5C5Cull);                                            \
                    qm_ = (qm_ - 0x0101010101010101ull) & ~qm_ & 0x8080808080808080ull;            \
                    bm_ = (bm_ - 0x0101010101010101ull) & ~bm_ & 0x8080808080808080ull;            \
                    c0_ = (w_ - 0x2020202020202020ull) & ~w_ & 0x8080808080808080ull;              \
                    hi_ = cl ? (w_ & 0x8080808080808080ull) : 0;                                   \
                    if (qm_ != 0 && ((bm_ | c0_ | hi_) & (qm_ - 1)) == 0) {                        \
                        size_t cl_ = b2_ + (size_t)yep_ctz64(qm_) / 8;                             \
                        recs[count] = ((uint64_t)((uint32_t)j_) << 32) |                           \
                                      ((uint64_t)((uint32_t)(cl_ - j_)) << 8) |                    \
                                      (uint64_t)(YEP_T_STR);                                       \
                        count++;                                                                   \
                        i = cl_ + 1;                                                               \
                        goto after;                                                                \
                    }                                                                              \
                    if ((qm_ | bm_ | c0_ | hi_) != 0) {                                            \
                        break; /* escape, c0 or hi: kernel authority */                            \
                    }                                                                              \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        {                                                                                          \
            const yep_text_kernels* kk_ = yep_text_active();                                       \
            ptrdiff_t r_ = kk_->quote_scan(p + j_, len - j_, '"', &esc_);                          \
            if (r_ >= 0 && esc_ == 0) {                                                            \
                size_t cl_ = j_ + (size_t)r_;                                                      \
                for (size_t b_ = j_; b_ < cl_;) {                                                  \
                    size_t room_ = cl_ - b_;                                                       \
                    if (room_ >= 8) {                                                              \
                        uint64_t v_;                                                               \
                        memcpy(&v_, p + b_, 8);                                                    \
                        uint64_t bad_ =                                                            \
                            (v_ - 0x2020202020202020ull) & ~v_ & 0x8080808080808080ull;            \
                        if (cl) {                                                                  \
                            bad_ |= v_ & 0x8080808080808080ull;                                    \
                        }                                                                          \
                        if (bad_) {                                                                \
                            goto lreject;                                                          \
                        }                                                                          \
                        b_ += 8;                                                                   \
                    } else {                                                                       \
                        if ((unsigned char)p[b_] < 0x20 || (cl && (unsigned char)p[b_] >= 0x80)) { \
                            goto lreject;                                                          \
                        }                                                                          \
                        b_++;                                                                      \
                    }                                                                              \
                }                                                                                  \
                recs[count] = ((uint64_t)((uint32_t)j_) << 32) |                                   \
                              ((uint64_t)((uint32_t)(cl_ - j_)) << 8) | (uint64_t)(YEP_T_STR);     \
                count++;                                                                           \
                i = cl_ + 1;                                                                       \
                goto after;                                                                        \
            }                                                                                      \
        }                                                                                          \
        {                                                                                          \
            size_t at_ = i;                                                                        \
            size_t cl_ = 0;                                                                        \
            if (!yep_json_string(p, len, &i, &cl_, &esc_)) {                                       \
                goto lreject;                                                                      \
            }                                                                                      \
            if (cl) { /* escaped strings skip the sweeps above: the                                \
                       * clean route still owes the strict sequence's                              \
                       * byte guarantees over the content */                                       \
                for (size_t b_ = at_ + 1; b_ < cl_; b_++) {                                        \
                    unsigned char uc_ = (unsigned char)p[b_];                                      \
                    if (uc_ < 0x20 || uc_ >= 0x80) {                                               \
                        goto lreject;                                                              \
                    }                                                                              \
                }                                                                                  \
            }                                                                                      \
            recs[count] = ((uint64_t)((uint32_t)(at_ + 1)) << 32) |                                \
                          ((uint64_t)((uint32_t)(cl_ - at_ - 1)) << 8) | (uint64_t)(YEP_T_STR);    \
            count++;                                                                               \
            i = cl_ + 1;                                                                           \
        }                                                                                          \
        goto after;                                                                                \
    } while (0)

/* The map member cycle — ONE contiguous region (the json-doc lesson:
 * a goto web across value/key/after regions scattered the hot path and
 * cost 2.7x on the flat-map shape). Values scan inline; only nested
 * containers, literals, and the kernel string fallback leave the
 * cycle. Acceptance is the chain's: no trailing commas, string keys
 * only, `:` required, closers only in first-member position. */
lmap1: /* {} closes here */
    LWS();
    if (p[i] == '}') {
        LCLOSE();
    }
    if (p[i] != '"') {
        goto lreject;
    }
    goto lmapkey;

lmap: /* a member MUST follow (after ','): JW_KEY — a closer rejects */
    /* compact fast path: the byte is the key's quote already — LWS's
     * loop would examine it once and exit */
    if (i < len && p[i] == '"') {
        goto lmapkey;
    }
    LWS();
    if (p[i] != '"') {
        goto lreject;
    }
lmapkey: { LSTR_SCAN(lmapcolon); }
lmapcolon: { /* the key verdict rides the walk: the just-emitted record IS the
              * key — a >1024-byte candidate pays the char-budget check once
              * instead of a post-parse full-tape re-walk (#739) */
    uint64_t kr_ = recs[count - 1];
    uint32_t kl_ = (uint32_t)(kr_ >> 8) & 0xFFFFFFu;
    if (kl_ + 2u > 1024u && !t->_long_key_known) {
        t->_long_key_known = 1;
        t->_long_key =
            (unsigned char)(yep_simple_key_chars(p + (size_t)(uint32_t)(kr_ >> 32), kl_) + 2 >
                            1024);
    }
}
    if (i < len && p[i] == ':') {
        i++;
        /* compact fast path: the value byte follows the colon with no
         * whitespace — one load dispatches it (the ws test below is
         * what LWS would run anyway) */
        if (i < len) {
            char v = p[i];
            if (v != ' ' && v != '\n' && v != '\r' && (cl || v != '\t')) {
                goto lmapval;
            }
        }
        LWS();
        goto lmapval;
    }
    LWS();
    if (p[i] != ':') {
        goto lreject;
    }
    i++;
    LWS();
lmapval: {
    char c = p[i];
    if (c == '"') {
        LSTR_SCAN(lmapafter);
    }
    if ((unsigned)(c - '0') <= 9u || c == '-') {
        size_t k = i + 1;
        /* the digits-only tracking makes the pure-integer case a
         * 3-cycle leading-zero check — number_shape (a call + a
         * rescan, 13% of json-doc) runs only for dot/exp spans */
        int digits_only = 1, saw_digit = (c != '-');
        LNUM_RUN;
        while (k < len) {
            char d = p[k];
            if ((unsigned)(d - '0') <= 9u) {
                k++;
                saw_digit = 1;
                continue;
            }
            /* the flag clears only on a CONSUMED byte: the run's
             * terminator (',', '}', ']', whitespace) must leave
             * digits_only intact, or every plain integer followed
             * by a delimiter falls into the full validator */
            if (d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') {
                digits_only = 0;
                k++;
                continue;
            }
            break;
        }
        if (strict_nums) {
            int ok;
            if (digits_only && saw_digit) {
                size_t d0 = (c == '-') ? i + 1 : i;
                ok = (k - d0 == 1) || p[d0] != '0';
            } else {
                ok = tape_num_ok(p + i, (size_t)(k - i));
            }
            if (!ok) {
                goto lreject;
            }
        }
        recs[count] =
            ((uint64_t)((uint32_t)i) << 32) | ((uint64_t)((uint32_t)(k - i)) << 8) |
            (uint64_t)(strict_nums ? ((digits_only && saw_digit) ? YEP_T_INT : YEP_T_FLOAT)
                                   : (uint64_t)(YEP_T_NUM));
        count++;
        i = k;
        goto lmapafter;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        size_t wl = c == 'f' ? 5 : 4;
        if (i + wl > len) {
            goto lreject;
        }
        uint32_t got4;
        memcpy(&got4, p + i, 4);
        if (got4 != (c == 't'   ? 0x65757274u /* "true" */
                     : c == 'n' ? 0x6C6C756Eu /* "null" */
                                : 0x736C6166u /* "fals" */) ||
            (c == 'f' && p[i + 4] != 'e')) {
            goto lreject;
        }
        if (i + wl < len) {
            char z = p[i + wl];
            if (z != ' ' && z != '\t' && z != '\n' && z != '\r' && z != ',' && z != ']' &&
                z != '}' && z != ':') {
                goto lreject;
            }
        }
        recs[count] = ((uint64_t)((uint32_t)i) << 32) | ((uint64_t)((uint32_t)wl) << 8) |
                      (uint64_t)(c == 'n' ? YEP_T_NULL : (c == 't' ? YEP_T_TRUE : YEP_T_FALSE));
        count++;
        i += wl;
        goto lmapafter;
    }
    if (c == '{') {
        LPUSH(1);
        goto lmap1;
    }
    if (c == '[') {
        LPUSH(0);
        goto lseq1;
    }
    goto lreject;
}

lmapafter:
    /* compact fast path: value ',' key directly — the two LWS hops the
     * general form runs both exit on byte one here */
    if (i < len && p[i] == ',') {
        i++;
        if (i < len && p[i] == '"') {
            goto lmapkey;
        }
        goto lmap;
    }
    LWS();
    if (p[i] == ',') {
        i++;
        goto lmap;
    }
    if (p[i] == '}') {
        LCLOSE();
    }
    goto lreject;

/* The sequence member cycle — the same single-region shape. */
lseq1: /* [] closes here */
    LWS();
    if (p[i] == ']') {
        LCLOSE();
    }
    goto lseqval;

lseq: /* a member MUST follow (after ','): JW_VALUE */
    LWS();
lseqval: {
    char c = p[i];
    if (c == '"') {
        LSTR_SCAN(lseqafter);
    }
    if ((unsigned)(c - '0') <= 9u || c == '-') {
        size_t k = i + 1;
        int digits_only = 1, saw_digit = (c != '-');
        LNUM_RUN;
        while (k < len) {
            char d = p[k];
            if ((unsigned)(d - '0') <= 9u) {
                k++;
                saw_digit = 1;
                continue;
            }
            /* consumed-only clearing: see the map arm's comment */
            if (d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') {
                digits_only = 0;
                k++;
                continue;
            }
            break;
        }
        if (strict_nums) {
            int ok;
            if (digits_only && saw_digit) {
                size_t d0 = (c == '-') ? i + 1 : i;
                ok = (k - d0 == 1) || p[d0] != '0';
            } else {
                ok = tape_num_ok(p + i, (size_t)(k - i));
            }
            if (!ok) {
                goto lreject;
            }
        }
        recs[count] =
            ((uint64_t)((uint32_t)i) << 32) | ((uint64_t)((uint32_t)(k - i)) << 8) |
            (uint64_t)(strict_nums ? ((digits_only && saw_digit) ? YEP_T_INT : YEP_T_FLOAT)
                                   : (uint64_t)(YEP_T_NUM));
        count++;
        i = k;
        goto lseqafter;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        size_t wl = c == 'f' ? 5 : 4;
        if (i + wl > len) {
            goto lreject;
        }
        uint32_t got4;
        memcpy(&got4, p + i, 4);
        if (got4 != (c == 't'   ? 0x65757274u /* "true" */
                     : c == 'n' ? 0x6C6C756Eu /* "null" */
                                : 0x736C6166u /* "fals" */) ||
            (c == 'f' && p[i + 4] != 'e')) {
            goto lreject;
        }
        if (i + wl < len) {
            char z = p[i + wl];
            if (z != ' ' && z != '\t' && z != '\n' && z != '\r' && z != ',' && z != ']' &&
                z != '}' && z != ':') {
                goto lreject;
            }
        }
        recs[count] = ((uint64_t)((uint32_t)i) << 32) | ((uint64_t)((uint32_t)wl) << 8) |
                      (uint64_t)(c == 'n' ? YEP_T_NULL : (c == 't' ? YEP_T_TRUE : YEP_T_FALSE));
        count++;
        i += wl;
        goto lseqafter;
    }
    if (c == '{') {
        LPUSH(1);
        goto lmap1;
    }
    if (c == '[') {
        LPUSH(0);
        goto lseq1;
    }
    goto lreject;
}

lseqafter:
    /* compact fast path: value ',' value directly — both LWS hops exit
     * on byte one here */
    if (i < len && p[i] == ',') {
        i++;
        if (i < len) {
            char v = p[i];
            if (v != ' ' && v != '\n' && v != '\r' && (cl || v != '\t')) {
                goto lseqval;
            }
        }
        goto lseq;
    }
    LWS();
    if (p[i] == ',') {
        i++;
        goto lseq;
    }
    if (p[i] == ']') {
        LCLOSE();
    }
    goto lreject;

ldone:
#undef LWS
#undef LNUM_RUN
#undef LPUSH
#undef LCLOSE
#undef LSTR_SCAN

{
    size_t tail = i;
    while (tail < len &&
           (p[tail] == ' ' || p[tail] == '\n' || p[tail] == '\r' || (p[tail] == '\t' && !cl))) {
        tail++;
    }
    if (tail != len) {
        goto lreject;
    }
}
    /* a col-0 tab-led continuation line inside the span is the suite's
     * Y79Y#4 error: defer to the engine, whose e_flow_ws owns the law
     * -- the walk's whitespace loops treat the tab as separation, which
     * accepted exactly the span the marshal drain's engine rejected,
     * splitting parseable from marshalable (the nightly's round 120) */
    for (size_t k = open + 1; k < len; k++) {
        if (p[k] == '\n' && k + 1 < len && p[k + 1] == '\t') {
            yeptris_tape_free(t);
            return YEPTRIS_ERROR_PARSE;
        }
    }
    t->count = count;
    t->_src = p;
    t->_srclen = len;
    return YEPTRIS_OK;

lreject:
    yeptris_tape_free(t);
    return YEPTRIS_ERROR_PARSE;
}

#define LNUM_RUN                                                                                   \
    do {                                                                                           \
        size_t ks_ = k;                                                                            \
        while (k + 8 <= len) {                                                                     \
            uint64_t w_;                                                                           \
            memcpy(&w_, p + k, 8);                                                                 \
            uint64_t nd_ =                                                                         \
                ((w_ + 0x4646464646464646ull) & 0x8080808080808080ull) |                           \
                (((w_ | 0x8080808080808080ull) - 0xB0B0B0B0B0B0B0B0ull) & 0x8080808080808080ull) | \
                (w_ & 0x8080808080808080ull);                                                      \
            if (nd_ != 0) {                                                                        \
                k += (size_t)yep_ctz64(nd_) >> 3;                                                  \
                break;                                                                             \
            }                                                                                      \
            k += 8;                                                                                \
        }                                                                                          \
        if (k > ks_) {                                                                             \
            saw_digit = 1;                                                                         \
        }                                                                                          \
    } while (0)
static YeptrisStatus yep_tape_walk_columns_fused(const char* p, size_t len, size_t open,
                                                 yeptris_json_tape* t, int strict_nums,
                                                 int clean_only) {
    /* The columns-primary edition of the record walk below: the same
     * arms (specialized member cycles, LSTR_SCAN, the digits law)
     * emitting the three columns directly — one pass, no records, no
     * materialization pass. The strict route's columns-eager contract
     * at the lenient lane's memory traffic. The record walk stays
     * byte-identical beside it; the differential suites pin the
     * equivalence. */
    if (tape_carve(t, len) != YEPTRIS_OK) {
        return YEPTRIS_ERROR_MEMORY;
    }
    uint8_t* kinds = t->kinds;
    uint32_t* offs = t->offs;
    uint32_t* lens = t->lens;
    uint32_t open_at[YEP_JSON_WALK_DEPTH];
    uint8_t kind[YEP_JSON_WALK_DEPTH];

    kinds[0] = (uint8_t)YEP_T_DOC;
    offs[0] = 0;
    lens[0] = 0;

    uint8_t top_kind = p[open] == '[' ? 0 : 1;
    kinds[1] = top_kind ? (uint8_t)YEP_T_MAP_OPEN : (uint8_t)YEP_T_SEQ_OPEN;
    offs[1] = 0;
    lens[1] = 0;
    size_t count = 2;
    int depth = 1;
    open_at[0] = 1;
    kind[0] = top_kind;

    size_t i = open + 1;
    /* clean_only (parse_json's route): tabs and non-ASCII bytes REJECT
     * — the caller falls to the strict sequence, which owns the pinned
     * error precedences those inputs carry (the tab reject, the UTF-8
     * gate). This replaces the entry's full-input gate_scan +
     * tab-memchr pre-passes: same routing by construction, zero scans
     * before the walk. */
    int cl = clean_only;
    if (top_kind) {
        goto lmap1;
    }
    goto lseq1;

    /* strict_nums: the parse_json route's contract is RFC 8259 AT
     * parse time, so its number arms validate the run in-place (the
     * settle pass disappears — the span is cache-warm here); the
     * standalone lenient entry keeps its deferred contract (0: the
     * run records unvalidated, yeptris_tape_convert owns it). */

    /* The specialized member loops (the #342 dispatch-chain cut): once
     * inside a container the grammar is a 2-state cycle — member or
     * separator — so the general chain's expect/key_slot machine and
     * its per-token re-dispatch are replaced by direct loops per kind.
     * Acceptance is IDENTICAL to the chain (the pinning suites hold:
     * Tape suite, LenientMatchesStrict, ErrorParity, the json corpora).
     *
     * lmap / lseq: positioned at a member (or the container's closer).
     * lvalue: scans one value (string/number/literal/container).
     * lafter: positioned after a complete value — ',' or the closer.
     * A close pops the frame; depth 0 ends the walk. */

#define LWS()                                                                                      \
    do {                                                                                           \
        while (i < len &&                                                                          \
               (p[i] == ' ' || p[i] == '\n' || p[i] == '\r' || (p[i] == '\t' && !cl))) {           \
            i++;                                                                                   \
        }                                                                                          \
        if (i >= len) {                                                                            \
            goto lreject;                                                                          \
        }                                                                                          \
    } while (0)

#define LPUSH(k)                                                                                   \
    do {                                                                                           \
        if (depth >= YEP_JSON_WALK_DEPTH) {                                                        \
            goto lreject;                                                                          \
        }                                                                                          \
        kinds[count] = (k) ? (uint8_t)YEP_T_MAP_OPEN : (uint8_t)YEP_T_SEQ_OPEN;                    \
        offs[count] = 0;                                                                           \
        lens[count] = 0;                                                                           \
        kind[depth] = (k);                                                                         \
        open_at[depth] = (uint32_t)count;                                                          \
        count++;                                                                                   \
        depth++;                                                                                   \
        i++;                                                                                       \
    } while (0)

#define LCLOSE()                                                                                   \
    do {                                                                                           \
        uint32_t lo_ = open_at[depth - 1];                                                         \
        kinds[count] = (uint8_t)YEP_T_CLOSE;                                                       \
        offs[count] = lo_;                                                                         \
        lens[count] = 0;                                                                           \
        offs[lo_] = (uint32_t)count;                                                               \
        count++;                                                                                   \
        i++;                                                                                       \
        depth--;                                                                                   \
        if (depth == 0) {                                                                          \
            goto ldone;                                                                            \
        }                                                                                          \
        if (kind[depth - 1]) {                                                                     \
            goto lmapafter;                                                                        \
        }                                                                                          \
        goto lseqafter;                                                                            \
    } while (0)

/* LSTR_SCAN(after) — the fused string arm, positioned at p[i] == '"'.
 * One SWAR round settles the common short span; quote_scan (the SIMD
 * close scan) settles longer no-escape spans, with a c0 sweep over the
 * content (quote_scan never tests control bytes); the kernel runs only
 * when escapes appear — its grammar authority covers the escape rules
 * quote_scan skips. `after` names the post-member label. */
#define LSTR_SCAN(after)                                                                           \
    do {                                                                                           \
        size_t j_ = i + 1;                                                                         \
        int esc_ = 0;                                                                              \
        if (j_ + 8 <= len) {                                                                       \
            uint64_t w_;                                                                           \
            memcpy(&w_, p + j_, 8);                                                                \
            uint64_t qm_ = (w_ ^ 0x2222222222222222ull);                                           \
            uint64_t bm_ = (w_ ^ 0x5C5C5C5C5C5C5C5Cull);                                           \
            qm_ = (qm_ - 0x0101010101010101ull) & ~qm_ & 0x8080808080808080ull;                    \
            bm_ = (bm_ - 0x0101010101010101ull) & ~bm_ & 0x8080808080808080ull;                    \
            uint64_t c0_ = (w_ - 0x2020202020202020ull) & ~w_ & 0x8080808080808080ull;             \
            uint64_t hi_ = cl ? (w_ & 0x8080808080808080ull) : 0;                                  \
            if (qm_ != 0 && ((bm_ | c0_ | hi_) & (qm_ - 1)) == 0) {                                \
                size_t cl_ = j_ + (size_t)yep_ctz64(qm_) / 8;                                      \
                kinds[count] = (uint8_t)YEP_T_STR;                                                 \
                offs[count] = (uint32_t)(j_);                                                      \
                lens[count] = (uint32_t)((cl_ - j_));                                              \
                count++;                                                                           \
                i = cl_ + 1;                                                                       \
                goto after;                                                                        \
            }                                                                                      \
            /* rounds 2-4 (up to 32 bytes inline): the email-class strings                         \
             * close here without the quote_scan call and its redundant                            \
             * content re-sweep; reached only when round 1 had no close,                           \
             * so the short-string case pays nothing */                                            \
            if ((qm_ | bm_ | c0_ | hi_) == 0) {                                                    \
                size_t nr_ = (len - j_) >> 3;                                                      \
                if (nr_ > 4) {                                                                     \
                    nr_ = 4;                                                                       \
                }                                                                                  \
                for (size_t r_ = 1; r_ < nr_; r_++) {                                              \
                    size_t b2_ = j_ + (r_ << 3);                                                   \
                    memcpy(&w_, p + b2_, 8);                                                       \
                    qm_ = (w_ ^ 0x2222222222222222ull);                                            \
                    bm_ = (w_ ^ 0x5C5C5C5C5C5C5C5Cull);                                            \
                    qm_ = (qm_ - 0x0101010101010101ull) & ~qm_ & 0x8080808080808080ull;            \
                    bm_ = (bm_ - 0x0101010101010101ull) & ~bm_ & 0x8080808080808080ull;            \
                    c0_ = (w_ - 0x2020202020202020ull) & ~w_ & 0x8080808080808080ull;              \
                    hi_ = cl ? (w_ & 0x8080808080808080ull) : 0;                                   \
                    if (qm_ != 0 && ((bm_ | c0_ | hi_) & (qm_ - 1)) == 0) {                        \
                        size_t cl_ = b2_ + (size_t)yep_ctz64(qm_) / 8;                             \
                        kinds[count] = (uint8_t)YEP_T_STR;                                         \
                        offs[count] = (uint32_t)(j_);                                              \
                        lens[count] = (uint32_t)((cl_ - j_));                                      \
                        count++;                                                                   \
                        i = cl_ + 1;                                                               \
                        goto after;                                                                \
                    }                                                                              \
                    if ((qm_ | bm_ | c0_ | hi_) != 0) {                                            \
                        break; /* escape, c0 or hi: kernel authority */                            \
                    }                                                                              \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        {                                                                                          \
            const yep_text_kernels* kk_ = yep_text_active();                                       \
            ptrdiff_t r_ = kk_->quote_scan(p + j_, len - j_, '"', &esc_);                          \
            if (r_ >= 0 && esc_ == 0) {                                                            \
                size_t cl_ = j_ + (size_t)r_;                                                      \
                for (size_t b_ = j_; b_ < cl_;) {                                                  \
                    size_t room_ = cl_ - b_;                                                       \
                    if (room_ >= 8) {                                                              \
                        uint64_t v_;                                                               \
                        memcpy(&v_, p + b_, 8);                                                    \
                        uint64_t bad_ =                                                            \
                            (v_ - 0x2020202020202020ull) & ~v_ & 0x8080808080808080ull;            \
                        if (cl) {                                                                  \
                            bad_ |= v_ & 0x8080808080808080ull;                                    \
                        }                                                                          \
                        if (bad_) {                                                                \
                            goto lreject;                                                          \
                        }                                                                          \
                        b_ += 8;                                                                   \
                    } else {                                                                       \
                        if ((unsigned char)p[b_] < 0x20 || (cl && (unsigned char)p[b_] >= 0x80)) { \
                            goto lreject;                                                          \
                        }                                                                          \
                        b_++;                                                                      \
                    }                                                                              \
                }                                                                                  \
                kinds[count] = (uint8_t)YEP_T_STR;                                                 \
                offs[count] = (uint32_t)(j_);                                                      \
                lens[count] = (uint32_t)((cl_ - j_));                                              \
                count++;                                                                           \
                i = cl_ + 1;                                                                       \
                goto after;                                                                        \
            }                                                                                      \
        }                                                                                          \
        {                                                                                          \
            size_t at_ = i;                                                                        \
            size_t cl_ = 0;                                                                        \
            if (!yep_json_string(p, len, &i, &cl_, &esc_)) {                                       \
                goto lreject;                                                                      \
            }                                                                                      \
            if (cl) { /* escaped strings skip the sweeps above: the                                \
                       * clean route still owes the strict sequence's                              \
                       * byte guarantees over the content */                                       \
                for (size_t b_ = at_ + 1; b_ < cl_; b_++) {                                        \
                    unsigned char uc_ = (unsigned char)p[b_];                                      \
                    if (uc_ < 0x20 || uc_ >= 0x80) {                                               \
                        goto lreject;                                                              \
                    }                                                                              \
                }                                                                                  \
            }                                                                                      \
            kinds[count] = (uint8_t)YEP_T_STR;                                                     \
            offs[count] = (uint32_t)((at_ + 1));                                                   \
            lens[count] = (uint32_t)((cl_ - at_ - 1));                                             \
            count++;                                                                               \
            i = cl_ + 1;                                                                           \
        }                                                                                          \
        goto after;                                                                                \
    } while (0)

/* The map member cycle — ONE contiguous region (the json-doc lesson:
 * a goto web across value/key/after regions scattered the hot path and
 * cost 2.7x on the flat-map shape). Values scan inline; only nested
 * containers, literals, and the kernel string fallback leave the
 * cycle. Acceptance is the chain's: no trailing commas, string keys
 * only, `:` required, closers only in first-member position. */
lmap1: /* {} closes here */
    LWS();
    if (p[i] == '}') {
        LCLOSE();
    }
    if (p[i] != '"') {
        goto lreject;
    }
    goto lmapkey;

lmap: /* a member MUST follow (after ','): JW_KEY — a closer rejects */
    /* compact fast path: the byte is the key's quote already — LWS's
     * loop would examine it once and exit */
    if (i < len && p[i] == '"') {
        goto lmapkey;
    }
    LWS();
    if (p[i] != '"') {
        goto lreject;
    }
lmapkey: { LSTR_SCAN(lmapcolon); }
lmapcolon:
    if (i < len && p[i] == ':') {
        i++;
        /* compact fast path: the value byte follows the colon with no
         * whitespace — one load dispatches it (the ws test below is
         * what LWS would run anyway) */
        if (i < len) {
            char v = p[i];
            if (v != ' ' && v != '\n' && v != '\r' && (cl || v != '\t')) {
                goto lmapval;
            }
        }
        LWS();
        goto lmapval;
    }
    LWS();
    if (p[i] != ':') {
        goto lreject;
    }
    i++;
    LWS();
lmapval: {
    char c = p[i];
    if (c == '"') {
        LSTR_SCAN(lmapafter);
    }
    if ((unsigned)(c - '0') <= 9u || c == '-') {
        size_t k = i + 1;
        /* the digits-only tracking makes the pure-integer case a
         * 3-cycle leading-zero check — number_shape (a call + a
         * rescan, 13% of json-doc) runs only for dot/exp spans */
        int digits_only = 1, saw_digit = (c != '-');
        LNUM_RUN;
        while (k < len) {
            char d = p[k];
            if ((unsigned)(d - '0') <= 9u) {
                k++;
                saw_digit = 1;
                continue;
            }
            /* the flag clears only on a CONSUMED byte: the run's
             * terminator (',', '}', ']', whitespace) must leave
             * digits_only intact, or every plain integer followed
             * by a delimiter falls into the full validator */
            if (d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') {
                digits_only = 0;
                k++;
                continue;
            }
            break;
        }
        if (strict_nums) {
            int ok;
            if (digits_only && saw_digit) {
                size_t d0 = (c == '-') ? i + 1 : i;
                ok = (k - d0 == 1) || p[d0] != '0';
            } else {
                ok = tape_num_ok(p + i, (size_t)(k - i));
            }
            if (!ok) {
                goto lreject;
            }
        }
        kinds[count] =
            strict_nums ? ((digits_only && saw_digit) ? (uint8_t)YEP_T_INT : (uint8_t)YEP_T_FLOAT)
                        : (uint8_t)YEP_T_NUM;
        offs[count] = (uint32_t)i;
        lens[count] = (uint32_t)(k - i);
        count++;
        i = k;
        goto lmapafter;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        size_t wl = c == 'f' ? 5 : 4;
        if (i + wl > len) {
            goto lreject;
        }
        uint32_t got4;
        memcpy(&got4, p + i, 4);
        if (got4 != (c == 't'   ? 0x65757274u /* "true" */
                     : c == 'n' ? 0x6C6C756Eu /* "null" */
                                : 0x736C6166u /* "fals" */) ||
            (c == 'f' && p[i + 4] != 'e')) {
            goto lreject;
        }
        if (i + wl < len) {
            char z = p[i + wl];
            if (z != ' ' && z != '\t' && z != '\n' && z != '\r' && z != ',' && z != ']' &&
                z != '}' && z != ':') {
                goto lreject;
            }
        }
        kinds[count] = c == 'n' ? (uint8_t)YEP_T_NULL
                                : (c == 't' ? (uint8_t)YEP_T_TRUE : (uint8_t)YEP_T_FALSE);
        offs[count] = (uint32_t)i;
        lens[count] = (uint32_t)wl;
        count++;
        i += wl;
        goto lmapafter;
    }
    if (c == '{') {
        LPUSH(1);
        goto lmap1;
    }
    if (c == '[') {
        LPUSH(0);
        goto lseq1;
    }
    goto lreject;
}

lmapafter:
    /* compact fast path: value ',' key directly — the two LWS hops the
     * general form runs both exit on byte one here */
    if (i < len && p[i] == ',') {
        i++;
        if (i < len && p[i] == '"') {
            goto lmapkey;
        }
        goto lmap;
    }
    LWS();
    if (p[i] == ',') {
        i++;
        goto lmap;
    }
    if (p[i] == '}') {
        LCLOSE();
    }
    goto lreject;

/* The sequence member cycle — the same single-region shape. */
lseq1: /* [] closes here */
    LWS();
    if (p[i] == ']') {
        LCLOSE();
    }
    goto lseqval;

lseq: /* a member MUST follow (after ','): JW_VALUE */
    LWS();
lseqval: {
    char c = p[i];
    if (c == '"') {
        LSTR_SCAN(lseqafter);
    }
    if ((unsigned)(c - '0') <= 9u || c == '-') {
        size_t k = i + 1;
        int digits_only = 1, saw_digit = (c != '-');
        LNUM_RUN;
        while (k < len) {
            char d = p[k];
            if ((unsigned)(d - '0') <= 9u) {
                k++;
                saw_digit = 1;
                continue;
            }
            /* consumed-only clearing: see the map arm's comment */
            if (d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') {
                digits_only = 0;
                k++;
                continue;
            }
            break;
        }
        if (strict_nums) {
            int ok;
            if (digits_only && saw_digit) {
                size_t d0 = (c == '-') ? i + 1 : i;
                ok = (k - d0 == 1) || p[d0] != '0';
            } else {
                ok = tape_num_ok(p + i, (size_t)(k - i));
            }
            if (!ok) {
                goto lreject;
            }
        }
        kinds[count] =
            strict_nums ? ((digits_only && saw_digit) ? (uint8_t)YEP_T_INT : (uint8_t)YEP_T_FLOAT)
                        : (uint8_t)YEP_T_NUM;
        offs[count] = (uint32_t)i;
        lens[count] = (uint32_t)(k - i);
        count++;
        i = k;
        goto lseqafter;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        size_t wl = c == 'f' ? 5 : 4;
        if (i + wl > len) {
            goto lreject;
        }
        uint32_t got4;
        memcpy(&got4, p + i, 4);
        if (got4 != (c == 't'   ? 0x65757274u /* "true" */
                     : c == 'n' ? 0x6C6C756Eu /* "null" */
                                : 0x736C6166u /* "fals" */) ||
            (c == 'f' && p[i + 4] != 'e')) {
            goto lreject;
        }
        if (i + wl < len) {
            char z = p[i + wl];
            if (z != ' ' && z != '\t' && z != '\n' && z != '\r' && z != ',' && z != ']' &&
                z != '}' && z != ':') {
                goto lreject;
            }
        }
        kinds[count] = c == 'n' ? (uint8_t)YEP_T_NULL
                                : (c == 't' ? (uint8_t)YEP_T_TRUE : (uint8_t)YEP_T_FALSE);
        offs[count] = (uint32_t)i;
        lens[count] = (uint32_t)wl;
        count++;
        i += wl;
        goto lseqafter;
    }
    if (c == '{') {
        LPUSH(1);
        goto lmap1;
    }
    if (c == '[') {
        LPUSH(0);
        goto lseq1;
    }
    goto lreject;
}

lseqafter:
    /* compact fast path: value ',' value directly — both LWS hops exit
     * on byte one here */
    if (i < len && p[i] == ',') {
        i++;
        if (i < len) {
            char v = p[i];
            if (v != ' ' && v != '\n' && v != '\r' && (cl || v != '\t')) {
                goto lseqval;
            }
        }
        goto lseq;
    }
    LWS();
    if (p[i] == ',') {
        i++;
        goto lseq;
    }
    if (p[i] == ']') {
        LCLOSE();
    }
    goto lreject;

ldone:
#undef LWS
#undef LNUM_RUN
#undef LPUSH
#undef LCLOSE
#undef LSTR_SCAN

{
    size_t tail = i;
    while (tail < len &&
           (p[tail] == ' ' || p[tail] == '\n' || p[tail] == '\r' || (p[tail] == '\t' && !cl))) {
        tail++;
    }
    if (tail != len) {
        goto lreject;
    }
}
    t->count = count;
    t->_src = p;
    t->_srclen = len;
    t->_cols_ready = 1;
    return YEPTRIS_OK;

lreject:
    yeptris_tape_free(t);
    return YEPTRIS_ERROR_PARSE;
}

static YEP_UNUSED_FN YeptrisStatus tape_walk_lnt(const char* p, size_t len, size_t open,
                                                 const uint32_t* idx, size_t nidx,
                                                 yeptris_json_tape* t) {
    if (tape_carve(t, len) != YEPTRIS_OK) {
        return YEPTRIS_ERROR_MEMORY;
    }
    uint8_t* kinds = t->kinds;
    uint32_t* offs = t->offs;
    uint32_t* lens = t->lens;
    uint32_t open_at[YEP_JSON_WALK_DEPTH];
    uint8_t kind[YEP_JSON_WALK_DEPTH];

    kinds[0] = YEP_T_DOC;
    offs[0] = 0;
    lens[0] = 0;

    uint8_t top_kind = p[open] == '[' ? 0 : 1;
    kinds[1] = top_kind ? YEP_T_MAP_OPEN : YEP_T_SEQ_OPEN;
    offs[1] = 0;
    lens[1] = 0;
    uint32_t top_open = 1;
    size_t count = 2;
    uint8_t top_expect = top_kind ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
    int key_slot = top_kind ? 1 : 0;
    int depth = 1;
    open_at[0] = top_open;
    kind[0] = top_kind;

    size_t si = 0;
    while (si < nidx && idx[si] <= open) {
        si++; /* the root opener (and anything before it) is consumed */
    }
    for (;;) {
        if (si >= nidx) {
            goto reject; /* EOF mid-structure */
        }
        size_t at = idx[si++];
        if (at >= len) {
            goto reject;
        }
        char c = p[at];

        if (top_expect == JW_COLON) {
            if (c != ':') {
                goto reject;
            }
            top_expect = JW_VALUE;
            key_slot = 0;
            continue;
        }
        if (top_expect == JW_COMMA_OR_CLOSE) {
            if (c == ',') {
                top_expect = top_kind ? JW_KEY : JW_VALUE;
                key_slot = top_kind ? 1 : 0;
                continue;
            }
            if (c != ']' && c != '}') {
                goto reject;
            }
        } else if (key_slot) {
            if (c != '"' && c != ']' && c != '}') {
                goto reject;
            }
        }

        if (c == '"') {
            size_t j = at + 1;
            size_t close = 0;
            int esc = 0;
            /* SWAR settle: one u64 window closes the corpus's 1-8
             * byte strings without the call; anything else (escape,
             * C0, longer) falls back to the kernel authority */
            if (j + 8 <= len) {
                uint64_t w;
                memcpy(&w, p + j, 8);
                uint64_t qm = (w ^ 0x2222222222222222ull);
                uint64_t bm = (w ^ 0x5C5C5C5C5C5C5C5Cull);
                qm = (qm - 0x0101010101010101ull) & ~qm & 0x8080808080808080ull;
                bm = (bm - 0x0101010101010101ull) & ~bm & 0x8080808080808080ull;
                uint64_t c0 = (w - 0x2020202020202020ull) & ~w & 0x8080808080808080ull;
                if (qm != 0 && ((bm | c0) & (qm - 1)) == 0) {
                    close = j + (size_t)yep_ctz64(qm) / 8;
                    kinds[count] = YEP_T_STR;
                    offs[count] = (uint32_t)j;
                    lens[count] = (uint32_t)(close - j);
                    count++;
                    goto str_done;
                }
            }
            size_t i = at;
            if (!yep_json_string(p, len, &i, &close, &esc)) {
                goto reject;
            }
            kinds[count] = YEP_T_STR;
            offs[count] = (uint32_t)(at + 1);
            lens[count] = (uint32_t)(close - at - 1);
            count++;
        str_done:
            if (key_slot) {
                /* colon peek: consume it without a loop round */
                if (si < nidx && p[idx[si]] == ':') {
                    si++;
                    top_expect = JW_VALUE;
                    key_slot = 0;
                } else {
                    top_expect = JW_COLON;
                }
            } else {
                goto comma_peek;
            }
            continue;
        }
        if ((unsigned)(c - '0') <= 9u || c == '-') {
            /* the deferred arm: the span runs to the next token,
             * trailing ws trimmed — no grammar scan, no shape split */
            size_t end = (si < nidx) ? idx[si] : len;
            while (end > at && (p[end - 1] == ' ' || p[end - 1] == '\t' || p[end - 1] == '\n' ||
                                p[end - 1] == '\r')) {
                end--;
            }
            kinds[count] = YEP_T_NUM;
            offs[count] = (uint32_t)at;
            lens[count] = (uint32_t)(end - at);
            count++;
        comma_peek:
            /* comma peek: the value-or-key separator consumes without
             * a loop round (the corpus's dominant cycle) */
            if (si < nidx && p[idx[si]] == ',') {
                si++;
                top_expect = top_kind ? JW_KEY : JW_VALUE;
                key_slot = top_kind ? 1 : 0;
            } else {
                top_expect = JW_COMMA_OR_CLOSE;
            }
            continue;
        }
        if (c == ']' || c == '}') {
            int want = c == ']' ? 0 : 1;
            if (top_kind != want ||
                (top_expect != JW_VALUE_OR_CLOSE && top_expect != JW_KEY_OR_CLOSE &&
                 top_expect != JW_COMMA_OR_CLOSE)) {
                goto reject;
            }
            kinds[count] = YEP_T_CLOSE;
            offs[count] = top_open;
            lens[count] = 0;
            offs[top_open] = (uint32_t)count;
            count++;
            depth--;
            if (depth == 0) {
                break;
            }
            top_kind = kind[depth - 1];
            top_expect = JW_COMMA_OR_CLOSE;
            top_open = open_at[depth - 1];
            key_slot = 0;
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth >= YEP_JSON_WALK_DEPTH) {
                goto reject;
            }
            kind[depth - 1] = top_kind;
            open_at[depth - 1] = top_open;
            top_kind = c == '[' ? 0 : 1;
            kinds[count] = c == '[' ? YEP_T_SEQ_OPEN : YEP_T_MAP_OPEN;
            offs[count] = 0;
            lens[count] = 0;
            top_open = (uint32_t)count;
            count++;
            top_expect = top_kind ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
            key_slot = top_kind ? 1 : 0;
            kind[depth] = top_kind;
            open_at[depth] = top_open;
            depth++;
            continue;
        }
        if (c == 't' || c == 'f' || c == 'n') {
            size_t wl = c == 'f' ? 5 : 4;
            if (at + wl > len) {
                goto reject;
            }
            uint32_t got4;
            memcpy(&got4, p + at, 4);
            if (got4 != (c == 't'   ? 0x65757274u /* "true" */
                         : c == 'n' ? 0x6C6C756Eu /* "null" */
                                    : 0x736C6166u /* "fals" */) ||
                (c == 'f' && p[at + 4] != 'e')) {
                goto reject;
            }
            if (at + wl < len) {
                char z = p[at + wl];
                if (z != ' ' && z != '\t' && z != '\n' && z != '\r' && z != ',' && z != ']' &&
                    z != '}' && z != ':') {
                    goto reject;
                }
            }
            kinds[count] = c == 'n' ? YEP_T_NULL : (c == 't' ? YEP_T_TRUE : YEP_T_FALSE);
            offs[count] = (uint32_t)at;
            lens[count] = (uint32_t)wl;
            count++;
            top_expect = JW_COMMA_OR_CLOSE;
            key_slot = 0;
            continue;
        }
        goto reject;
    }
    /* trailing residue: after the root close, any remaining token is
     * non-ws garbage (pure trailing ws emits no tokens) */
    if (si != nidx) {
        goto reject;
    }
    t->count = count;
    t->_src = p;
    t->_srclen = len;
    return YEPTRIS_OK;

reject:
    yeptris_tape_free(t);
    return YEPTRIS_ERROR_PARSE;
}

YEPTRIS_API YeptrisStatus yeptris_parse_json_tape_lenient(const char* source, size_t len,
                                                          yeptris_json_tape* tape) {
    if ((source == NULL && len != 0) || tape == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    memset(tape, 0, sizeof(*tape));
    if (len > 0xFFFFFFF0u) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "tape spans are uint32; document exceeds 4 GiB");
        return YEPTRIS_ERROR_ARG;
    }

    /* lenient defers grammar only; strings borrow validated spans, so
     * the encoding gate keeps the strict fallback (non-ASCII rides
     * yeptris_parse_json_tape unchanged) */
    if (len > 0 && yep_text_active()->gate_scan(source, len)) {
        return yeptris_parse_json_tape(source, len, tape);
    }

    size_t at = 0;
    while (at < len &&
           (source[at] == ' ' || source[at] == '\t' || source[at] == '\n' || source[at] == '\r')) {
        at++;
    }
    if (at >= len) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, at,
                      "empty document (lenient tape)");
        return YEPTRIS_ERROR_PARSE;
    }

    if (source[at] == '[' || source[at] == '{') {
        /* the fused walk IS the lenient route: one pass beats the
         * stage-1 + span two-pass on every bench corpus (json-doc
         * 1.65x, json-users 2.2x — the ledger's #85 scoping), and
         * this exact call already carried the route's verdicts as the
         * escape/C0 fallback, so acceptance is unchanged. */
        int rc = yep_tape_walk_lenient_fused(source, len, at, tape, 0, 0, 0);
        if (rc != YEPTRIS_OK) {
            return rc;
        }
        /* the YAML simple-key law rides the fused route too: a key
         * whose raw span crosses 1024 defers to the engine's clean
         * error — the JSON-class walk has no YAML laws, and the other
         * routes (engine_run, the marshal drains) reject the same
         * span, so acceptance here broke whole/chunked marshal parity
         * (the nightly's round 108) */
        if (yep_tape_long_simple_key(tape)) {
            yep_error_set(yep_error_tls(), YEP_ERR_KEY_TOO_LONG, 1, (uint32_t)(at + 1), at, NULL);
            return YEPTRIS_ERROR_PARSE;
        }
        return YEPTRIS_OK;
    }

    /* scalar root: one record, same deferred split for numbers */
    if (tape_carve(tape, len) != YEPTRIS_OK) {
        return YEPTRIS_ERROR_MEMORY;
    }
    tape->_src = source;
    tape->_srclen = len;
    tape_ctx c = {tape, len + 2, {0}, 0, 0};
    if (!rec_put(&c, YEP_T_DOC, 0, 0)) {
        goto lnt_root_mem;
    }
    if (source[at] == '"') {
        size_t i = at;
        size_t close = 0;
        int esc = 0;
        if (!yep_json_string(source, len, &i, &close, &esc) ||
            !rec_put(&c, YEP_T_STR, (uint32_t)(at + 1), (uint32_t)(close - at - 1))) {
            goto lnt_root_reject;
        }
        while (i < len &&
               (source[i] == ' ' || source[i] == '\t' || source[i] == '\n' || source[i] == '\r')) {
            i++;
        }
        if (i != len) {
            goto lnt_root_reject;
        }
        return YEPTRIS_OK;
    }
    if (source[at] == 't' || source[at] == 'f' || source[at] == 'n') {
        const char* word = source[at] == 't' ? "true" : (source[at] == 'f' ? "false" : "null");
        size_t i = at;
        if (!yep_json_literal(source, len, &i, word)) {
            goto lnt_root_reject;
        }
        while (i < len &&
               (source[i] == ' ' || source[i] == '\t' || source[i] == '\n' || source[i] == '\r')) {
            i++;
        }
        if (i != len) {
            goto lnt_root_reject;
        }
        uint8_t k = source[at] == 'n' ? YEP_T_NULL : (source[at] == 't' ? YEP_T_TRUE : YEP_T_FALSE);
        if (!rec_put(&c, k, (uint32_t)at, (uint32_t)(i - at))) {
            goto lnt_root_mem;
        }
        return YEPTRIS_OK;
    }
    {
        size_t end = len;
        while (end > at && (source[end - 1] == ' ' || source[end - 1] == '\t' ||
                            source[end - 1] == '\n' || source[end - 1] == '\r')) {
            end--;
        }
        if (!rec_put(&c, YEP_T_NUM, (uint32_t)at, (uint32_t)(end - at))) {
            goto lnt_root_mem;
        }
        return YEPTRIS_OK;
    }

lnt_root_mem:
    yeptris_tape_free(tape);
    return YEPTRIS_ERROR_MEMORY;
lnt_root_reject:
    yeptris_tape_free(tape);
    yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, at,
                  "not strict JSON at byte %zu (lenient tape)", at);
    return YEPTRIS_ERROR_PARSE;
}

YEPTRIS_API YeptrisStatus yeptris_parse_json_tape(const char* source, size_t len,
                                                  yeptris_json_tape* tape) {
    if ((source == NULL && len != 0) || tape == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    memset(tape, 0, sizeof(*tape));
    if (len > 0xFFFFFFF0u) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "tape spans are uint32; document exceeds 4 GiB");
        return YEPTRIS_ERROR_ARG;
    }

    /* #474: the entry's full-input gate_scan pre-pass moves to the
     * reject path (the #342/#81 law on parse_json, now here): the
     * columns-primary fused walk runs clean_only — tabs and non-ASCII
     * bytes REJECT inline — so on accepted documents the NEON gate
     * scan (the profile's 5% lane) never runs. A walk reject re-gates:
     * the record walk and the encoding-owed decision read `gated` only
     * when actually taken, and every error precedence below is
     * unchanged (the validating sequence consumes the same bit). */
    int gated;
    size_t off = 0;
    while (off < len && (source[off] == ' ' || source[off] == '\t' || source[off] == '\n' ||
                         source[off] == '\r')) {
        off++;
    }
    int container = off < len && (source[off] == '[' || source[off] == '{');
    if (container) {
        /* the columns-primary fused walk: one pass, columns eager
         * by construction, strict numbers classified at walk time */
        if (yep_tape_walk_columns_fused(source, len, off, tape, 1, 1) == YEPTRIS_OK) {
            return YEPTRIS_OK;
        }
    }
    gated = len > 0 && yep_text_active()->gate_scan(source, len);
    if (container && !gated) {
        /* the token-contract route: stage 1 (vector indexes) + the
         * lean walk. Measured BEHIND the fused walk on json-doc
         * (417-426 vs 526-533: stage 1 at 1408 MB/s + a walk whose
         * arms carry the same grammar cost) — gated OFF until the
         * sized cuts land (the op-table classifier trick, the
         * specialized map/seq loops, stage-1 string closes). The
         * fused walk is the shipped route; the law. */
#if YEP_TOKEN_CONTRACT_ROUTE
        uint32_t* idx = (uint32_t*)malloc((len + 2) * sizeof(uint32_t));
        if (idx == NULL) {
            return YEPTRIS_ERROR_MEMORY;
        }
        size_t nidx = 0;
        unsigned sflags = 0;
        if (yep_text_active()->json_stage1(source, len, idx, &nidx, &sflags)) {
            YeptrisStatus st = tape_walk_idx(source, len, off, idx, nidx, tape);
            free(idx);
            if (st != YEPTRIS_ERROR_PARSE) {
                return st; /* OK or MEMORY; a reject falls through */
            }
        } else {
            free(idx);
        }
#endif
        /* the record walk — the DOM route's exact call and flags
         * (strict numbers at record time). Non-ASCII documents stay
         * out of it: the gate above keeps today's routing, where the
         * validated lenient walk below owns them. */
        YeptrisStatus st = tape_walk(source, len, off, tape, 1);
        if (st != YEPTRIS_ERROR_PARSE) {
            return st; /* OK or MEMORY; a reject falls through */
        }
    }

    /* The validating sequence — parse_json's fallback, byte for byte
     * in precedence: grammar, then encoding when the gate tripped. */
    size_t verr = 0;
    if (!yep_json_document(source, len, &verr)) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, verr,
                      "not strict JSON at byte %zu", verr);
        return YEPTRIS_ERROR_PARSE;
    }
    if (gated) {
        size_t uerr = 0;
        if (!yep_utf8_validate((const unsigned char*)source, len, &uerr)) {
            yep_error_set(yep_error_tls(), YEP_ERR_ENCODING, 0, 0, uerr,
                          "ill-formed UTF-8 at byte %zu", uerr);
            return YEPTRIS_ERROR_ENCODING;
        }
    }

    size_t at = 0;
    while (at < len &&
           (source[at] == ' ' || source[at] == '\t' || source[at] == '\n' || source[at] == '\r')) {
        at++;
    }
    if (at < len && (source[at] == '[' || source[at] == '{')) {
        YeptrisStatus st = tape_walk(source, len, at, tape, 0);
        if (st == YEPTRIS_OK || st == YEPTRIS_ERROR_MEMORY) {
            return st;
        }
        yep_error_set(yep_error_tls(), YEP_ERR_INTERNAL, 0, 0, at,
                      "tape walk rejected a validated document at byte %zu", at);
        return YEPTRIS_ERROR_INTERNAL;
    }
    if (tape_carve(tape, len) != YEPTRIS_OK) {
        return YEPTRIS_ERROR_MEMORY;
    }
    tape->_src = source;
    tape->_srclen = len;
    tape_ctx c = {tape, len + 2, {0}, 0, 0};
    if (!rec_put(&c, YEP_T_DOC, 0, 0) || !tape_put_root_scalar(&c, source, len, at)) {
        yep_error_set(yep_error_tls(), YEP_ERR_INTERNAL, 0, 0, at,
                      "tape scalar conversion rejected a validated root at byte %zu", at);
        yeptris_tape_free(tape);
        return YEPTRIS_ERROR_INTERNAL;
    }
    return YEPTRIS_OK;
}

YEPTRIS_API size_t yeptris_tape_convert(yeptris_json_tape* t, size_t from, size_t to,
                                        int64_t* ivals, double* dvals) {
    yeptris_tape_columns(t);
    if (t == NULL || t->_src == NULL || from > to || to > t->count) {
        return SIZE_MAX;
    }
    const char* p = (const char*)t->_src;
    size_t n = 0;
    for (size_t i = from; i < to; i++) {
        uint8_t k = t->kinds[i];
        if (k != YEP_T_INT && k != YEP_T_FLOAT && k != YEP_T_NUM) {
            continue;
        }
        size_t j = 0;
        int64_t iv = 0;
        double dv = 0;
        int shape = 0;
        if (!yep_json_number_scan(p + t->offs[i], t->lens[i], &j, &shape, &iv, &dv) ||
            j != t->lens[i]) {
            /* full-span consumption: strict spans always re-scan whole
             * (grammar-validated at parse); a lenient NUM run that
             * stops early carries non-number bytes — the drain-side
             * reject the deferred contract promises */
            return SIZE_MAX;
        }
        if (shape == 0) {
            if (ivals != NULL) {
                ivals[i] = iv;
            }
        } else {
            if (shape == 2) {
                t->int_min = 1;
            }
            if (dvals != NULL) {
                dvals[i] = dv;
            }
        }
        n++;
    }
    return n;
}

YEPTRIS_API void yeptris_tape_free(yeptris_json_tape* tape) {
    if (tape == NULL) {
        return;
    }
    yep_free(yep_system_allocator(), tape->_block);
    memset(tape, 0, sizeof(*tape));
}
