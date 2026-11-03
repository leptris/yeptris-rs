/* numbers.c — the typed-value conversion kernels (TODO.impl/08B).
 *
 * Extracted verbatim from parse.c's node accessors: behavior is
 * pinned by the typed-accessor spec battery and the 20k randomized
 * strtod cross-checks. */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "numbers.h"

#define NUM_BUF 80

/* strips '_' and ',' into a NUL-terminated stack buffer; returns the
 * cleaned length (the raw length when nothing needed stripping —
 * callers then use the original bytes) */
static size_t num_clean(const char* p, uint32_t len, char* buf) {
    if (memchr(p, '_', len) == NULL && memchr(p, ',', len) == NULL) {
        return len; /* caller reads p directly */
    }
    size_t o = 0;
    for (uint32_t i = 0; i < len && o + 1 < NUM_BUF; i++) {
        char c = p[i];
        if (c == '_' || c == ',') {
            continue;
        }
        buf[o++] = c;
    }
    buf[o] = '\0';
    return o;
}

/* Clinger-bounded exact decimal: mantissa < 2^53, adjusted
 * exponent within +-22 — 23-entry pow10 table (08B) */
static const double k_pow10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static int fast_double(const char* s, size_t len, double* out) {
    size_t i = 0;
    int neg = 0;
    if (i < len && (s[i] == '-' || s[i] == '+')) {
        neg = s[i] == '-';
        i++;
    }
    uint64_t m = 0;
    int digits = 0;
    int dot = -1;
    for (; i < len; i++) {
        char c = s[i];
        if (c == '.') {
            if (dot >= 0) {
                return 0;
            }
            dot = (int)digits;
            continue;
        }
        if (c < '0' || c > '9') {
            break;
        }
        if (digits >= 15) {
            return 0; /* outside the exact range */
        }
        m = m * 10u + (uint64_t)(c - '0');
        digits++;
    }
    if (digits == 0 || (i < len && s[i] != 'e' && s[i] != 'E')) {
        return 0; /* empty mantissa or trailing junk */
    }
    int e10 = 0;
    if (i < len) {
        i++; /* e/E */
        int eneg = 0;
        if (i < len && (s[i] == '-' || s[i] == '+')) {
            eneg = s[i] == '-';
            i++;
        }
        if (i >= len) {
            return 0;
        }
        for (; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') {
                return 0;
            }
            e10 = e10 * 10 + (s[i] - '0');
            if (e10 > 308) {
                return 0;
            }
        }
        if (eneg) {
            e10 = -e10;
        }
    }
    if (dot >= 0) {
        e10 -= digits - dot;
    }
    if (e10 > 22 || e10 < -22 || m >= (1ull << 53)) {
        return 0; /* outside Clinger's exact range */
    }
    if (e10 >= 0) {
        double v = (double)m * k_pow10[e10];
        *out = neg ? -v : v;
    } else {
        double v = (double)m / k_pow10[-e10];
        *out = neg ? -v : v;
    }
    return 1;
}

int yep_num_i64(const char* p, uint32_t len, int64_t* out) {
    if (p == NULL || len == 0) {
        return 1;
    }
    char buf[NUM_BUF];
    size_t l = num_clean(p, len, buf);
    const char* num =
        (l == len && memchr(p, '_', len) == NULL && memchr(p, ',', len) == NULL) ? p : buf;
    if (num != p) {
        /* cleaned copy already NUL-terminated by num_clean */
    } else {
        if (l >= NUM_BUF) {
            return 1;
        }
        memcpy(buf, num, l);
        buf[l] = '\0';
        num = buf;
    }
    int base = 10;
    const char* s = buf;
    if (buf[0] == '-' || buf[0] == '+') {
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
    } else if (s[0] == '0' && s[1] == 'o') {
        memmove(buf + (size_t)(s - buf) + 1, s + 2, l - (size_t)(s - buf) - 1);
        l -= 1;
        buf[l] = '\0';
        base = 8; /* 0o17 -> 017 octal */
    } else if (s[0] == '0' && s[1] != '\0' && s[1] != '.') {
        base = 8; /* compat leading-0 octal */
    }
    if (memchr(buf, ':', l) != NULL) {
        /* Psych's sexagesimal fold, verbatim (scalar_scanner.rb):
         * component e weighs 60 ** |e - 2| and the SIGN rides the
         * first component only (-1:30 -> -3600 + 1800 = -1800, not
         * -5400). A 2-component value is H:M, seconds implicitly
         * zero (1:30 -> 5400). TODO.restructure/43 / issue #30. */
        const char* q = buf;
        long long sign = 1;
        if (q[0] == '-' || q[0] == '+') {
            if (q[0] == '-')
                sign = -1;
            q++;
        }
        long long v = 0;
        while (*q >= '0' && *q <= '9') {
            int d = *q - '0';
            /* the ERANGE law, spelled out: the plain path rejects
             * unrepresentable values via strtoll, so the fold must
             * too (UBSan: 2222222222222222222 * 10 overflowed, the
             * nightly's round on the combined build) */
            if (v > (LLONG_MAX - (long long)d) / 10) {
                return 1;
            }
            v = v * 10 + d;
            q++;
        }
        if (v > LLONG_MAX / 3600) {
            return 1;
        }
        v *= 3600; /* e0 weight */
        if (sign < 0)
            v = -v;
        int groups = 0;
        while (*q == ':' && groups < 2) {
            q++;
            long long g = 0;
            int d = 0;
            while (*q >= '0' && *q <= '9') {
                int dg = *q - '0';
                /* the group component rides the same ERANGE law as
                 * the first component -- the catch-124 fix guarded
                 * the hours loop but left this one raw (UBSan:
                 * 999999999999999999 * 10, the nightly's run
                 * 39049702014) */
                if (g > (LLONG_MAX - (long long)dg) / 10) {
                    return 1;
                }
                g = g * 10 + dg;
                q++;
                d++;
            }
            if (d == 0) {
                return 1;
            }
            long long w = groups == 0 ? 60 : 1; /* e1 then e2 */
            if (g > LLONG_MAX / w || v > LLONG_MAX - g * w) {
                return 1;
            }
            v += g * w;
            groups++;
        }
        if (groups == 0 || *q != '\0') {
            return 1;
        }
        *out = v;
        return 0;
    }
    char* end = NULL;
    errno = 0;
    long long v = strtoll(buf, &end, base);
    if (end == buf || *end != '\0' || errno == ERANGE) {
        return 1;
    }
    *out = (int64_t)v;
    return 0;
}

