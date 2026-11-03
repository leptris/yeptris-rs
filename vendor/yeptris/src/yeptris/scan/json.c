/* json.c — the strict-JSON grammar (RFC 8259), TODO.impl/08C/21.
 *
 * MECE: this is the ONLY place that decides what strict JSON is —
 * the engine's flow fast path (parse/engine.c) and the whole-input
 * JSON mode (yeptris_parse_json) both consume these primitives; the
 * grammar never diverges between them.
 */

#include <stdint.h>
#include <string.h>

#include "common/chartype.h"
#include "common/simd_text.h"

#include "parse/numbers.h"
#include "parse/scalars.h"
#include "scan/json.h"

/* ---- whole-document strict validation (JSON mode) ----
 *
 * optional ws, one JSON value, optional ws, EOF. err (may be NULL)
 * receives the offset of the first violation. Depth is bounded by the
 * shared engine limit; exceeding it fails validation like any other
 * violation. */

#define JX_MAX_DEPTH 256

enum { JV_VALUE_OR_CLOSE = 0, JV_VALUE, JV_KEY_OR_CLOSE, JV_KEY, JV_COLON, JV_COMMA_OR_CLOSE };

static int jv_ws(const char* p, size_t len, size_t* i) {
    while (*i < len) {
        char c = p[*i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            (*i)++;
            continue;
        }
        return 1;
    }
    return 0; /* EOF */
}

