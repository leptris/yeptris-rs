/* simd_text_avx2.c — AVX2 kernels (TODO.impl/04).
 *
 * Compiled with -mavx2 -mpopcnt on x86 (src/CMakeLists.txt). Chunked
 * 32-byte processing with scalar tails: reads never pass len. The
 * differential suite proves equivalence with the scalar reference.
 *
 * Deferred (recorded in the item ledger): SIMD stopset_find returned
 * with TODO.restructure/68 — the nibble-class kernel above (the
 * campaign profiles made it the hottest kernel in the block paths).
 * Still scalar: the runtime-bitmap path never vectorized well; a
 * consumer that needs it can adopt the yep_stopset form.
 */

#include "port.h" /* defines YEP_ARCH_* — must precede the guard below */

#if defined(YEP_ARCH_X86)

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

#include "simd_text.h"

#define YEP_AVX2_CHUNK 32

static inline uint32_t yep_avx2_eq_mask(const char* p, char c) {
    __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)p);
    __m256i eq = _mm256_cmpeq_epi8(v, _mm256_set1_epi8(c));
    return (uint32_t)_mm256_movemask_epi8(eq);
}

static int yep_avx2_contains(const char* s, size_t len, char c) {
    size_t i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        if (yep_avx2_eq_mask(s + i, c)) {
            return 1;
        }
    }
    return yep_text_contains_scalar(s + i, len - i, c);
}

static ptrdiff_t yep_avx2_find(const char* s, size_t len, char c) {
    size_t i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        uint32_t m = yep_avx2_eq_mask(s + i, c);
        if (m) {
            return (ptrdiff_t)(i + (size_t)yep_ctz32(m));
        }
    }
    ptrdiff_t tail = yep_text_find_scalar(s + i, len - i, c);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

static size_t yep_avx2_count(const char* s, size_t len, char c) {
    size_t n = 0, i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        n += (size_t)yep_popcount32(yep_avx2_eq_mask(s + i, c));
    }
    return n + yep_text_count_char_scalar(s + i, len - i, c);
}

static void yep_avx2_count3(const char* s, size_t len, char c0, char c1, char c2, size_t* n0,
                            size_t* n1, size_t* n2) {
    size_t a = 0, b = 0, d = 0, i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        uint32_t m0 = yep_avx2_eq_mask(s + i, c0);
        uint32_t m1 = yep_avx2_eq_mask(s + i, c1);
        uint32_t m2 = yep_avx2_eq_mask(s + i, c2);
        a += (size_t)yep_popcount32(m0);
        b += (size_t)yep_popcount32(m1);
        d += (size_t)yep_popcount32(m2);
    }
    size_t ta = 0, tb = 0, td = 0;
    yep_text_count3_scalar(s + i, len - i, c0, c1, c2, &ta, &tb, &td);
    *n0 = a + ta;
    *n1 = b + tb;
    *n2 = d + td;
}

static void yep_avx2_copy_count3(char* dst, const char* src, size_t len, char c0, char c1, char c2,
                                 size_t* n0, size_t* n1, size_t* n2) {
    size_t a = 0, b = 0, d = 0, i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(src + i));
        _mm256_storeu_si256((__m256i*)(void*)(dst + i), v);
        __m256i s0 = _mm256_set1_epi8(c0), s1 = _mm256_set1_epi8(c1), s2 = _mm256_set1_epi8(c2);
        a += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, s0)));
        b += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, s1)));
        d += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, s2)));
    }
    if (i < len) {
        memcpy(dst + i, src + i, len - i);
    }
    size_t ta = 0, tb = 0, td = 0;
    if (i < len) { /* NULL dst + 0 offset is still UB to form */
        yep_text_count3_scalar(dst + i, len - i, c0, c1, c2, &ta, &tb, &td);
    }
    *n0 = a + ta;
    *n1 = b + tb;
    *n2 = d + td;
}

static ptrdiff_t yep_avx2_find_not(const char* s, size_t len, char c) {
    size_t i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        uint32_t m = yep_avx2_eq_mask(s + i, c) ^ 0xFFFFFFFFu;
        if (m) {
            return (ptrdiff_t)(i + (size_t)yep_ctz32(m));
        }
    }
    ptrdiff_t tail = yep_text_find_not_scalar(s + i, len - i, c);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

