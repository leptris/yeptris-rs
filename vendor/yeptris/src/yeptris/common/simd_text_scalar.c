/* simd_text_scalar.c — the reference truth for every kernel.
 *
 * Deliberately naive: the differential suite compares the active ISA table
 * AND these functions against independently-written naive references in
 * the test, so a shared bug between ISA and scalar code cannot hide.
 */

#include <string.h>

#include "simd_text.h"

int yep_text_contains_scalar(const char* s, size_t len, char c) {
    return yep_text_find_scalar(s, len, c) >= 0;
}

ptrdiff_t yep_text_find_scalar(const char* s, size_t len, char c) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == c) {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

ptrdiff_t yep_text_find3_scalar(const char* s, size_t len, char c0, char c1, char c2) {
    if (len < 3) {
        return -1;
    }
    for (size_t i = 0; i + 2 < len; i++) {
        if (s[i] == c0 && s[i + 1] == c1 && s[i + 2] == c2) {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

size_t yep_text_count_char_scalar(const char* s, size_t len, char c) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        n += (s[i] == c);
    }
    return n;
}

void yep_text_count3_scalar(const char* s, size_t len, char c0, char c1, char c2, size_t* n0,
                            size_t* n1, size_t* n2) {
    size_t a = 0, b = 0, d = 0;
    for (size_t i = 0; i < len; i++) {
        a += (s[i] == c0);
        b += (s[i] == c1);
        d += (s[i] == c2);
    }
    *n0 = a;
    *n1 = b;
    *n2 = d;
}

void yep_text_copy_count3_scalar(char* dst, const char* src, size_t len, char c0, char c1, char c2,
                                 size_t* n0, size_t* n1, size_t* n2) {
    /* len 0 with NULL dst (count-only callers) must not reach memcpy:
     * the nonnull attribute makes the call UB even for size 0 */
    if (len == 0) {
        *n0 = *n1 = *n2 = 0;
        return;
    }
    memcpy(dst, src, len);
    yep_text_count3_scalar(dst, len, c0, c1, c2, n0, n1, n2);
}

ptrdiff_t yep_text_find_not_scalar(const char* s, size_t len, char c) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] != c) {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

ptrdiff_t yep_text_stopset_find_scalar(const yep_stopset* ss, const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (yep_stopset_test(ss->bitmap, (unsigned char)s[i])) {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

ptrdiff_t yep_text_qbc_find_scalar(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c < 0x20) {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

ptrdiff_t yep_text_quote_scan_scalar(const char* s, size_t len, char q, int* has_escape) {
    int esc = 0;
    for (size_t i = 0; i < len; i++) {
        if (q == '"' && s[i] == '\\') {
            esc = 1;
            i++; /* skip the escaped byte (a trailing lone backslash ends scanning) */
            continue;
        }
        if (s[i] == q) {
            if (q == '\'' && i + 1 < len && s[i + 1] == '\'') {
                esc = 1;
                i++; /* doubled quote inside a single-quoted scalar */
                continue;
            }
            if (has_escape != NULL) {
                *has_escape = esc;
            }
            return (ptrdiff_t)i;
        }
    }
    if (has_escape != NULL) {
        *has_escape = esc;
    }
    return -1;
}

void yep_text_scan_stats_scalar(const char* s, size_t len, yep_text_stats* out) {
    if (s == NULL || len == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }

    yep_text_stats st = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            st.nonascii = 1;
            continue;
        }
        switch (c) {
        case '\n':
            st.nl++;
            break;
        case ',':
            st.comma++;
            break;
        case '-':
            st.dash++;
            break;
        case ':':
            st.colon++;
            break;
        case '[':
            st.bracket++;
            break;
        case '{':
            st.brace++;
            break;
        case '"':
            st.dq++;
            break;
        case '\'':
            st.sq++;
            break;
        case '|':
            st.pipe++;
            break;
        case '&':
            st.amp++;
            break;
        default:
            break;
        }
        if ((c < 0x20 && c != 9 && c != 10 && c != 13) || c == 0x7F) {
            st.bad_printable = 1;
        }
    }
    *out = st;
}

int yep_text_gate_scan_scalar(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 0x20 && c <= 0x7E) || c == 0x09 || c == 0x0A || c == 0x0D)) {
            return 1;
        }
    }
    return 0;
}

/* yep_swar_eq8 lives in simd_text.h — shared with the walker's
 * fused skips (TODO.restructure/86 wave 2). */

/* Eight bytes per step, one data-dependent exit (the break's chunk):
 * the byte-at-a-time trio cost ~60 cycles/line in branch mispredicts
 * alone on the short-line shapes (anchor-heavy's hottest symbol).
 * cap bounds the walk: 0 returns when no break sits within cap bytes
 * (a long line — the caller sweeps) so the ISA kernels use this as
 * BOTH the short-line test and the short-line answer (the previous
 * shape paid a 32-byte probe and then rescanned the same line). */