int yep_json_document(const char* p, size_t len, size_t* err) {
    size_t i = 0;
    if (err != NULL) {
        *err = 0;
    }
    if (!jv_ws(p, len, &i)) {
        if (err != NULL) {
            *err = i;
        }
        return 0; /* empty is not a JSON document */
    }
    uint8_t kind[JX_MAX_DEPTH];
    uint8_t expect[JX_MAX_DEPTH];
    int depth = 0;
    /* virtual root frame: expects exactly one value */
    kind[0] = 0;
    expect[0] = JV_VALUE;
    depth = 1;
    int root_done = 0;
    for (;;) {
        if (!jv_ws(p, len, &i)) {
            if (err != NULL) {
                *err = i;
            }
            return 0; /* EOF inside a value */
        }
        char c = p[i];
        if (c == ']' || c == '}') {
            int want = (c == ']') ? 0 : 1;
            if (depth == 1 || kind[depth - 1] != want ||
                (expect[depth - 1] != JV_VALUE_OR_CLOSE && expect[depth - 1] != JV_KEY_OR_CLOSE &&
                 expect[depth - 1] != JV_COMMA_OR_CLOSE)) {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
            depth--;
            if (depth == 1) {
                /* closed the root collection */
                root_done = 1;
                i++;
                break;
            }
            expect[depth - 1] = JV_COMMA_OR_CLOSE;
            i++;
            continue;
        }
        if (root_done) {
            if (err != NULL) {
                *err = i;
            }
            return 0; /* trailing content */
        }
        switch (expect[depth - 1]) {
        case JV_COLON:
            if (c != ':') {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
            expect[depth - 1] = JV_VALUE;
            i++;
            continue;
        case JV_COMMA_OR_CLOSE:
            if (c != ',') {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
            expect[depth - 1] = kind[depth - 1] ? JV_KEY : JV_VALUE;
            i++;
            continue;
        default:
            break;
        }
        if (c == '{' || c == '[') {
            if (depth >= JX_MAX_DEPTH) {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
            if (kind[depth - 1] == 1 &&
                (expect[depth - 1] == JV_KEY_OR_CLOSE || expect[depth - 1] == JV_KEY)) {
                if (err != NULL) {
                    *err = i;
                }
                return 0; /* strict JSON: a container is not a map key */
            }
            kind[depth] = (c == '[') ? 0 : 1;
            expect[depth] = kind[depth] ? JV_KEY_OR_CLOSE : JV_VALUE_OR_CLOSE;
            depth++;
            i++;
            continue;
        }
        int he = 0;
        size_t close;
        if (c == '"') {
            if (!yep_json_string(p, len, &i, &close, &he)) {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            if (!yep_json_number(p, len, &i)) {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
        } else if (c == 't') {
            if (!yep_json_literal(p, len, &i, "true")) {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
        } else if (c == 'f') {
            if (!yep_json_literal(p, len, &i, "false")) {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
        } else if (c == 'n') {
            if (!yep_json_literal(p, len, &i, "null")) {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
        } else {
            if (err != NULL) {
                *err = i;
            }
            return 0;
        }
        if (depth == 1) {
            root_done = 1;
            break; /* scalar root */
        }
        if (kind[depth - 1] == 1 && expect[depth - 1] == JV_KEY_OR_CLOSE) {
            if (c != '"') {
                if (err != NULL) {
                    *err = i;
                }
                return 0; /* JSON map keys are strings only */
            }
            expect[depth - 1] = JV_COLON;
        } else if (kind[depth - 1] == 1 && expect[depth - 1] == JV_KEY) {
            if (c != '"') {
                if (err != NULL) {
                    *err = i;
                }
                return 0;
            }
            expect[depth - 1] = JV_COLON;
        } else {
            expect[depth - 1] = JV_COMMA_OR_CLOSE;
        }
    }
    /* trailing whitespace only, and then the input must end */
    while (i < len) {
        char c = p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
            continue;
        }
        if (err != NULL) {
            *err = i;
        }
        return 0;
    }
    return 1;
}

/* ---- token primitives (shared with the engine fast path) ---- */

int yep_json_ws(const char* p, size_t len, size_t* i, int* saw_tab) {
    while (*i < len) {
        char c = p[*i];
        if (c == ' ' || c == '\n' || c == '\r') {
            (*i)++;
            continue;
        }
        if (c == '\t') {
            *saw_tab = 1; /* tabs: let the general kernel rule on them */
            return 0;
        }
        return 1; /* token byte */
    }
    return -1; /* EOF */
}

/* Strict RFC 8259 number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?
 *
 * THE grammar walk, fused with conversion (TODO.restructure/26): one
 * pass validates and converts — digits accumulate into the uint64
 * magnitude with overflow promotion, so integer JSON numbers (the
 * hot case) never get a second scan. Any '.'/'e' marks the value a
 * float; the CONVERTER for an already-validated span stays the
 * number-kernel SSOT (yep_num_f64) — grammar here, conversion there,
 * exactly one of each. Out params may be NULL (pure validation). */
int yep_json_number_scan(const char* p, size_t len, size_t* i, int* is_float, int64_t* iv,
                         double* dv) {
    size_t k = *i;
    int neg = 0;
    if (k >= len) {
        return 0; /* the caller's index is at the end: p[len] is OOB */
    }
    if (p[k] == '-') {
        neg = 1;
        k++;
    }
    if (k >= len || p[k] < '0' || p[k] > '9') {
        return 0;
    }
    /* Short-integer fast path (the json-doc corpus's dominant shape —
     * 1-3 digit ids/vals). Three digits max so the magnitude fits a
     * u16 multiply chain with no overflow check; the float/exponent
     * arms and the 8-digit SWAR fall through untouched. The trailing
     * delimiter rule is the same contract the slow path enforces. */
    {
        size_t d0 = k;
        if (p[k] != '0') {
            unsigned mag32 = (unsigned)(p[k] - '0');
            size_t n = 1;
            while (n < 3 && d0 + n < len && p[d0 + n] >= '0' && p[d0 + n] <= '9') {
                mag32 = mag32 * 10u + (unsigned)(p[d0 + n] - '0');
                n++;
            }
            size_t end = d0 + n;
            if (end >= len || (p[end] != '.' && p[end] != 'e' && p[end] != 'E' && p[end] != '0' &&
                               (p[end] < '1' || p[end] > '9'))) {
                /* no more digits and not a float/exponent — settle */
                if (end < len) {
                    char c = p[end];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',' && c != ']' &&
                        c != '}' && c != ':') {
                        return 0; /* "1x" is YAML, not JSON */
                    }
                }
                if (is_float != NULL) {
                    *is_float = 0;
                }
                if (iv != NULL) {
                    *iv = neg ? -(int64_t)mag32 : (int64_t)mag32;
                }
                *i = end;
                return 1;
            }
            /* more digits or float text: fall through to the full walk */
        } else {
            /* lone zero (or 0. / 0e — fall through) */
            size_t end = k + 1;
            if (end >= len || (p[end] != '.' && p[end] != 'e' && p[end] != 'E' &&
                               (p[end] < '0' || p[end] > '9'))) {
                if (end < len) {
                    char c = p[end];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',' && c != ']' &&
                        c != '}' && c != ':') {
                        return 0;
                    }
                }
                /* leading-zero reject is the full walk's job: "01" falls
                 * through (end points at '1') and the slow path rejects */
                if (is_float != NULL) {
                    *is_float = 0;
                }
                if (iv != NULL) {
                    *iv = 0; /* -0 is still 0 as int64 */
                }
                *i = end;
                return 1;
            }
        }
    }
    uint64_t mag = 0;
    int overflow = 0;
    if (p[k] == '0') {
        k++;
    } else {
        /* simdjson's eight-digits-at-once SWAR (their
         * parse_eight_digits_unrolled, verbatim constants): eight
         * branch-free ops replace eight iterations of the mul/add
         * loop. Fires only while eight digits AND the magnitude can
         * absorb them exactly — the per-digit tail keeps the overflow
         * promotion contract. Short numbers (the corpus's 1-6 digit
         * ids) stay on the original loop. */
        while (k + 8 <= len) {
            uint64_t w;
            memcpy(&w, p + k, 8);
            uint64_t t = w ^ 0x3030303030303030ull;
            uint64_t hi = t & 0xF0F0F0F0F0F0F0F0ull;
            uint64_t lo =
                (((t & 0x0F0F0F0F0F0F0F0Full) + 0x0606060606060606ull) & 0x1010101010101010ull);
            if (hi != 0 || lo != 0) {
                break; /* a non-digit byte inside the window */
            }
            if (mag > (UINT64_MAX - 99999999ull) / 100000000ull) {
                break; /* 8 more digits may not fit: the slow tail promotes */
            }
            uint64_t v = w - 0x3030303030303030ull;
            v = (v * 10) + (v >> 8);
            const uint64_t mask = 0x000000FF000000FF;
            const uint64_t mul1 = 0x000F424000000064;
            const uint64_t mul2 = 0x0000271000000001;
            v = (((v & mask) * mul1) + (((v >> 16) & mask) * mul2)) >> 32;
            mag = mag * 100000000ull + v;
            k += 8;
        }
        while (k < len && p[k] >= '0' && p[k] <= '9') {
            unsigned d = (unsigned)(p[k] - '0');
            if (mag > (UINT64_MAX - d) / 10) {
                overflow = 1; /* keep scanning the grammar; promote */
            } else {
                mag = mag * 10 + d;
            }
            k++;
        }
    }
    int flt = 0;
    if (k < len && p[k] == '.') {
        flt = 1;
        k++;
        if (k >= len || p[k] < '0' || p[k] > '9') {
            return 0;
        }
        while (k < len && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
    }
    if (k < len && (p[k] == 'e' || p[k] == 'E')) {
        flt = 1;
        k++;
        if (k < len && (p[k] == '-' || p[k] == '+')) {
            k++;
        }
        if (k >= len || p[k] < '0' || p[k] > '9') {
            return 0;
        }
        while (k < len && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
    }
    if (k < len) {
        char c = p[k];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',' && c != ']' && c != '}' &&
            c != ':') {
            return 0; /* "1x" is YAML, not JSON */
        }
    }
    /* promotion first, outputs once. is_float reports TEXT shape:
     *   0 = integer, iv exact (INT64_MIN included)
     *   1 = float text (dot/exponent), dv converted
     *   2 = integer text beyond int64 — dv carries the approximate
     *       double; exact-Bignum hosts rebuild from the span
     * (overflow tracks uint64 saturation; the int64 bound is the
     * value test below) */
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (flt) {
        if (is_float != NULL) {
            *is_float = 1;
        }
        if (dv != NULL) {
            yep_num_f64(p + *i, (uint32_t)(k - *i), dv);
        }
    } else if (overflow || mag > limit) {
        if (is_float != NULL) {
            *is_float = 2;
        }
        if (dv != NULL) {
            yep_num_f64(p + *i, (uint32_t)(k - *i), dv);
        }
    } else {
        if (is_float != NULL) {
            *is_float = 0;
        }
        if (iv != NULL) {
            *iv = (neg && mag == (uint64_t)INT64_MAX + 1) ? INT64_MIN
                                                          : (neg ? -(int64_t)mag : (int64_t)mag);
        }
    }
    *i = k;
    return 1;
}

int yep_json_number(const char* p, size_t len, size_t* i) {
    return yep_json_number_scan(p, len, i, NULL, NULL, NULL);
}

/* The lazy tape's number arm: validate the grammar and report the
 * TEXT shape (0 int / 1 float) with NO magnitude accumulation — ints
 * skip the SWAR entirely; conversion happens at materialize through
 * yep_json_number_scan. Same accepts, same rejects, same advance as
 * the full scan (the 2M tape-diff oracle pins the equivalence). */
int yep_json_number_shape(const char* p, size_t len, size_t* i, int* is_float) {
    size_t k = *i;
    if (k >= len) {
        return 0; /* the caller's index is at the end: p[len] is OOB */
    }
    if (p[k] == '-') {
        k++;
    }
    if (k >= len || p[k] < '0' || p[k] > '9') {
        return 0;
    }
    if (p[k] == '0') {
        k++; /* a leading zero admits no digit after it ("01" dies at
                the delimiter rule, exactly like the full scan) */
    } else {
        while (k < len && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
    }
    int flt = 0;
    if (k < len && p[k] == '.') {
        flt = 1;
        k++;
        if (k >= len || p[k] < '0' || p[k] > '9') {
            return 0;
        }
        while (k < len && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
    }
    if (k < len && (p[k] == 'e' || p[k] == 'E')) {
        flt = 1;
        k++;
        if (k < len && (p[k] == '-' || p[k] == '+')) {
            k++;
        }
        if (k >= len || p[k] < '0' || p[k] > '9') {
            return 0;
        }
        while (k < len && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
    }
    if (k < len) {
        char c = p[k];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',' && c != ']' && c != '}' &&
            c != ':') {
            return 0; /* "1x" is YAML, not JSON */
        }
    }
    *is_float = flt;
    *i = k;
    return 1;
}

int yep_json_literal(const char* p, size_t len, size_t* i, const char* word) {
    size_t w = 0;
    while (word[w] != '\0') {
        if (*i + w >= len || p[*i + w] != word[w]) {
            return 0;
        }
        w++;
    }
    *i += w;
    if (*i < len) {
        char c = p[*i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',' && c != ']' && c != '}' &&
            c != ':') {
            return 0; /* "truex" is a YAML plain, not JSON */
        }
    }
    return 1;
}

/* Strict-JSON quoted scalar: no raw breaks, only JSON escapes. The
 * qbc_find kernel (first '"', '\\', or C0) decides close/escape/
 * reject — one SIMD pass, no second scan. On success *i sits just
 * past the close, *close_out is the close quote index, *has_esc
 * reports backslashes. */
/* The escape grammar, ONE authority for the string scans (the
 * TODO.restructure/47 index descent was measured dead and reverted;
 * re-exporting this is a one-liner if a consumer ever returns). */
static int yep_json_escape(const char* p, size_t len, size_t at, size_t* next) {
    if (at + 1 >= len) {
        return 0;
    }
    char e2 = p[at + 1];
    if (e2 == 'u') {
        for (size_t h = 2; h <= 5; h++) {
            if (at + h >= len || !yep_ct_is((unsigned char)p[at + h], YEP_CT_HEXDIGIT)) {
                return 0;
            }
        }
        uint32_t cp = 0;
        for (size_t h = 2; h <= 5; h++) {
            cp = (cp << 4) | (uint32_t)hexval((unsigned char)p[at + h]);
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* high surrogate must pair */
            if (at + 12 >= len || p[at + 6] != '\\' || p[at + 7] != 'u') {
                return 0;
            }
            uint32_t lo = 0;
            for (size_t h = 8; h <= 11; h++) {
                if (!yep_ct_is((unsigned char)p[at + h], YEP_CT_HEXDIGIT)) {
                    return 0;
                }
                lo = (lo << 4) | (uint32_t)hexval((unsigned char)p[at + h]);
            }
            if (lo < 0xDC00 || lo > 0xDFFF) {
                return 0;
            }
            *next = at + 12;
            return 1;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return 0; /* lone low surrogate */
        }
        *next = at + 6;
        return 1;
    }
    if (e2 != '"' && e2 != '\\' && e2 != '/' && e2 != 'b' && e2 != 'f' && e2 != 'n' && e2 != 'r' &&
        e2 != 't') {
        return 0; /* YAML-only escape (\a, \x…): not JSON */
    }
    *next = at + 2;
    return 1;
}

int yep_json_string(const char* p, size_t len, size_t* i, size_t* close_out, int* has_esc) {
    const yep_text_kernels* k = yep_text_active();
    size_t j = *i + 1;
    int esc = 0;
    /* Short-string fast path: the json-doc corpus's keys/values run
     * 1-8 bytes; settling them scalar skips the kernel dispatch and
     * the vector prologue. A backslash falls through to the vector
     * loop (the escape grammar stays there, single-sourced). */
    {
        size_t lim = len - j;
        if (lim > 8) {
            lim = 8;
        }
        for (size_t w = 0; w < lim; w++) { /* w: not k — MSVC C4456 */
            char c = p[j + w];
            if (c == '"') {
                *close_out = j + w;
                *has_esc = 0;
                *i = j + w + 1;
                return 1;
            }
            if (c == '\\' || (unsigned char)c < 0x20) {
                break; /* escape -> vector path; C0 -> it rejects */
            }
        }
    }
    for (;;) {
        /* the SIMD string-stop kernel (TODO.restructure/46): first
         * '"', '\\', or C0 — one vector pass, the milestone-55 LUT
         * lessons absent by construction. The byte at the hit
         * classifies: quote closes, backslash escapes, else reject. */
        ptrdiff_t r = k->qbc_find(p + j, len - j);
        if (r < 0) {
            return 0; /* unterminated */
        }
        size_t at = j + (size_t)r;
        char c = p[at];
        if (c == '"') {
            *close_out = at;
            *has_esc = esc;
            *i = at + 1;
            return 1;
        }
        if (c != '\\') {
            return 0; /* raw C0 control (incl. breaks) inside a string */
        }
        /* escape: the shared grammar authority (the index-driven
         * descents walk the same rules — TODO.restructure/47) */
        esc = 1;
        if (!yep_json_escape(p, len, at, &j)) {
            return 0;
        }
    }
}

/* ---- the span walker (TODO.restructure/53) ----
 * Ported verbatim from the engine's pass-1 state machine: the grammar
 * is UNCHANGED, it just lives where both consumers reach it. States
 * mirror the old JX_* set. */

#define JW_SIMPLE_KEY_MAX 1024 /* YAML 1.2 simple-key limit (libyaml parity) */

void yep_json_walk_init(yep_json_walk* w, const char* p, size_t len, size_t open, int max_depth) {
    w->p = p;
    w->len = len;
    w->max_depth = max_depth;
    w->i = open + 1;
    w->close = 0;
    w->long_key = 0;
    w->saw_tab = 0;
    w->depth = 1;
    w->kind[0] = (p[open] == '[') ? 0 : 1;
    w->expect[0] = w->kind[0] ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
    w->strict = 0;
}

yep_jw_status yep_json_walk_next(yep_json_walk* w, yep_json_tok* t) {
    const char* p = w->p;
    size_t len = w->len;
    for (;;) {
        if (yep_json_ws(p, len, &w->i, &w->saw_tab) != 1) {
            return YEP_JW_REJECT; /* EOF or a tab the general kernel judges */
        }
        size_t i = w->i;
        char c = p[i];
        if (c == ']' || c == '}') {
            int want = (c == ']') ? 0 : 1;
            if (w->kind[w->depth - 1] != want || (w->expect[w->depth - 1] != JW_VALUE_OR_CLOSE &&
                                                  w->expect[w->depth - 1] != JW_KEY_OR_CLOSE &&
                                                  w->expect[w->depth - 1] != JW_COMMA_OR_CLOSE)) {
                return YEP_JW_REJECT; /* mismatched or premature close */
            }
            t->at = i;
            t->end = i + 1;
            t->cls = c;
            t->has_escape = 0;
            w->depth--;
            w->i = i + 1;
            if (w->depth == 0) {
                w->close = i;
                return YEP_JW_DONE;
            }
            w->expect[w->depth - 1] = JW_COMMA_OR_CLOSE;
            return YEP_JW_OK;
        }
        switch (w->expect[w->depth - 1]) {
        case JW_COLON:
            if (c != ':') {
                return YEP_JW_REJECT;
            }
            w->expect[w->depth - 1] = JW_VALUE;
            w->i = i + 1;
            continue;
        case JW_COMMA_OR_CLOSE:
            if (c != ',') {
                return YEP_JW_REJECT;
            }
            w->expect[w->depth - 1] = w->kind[w->depth - 1] ? JW_KEY : JW_VALUE;
            w->i = i + 1;
            continue;
        default:
            break;
        }
        if (c == '{' || c == '[') {
            if (w->depth >= YEP_JSON_WALK_DEPTH || w->depth >= w->max_depth) {
                return YEP_JW_REJECT; /* the general kernel reports depth errors */
            }
            if (w->kind[w->depth - 1] == 1 &&
                (w->expect[w->depth - 1] == JW_KEY_OR_CLOSE || w->expect[w->depth - 1] == JW_KEY) &&
                w->strict) {
                return YEP_JW_REJECT; /* strict JSON: a container is not a map key */
            }
            t->at = i;
            t->end = i + 1;
            t->cls = c;
            t->has_escape = 0;
            w->kind[w->depth] = (c == '[') ? 0 : 1;
            w->expect[w->depth] = w->kind[w->depth] ? JW_KEY_OR_CLOSE : JW_VALUE_OR_CLOSE;
            w->depth++;
            w->i = i + 1;
            return YEP_JW_OK;
        }
        t->at = i;
        t->has_escape = 0;
        if (c == '"') {
            size_t close;
            if (!yep_json_string(p, len, &w->i, &close, &t->has_escape)) {
                return YEP_JW_REJECT;
            }
            t->cls = '"';
            t->end = w->i;
            if (w->kind[w->depth - 1] == 1 &&
                (w->expect[w->depth - 1] == JW_KEY_OR_CLOSE || w->expect[w->depth - 1] == JW_KEY) &&
                w->i - i > JW_SIMPLE_KEY_MAX) {
                w->long_key = 1;
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            /* the ONE number pass: validate, advance, convert, and
             * shape — the previous shape rode a second text walk and
             * value consumers (the tape) rescanned a third time */
            if (!yep_json_number_scan(p, len, &w->i, &t->nshape, &t->ival, &t->dval)) {
                return YEP_JW_REJECT;
            }
            t->cls = '#';
            t->end = w->i;
            t->is_float = (t->nshape == 1);
        } else if (c == 't' || c == 'f' || c == 'n') {
            const char* word = (c == 't') ? "true" : (c == 'f') ? "false" : "null";
            if (!yep_json_literal(p, len, &w->i, word)) {
                return YEP_JW_REJECT;
            }
            t->cls = 'a';
            t->end = w->i;
        } else {
            return YEP_JW_REJECT; /* YAML plain scalar / comment / indicator */
        }
        if (w->kind[w->depth - 1] == 1 && w->expect[w->depth - 1] == JW_KEY_OR_CLOSE) {
            if (w->strict && t->cls != '"') {
                return YEP_JW_REJECT; /* strict JSON: keys are strings */
            }
            w->expect[w->depth - 1] = JW_COLON;
        } else if (w->kind[w->depth - 1] == 1 && w->expect[w->depth - 1] == JW_KEY) {
            if (t->cls != '"') {
                return YEP_JW_REJECT; /* JSON map keys are strings only */
            }
            w->expect[w->depth - 1] = JW_COLON;
        } else {
            w->expect[w->depth - 1] = JW_COMMA_OR_CLOSE;
        }
        return YEP_JW_OK;
    }
}

/* ---- stage 1: the structural indexer (the token-contract front) ---
 * The scalar reference for the kernels table's json_stage1 slot: the
 * token positions stage 2 dispatches on — operators, string OPEN
 * quotes (escape-aware), and scalar-run starts — in position order.
 * Same accepts/rejects as the ISA kernels (the differential suite
 * pins it); no grammar validation, only the unterminated-string
 * parity error. The mask-level identity set (escape scanner via the
 * ODD_BITS borrow trick, prefix-xor string parity) is simdjson's. */

YEPTRIS_API int yep_json_stage1_scalar(const char* p, size_t len, uint32_t* idx, size_t* nidx,
                                       unsigned* flags) {
    size_t n = 0;
    uint64_t prev_in_string = 0, esc_carry = 0;
    for (size_t off = 0; off < len; off += 64) {
        size_t cn = len - off < 64 ? len - off : 64;
        uint64_t q = 0, bs = 0, op = 0, ws = 0, c0 = 0;
        for (size_t k = 0; k < cn; k++) {
            unsigned char c = (unsigned char)p[off + k];
            uint64_t bit = 1ull << k;
            if (c == '"') {
                q |= bit;
            } else if (c == '\\') {
                bs |= bit;
            } else if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':') {
                op |= bit;
            } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ws |= bit;
            }
            if (c < 0x20) {
                c0 |= bit;
            }
        }
        uint64_t valid = cn == 64 ? ~0ull : ((1ull << cn) - 1ull);
        n = yep_json_stage1_resolve(q, bs, op, ws, c0, valid, &prev_in_string, &esc_carry, off, idx,
                                    n, flags);
    }
    *nidx = n;
    return prev_in_string ? 0 : 1;
}

YEPTRIS_API int yep_json_stage1_masks_scalar(const char* p, size_t len, struct yep_s1_block* blocks,
                                             size_t* nblocks, unsigned* flags) {
    size_t nb = 0;
    uint64_t prev_in_string = 0, esc_carry = 0;
    for (size_t off = 0; off < len; off += 64) {
        size_t cn = len - off < 64 ? len - off : 64;
        uint64_t q = 0, bs = 0, op = 0, c0 = 0;
        for (size_t k = 0; k < cn; k++) {
            unsigned char c = (unsigned char)p[off + k];
            uint64_t bit = 1ull << k;
            if (c == '"') {
                q |= bit;
            } else if (c == '\\') {
                bs |= bit;
            } else if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':') {
                op |= bit;
            }
            if (c < 0x20) {
                c0 |= bit;
            }
        }
        uint64_t valid = cn == 64 ? ~0ull : ((1ull << cn) - 1ull);
        blocks[nb].q = q;
        blocks[nb].bs = bs;
        blocks[nb].op = op;
        blocks[nb].c0 = c0;
        blocks[nb].tokens =
            yep_json_stage1_tokens(q, bs, op, c0, valid, &prev_in_string, &esc_carry, flags);
        nb++;
    }
    *nblocks = nb;
    return prev_in_string ? 0 : 1;
}

/* The per-chunk classifier: the kernels table's json_chunk slot
 * (scalar reference in common, NEON/AVX2 TUs on their ISAs). */