static ptrdiff_t yep_avx2_find3(const char* s, size_t len, char c0, char c1, char c2) {
    if (len < 3) {
        return -1;
    }
    size_t i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        uint32_t m = yep_avx2_eq_mask(s + i, c0);
        while (m) {
            size_t k = (size_t)yep_ctz32(m);
            size_t at = i + k;
            if (at + 2 < len && s[at + 1] == c1 && s[at + 2] == c2) {
                return (ptrdiff_t)at;
            }
            m &= m - 1;
        }
    }
    ptrdiff_t tail = yep_text_find3_scalar(s + i, len - i, c0, c1, c2);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

static ptrdiff_t yep_avx2_qbc_find(const char* s, size_t len) {
    size_t i = 0;
    const __m256i dq = _mm256_set1_epi8('"');
    const __m256i bs = _mm256_set1_epi8('\\');
    const __m256i c1f = _mm256_set1_epi8(0x1F);
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(s + i));
        /* unsigned v < 0x20 <=> min_epu8(v, 0x1F) == v */
        __m256i m =
            _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, dq), _mm256_cmpeq_epi8(v, bs)),
                            _mm256_cmpeq_epi8(_mm256_min_epu8(v, c1f), v));
        uint32_t bits = (uint32_t)_mm256_movemask_epi8(m);
        if (bits != 0) {
            return (ptrdiff_t)(i + (size_t)yep_ctz32(bits));
        }
    }
    ptrdiff_t r = yep_text_qbc_find_scalar(s + i, len - i);
    return r < 0 ? r : (ptrdiff_t)i + r;
}

static ptrdiff_t yep_avx2_quote_scan(const char* s, size_t len, char q, int* has_escape) {
    int esc = 0;
    size_t i = 0;
    while (i < len) {
        for (; i + YEP_AVX2_CHUNK <= len;) {
            uint32_t m = yep_avx2_eq_mask(s + i, q);
            if (q == '"') {
                m |= yep_avx2_eq_mask(s + i, '\\');
            }
            if (!m) {
                i += YEP_AVX2_CHUNK;
                continue;
            }
            size_t k = (size_t)yep_ctz32(m);
            if (q == '"' && s[i + k] == '\\') {
                esc = 1;
                i += k + 2; /* skip the escaped byte */
                break;
            }
            if (q == '\'' && i + k + 1 < len && s[i + k + 1] == '\'') {
                esc = 1;
                i += k + 2; /* doubled quote inside a single-quoted scalar */
                break;
            }
            if (has_escape != NULL) {
                *has_escape = esc;
            }
            return (ptrdiff_t)(i + k);
        }
        if (i >= len) {
            break; /* an escape at the boundary consumed the rest: unterminated
                    * (a lone '\' as the last byte pushed i past len; the tail
                    * call would underflow len - i and read out of bounds) */
        }
        if (i + YEP_AVX2_CHUNK > len) {
            int tail_esc = 0;
            ptrdiff_t r = yep_text_quote_scan_scalar(s + i, len - i, q, &tail_esc);
            esc |= tail_esc;
            if (r >= 0) {
                if (has_escape != NULL) {
                    *has_escape = esc;
                }
                return (ptrdiff_t)i + r;
            }
            break;
        }
    }
    if (has_escape != NULL) {
        *has_escape = esc;
    }
    return -1;
}

