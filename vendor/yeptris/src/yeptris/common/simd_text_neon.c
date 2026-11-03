/* simd_text_neon.c — NEON kernels (TODO.impl/04).
 *
 * NEON is architectural on AArch64, so this TU needs no extra -m flags.
 * 16-byte chunks, scalar tails. scan_stats accumulates vertically
 * (vpadalq into u16 lanes, one reduce per batch — the leptris count
 * lesson); the single-char kernels still reduce UADDV over 0/1 lanes
 * (the sizing ops are the ≥8×-vs-scalar acceptance target). Position
 * queries reduce through a stack movemask helper — correct first, and
 * the perf ledger records it as a refinement candidate if 06's
 * profiles care. stopset_find is vectorized (TODO.restructure/68,
 * nibble-class tbl).
 */

#include "port.h" /* defines YEP_ARCH_* — must precede the guard below */

#if defined(YEP_ARCH_AARCH64)

#include <arm_neon.h>
#include <stdint.h>
#include <string.h>

#include "simd_text.h"

#define YEP_NEON_CHUNK 16

static inline uint8x16_t yep_neon_load(const char* p) {
    return vld1q_u8((const uint8_t*)(const void*)p);
}

static inline uint8x16_t yep_neon_eq(const char* p, uint8_t c) {
    return vceqq_u8(yep_neon_load(p), vdupq_n_u8(c));
}

/* 0x00/0xFF lanes -> per-lane bitmask. LE: widening form — the two
 * mask bits of each byte pair land in one u16 lane, a shift vector
 * spreads them to their final positions, ONE u16 reduce (the leptris
 * scan_events shape; no stack spill). vshlq_u16's shift vector is the
 * ACLE-mandated int16x8_t (the GCC-aarch64 lesson, leptris #487).
 * BE keeps the endian-neutral pow2-table spill. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline uint16_t yep_neon_bits(uint8x16_t m) {
    static const uint8_t pow2[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8_t tmp[16];
    vst1q_u8(tmp, vandq_u8(m, vld1q_u8(pow2)));
    uint16_t lo = (uint16_t)(tmp[0] | tmp[1] | tmp[2] | tmp[3] | tmp[4] | tmp[5] | tmp[6] | tmp[7]);
    uint16_t hi =
        (uint16_t)(tmp[8] | tmp[9] | tmp[10] | tmp[11] | tmp[12] | tmp[13] | tmp[14] | tmp[15]);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}
#else
static inline uint16_t yep_neon_bits(uint8x16_t m) {
    static const int16x8_t spread = {0, 2, 4, 6, 8, 10, 12, 14}; /* lane k -> bit 2k */
    /* per u16 lane: byte 2k carries bit 7 (u16 bit 7), byte 2k+1
     * carries bit 15; >>7 lands b0 at bit 0, >>14 lands b1 at bit 1
     * (bit 7 shifted out), &3 drops b1's >>7 residue at bit 8 */
    uint16x8_t p = vreinterpretq_u16_u8(vandq_u8(m, vdupq_n_u8(0x80)));
    uint16x8_t r = vandq_u16(vorrq_u16(vshrq_n_u16(p, 7), vshrq_n_u16(p, 14)), vdupq_n_u16(3));
    return (uint16_t)vaddvq_u16(vshlq_u16(r, spread));
}
#endif

/* Occurrences of c in one chunk: UADDV over 0/1 lanes. */
static inline size_t yep_neon_chunk_count(const char* p, uint8_t c) {
    uint8x16_t one = vandq_u8(yep_neon_eq(p, c), vdupq_n_u8(1));
    return (size_t)vaddvq_u8(one);
}

static int yep_neon_contains(const char* s, size_t len, char c) {
    size_t i = 0;
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        if (yep_neon_chunk_count(s + i, (uint8_t)c) != 0) {
            return 1;
        }
    }
    return yep_text_contains_scalar(s + i, len - i, c);
}

static ptrdiff_t yep_neon_find(const char* s, size_t len, char c) {
    size_t i = 0;
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        uint16_t m = yep_neon_bits(yep_neon_eq(s + i, (uint8_t)c));
        if (m) {
            return (ptrdiff_t)(i + (size_t)__builtin_ctz(m));
        }
    }
    ptrdiff_t tail = yep_text_find_scalar(s + i, len - i, c);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

