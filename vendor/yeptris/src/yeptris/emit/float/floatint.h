/* floatint.h — internal float-printing declarations (TODO.impl/14).
 *
 * CLEAN-ROOM IMPLEMENTATION: no third-party code is vendored (licensing
 * decision 2026-09-01). What follows implements the published interval
 * method for shortest round-trip decimal output (the Dragon4/Steele-
 * White family, described in the literature) entirely from the math:
 * find the shortest decimal that lies strictly inside the rounding
 * interval (v - w_low, v + w_high) of the binary value.
 *
 * Two tiers, one algorithm:
 *   - tier A (print.c): the same digit loop over exact 128-bit
 *     integers — covers every value whose scaled representation fits
 *     (doubles with E in [-32, 75], floats with E in [-32, 104]);
 *   - tier B (dragon.c): the same loop over a fixed-capacity limb
 *     vector — exact for everything else (extreme exponents), and the
 *     correctness oracle for tier A in tests.
 */

#ifndef YEP_FLOATINT_H
#define YEP_FLOATINT_H

#include <stdint.h>

#include "emit/float/api.h"

/* YEP_FLOAT_FORCE_LIMBS: test hook — exercises the portable lo/hi
 * tier on hosts that also have __int128 (the limbs math is otherwise
 * only compiled on 32-bit targets). */
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS) && !defined(YEP_FLOAT_FORCE_LIMBS)
typedef unsigned __int128 yep_u128;
#define YEP_MUL64(a, b) ((yep_u128)(uint64_t)(a) * (uint64_t)(b))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
/* MSVC has no __int128: the lo/hi pair behind the SAME helper surface
 * (print.c's interval loop rides only these — no operator arithmetic
 * anywhere, so both branches share one source shape). */
typedef struct {
    uint64_t lo, hi;
} yep_u128;
#define YEP_MUL64(a, b) yep_u128_mul64(a, b)
#else
/* Any compiler without __int128 (32-bit GCC/Clang, MSVC x86-32): the
 * same lo/hi pair; multiplication via 32-bit limbs (below). */
typedef struct {
    uint64_t lo, hi;
} yep_u128;
#define YEP_MUL64(a, b) yep_u128_mul64(a, b)
#endif

static inline yep_u128 yep_u128_of(uint64_t v) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return (yep_u128)v;
#else
    yep_u128 r = {v, 0};
    return r;
#endif
}

static inline yep_u128 yep_u128_max(void) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return ~(yep_u128)0;
#else
    yep_u128 r = {~(uint64_t)0, ~(uint64_t)0};
    return r;
#endif
}

static inline yep_u128 yep_u128_add(yep_u128 a, yep_u128 b) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return a + b;
#else
    yep_u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u);
    return r;
#endif
}

static inline yep_u128 yep_u128_sub(yep_u128 a, yep_u128 b) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return a - b;
#else
    yep_u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1u : 0u);
    return r;
#endif
}

/* n < 128 */
static inline yep_u128 yep_u128_shl(yep_u128 a, unsigned n) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return a << n;
#else
    yep_u128 r = {0, 0};
    if (n == 0) {
        return a;
    }
    if (n < 64) {
        r.hi = (a.hi << n) | (a.lo >> (64 - n));
        r.lo = a.lo << n;
    } else {
        r.hi = a.lo << (n - 64);
    }
    return r;
#endif
}

/* n < 128 */
static inline yep_u128 yep_u128_shr(yep_u128 a, unsigned n) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return a >> n;
#else
    yep_u128 r = {0, 0};
    if (n == 0) {
        return a;
    }
    if (n < 64) {
        r.lo = (a.lo >> n) | (a.hi << (64 - n));
        r.hi = a.hi >> n;
    } else {
        r.lo = a.hi >> (n - 64);
    }
    return r;
#endif
}

static inline int yep_u128_cmp(yep_u128 a, yep_u128 b) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return a < b ? -1 : (a > b ? 1 : 0);
#else
    if (a.hi != b.hi) {
        return a.hi < b.hi ? -1 : 1;
    }
    if (a.lo != b.lo) {
        return a.lo < b.lo ? -1 : 1;
    }
    return 0;