static void yep_avx2_scan_stats(const char* s, size_t len, yep_text_stats* out) {
    if (s == NULL || len == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }

    const __m256i ktab = _mm256_set1_epi8('\n'), kcomma = _mm256_set1_epi8(','),
                  kdash = _mm256_set1_epi8('-'), kcolon = _mm256_set1_epi8(':'),
                  kbrk = _mm256_set1_epi8('['), kbrce = _mm256_set1_epi8('{'),
                  kdq = _mm256_set1_epi8('"'), ksq = _mm256_set1_epi8('\''),
                  kpipe = _mm256_set1_epi8('|'), kamp = _mm256_set1_epi8('&');
    const __m256i kflip = _mm256_set1_epi8((char)(unsigned char)0x80),
                  klow = _mm256_set1_epi8((char)(unsigned char)0xA0),
                  ktab9 = _mm256_set1_epi8('\t'), klf = _mm256_set1_epi8('\n'),
                  kcr = _mm256_set1_epi8('\r'), kdel = _mm256_set1_epi8((char)(unsigned char)0x7F);
    size_t c_nl = 0, c_co = 0, c_da = 0, c_cl = 0, c_br = 0, c_bc = 0, c_dq = 0, c_sq = 0, c_pi = 0,
           c_am = 0;
    uint32_t bad = 0, hi = 0;
    size_t i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        const __m256i* pp = (const __m256i*)(const void*)(s + i);
        __m256i v = _mm256_loadu_si256(pp);
        hi |= (uint32_t)_mm256_movemask_epi8(v); /* sign bit = bit7 = non-ASCII */
        c_nl += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, ktab)));
        c_co +=
            (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kcomma)));
        c_da += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kdash)));
        c_cl +=
            (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kcolon)));
        c_br += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kbrk)));
        c_bc += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kbrce)));
        c_dq += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kdq)));
        c_sq += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, ksq)));
        c_pi += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kpipe)));
        c_am += (size_t)yep_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kamp)));
        /* c-printable ASCII violations: b < 0x20 (signed trick: flip
         * the sign bit, then b < 0x20 <=> w < 0xA0), except TAB/LF/CR;
         * plus DEL. Non-ASCII is tracked separately in `hi`. */
        __m256i flipped = _mm256_xor_si256(v, kflip);
        __m256i lo = _mm256_cmpgt_epi8(klow, flipped); /* w < 0xA0 signed */
        __m256i allowed =
            _mm256_or_si256(_mm256_cmpeq_epi8(v, ktab9),
                            _mm256_or_si256(_mm256_cmpeq_epi8(v, klf), _mm256_cmpeq_epi8(v, kcr)));
        /* non-ASCII bytes are the validator's business, not the
         * c-printable-ASCII rule — mask them out (flipped < 0 <=> b < 0x80) */
        __m256i is_ascii = _mm256_cmpgt_epi8(_mm256_setzero_si256(), flipped);
        __m256i badv = _mm256_and_si256(is_ascii, _mm256_or_si256(_mm256_andnot_si256(allowed, lo),
                                                                  _mm256_cmpeq_epi8(v, kdel)));
        bad |= (uint32_t)_mm256_movemask_epi8(badv);
    }
    yep_text_stats tail = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    yep_text_scan_stats_scalar(s + i, len - i, &tail);
    out->nl = c_nl + tail.nl;
    out->comma = c_co + tail.comma;
    out->dash = c_da + tail.dash;
    out->colon = c_cl + tail.colon;
    out->bracket = c_br + tail.bracket;
    out->brace = c_bc + tail.brace;
    out->dq = c_dq + tail.dq;
    out->sq = c_sq + tail.sq;
    out->pipe = c_pi + tail.pipe;
    out->amp = c_am + tail.amp;
    out->nonascii = (hi != 0) || tail.nonascii;
    out->bad_printable = (bad != 0) || tail.bad_printable;
}

static int yep_avx2_gate_scan(const char* s, size_t len) {
    const __m256i sp = _mm256_set1_epi8(0x20), del = _mm256_set1_epi8(0x7F);
    const __m256i tab = _mm256_set1_epi8(0x09), lf = _mm256_set1_epi8(0x0A),
                  cr = _mm256_set1_epi8(0x0D), ones = _mm256_set1_epi8(-1);
    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i x = _mm256_loadu_si256((const __m256i*)(s + i));
        /* ctrl is SIGNED 0x20 > x: catches C0 (0x00-0x1F, the full
         * range — 0x1F included) AND every byte >= 0x80 (they read
         * negative); allow clears TAB/LF/CR only; DEL is separate */
        __m256i ctrl = _mm256_cmpgt_epi8(sp, x);
        __m256i allow =
            _mm256_or_si256(_mm256_cmpeq_epi8(x, tab),
                            _mm256_or_si256(_mm256_cmpeq_epi8(x, lf), _mm256_cmpeq_epi8(x, cr)));
        __m256i bad = _mm256_or_si256(_mm256_and_si256(ctrl, _mm256_andnot_si256(allow, ones)),
                                      _mm256_cmpeq_epi8(x, del));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(bad, _mm256_set1_epi8(0))) != -1) {
            return 1;
        }
    }
    return yep_text_gate_scan_scalar(s + i, len - i);
}

