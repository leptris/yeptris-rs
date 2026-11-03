/* dragon.c — tier B: exact shortest/fixed output via the interval
 * method over a fixed-capacity limb vector (TODO.impl/14, clean room).
 *
 * v = r/s exactly with r, s, mm, mp integers: value r/s, round-trip
 * interval ((r - mm)/s, (r + mp)/s). Each digit step advances r, mm,
 * mp by 10 (s fixed) so the boundaries stay in the remaining-value
 * space. A position can stop iff truncating (error r/s <= mm/s) or
 * rounding up (error (s-r)/s <= mp/s) stays inside; when both are
 * legal the nearer wins, ties to the even digit (reparse is
 * round-to-nearest-even). When neither is legal, another digit is
 * required — IEEE-754 guarantees termination by 17 digits (double) /
 * 9 (float).
 */

#include <string.h>

#include "emit/float/floatint.h"

/* ---- tiny limb vector: 32-bit limbs, little-endian ---- */

#define LIMBS 44 /* < 1080 binary digits + decimal scaling headroom */

typedef struct {
    uint32_t d[LIMBS];
    int n;
} num;

static void num_zero(num* a) {
    memset(a, 0, sizeof(*a));
    a->n = 1;
}

static void num_set_u64(num* a, uint64_t v) {
    num_zero(a);
    a->d[0] = (uint32_t)v;
    a->d[1] = (uint32_t)(v >> 32);
    a->n = a->d[1] ? 2 : 1;
}

static int num_cmp(const num* a, const num* b) {
    if (a->n != b->n) {
        return a->n < b->n ? -1 : 1;
    }
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i]) {
            return a->d[i] < b->d[i] ? -1 : 1;
        }
    }
    return 0;
}

static void num_trim(num* a) {
    while (a->n > 1 && a->d[a->n - 1] == 0) {
        a->n--;
    }
}

static int num_add(num* a, const num* b) {
    uint64_t carry = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        uint64_t s = (uint64_t)a->d[i] + (uint64_t)(i < b->n ? b->d[i] : 0) + carry;
        a->d[i] = (uint32_t)s;
        carry = s >> 32;
    }
    a->n = n;
    if (carry) {
        if (a->n >= LIMBS) {
            return -1;
        }
        a->d[a->n++] = (uint32_t)carry;
    }
    return 0;
}

static void num_sub(num* a, const num* b) {
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t s = (int64_t)a->d[i] - (int64_t)(i < b->n ? b->d[i] : 0) - borrow;
        if (s < 0) {
            s += ((int64_t)1 << 32);
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->d[i] = (uint32_t)s;
    }
    num_trim(a);
}

static int num_shl(num* a, int bits) {
    for (; bits >= 32; bits -= 32) {
        for (int i = a->n; i > 0; i--) {
            a->d[i] = a->d[i - 1];
        }
        a->d[0] = 0;
        a->n++;
        if (a->n > LIMBS) {
            return -1;
        }
    }
    if (bits > 0) {
        uint32_t carry = 0;
        for (int i = 0; i < a->n; i++) {
            uint32_t nc = a->d[i] >> (32 - bits);
            a->d[i] = (a->d[i] << bits) | carry;
            carry = nc;
        }
        if (carry) {
            if (a->n >= LIMBS) {
                return -1;
            }
            a->d[a->n++] = carry;
        }
    }
    return 0;
}

static void num_shr(num* a, int bits) {
    int limbs = bits / 32;
    int rem = bits % 32;
    if (limbs >= a->n) {
        memset(a->d, 0, sizeof(uint32_t) * (size_t)a->n);
        a->n = 1;
        return;
    }
    for (int i = 0; i + limbs < a->n; i++) {
        /* rem == 0 would shift by 32 (UB on uint32_t) — the hi limb
         * is only meaningful when there IS a fractional shift */
        if (rem == 0) {
            a->d[i] = a->d[i + limbs];
            continue;
        }
        uint32_t lo = a->d[i + limbs] >> rem;
        uint32_t hi = (i + limbs + 1 < a->n) ? a->d[i + limbs + 1] << (32 - rem) : 0;
        a->d[i] = lo | hi;
    }
    for (int i = a->n - limbs; i < a->n; i++) {
        a->d[i] = 0;
    }
    a->n -= limbs;
    num_trim(a);
}

static int num_mul10(num* a) {
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t p = (uint64_t)a->d[i] * 10 + carry;
        a->d[i] = (uint32_t)p;
        carry = p >> 32;
    }
    if (carry) {
        if (a->n >= LIMBS) {
            return -1;
        }
        a->d[a->n++] = (uint32_t)carry;
    }
    return 0;
}

static int num_is_zero(const num* a) {
    return a->n == 1 && a->d[0] == 0;
}

/* Decimal digits of a (a >= 0) into dst[cap]; a == 0 yields "0".
 * Returns the length, or -1 on overflow of cap. */
static int num_to_digits(const num* a, char* dst, int cap) {
    char rev[400];
    int n = 0;
    if (num_is_zero(a)) {
        if (cap < 1) {
            return -1;
        }
        dst[0] = '0';
        return 1;
    }
    num t = *a;
    while (!num_is_zero(&t)) {
        uint64_t rem = 0;
        for (int i = t.n - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | t.d[i];
            t.d[i] = (uint32_t)(cur / 1000000000u);
            rem = cur % 1000000000u;
        }
        num_trim(&t);
        for (int k = 0; k < 9; k++) {
            rev[n++] = (char)('0' + (int)(rem % 10));
            rem /= 10;
            if (rem == 0 && num_is_zero(&t)) {
                break;
            }
        }
        if (n >= (int)sizeof(rev)) {
            return -1;
        }
    }
    if (n > cap) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        dst[i] = rev[n - 1 - i];
    }
    return n;
}