int yep_num_f64(const char* p, uint32_t len, double* out) {
    if (p == NULL || len == 0) {
        return 1;
    }
    char buf[NUM_BUF];
    size_t l = num_clean(p, len, buf);
    const char* num =
        (l == len && memchr(p, '_', len) == NULL && memchr(p, ',', len) == NULL) ? p : buf;
    /* 08B fast path: the common decimal shape converts exactly with
     * integer arithmetic; everything else falls to strtod */
    {
        double fast;
        if (fast_double(num, l, &fast)) {
            *out = fast;
            return 0;
        }
    }
    if (num != p) {
        /* cleaned copy is NUL-terminated */
    } else {
        if (l >= NUM_BUF) {
            return 1;
        }
        memcpy(buf, num, l);
        buf[l] = '\0';
        num = buf;
    }
    /* .inf / .nan family (sign allowed on inf) */
    {
        const char* s = buf;
        size_t sl = l;
        if (s[0] == '-' || s[0] == '+') {
            s++;
            sl--;
        }
        if (sl == 4 && s[0] == '.') {
            if ((s[1] == 'i' || s[1] == 'I') && (s[2] == 'n' || s[2] == 'N') &&
                (s[3] == 'f' || s[3] == 'F')) {
                *out = (buf[0] == '-') ? -INFINITY : INFINITY;
                return 0;
            }
            if ((s[1] == 'n' || s[1] == 'N') && (s[2] == 'a' || s[2] == 'A') &&
                (s[3] == 'n' || s[3] == 'N')) {
                *out = NAN;
                return 0;
            }
        }
    }
    /* sexagesimal float — Psych's fold: weights 60**|e-2|, sign on
     * the first component, the fraction on the LAST component
     * (1:30.5 -> 1*3600 + 30.5*60 = 5430.0). */
    if (memchr(buf, ':', l) != NULL) {
        const char* s = buf;
        double sign = 1.0;
        if (s[0] == '-' || s[0] == '+') {
            if (s[0] == '-')
                sign = -1.0;
            s++;
        }
        double v = 0.0;
        const char* q = s;
        while (*q >= '0' && *q <= '9') {
            v = v * 10.0 + (*q - '0');
            q++;
        }
        v *= 3600.0;
        v *= sign;
        int groups = 0;
        double last = 0.0;
        double weight = 60.0;
        for (;;) {
            if (*q != ':' || groups >= 2)
                break;
            q++;
            double g = 0.0;
            int d = 0;
            while (*q >= '0' && *q <= '9') {
                g = g * 10.0 + (*q - '0');
                q++;
                d++;
            }
            if (d == 0) {
                return 1;
            }
            if (*q == '.') {
                double frac = 0.0, scale = 0.1;
                q++;
                while (*q >= '0' && *q <= '9') {
                    frac += (*q - '0') * scale;
                    scale /= 10.0;
                    q++;
                }
                g += frac;
            }
            v += g * weight;
            weight = 1.0; /* e2 */
            last = g;
            groups++;
        }
        (void)last;
        if (groups == 0 || *q != '\0') {
            return 1;
        }
        *out = v;
        return 0;
    }
    char* end = NULL;
    errno = 0;
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0') {
        return 1;
    }
    (void)errno;
    *out = v;
    return 0;
}

int yep_num_bool(const char* p, uint32_t len) {
    static const char* k_true[] = {"true", "True", "TRUE", "y",  "Y", "yes",
                                   "Yes",  "YES",  "on",   "On", "ON"};
    for (size_t i = 0; i < sizeof(k_true) / sizeof(k_true[0]); i++) {
        size_t m = strlen(k_true[i]);
        if (len == m && memcmp(p, k_true[i], m) == 0) {
            return 1;
        }
    }
    return 0;
}

int yep_num_bool_ci(const char* p, uint32_t len) {
    /* the compat resolver matches bool words case-insensitively;
     * truth follows the same rule (Psych's scanner downcases) */
    static const char* k_true[] = {"y", "yes", "true", "on"};
    char buf[8];
    if (len == 0 || len > 5) {
        return 0;
    }
    for (uint32_t i = 0; i < len; i++) {
        char c = p[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    for (size_t i = 0; i < sizeof(k_true) / sizeof(k_true[0]); i++) {
        size_t m = strlen(k_true[i]);
        if (len == m && memcmp(buf, k_true[i], m) == 0) {
            return 1;
        }
    }
    return 0;
}