/* First member byte via two pshufb lookups per nibble-class group
 * (TODO.restructure/68): lane matches iff lo[b&15] & hi[b>>4] != 0 —
 * exact for the whole class, so the first nonzero lane IS the answer.
 * Tables broadcast to both 128-bit halves (pshufb is per-lane);
 * indices are 0-15 by construction, so no zeroing surprises. */
static ptrdiff_t yep_avx2_stopset_find(const yep_stopset* ss, const char* s, size_t len) {
    /* two loaded groups is the sweet spot (every YAML stop class is
     * 1-2); wider classes take the scalar bitmap walk */
    if (ss->groups == 0 || ss->groups > 2) {
        return yep_text_stopset_find_scalar(ss, s, len);
    }
    const __m256i tlo0 =
        _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(const void*)ss->lo[0]));
    const __m256i thi0 =
        _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(const void*)ss->hi[0]));
    const __m256i tlo1 =
        _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(const void*)ss->lo[1]));
    const __m256i thi1 =
        _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(const void*)ss->hi[1]));
    const __m256i f = _mm256_set1_epi8(15);
    const int two = ss->groups > 1;
    size_t i = 0;
    for (; i + YEP_AVX2_CHUNK <= len; i += YEP_AVX2_CHUNK) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(s + i));
        __m256i lo = _mm256_and_si256(v, f);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), f);
        __m256i m = _mm256_and_si256(_mm256_shuffle_epi8(tlo0, lo), _mm256_shuffle_epi8(thi0, hi));
        if (two) {
            m = _mm256_or_si256(
                m, _mm256_and_si256(_mm256_shuffle_epi8(tlo1, lo), _mm256_shuffle_epi8(thi1, hi)));
        }
        /* nonzero test via the eq-zero complement: a lane's bitmask
         * can be 0x80 (a group's 8th member) — cmpgt_epi8 is SIGNED
         * and read that as negative, missing the hit (the CI AVX2
         * run's first catch; NEON's unsigned vmaxv never had it) */
        uint32_t z = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(m, _mm256_setzero_si256()));
        uint32_t nz = ~z;
        if (nz != 0) {
            return (ptrdiff_t)(i + (size_t)yep_ctz32(nz));
        }
    }
    ptrdiff_t tail = yep_text_stopset_find_scalar(ss, s + i, len - i);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

/* The fused line-facts sweep (TODO.restructure/76), AVX2 flavor:
 * movemask + ctz extract the hit lanes with no spill. Gated to >=64B
 * remaining spans like NEON (short lines keep the scalar walk). */
static void yep_avx2_line_facts(const char* s, size_t len, size_t pos, yep_line_facts* out) {
    /* the capped SWAR walk settles short lines in one pass — it is
     * BOTH the test and the answer (TODO 84's probe-then-rescan paid
     * the line content twice) */
    if (yep_text_line_facts_capped(s, len, pos, 128, out)) {
        return;
    }
    const __m256i knl = _mm256_set1_epi8('\n'), kcr = _mm256_set1_epi8('\r'),
                  ksp = _mm256_set1_epi8(' '), kco = _mm256_set1_epi8(':'),
                  khash = _mm256_set1_epi8('#');
    size_t i = pos;
    int have_end = 0, have_indent = 0, have_stop = 0;
    uint32_t end = 0, indent = 0, stop = 0;
    for (; i + 32 <= len; i += 32) {
        if (have_end) {
            break; /* indent <= end, stop < end: settled or absent */
        }
        __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(s + i));
        uint32_t br = (uint32_t)_mm256_movemask_epi8(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, knl), _mm256_cmpeq_epi8(v, kcr)));
        if (!have_indent) {
            uint32_t nsp = (uint32_t)_mm256_movemask_epi8(
                _mm256_xor_si256(_mm256_cmpeq_epi8(v, ksp), _mm256_set1_epi8(-1)));
            if (nsp) {
                indent = (uint32_t)(i + (size_t)yep_ctz32(nsp));
                have_indent = 1;
            }
        }
        if (br) {
            end = (uint32_t)(i + (size_t)yep_ctz32(br));
            have_end = 1;
        }
        if (!have_stop && have_indent) {
            uint32_t st = br;
            st |= (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, kco));
            st |= (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, khash));
            if (st) {
                /* bytes left of the indent are spaces — no set lane can
                 * sit before `from`; a set lane at/after end means every
                 * later one is too: one ctz answers exactly */
                uint32_t from = (uint32_t)(indent > i ? indent - i : 0);
                uint32_t at = (uint32_t)yep_ctz32(st);
                if (at >= from && (!have_end || (uint32_t)(i + at) < end)) {
                    stop = (uint32_t)(i + at);
                    have_stop = 1;
                }
            }
        }
    }
    uint32_t stop_set = 0;
    if (!have_end || !have_indent || !have_stop) {
        yep_line_facts t;
        yep_text_line_facts_scalar(s, len, i, &t);
        if (!have_end) {
            end = t.end;
        }
        if (!have_indent) {
            indent = t.indent;
        }
        if (!have_stop) {
            if (have_end) {
                stop = end;
                stop_set = 0;
            } else {
                stop = t.stop;
                stop_set = t.stop_set;
            }
        }
    }
    out->end = end;
    out->indent = indent;
    out->stop = stop;
    out->stop_set = have_stop ? 1u : stop_set;
}