static size_t yep_neon_count(const char* s, size_t len, char c) {
    size_t n = 0, i = 0;
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        n += yep_neon_chunk_count(s + i, (uint8_t)c);
    }
    return n + yep_text_count_char_scalar(s + i, len - i, c);
}

static void yep_neon_count3(const char* s, size_t len, char c0, char c1, char c2, size_t* n0,
                            size_t* n1, size_t* n2) {
    size_t a = 0, b = 0, d = 0, i = 0;
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        a += yep_neon_chunk_count(s + i, (uint8_t)c0);
        b += yep_neon_chunk_count(s + i, (uint8_t)c1);
        d += yep_neon_chunk_count(s + i, (uint8_t)c2);
    }
    size_t ta = 0, tb = 0, td = 0;
    yep_text_count3_scalar(s + i, len - i, c0, c1, c2, &ta, &tb, &td);
    *n0 = a + ta;
    *n1 = b + tb;
    *n2 = d + td;
}

static void yep_neon_copy_count3(char* dst, const char* src, size_t len, char c0, char c1, char c2,
                                 size_t* n0, size_t* n1, size_t* n2) {
    size_t a = 0, b = 0, d = 0, i = 0;
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        uint8x16_t v = yep_neon_load(src + i);
        vst1q_u8((uint8_t*)(void*)(dst + i), v);
        a += (size_t)vaddvq_u8(vandq_u8(vceqq_u8(v, vdupq_n_u8((uint8_t)c0)), vdupq_n_u8(1)));
        b += (size_t)vaddvq_u8(vandq_u8(vceqq_u8(v, vdupq_n_u8((uint8_t)c1)), vdupq_n_u8(1)));
        d += (size_t)vaddvq_u8(vandq_u8(vceqq_u8(v, vdupq_n_u8((uint8_t)c2)), vdupq_n_u8(1)));
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

static ptrdiff_t yep_neon_find_not(const char* s, size_t len, char c) {
    const uint8x16_t kc = vdupq_n_u8((uint8_t)c);
    size_t i = 0;
    /* 32 B per iteration (TODO.restructure/74): one shared constant,
     * one bits-test per two vectors; the hit half re-tests alone */
    for (; i + 2 * YEP_NEON_CHUNK <= len; i += 2 * YEP_NEON_CHUNK) {
        uint8x16_t ne0 = vmvnq_u8(vceqq_u8(yep_neon_load(s + i), kc));
        uint8x16_t ne1 = vmvnq_u8(vceqq_u8(yep_neon_load(s + i + 16), kc));
        uint16_t m0 = yep_neon_bits(ne0);
        uint16_t m1 = yep_neon_bits(ne1);
        if (m0) {
            return (ptrdiff_t)(i + (size_t)__builtin_ctz(m0));
        }
        if (m1) {
            return (ptrdiff_t)(i + 16 + (size_t)__builtin_ctz(m1));
        }
    }
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        uint16_t m = yep_neon_bits(vmvnq_u8(yep_neon_eq(s + i, (uint8_t)c)));
        if (m) {
            return (ptrdiff_t)(i + (size_t)__builtin_ctz(m));
        }
    }
    ptrdiff_t tail = yep_text_find_not_scalar(s + i, len - i, c);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

static ptrdiff_t yep_neon_find3(const char* s, size_t len, char c0, char c1, char c2) {
    if (len < 3) {
        return -1;
    }
    size_t i = 0;
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        uint16_t m = yep_neon_bits(yep_neon_eq(s + i, (uint8_t)c0));
        while (m) {
            size_t k = (size_t)__builtin_ctz(m);
            size_t at = i + k;
            if (at + 2 < len && s[at + 1] == c1 && s[at + 2] == c2) {
                return (ptrdiff_t)at;
            }
            m = (uint16_t)(m & (m - 1));
        }
    }
    ptrdiff_t tail = yep_text_find3_scalar(s + i, len - i, c0, c1, c2);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

static ptrdiff_t yep_neon_qbc_find(const char* s, size_t len) {
    size_t i = 0;
    for (; i + YEP_NEON_CHUNK <= len; i += YEP_NEON_CHUNK) {
        uint8x16_t v = yep_neon_load(s + i);
        uint8x16_t m =
            vorrq_u8(vorrq_u8(vceqq_u8(v, vdupq_n_u8('"')), vceqq_u8(v, vdupq_n_u8('\\'))),
                     vcltq_u8(v, vdupq_n_u8(0x20)));
        uint16_t bits = yep_neon_bits(m);
        if (bits != 0) {
            return (ptrdiff_t)(i + (size_t)__builtin_ctz((uint32_t)bits));
        }
    }
    ptrdiff_t r = yep_text_qbc_find_scalar(s + i, len - i);
    return r < 0 ? r : (ptrdiff_t)i + r;
}

static ptrdiff_t yep_neon_quote_scan(const char* s, size_t len, char q, int* has_escape) {
    int esc = 0;
    size_t i = 0;
    while (i < len) {
        int did_break = 0;
        for (; i + YEP_NEON_CHUNK <= len;) {
            uint16_t mq = yep_neon_bits(yep_neon_eq(s + i, (uint8_t)q));
            uint16_t mb = 0;
            if (q == '"') {
                mb = yep_neon_bits(yep_neon_eq(s + i, (uint8_t)'\\'));
            }
            if (mq == 0 && mb == 0) {
                i += YEP_NEON_CHUNK;
                continue;
            }
            size_t kq = mq ? (size_t)__builtin_ctz(mq) : SIZE_MAX;
            size_t kb = mb ? (size_t)__builtin_ctz(mb) : SIZE_MAX;
            if (q == '"' && kb < kq) {
                esc = 1;
                i += kb + 2; /* skip the escaped byte */
                did_break = 1;
                break;
            }
            if (q == '\'' && i + kq + 1 < len && s[i + kq + 1] == '\'') {
                esc = 1;
                i += kq + 2; /* doubled quote inside a single-quoted scalar */
                did_break = 1;
                break;
            }
            if (has_escape != NULL) {
                *has_escape = esc;
            }
            return (ptrdiff_t)(i + kq);
        }
        if (did_break) {
            continue;
        }
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
    if (has_escape != NULL) {
        *has_escape = esc;
    }
    return -1;
}

static void yep_neon_scan_stats(const char* s, size_t len, yep_text_stats* out) {
    if (s == NULL || len == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }

    const uint8x16_t ktab = vdupq_n_u8('\n'), kcomma = vdupq_n_u8(','), kdash = vdupq_n_u8('-'),
                     kcolon = vdupq_n_u8(':'), kbrk = vdupq_n_u8('['), kbrce = vdupq_n_u8('{'),
                     kdq = vdupq_n_u8('"'), ksq = vdupq_n_u8('\''), kpipe = vdupq_n_u8('|'),
                     kamp = vdupq_n_u8('&');
    const uint8x16_t k80 = vdupq_n_u8(0x80), ktab9 = vdupq_n_u8('\t'), klf = vdupq_n_u8('\n'),
                     kcr = vdupq_n_u8('\r'), kdel = vdupq_n_u8(0x7F), klow20 = vdupq_n_u8(0x20);
    const uint8x16_t one = vdupq_n_u8(1);
    /* vertical accumulation (leptris NEON lesson, its 0.19.x count_char):
     * a per-chunk UADDV reduce serializes on the vector->GPR boundary —
     * ten of them per chunk starved the loop. Pairwise-add-accumulate
     * into u16 lanes, reduce once per batch. The u16 constraint is on
     * the REDUCE, not just the lanes: vaddvq_u16 returns uint16_t, so
     * the batch total must stay under 65536 — batches of 4095 chunks
     * sum to at most 65520 (lane peak 8190). Presence flags fold the
     * same way (OR is idempotent — one vmaxv at the end replaces the
     * per-chunk ones). */
    uint16x8_t a_nl = vdupq_n_u16(0), a_co = vdupq_n_u16(0), a_da = vdupq_n_u16(0),
               a_cl = vdupq_n_u16(0), a_br = vdupq_n_u16(0), a_bc = vdupq_n_u16(0),
               a_dq = vdupq_n_u16(0), a_sq = vdupq_n_u16(0), a_pi = vdupq_n_u16(0),
               a_am = vdupq_n_u16(0);
    uint8x16_t any_hi = vdupq_n_u8(0), any_bad = vdupq_n_u8(0);
    size_t c_nl = 0, c_co = 0, c_da = 0, c_cl = 0, c_br = 0, c_bc = 0, c_dq = 0, c_sq = 0, c_pi = 0,
           c_am = 0;
    size_t i = 0;
#define YEP_STATS_DRAIN()                                                                          \
    do {                                                                                           \
        c_nl += (size_t)vaddvq_u16(a_nl);                                                          \
        a_nl = vdupq_n_u16(0);                                                                     \
        c_co += (size_t)vaddvq_u16(a_co);                                                          \
        a_co = vdupq_n_u16(0);                                                                     \
        c_da += (size_t)vaddvq_u16(a_da);                                                          \
        a_da = vdupq_n_u16(0);                                                                     \
        c_cl += (size_t)vaddvq_u16(a_cl);                                                          \
        a_cl = vdupq_n_u16(0);                                                                     \
        c_br += (size_t)vaddvq_u16(a_br);                                                          \
        a_br = vdupq_n_u16(0);                                                                     \
        c_bc += (size_t)vaddvq_u16(a_bc);                                                          \
        a_bc = vdupq_n_u16(0);                                                                     \
        c_dq += (size_t)vaddvq_u16(a_dq);                                                          \
        a_dq = vdupq_n_u16(0);                                                                     \
        c_sq += (size_t)vaddvq_u16(a_sq);                                                          \
        a_sq = vdupq_n_u16(0);                                                                     \
        c_pi += (size_t)vaddvq_u16(a_pi);                                                          \
        a_pi = vdupq_n_u16(0);                                                                     \
        c_am += (size_t)vaddvq_u16(a_am);                                                          \
        a_am = vdupq_n_u16(0);                                                                     \
    } while (0)
    while (i + 16 <= len) {
        size_t end = i + 65504; /* 4095 inner chunks: reduce stays u16 */
        if (end + 16 > len) {
            end = len - 16; /* the last inner iteration starts here */
        }
        for (; i <= end; i += 16) {
            uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)(s + i));
            any_hi = vorrq_u8(any_hi, vcgeq_u8(v, k80)); /* any non-ASCII byte */
            a_nl = vpadalq_u8(a_nl, vandq_u8(vceqq_u8(v, ktab), one));
            a_co = vpadalq_u8(a_co, vandq_u8(vceqq_u8(v, kcomma), one));
            a_da = vpadalq_u8(a_da, vandq_u8(vceqq_u8(v, kdash), one));
            a_cl = vpadalq_u8(a_cl, vandq_u8(vceqq_u8(v, kcolon), one));
            a_br = vpadalq_u8(a_br, vandq_u8(vceqq_u8(v, kbrk), one));
            a_bc = vpadalq_u8(a_bc, vandq_u8(vceqq_u8(v, kbrce), one));
            a_dq = vpadalq_u8(a_dq, vandq_u8(vceqq_u8(v, kdq), one));
            a_sq = vpadalq_u8(a_sq, vandq_u8(vceqq_u8(v, ksq), one));
            a_pi = vpadalq_u8(a_pi, vandq_u8(vceqq_u8(v, kpipe), one));
            a_am = vpadalq_u8(a_am, vandq_u8(vceqq_u8(v, kamp), one));
            /* c-printable-ASCII violations, non-ASCII masked out:
             * b < 0x20 except TAB/LF/CR, plus DEL */
            uint8x16_t is_ascii = vcltzq_s8(vreinterpretq_s8_u8(veorq_u8(v, k80)));
            uint8x16_t lo = vcltq_u8(v, klow20);
            uint8x16_t allowed =
                vorrq_u8(vceqq_u8(v, ktab9), vorrq_u8(vceqq_u8(v, klf), vceqq_u8(v, kcr)));
            any_bad = vorrq_u8(
                any_bad, vandq_u8(is_ascii, vorrq_u8(vbicq_u8(lo, allowed), vceqq_u8(v, kdel))));
        }
        if (i + 16 > len) {
            break;
        }
        YEP_STATS_DRAIN();
    }
    YEP_STATS_DRAIN();
#undef YEP_STATS_DRAIN
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
    out->nonascii = (vmaxvq_u8(any_hi) != 0) || tail.nonascii;
    out->bad_printable = (vmaxvq_u8(any_bad) != 0) || tail.bad_printable;
}

