/* print.c — the float printer (TODO.impl/14, clean room; see
 * floatint.h for the licensing note and the two-tier design).
 *
 * Public entries decode IEEE bits, dispatch tiers, and render the
 * digit split into the canonical YAML form. The u128 tier carries the
 * same interval logic as dragon.c over machine integers; values whose
 * exact scaled representation exceeds 128 bits fall through to the
 * limb tier (correct for every double/float, slower for extremes).
 */

#include <math.h>
#include <string.h>

#include "emit/float/floatint.h"

/* ---- decode ---- */

typedef struct {
    uint64_t m2;
    int e2;
    int pow2_low;
    int negative;
} yep_fdecode;

static int decode_double(double d, yep_fdecode* out) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    out->negative = (int)(bits >> 63);
    uint32_t exp = (uint32_t)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0xFFFFFFFFFFFFFull;
    if (exp == 0x7FF) {
        return 0; /* inf/nan handled by the caller */
    }
    if (exp == 0) {
        out->m2 = mant;
        out->e2 = -1074;
        out->pow2_low = 0; /* subnormals: uniform spacing */
    } else {
        out->m2 = mant | (1ull << 52);
        out->e2 = (int)exp - 1075;
        out->pow2_low = (mant == 0); /* exact power of two */
    }
    return 1;
}

static int decode_float(float f, yep_fdecode* out) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    out->negative = (int)(bits >> 31);
    uint32_t exp = (bits >> 23) & 0xFF;
    uint32_t mant = bits & 0x7FFFFF;
    if (exp == 0xFF) {
        return 0;
    }
    if (exp == 0) {
        out->m2 = mant;
        out->e2 = -149;
        out->pow2_low = 0;
    } else {
        out->m2 = mant | (1u << 23);
        out->e2 = (int)exp - 150;
        out->pow2_low = (mant == 0);
    }
    return 1;
}

/* ---- tier A: the interval loop over u128 ---- */

typedef struct {
    yep_u128 v;
    int n; /* limb count in 64-bit halves, 1 or 2 */
} u128v;

/* u128_cmp/u128_max/fits10 live in floatint.h — the portable helper
 * surface (MSVC rides the same expressions; no operator arithmetic) */
#define u128_cmp yep_u128_cmp
#define fits10(x) yep_u128_fits10(x)
#define u128_max yep_u128_max()