/* The flow-kernel chunk classifier: one 256-bit lane pass -> four
 * byte-class masks (movemask-native). Chunks are exactly 32 bytes
 * except the final partial one, which the scalar reference settles
 * (reads exactly n bytes). The c0 class is an UNSIGNED < 0x20 —
 * cmplt_epi8 is signed and would flag every >= 0x80 byte — so the
 * class is min_epu8(v, 0x1F) == v (the same identity qbc_find uses). */
yep_chunk_masks yep_text_json_chunk_avx2(const char* p, size_t n) {
    if (n < 32) {
        return yep_text_json_chunk_scalar(p, n);
    }
    yep_chunk_masks m;
    const __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)p);
    const __m256i c0t = _mm256_set1_epi8(0x1F);
    m.quote = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('"')));
    m.bs = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\')));
    __m256i s = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('{')),
                        _mm256_cmpeq_epi8(v, _mm256_set1_epi8('}'))),
        _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('[')),
                                        _mm256_cmpeq_epi8(v, _mm256_set1_epi8(']'))),
                        _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(',')),
                                        _mm256_cmpeq_epi8(v, _mm256_set1_epi8(':')))));
    m.structurals = (uint32_t)_mm256_movemask_epi8(s);
    /* valstart: the unsigned digit range as the min/max identity —
     * max(v,'0')==v gives v >= '0' (signed cmpeq_epi8 can't); the
     * saturating form would false-flag every byte below '0' */
    const __m256i d0 = _mm256_set1_epi8('0'), d9 = _mm256_set1_epi8('9');
    __m256i dig = _mm256_and_si256(_mm256_cmpeq_epi8(_mm256_max_epu8(v, d0), v),
                                   _mm256_cmpeq_epi8(_mm256_min_epu8(v, d9), v));
    __m256i vs = _mm256_or_si256(
        _mm256_or_si256(dig, _mm256_cmpeq_epi8(v, _mm256_set1_epi8('-'))),
        _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('t')),
                                        _mm256_cmpeq_epi8(v, _mm256_set1_epi8('f'))),
                        _mm256_cmpeq_epi8(v, _mm256_set1_epi8('n'))));
    m.valstart = (uint32_t)_mm256_movemask_epi8(vs);
    m.c0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_min_epu8(v, c0t), v));
    m.valid = 0xFFFFFFFFu;
    return m;
}

/* The JSON structural indexer (the token-contract front): vector
 * classification of 64-byte blocks into QUOTE/BS/OP/WS u64 masks
 * (two 32-byte lane passes each, movemask-native), then the shared
 * mask resolver (simd_text.h). The final partial block builds its
 * masks byte-wise — no loads past len. */