static int yep_neon_gate_scan(const char* s, size_t len) {
    const uint8x16_t sp = vdupq_n_u8(0x20), del = vdupq_n_u8(0x7F), hi = vdupq_n_u8(0x80);
    const uint8x16_t tab = vdupq_n_u8(0x09), lf = vdupq_n_u8(0x0A), cr = vdupq_n_u8(0x0D);
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t x = vld1q_u8((const uint8_t*)(s + i));
        uint8x16_t ctrl = vcltq_u8(x, sp); /* C0 complete: 0x1F included */
        uint8x16_t allow = vorrq_u8(vceqq_u8(x, tab), vorrq_u8(vceqq_u8(x, lf), vceqq_u8(x, cr)));
        uint8x16_t nonascii = vcgeq_u8(x, hi);
        uint8x16_t bad = vorrq_u8(vorrq_u8(vbicq_u8(ctrl, allow), nonascii), vceqq_u8(x, del));
        if (vmaxvq_u32(vreinterpretq_u32_u8(bad)) != 0) {
            return 1;
        }
    }
    return yep_text_gate_scan_scalar(s + i, len - i);
}

/* First member byte via two tbl lookups per nibble-class group
 * (TODO.restructure/68): lane matches iff lo[b&15] & hi[b>>4] != 0 —
 * exact for the whole class, so the first nonzero lane IS the answer.
 * The hit chunk spills one 16-byte vector and scans it scalar (a span
 * hits once, at its end). */