int yep_text_line_facts_capped(const char* s, size_t len, size_t pos, size_t cap,
                               yep_line_facts* out) {
    static const uint64_t k_nl = 0x0A0A0A0A0A0A0A0Aull, k_cr = 0x0D0D0D0D0D0D0D0Dull,
                          k_sp = 0x2020202020202020ull, k_co = 0x3A3A3A3A3A3A3A3Aull,
                          k_ha = 0x2323232323232323ull;
    size_t whole = len - pos;
    size_t n = whole < cap ? whole : cap;
    size_t end = n, indent = n, stop = n;
    int have_indent = 0, stop_set = 0, have_end = 0;
    size_t i = 0;
    while (i < n) {
        size_t avail = n - i < 8 ? n - i : 8;
        uint64_t x;
        if (avail == 8) {
            memcpy(&x, s + pos + i, 8);
        } else {
            char buf[8] = {0, 0, 0, 0, 0, 0, 0, 0}; /* NUL padding: matches no fact byte */
            memcpy(buf, s + pos + i, avail);
            memcpy(&x, buf, 8);
        }
        uint64_t valid = avail == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (avail * 8)) - 1);
        uint64_t br = (yep_swar_eq8(x, k_nl) | yep_swar_eq8(x, k_cr)) & valid;
        /* flags strictly below the FIRST break (br - 1 alone keeps the
         * higher break flags of the same chunk) */
        uint64_t room = (br ? ((br & (~br + 1)) - 1) : valid) & YEP_SWAR_FLAGS & valid;
        if (!have_indent) {
            uint64_t nons = ~(yep_swar_eq8(x, k_sp)) & room;
            if (nons) {
                indent = i + (size_t)(yep_ctz64(nons) >> 3);
                have_indent = 1;
            }
        }
        if (have_indent && !stop_set) {
            uint64_t stm = (yep_swar_eq8(x, k_co) | yep_swar_eq8(x, k_ha)) & room;
            if (stm) {
                size_t cand = i + (size_t)(yep_ctz64(stm) >> 3);
                if (cand >= indent) { /* a stop byte cannot precede the first non-space */
                    stop = cand;
                    stop_set = 1;
                }
            }
        }
        if (br) {
            end = i + (size_t)(yep_ctz64(br) >> 3);
            have_end = 1;
            break;
        }
        i += 8;
    }
    if (!have_end && whole > cap) {
        return 0; /* the line continues past the cap: the caller sweeps */
    }
    if (!have_indent) {
        indent = end; /* spaces ran to the break */
    }
    if (!stop_set) {
        stop = end;
    }
    out->end = (uint32_t)(pos + end);
    out->indent = (uint32_t)(pos + indent);
    out->stop = (uint32_t)(pos + stop);
    out->stop_set = stop_set;
    return 1;
}

void yep_text_line_facts_scalar(const char* s, size_t len, size_t pos, yep_line_facts* out) {
    (void)yep_text_line_facts_capped(s, len, pos, (size_t)-1, out);
}

yep_chunk_masks yep_text_json_chunk_scalar(const char* p, size_t n) {
    yep_chunk_masks m = {0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        uint32_t bit = 1u << i;
        m.valid |= bit;
        if (c == '"') {
            m.quote |= bit;
        } else if (c == '\\') {
            m.bs |= bit;
        } else if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':') {
            m.structurals |= bit;
        } else if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
            m.valstart |= bit;
        } else if (c < 0x20) {
            m.c0 |= bit;
        }
    }
    return m;
}

/* the stage-1 scalar reference lives in scan/json.c (the grammar
 * home); the table carries it so dispatch stays one point */
extern int yep_json_stage1_scalar(const char* p, size_t len, uint32_t* idx, size_t* nidx,
                                  unsigned* flags);
extern int yep_json_stage1_masks_scalar(const char* p, size_t len, struct yep_s1_block* blocks,
                                        size_t* nblocks, unsigned* flags);

const yep_text_kernels yep_text_kernels_scalar = {
    yep_text_contains_scalar,     yep_text_find_scalar,         yep_text_find3_scalar,
    yep_text_count_char_scalar,   yep_text_count3_scalar,       yep_text_copy_count3_scalar,
    yep_text_find_not_scalar,     yep_text_stopset_find_scalar, yep_text_quote_scan_scalar,
    yep_text_scan_stats_scalar,   yep_text_qbc_find_scalar,     yep_text_gate_scan_scalar,
    yep_text_line_facts_scalar,   yep_text_json_chunk_scalar,   yep_json_stage1_scalar,
    yep_json_stage1_masks_scalar,
};