int yep_u128_shortest(uint64_t m2, int e2, int mb, int pow2_low, int tie_even, yep_dsplit* out) {
    yep_u128 r = yep_u128_of(m2), s = yep_u128_of(1), mm = yep_u128_of(1), mp = yep_u128_of(1);
    /* Capacity: every value below must fit 128 bits. For e2 >= 0 the
     * operands grow by 2^e2 (double: e2 <= 75 keeps r < 2^128); for
     * e2 < 0 only s grows (s = 2^-e2 <= 2^96 for the tier range). */
    if (e2 >= 0) {
        if (e2 > 75) {
            return 0;
        }
        r = yep_u128_shl(r, (unsigned)e2);
        if (e2 >= 1) {
            mp = yep_u128_shl(mp, (unsigned)(e2 - 1));
        } else {
            mp = yep_u128_of(1);
        }
        mm = mp;
        if (yep_u128_cmp(r, yep_u128_shr(u128_max, (unsigned)e2)) > 0) {
            return 0; /* shifted out of range */
        }
    } else {
        int bits = 1 - e2; /* half-ulp denominator (see dragon.c) */
        if (bits > 96) {
            return 0;
        }
        r = yep_u128_shl(r, 1);
        s = yep_u128_shl(s, (unsigned)bits);
        if (yep_u128_cmp(r, yep_u128_shr(u128_max, 1)) > 0 ||
            yep_u128_cmp(s, yep_u128_shr(u128_max, (unsigned)bits)) > 0) {
            return 0;
        }
    }
    if (pow2_low) {
        r = yep_u128_shl(r, 1);
        s = yep_u128_shl(s, 1);
        mp = yep_u128_shl(mp, 1);
        if (yep_u128_cmp(r, yep_u128_shr(u128_max, 1)) > 0 ||
            yep_u128_cmp(s, yep_u128_shr(u128_max, 1)) > 0 ||
            yep_u128_cmp(mp, yep_u128_shr(u128_max, 1)) > 0) {
            return 0;
        }
    }
    /* digit loop grows r, mm, mp by up to 10^(1 + 17) worst case and s
     * by up to 10^(~22): 128-bit headroom must hold */
    int k = 0;
    {
        yep_u128 rmp = yep_u128_add(r, mp);
        int guard = 0;
        while (u128_cmp(rmp, s) <= 0) {
            if (!fits10(r) || !fits10(mm) || !fits10(mp)) {
                return 0;
            }
            r = yep_u128_mul_small(r, 10);
            mm = yep_u128_mul_small(mm, 10);
            mp = yep_u128_mul_small(mp, 10);
            rmp = yep_u128_add(r, mp);
            k--;
            if (++guard > 24) {
                return 0;
            }
        }
        guard = 0;
        while (u128_cmp(r, s) >= 0) {
            if (!fits10(s)) {
                return 0;
            }
            s = yep_u128_mul_small(s, 10);
            k++;
            if (++guard > 24) {
                return 0;
            }
        }
    }

    const int max_digits = (mb >= 52) ? 17 : 9;
    char digits[24];
    int n = 0;
    for (;;) {
        if (!fits10(r) || !fits10(mm) || !fits10(mp)) {
            return 0; /* out of u128 range mid-loop: tier B */
        }
        r = yep_u128_mul_small(r, 10);
        mm = yep_u128_mul_small(mm, 10);
        mp = yep_u128_mul_small(mp, 10);
        int d = 0;
        while (u128_cmp(r, s) >= 0) {
            r = yep_u128_sub(r, s);
            d++;
        }
        digits[n] = (char)('0' + d);
        n++;
        /* an exact boundary hit ties at reparse to the even float —
         * acceptable only when v itself is even (tie_even) */
        int can_down = u128_cmp(r, mm) < 0 || (tie_even && u128_cmp(r, mm) == 0);
        int can_up = u128_cmp(yep_u128_sub(s, r), mp) < 0 ||
                     (tie_even && u128_cmp(yep_u128_sub(s, r), mp) == 0);
        if (can_down || can_up) {
            int up;
            if (can_down && can_up) {
                yep_u128 t = yep_u128_add(r, r);
                int c = u128_cmp(t, s);
                up = (c > 0) || (c == 0 && (d % 2) != 0);
            } else {
                up = can_up;
            }
            if (up) {
                int i = n - 1;
                for (;;) {
                    if (digits[i] != '9') {
                        digits[i]++;
                        break;
                    }
                    digits[i] = '0';
                    if (i == 0) {
                        for (int j = n; j > 0; j--) {
                            digits[j] = digits[j - 1];
                        }
                        digits[0] = '1';
                        n++;
                        k++;
                        break;
                    }
                    i--;
                }
            }
            break;
        }
        if (n >= max_digits) {
            break; /* 17 (9) significant digits always round-trip */
        }
        if (n >= 23) {
            return 0;
        }
    }
    memcpy(out->digits, digits, (size_t)n);
    out->len = n;
    out->k = k;
    return 1;
}

/* ---- rendering ---- */

int yep_float_render(const yep_dsplit* ds, int negative, char* buf) {
    int o = 0;
    if (negative) {
        buf[o++] = '-';
    }
    const char* d = ds->digits;
    int len = ds->len;
    int k = ds->k;
    if (len == 1 && d[0] == '0') {
        memcpy(buf + o, "0.0", 3);
        return o + 3;
    }
    /* plain notation for 10^-4 <= |v| < 10^16 (k in [-3, 16]) */
    if (k >= -3 && k <= 16) {
        if (k <= 0) {
            buf[o++] = '0';
            buf[o++] = '.';
            for (int i = 0; i < -k; i++) {
                buf[o++] = '0';
            }
            memcpy(buf + o, d, (size_t)len);
            return o + len;
        }
        if (k < len) {
            memcpy(buf + o, d, (size_t)k);
            buf[o + k] = '.';
            memcpy(buf + o + k + 1, d + k, (size_t)(len - k));
            return o + len + 1;
        }
        memcpy(buf + o, d, (size_t)len);
        for (int i = 0; i < k - len; i++) {
            buf[o + len + i] = '0';
        }
        int end = o + k;
        buf[end] = '.';
        buf[end + 1] = '0';
        return end + 2;
    }
    /* scientific: d.dddde[+-]XX, at least one fraction digit */
    buf[o++] = d[0];
    buf[o++] = '.';
    if (len == 1) {
        buf[o++] = '0';
    } else {
        memcpy(buf + o, d + 1, (size_t)(len - 1));
        o += len - 1;
    }
    buf[o++] = 'e';
    int e = k - 1;
    if (e < 0) {
        buf[o++] = '-';
        e = -e;
    } else {
        buf[o++] = '+';
    }
    char er[8];
    int en = 0;
    while (e > 0) {
        er[en++] = (char)('0' + e % 10);
        e /= 10;
    }
    if (en == 0) {
        er[en++] = '0';
    }
    if (en == 1) {
        er[en++] = '0'; /* two-digit minimum: 1.0e-07, Float#to_s shape */
    }
    for (int i = 0; i < en; i++) {
        buf[o++] = er[en - 1 - i];
    }
    return o;
}