static ptrdiff_t yep_neon_stopset_find(const yep_stopset* ss, const char* s, size_t len) {
    /* two loaded groups is the sweet spot (every YAML stop class is
     * 1-2); wider classes take the scalar bitmap walk */
    if (ss->groups == 0 || ss->groups > 2) {
        return yep_text_stopset_find_scalar(ss, s, len);
    }
    const uint8x16_t tlo0 = vld1q_u8(ss->lo[0]), thi0 = vld1q_u8(ss->hi[0]);
    const uint8x16_t tlo1 = vld1q_u8(ss->lo[1]), thi1 = vld1q_u8(ss->hi[1]);
    const uint8x16_t f = vdupq_n_u8(15);
    const int two = ss->groups > 1;
    size_t i = 0;
    /* 32 B per iteration (TODO.restructure/74): the tbl work for both
     * vectors interleaves while their loads stream; one vmaxv covers
     * both halves — the hit half spills alone */
    for (; i + 32 <= len; i += 32) {
        uint8x16_t v0 = vld1q_u8((const uint8_t*)(const void*)(s + i));
        uint8x16_t v1 = vld1q_u8((const uint8_t*)(const void*)(s + i + 16));
        uint8x16_t m0 =
            vandq_u8(vqtbl1q_u8(tlo0, vandq_u8(v0, f)), vqtbl1q_u8(thi0, vshrq_n_u8(v0, 4)));
        uint8x16_t m1 =
            vandq_u8(vqtbl1q_u8(tlo0, vandq_u8(v1, f)), vqtbl1q_u8(thi0, vshrq_n_u8(v1, 4)));
        if (two) {
            m0 = vorrq_u8(m0, vandq_u8(vqtbl1q_u8(tlo1, vandq_u8(v0, f)),
                                       vqtbl1q_u8(thi1, vshrq_n_u8(v0, 4))));
            m1 = vorrq_u8(m1, vandq_u8(vqtbl1q_u8(tlo1, vandq_u8(v1, f)),
                                       vqtbl1q_u8(thi1, vshrq_n_u8(v1, 4))));
        }
        uint8x16_t many = vorrq_u8(m0, m1);
        if (vmaxvq_u8(many) != 0) {
            unsigned char hit[16] __attribute__((aligned(16)));
            if (vmaxvq_u8(m0) != 0) {
                vst1q_u8(hit, m0);
                for (int k = 0; k < 16; k++) {
                    if (hit[k] != 0) {
                        return (ptrdiff_t)(i + (size_t)k);
                    }
                }
            }
            vst1q_u8(hit, m1);
            for (int k = 0; k < 16; k++) {
                if (hit[k] != 0) {
                    return (ptrdiff_t)(i + 16 + (size_t)k);
                }
            }
        }
    }
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)(s + i));
        uint8x16_t lo = vandq_u8(v, f), hi = vshrq_n_u8(v, 4);
        uint8x16_t m = vandq_u8(vqtbl1q_u8(tlo0, lo), vqtbl1q_u8(thi0, hi));
        if (two) {
            m = vorrq_u8(m, vandq_u8(vqtbl1q_u8(tlo1, lo), vqtbl1q_u8(thi1, hi)));
        }
        if (vmaxvq_u8(m) != 0) {
            unsigned char hit[16] __attribute__((aligned(16)));
            vst1q_u8(hit, m);
            for (int k = 0; k < 16; k++) {
                if (hit[k] != 0) {
                    return (ptrdiff_t)(i + (size_t)k);
                }
            }
        }
    }
    ptrdiff_t tail = yep_text_stopset_find_scalar(ss, s + i, len - i);
    return tail < 0 ? -1 : (ptrdiff_t)i + tail;
}