int yep_text_json_stage1_avx2(const char* p, size_t len, uint32_t* idx, size_t* nidx,
                              unsigned* flags) {
    size_t n = 0;
    uint64_t prev_in_string = 0, esc_carry = 0;
    size_t off = 0;
    for (; off + 64 <= len; off += 64) {
        uint64_t q = 0, bs = 0, op = 0, ws = 0, c0 = 0;
        for (unsigned half = 0; half < 2; half++) {
            const __m256i v =
                _mm256_loadu_si256((const __m256i*)(const void*)(p + off + 32 * half));
            uint64_t fq = (uint64_t)(uint32_t)_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"')));
            uint64_t fb = (uint64_t)(uint32_t)_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\')));
            __m256i o = _mm256_or_si256(
                _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('{')),
                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('}'))),
                _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('[')),
                                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8(']'))),
                                _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(',')),
                                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8(':')))));
            uint64_t fo = (uint64_t)(uint32_t)_mm256_movemask_epi8(o);
            __m256i w =
                _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(' ')),
                                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\t'))),
                                _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n')),
                                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\r'))));
            uint64_t fw = (uint64_t)(uint32_t)_mm256_movemask_epi8(w);
            /* unsigned min(v, 0x20) == v  <=>  v < 0x20 */
            uint64_t fc = (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
                _mm256_max_epu8(v, _mm256_set1_epi8(0x1F)), _mm256_set1_epi8(0x1F)));
            q |= fq << (32 * half);
            bs |= fb << (32 * half);
            op |= fo << (32 * half);
            ws |= fw << (32 * half);
            c0 |= fc << (32 * half);
        }
        n = yep_json_stage1_resolve(q, bs, op, ws, c0, ~0ull, &prev_in_string, &esc_carry, off, idx,
                                    n, flags);
    }
    if (off < len) {
        size_t cn = len - off;
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
        n = yep_json_stage1_resolve(q, bs, op, ws, c0, (1ull << cn) - 1ull, &prev_in_string,
                                    &esc_carry, off, idx, n, flags);
    }
    *nidx = n;
    return prev_in_string ? 0 : 1;
}

/* The classification half of the structural indexer (AVX2 twin of the
 * NEON masks slot): two 32-byte lane passes per 64-byte block. */
int yep_text_json_stage1_masks_avx2(const char* p, size_t len, struct yep_s1_block* blocks,
                                    size_t* nblocks, unsigned* flags) {
    size_t nb = 0;
    uint64_t prev_in_string = 0, esc_carry = 0;
    size_t off = 0;
    for (; off + 64 <= len; off += 64) {
        uint64_t q = 0, bs = 0, op = 0, c0 = 0;
        for (unsigned half = 0; half < 2; half++) {
            const __m256i v =
                _mm256_loadu_si256((const __m256i*)(const void*)(p + off + 32 * half));
            uint64_t fq = (uint64_t)(uint32_t)_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"')));
            uint64_t fb = (uint64_t)(uint32_t)_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\')));
            __m256i o = _mm256_or_si256(
                _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('{')),
                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8('}'))),
                _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('[')),
                                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8(']'))),
                                _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(',')),
                                                _mm256_cmpeq_epi8(v, _mm256_set1_epi8(':')))));
            uint64_t fo = (uint64_t)(uint32_t)_mm256_movemask_epi8(o);
            uint64_t fc = (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
                _mm256_max_epu8(v, _mm256_set1_epi8(0x1F)), _mm256_set1_epi8(0x1F)));
            q |= fq << (32 * half);
            bs |= fb << (32 * half);
            op |= fo << (32 * half);
            c0 |= fc << (32 * half);
        }
        blocks[nb].q = q;
        blocks[nb].bs = bs;
        blocks[nb].op = op;
        blocks[nb].c0 = c0;
        blocks[nb].tokens =
            yep_json_stage1_tokens(q, bs, op, c0, ~0ull, &prev_in_string, &esc_carry, flags);
        nb++;
    }
    if (off < len) {
        size_t cn = len - off;
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
        uint64_t valid = (1ull << cn) - 1ull;
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

const yep_text_kernels yep_text_kernels_avx2 = {
    yep_avx2_contains,
    yep_avx2_find,
    yep_avx2_find3,
    yep_avx2_count,
    yep_avx2_count3,
    yep_avx2_copy_count3,
    yep_avx2_find_not,
    yep_avx2_stopset_find,
    yep_avx2_quote_scan,
    yep_avx2_scan_stats,
    yep_avx2_qbc_find,
    yep_avx2_gate_scan,
    yep_avx2_line_facts,
    yep_text_json_chunk_avx2,
    yep_text_json_stage1_avx2,
    yep_text_json_stage1_masks_avx2,
};

#endif /* YEP_ARCH_X86 */