/* ---- public entries ---- */

int yep_d2s_shortest(double d, char* buf) {
    yep_fdecode dec;
    if (!decode_double(d, &dec)) {
        return 0; /* non-finite: the caller consults yep_d2s_nonfinite */
    }
    if (dec.m2 == 0) {
        int o = dec.negative;
        if (dec.negative) {
            buf[0] = '-';
        }
        memcpy(buf + o, "0.0", 3);
        return o + 3;
    }
    yep_dsplit ds;
    int tie_even = (dec.m2 % 2) == 0;
    if (!yep_u128_shortest(dec.m2, dec.e2, 52, dec.pow2_low, tie_even, &ds)) {
        if (yep_dragon_shortest(dec.m2, dec.e2, 52, dec.pow2_low, tie_even, &ds) != 0) {
            return 0;
        }
    }
    return yep_float_render(&ds, dec.negative, buf);
}

int yep_f2s_shortest(float f, char* buf) {
    yep_fdecode dec;
    if (!decode_float(f, &dec)) {
        return 0;
    }
    if (dec.m2 == 0) {
        int o = dec.negative;
        if (dec.negative) {
            buf[0] = '-';
        }
        memcpy(buf + o, "0.0", 3);
        return o + 3;
    }
    yep_dsplit ds;
    int tie_even = (dec.m2 % 2) == 0;
    if (!yep_u128_shortest(dec.m2, dec.e2, 23, dec.pow2_low, tie_even, &ds)) {
        if (yep_dragon_shortest(dec.m2, dec.e2, 23, dec.pow2_low, tie_even, &ds) != 0) {
            return 0;
        }
    }
    return yep_float_render(&ds, dec.negative, buf);
}

int yep_d2fixed(double d, uint32_t precision, char* buf) {
    if (!isfinite(d)) {
        int n = yep_d2s_nonfinite(d, buf);
        if (n == 0) {
            return 0;
        }
        /* fixed notation of non-finite is the word itself */
        return n;
    }
    yep_fdecode dec;
    decode_double(d, &dec);
    if (dec.m2 == 0) {
        int o = 0;
        if (dec.negative && precision > 0) {
            buf[o++] = '-';
        }
        buf[o++] = '0';
        if (precision > 0) {
            buf[o++] = '.';
            for (uint32_t i = 0; i < precision; i++) {
                buf[o++] = '0';
            }
        }
        return o;
    }
    /* sign applies after digit production (printf prints -0.00) */
    char tmp[512];
    int n = yep_dragon_fixed(dec.m2, dec.e2, precision, tmp);
    if (n <= 0) {
        return n;
    }
    if (dec.negative) {
        buf[0] = '-';
        memcpy(buf + 1, tmp, (size_t)n);
        return n + 1;
    }
    memcpy(buf, tmp, (size_t)n);
    return n;
}

int yep_d2s_nonfinite(double d, char* buf) {
    if (isnan(d)) {
        memcpy(buf, ".nan", 4);
        return 4;
    }
    if (isinf(d)) {
        if (d < 0) {
            memcpy(buf, "-.inf", 5);
            return 5;
        }
        memcpy(buf, ".inf", 4);
        return 4;
    }
    return 0;
}