/* The fused line-facts sweep (TODO.restructure/76): one pass over the
 * line's chunks produces break/non-space/stop masks; a chunk spills
 * only when it carries a still-missing fact (usually chunk 0 carries
 * all three). Any fact the vector loop cannot land falls to one exact
 * scalar sweep of the whole span — rare (long lines whose break sits
 * in the tail) and always correct. */
static void yep_neon_line_facts(const char* s, size_t len, size_t pos, yep_line_facts* out) {
    /* The SWAR walk is BOTH the short-line test and the short-line
     * answer: a break inside 64 bytes settles the facts there — the
     * previous shape paid a 32-byte byte probe and then rescanned the
     * whole line (the double scan was the dominant short-line cost).
     * 76 measured the vector sweep 2x slower at 16-32 byte lines; 84
     * pinned the gate at the LINE, not the remaining buffer. */
    if (yep_text_line_facts_capped(s, len, pos, 128, out)) {
        return;
    }
    const uint8x16_t ksp = vdupq_n_u8(' '), knl = vdupq_n_u8('\n'), kcr = vdupq_n_u8('\r'),
                     kco = vdupq_n_u8(':'), khash = vdupq_n_u8('#');
    size_t i = pos;
    int have_end = 0, have_indent = 0, have_stop = 0;
    uint32_t end = 0, indent = 0, stop = 0;
    for (; i + 16 <= len; i += 16) {
        if (have_end) {
            break; /* indent <= end, stop < end: settled or absent */
        }
        uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)(s + i));
        uint8x16_t br = vorrq_u8(vceqq_u8(v, knl), vceqq_u8(v, kcr));
        int br_any = (int)vmaxvq_u8(br);
        if (!have_indent) {
            if (vmaxvq_u8(vmvnq_u8(vceqq_u8(v, ksp))) != 0) {
                for (int k = 0; k < 16; k++) {
                    if (s[i + k] != ' ') { /* extract from source: no spill */
                        indent = (uint32_t)(i + (size_t)k);
                        have_indent = 1;
                        break;
                    }
                }
            }
        }
        if (br_any) {
            for (int k = 0; k < 16; k++) {
                if (s[i + k] == '\n' || s[i + k] == '\r') {
                    end = (uint32_t)(i + (size_t)k);
                    have_end = 1;
                    break;
                }
            }
        }
        if (!have_stop && have_indent) {
            uint8x16_t st = vorrq_u8(br, vorrq_u8(vceqq_u8(v, kco), vceqq_u8(v, khash)));
            if (vmaxvq_u8(st) != 0) {
                size_t from = indent > i ? (size_t)(indent - i) : 0;
                size_t lim = 16;
                if (have_end && end < (uint32_t)(i + 16)) {
                    lim = (size_t)(end - i); /* stop is strictly before end */
                }
                for (size_t k = from; k < lim; k++) {
                    char c = s[i + k];
                    if (c == '\n' || c == '\r' || c == '#' || c == ':') {
                        stop = (uint32_t)(i + k);
                        have_stop = 1;
                        break;
                    }
                }
            }
        }
    }
    uint32_t stop_set = 0;
    if (!have_end || !have_indent || !have_stop) {
        /* continuation from the chunk cursor (a 17-31 byte line runs
         * one vector chunk then at most 15 scalar bytes; a stop in
         * [indent, i) cannot exist when have_stop is still 0 — that
         * chunk's set-probe was zero) */
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
                stop = end; /* end settled: no stop existed before it */
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

/* The flow-kernel chunk classifier: two 16-byte halves -> four
 * byte-class masks. Chunks are exactly 32 bytes except the final
 * partial one, which the scalar reference settles (the contract reads
 * exactly n bytes; a vld1q on a tail would stride past the end). */
yep_chunk_masks yep_text_json_chunk_neon(const char* p, size_t n) {
    if (n < 32) {
        return yep_text_json_chunk_scalar(p, n);
    }
    yep_chunk_masks m = {0, 0, 0, 0, 0, 0xFFFFFFFFu};
    const uint8x16_t dq = vdupq_n_u8('"'), dbs = vdupq_n_u8('\\');
    const uint8x16_t c0t = vdupq_n_u8(0x20);
    const uint8x16_t op = vdupq_n_u8('{'), cl = vdupq_n_u8('}');
    const uint8x16_t ls = vdupq_n_u8('['), rs = vdupq_n_u8(']');
    const uint8x16_t cm = vdupq_n_u8(','), co = vdupq_n_u8(':');
    const uint8x16_t d0 = vdupq_n_u8('0'), d9 = vdupq_n_u8('9');
    const uint8x16_t da = vdupq_n_u8('-'), dt = vdupq_n_u8('t');
    const uint8x16_t df = vdupq_n_u8('f'), dn = vdupq_n_u8('n');
    for (unsigned half = 0; half < 2; half++) {
        uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)(p + 16 * half));
        uint32_t q = yep_neon_bits(vceqq_u8(v, dq));
        uint32_t b = yep_neon_bits(vceqq_u8(v, dbs));
        uint32_t s = yep_neon_bits(vorrq_u8(vorrq_u8(vorrq_u8(vceqq_u8(v, op), vceqq_u8(v, cl)),
                                                     vorrq_u8(vceqq_u8(v, ls), vceqq_u8(v, rs))),
                                            vorrq_u8(vceqq_u8(v, cm), vceqq_u8(v, co))));
        uint32_t vs = yep_neon_bits(
            vorrq_u8(vorrq_u8(vandq_u8(vcgeq_u8(v, d0), vcleq_u8(v, d9)), vceqq_u8(v, da)),
                     vorrq_u8(vorrq_u8(vceqq_u8(v, dt), vceqq_u8(v, df)), vceqq_u8(v, dn))));
        uint32_t c = yep_neon_bits(vcltq_u8(v, c0t));
        if (half) {
            q <<= 16;
            b <<= 16;
            s <<= 16;
            vs <<= 16;
            c <<= 16;
        }
        m.quote |= q;
        m.bs |= b;
        m.structurals |= s;
        m.valstart |= vs;
        m.c0 |= c;
    }
    return m;
}