#endif
}

#if defined(_MSC_VER) && defined(_M_X64)
static inline yep_u128 yep_u128_mul64(uint64_t a, uint64_t b) {
    yep_u128 r;
    r.lo = _umul128(a, b, &r.hi);
    return r;
}
#elif defined(_MSC_VER) && defined(_M_ARM64)
static inline yep_u128 yep_u128_mul64(uint64_t a, uint64_t b) {
    yep_u128 r;
    r.hi = __umulh(a, b);
    r.lo = a * b;
    return r;
}
#elif !defined(__SIZEOF_INT128__) || defined(YEP_FLOAT_FORCE_LIMBS)
/* portable: 32-bit limbs, exact (the struct tier only) */
static inline yep_u128 yep_u128_mul64(uint64_t a, uint64_t b) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32;
    uint64_t b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t ll = a0 * b0;
    uint64_t lh = a0 * b1;
    uint64_t hl = a1 * b0;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    yep_u128 r;
    r.lo = (mid << 32) | (uint32_t)ll;
    r.hi = (a1 * b1) + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return r;
}
#endif

/* x * 10 stays in range */
static inline int yep_u128_fits10(yep_u128 x) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return x <= (~(yep_u128)0) / 10;
#else
    /* exact: x*10 = (hi*10 << 64) + lo*10 — overflow iff hi*10
     * escapes 64 bits or the lo-carry wraps the sum */
    yep_u128 hp = YEP_MUL64(x.hi, 10u);
    if (hp.hi != 0) {
        return 0;
    }
    yep_u128 lp = YEP_MUL64(x.lo, 10u);
    return hp.lo + lp.hi >= hp.lo;
#endif
}

/* m small (< 2^32); caller guarantees no overflow (fits10 guarded) */
static inline yep_u128 yep_u128_mul_small(yep_u128 a, uint32_t m) {
#if defined(__SIZEOF_INT128__) && !defined(YEP_FLOAT_FORCE_LIMBS)
    return a * m;
#else
    yep_u128 r;
    yep_u128 lp = YEP_MUL64(a.lo, m);
    r.lo = lp.lo;
    r.hi = a.hi * m + lp.hi;
    return r;
#endif
}

/* The shortest-roundtrip result both tiers produce. digits[] is the
 * rounded digit sequence (no leading zero, trailing zeros kept only
 * when significant), len its length; k is the decimal exponent so the
 * value is 0.<digits> * 10^k (i.e. in [10^(k-1), 10^k)). */
typedef struct {
    char digits[24];
    int len;
    int k;
} yep_dsplit;

/* Tier B (dragon.c): exact shortest for any finite double/float bits.
 * m2/e2 is the (unified) significand/exponent: v = m2 * 2^e2 with
 * 2^mb <= m2 < 2^(mb+1) for normals, m2 < 2^mb for subnormals. */
int yep_dragon_shortest(uint64_t m2, int e2, int mb, int pow2_low, int tie_even, yep_dsplit* out);

/* Tier B exact fixed notation: digits of round_half_even(v * 10^p). */
int yep_dragon_fixed(uint64_t m2, int e2, uint32_t precision, char* buf);

/* Tier A (print.c): u128 fast tier; returns 0 when the value is out
 * of the exact-128 range and tier B must be used. */
int yep_u128_shortest(uint64_t m2, int e2, int mb, int pow2_low, int tie_even, yep_dsplit* out);

/* tie_even: whether a decimal landing exactly on a round-trip boundary
 * may be accepted — reparse ties to the even float, which is v only
 * when v's significand is even (callers pass m2 % 2 == 0). */

/* Shared rendering of a digit split into the canonical YAML form:
 * plain notation for 10^-4 <= |v| < 10^16, else d.dddde[+-]XX with a
 * leading "1.0e+20"-style fraction so the value re-parses as float. */
int yep_float_render(const yep_dsplit* ds, int negative, char* buf);

#endif /* YEP_FLOATINT_H */