/* ---- exact shortest ---- */

int yep_dragon_shortest(uint64_t m2, int e2, int mb, int pow2_low, int tie_even, yep_dsplit* out) {
    num r, s, mm, mp;
    num_set_u64(&r, m2);
    num_zero(&s);
    s.d[0] = 1;
    num_set_u64(&mm, 1);
    num_set_u64(&mp, 1);
    if (e2 >= 0) {
        if (num_shl(&r, e2) != 0 || num_shl(&mm, e2 - 1) != 0) {
            return -1;
        }
        mp = mm;
    } else {
        /* v = m2 / 2^-e2 with HALF-ulp boundaries: express with r = 2*m2
         * over s = 2^(1-e2) so mm = mp = 1 is exactly the half-ulp */
        if (num_shl(&r, 1) != 0 || num_shl(&s, 1 - e2) != 0) {
            return -1;
        }
    }
    if (pow2_low) {
        /* power of two: the lower neighbor is one binade down, so the
         * lower half-ulp is half the upper — widen by scaling r, s, mp
         * by 2 (mm untouched halves relative to mp) */
        if (num_shl(&r, 1) != 0 || num_shl(&s, 1) != 0 || num_shl(&mp, 1) != 0) {
            return -1;
        }
    }

    /* normalize the remaining value into [1/10, 1): v = 0.<digits> x 10^k */
    int k = 0;
    {
        num rmp = r;
        if (num_add(&rmp, &mp) != 0) {
            return -1;
        }
        while (num_cmp(&rmp, &s) <= 0) {
            num_mul10(&r);
            num_mul10(&mm);
            num_mul10(&mp);
            rmp = r;
            if (num_add(&rmp, &mp) != 0) {
                return -1;
            }
            k--;
        }
        while (num_cmp(&r, &s) >= 0) {
            if (num_mul10(&s) != 0) {
                return -1;
            }
            k++;
        }
    }

    const int max_digits = (mb >= 52) ? 17 : 9;
    char digits[24];
    int n = 0;
    for (;;) {
        num_mul10(&r);
        num_mul10(&mm);
        num_mul10(&mp);
        int d = 0;
        while (num_cmp(&r, &s) >= 0) {
            num_sub(&r, &s);
            d++;
        }
        digits[n] = (char)('0' + d);
        n++;
        /* can truncation stay inside? error r <= mm */
        num rmm = mm;
        int eq_down = num_cmp(&r, &rmm) == 0;
        int can_down = num_cmp(&r, &rmm) < 0 || (tie_even && eq_down);
        /* can rounding up stay inside? error (s - r) <= mp */
        num sr = s;
        num_sub(&sr, &r);
        int eq_up = num_cmp(&sr, &mp) == 0;
        int can_up = num_cmp(&sr, &mp) < 0 || (tie_even && eq_up);
        if (can_down || can_up) {
            int up;
            if (can_down && can_up) {
                num t = r;
                if (num_add(&t, &r) != 0) {
                    return -1;
                }
                int c = num_cmp(&t, &s);
                if (c > 0) {
                    up = 1;
                } else if (c < 0) {
                    up = 0;
                } else {
                    up = (d % 2) != 0; /* tie to the even digit */
                }
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
            /* safety net: 17 (9) significant digits always round-trip */
            break;
        }
        if (n >= 23) {
            return -1;
        }
    }
    memcpy(out->digits, digits, (size_t)n);
    out->len = n;
    out->k = k;
    return 0;
}

/* ---- exact fixed notation: digits of round_half_even(v x 10^p) ---- */

int yep_dragon_fixed(uint64_t m2, int e2, uint32_t precision, char* buf) {
    num q;
    num_set_u64(&q, m2);
    if (e2 >= 0) {
        if (num_shl(&q, e2) != 0) {
            return -1;
        }
    }
    for (uint32_t i = 0; i < precision; i++) {
        if (num_mul10(&q) != 0) {
            return -1;
        }
    }
    if (e2 < 0) {
        /* digits = round_half_even(N / 2^-e2): the remainder against
         * the exact half decides; ties round to the even quotient */
        int bits = -e2;
        num half;
        num_set_u64(&half, 1);
        if (num_shl(&half, bits - 1) != 0) {
            return -1;
        }
        num qshift = q;
        num_shr(&qshift, bits);
        num back = qshift;
        if (num_shl(&back, bits) != 0) {
            return -1;
        }
        num rem = q;
        num_sub(&rem, &back);
        int cmp = num_cmp(&rem, &half);
        if (cmp > 0 || (cmp == 0 && (qshift.d[0] & 1))) {
            num one;
            num_set_u64(&one, 1);
            if (num_add(&qshift, &one) != 0) {
                return -1;
            }
        }
        q = qshift;
    }
    char digits[400];
    int len = num_to_digits(&q, digits, (int)sizeof(digits));
    if (len < 0) {
        return -1;
    }
    const char* tmp = digits;
    if (precision == 0) {
        memcpy(buf, tmp, (size_t)len);
        return len;
    }
    if ((uint32_t)len <= precision) {
        int pad = (int)precision - len;
        int o = 0;
        buf[o++] = '0';
        buf[o++] = '.';
        for (int i = 0; i < pad; i++) {
            buf[o++] = '0';
        }
        memcpy(buf + o, tmp, (size_t)len);
        return o + len;
    }
    memcpy(buf, tmp, (size_t)(len - (int)precision));
    buf[len - (int)precision] = '.';
    memcpy(buf + len - (int)precision + 1, tmp + (len - (int)precision), (size_t)precision);
    return len + 1;
}