/* The JSON structural indexer (the token-contract front): vector
 * classification of 64-byte blocks into QUOTE/BS/OP/WS u64 masks
 * (four 16-byte halves each), then the shared mask resolver (the
 * simdjson identity set, simd_text.h). The final partial block
 * builds its masks byte-wise — no loads past len. */
int yep_text_json_stage1_neon(const char* p, size_t len, uint32_t* idx, size_t* nidx,
                              unsigned* flags) {
    size_t n = 0;
    uint64_t prev_in_string = 0, esc_carry = 0;
    size_t off = 0;
    const uint8x16_t dq = vdupq_n_u8('"'), dbs = vdupq_n_u8('\\');
    const uint8x16_t d1 = vdupq_n_u8('{'), d2 = vdupq_n_u8('}');
    const uint8x16_t d3 = vdupq_n_u8('['), d4 = vdupq_n_u8(']');
    const uint8x16_t d5 = vdupq_n_u8(','), d6 = vdupq_n_u8(':');
    const uint8x16_t wsp = vdupq_n_u8(' '), wtab = vdupq_n_u8('\t');
    const uint8x16_t wnl = vdupq_n_u8('\n'), wcr = vdupq_n_u8('\r');
    const uint8x16_t c0t = vdupq_n_u8(0x20);
    for (; off + 64 <= len; off += 64) {
        uint64_t q = 0, bs = 0, op = 0, ws = 0, c0 = 0;
        for (unsigned half = 0; half < 4; half++) {
            uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)(p + off + 16 * half));
            uint64_t fq = yep_neon_bits(vceqq_u8(v, dq));
            uint64_t fb = yep_neon_bits(vceqq_u8(v, dbs));
            uint64_t fo =
                yep_neon_bits(vorrq_u8(vorrq_u8(vorrq_u8(vceqq_u8(v, d1), vceqq_u8(v, d2)),
                                                vorrq_u8(vceqq_u8(v, d3), vceqq_u8(v, d4))),
                                       vorrq_u8(vceqq_u8(v, d5), vceqq_u8(v, d6))));
            uint64_t fw = yep_neon_bits(vorrq_u8(vorrq_u8(vceqq_u8(v, wsp), vceqq_u8(v, wtab)),
                                                 vorrq_u8(vceqq_u8(v, wnl), vceqq_u8(v, wcr))));
            uint64_t fc = yep_neon_bits(vcltq_u8(v, c0t));
            q |= fq << (16 * half);
            bs |= fb << (16 * half);
            op |= fo << (16 * half);
            ws |= fw << (16 * half);
            c0 |= fc << (16 * half);
        }
        n = yep_json_stage1_resolve(q, bs, op, ws, c0, ~0ull, &prev_in_string, &esc_carry, off, idx,
                                    n, flags);
    }
    if (off < len) { /* the tail: byte-wise masks, no loads past len */
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

/* The classification half of the structural indexer: the same
 * 64-byte/4-half mask build, no emission, blocks out instead of idx. */
int yep_text_json_stage1_masks_neon(const char* p, size_t len, struct yep_s1_block* blocks,
                                    size_t* nblocks, unsigned* flags) {
    size_t nb = 0;
    uint64_t prev_in_string = 0, esc_carry = 0;
    size_t off = 0;
    const uint8x16_t dq = vdupq_n_u8('"'), dbs = vdupq_n_u8('\\');
    const uint8x16_t d1 = vdupq_n_u8('{'), d2 = vdupq_n_u8('}');
    const uint8x16_t d3 = vdupq_n_u8('['), d4 = vdupq_n_u8(']');
    const uint8x16_t d5 = vdupq_n_u8(','), d6 = vdupq_n_u8(':');
    const uint8x16_t c0t = vdupq_n_u8(0x20);
    for (; off + 64 <= len; off += 64) {
        uint64_t q = 0, bs = 0, op = 0, c0 = 0;
        for (unsigned half = 0; half < 4; half++) {
            uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)(p + off + 16 * half));
            uint64_t fq = yep_neon_bits(vceqq_u8(v, dq));
            uint64_t fb = yep_neon_bits(vceqq_u8(v, dbs));
            uint64_t fo =
                yep_neon_bits(vorrq_u8(vorrq_u8(vorrq_u8(vceqq_u8(v, d1), vceqq_u8(v, d2)),
                                                vorrq_u8(vceqq_u8(v, d3), vceqq_u8(v, d4))),
                                       vorrq_u8(vceqq_u8(v, d5), vceqq_u8(v, d6))));
            uint64_t fc = yep_neon_bits(vcltq_u8(v, c0t));
            q |= fq << (16 * half);
            bs |= fb << (16 * half);
            op |= fo << (16 * half);
            c0 |= fc << (16 * half);
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

const yep_text_kernels yep_text_kernels_neon = {
    yep_neon_contains,
    yep_neon_find,
    yep_neon_find3,
    yep_neon_count,
    yep_neon_count3,
    yep_neon_copy_count3,
    yep_neon_find_not,
    yep_neon_stopset_find,
    yep_neon_quote_scan,
    yep_neon_scan_stats,
    yep_neon_qbc_find,
    yep_neon_gate_scan,
    yep_neon_line_facts,
    yep_text_json_chunk_neon,
    yep_text_json_stage1_neon,
    yep_text_json_stage1_masks_neon,
};

#endif /* YEP_ARCH_AARCH64 */
