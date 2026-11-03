/* compat11.c — libyaml/Psych 1.1 implicit semantics (TODO.impl/10).
 *
 * The oracle is psych's scalar_scanner.rb; every divergence is a
 * compat bug (15's ported tests enforce it):
 *   bool: y/Y/yes/on/true/n/N/no/off/false, any case pattern psych
 *     matches (/^(yes|true|on)$/i family)
 *   int: decimal with _, 0b binary, leading-0 octal, 0x hex
 *   float: psych FLOAT + sexagesimal ints/floats + .inf/.nan
 *   timestamp: the yaml.org timestamp production (plus plain dates)
 *   '=' is the value tag; '<<' is merge (grammar-level anyway)
 * else str.
 */

#include <stdint.h>
#include <string.h>

#include "resolver.h"

static int eq(const char* p, uint32_t n, const char* s) {
    uint32_t m = 0;
    while (s[m] != '\0') {
        m++;
    }
    return n == m && (n == 0 || memcmp(p, s, n) == 0);
}

static int ci_eq3(const char* p, uint32_t n, const char* s) {
    /* case-insensitive match (psych's /i booleans) */
    uint32_t m = 0;
    while (s[m] != '\0') {
        m++;
    }
    if (n != m) {
        return 0;
    }
    for (uint32_t k = 0; k < n; k++) {
        char a = p[k], b = s[k];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int is_digits_us(const char* p, uint32_t n) {
    if (n == 0) {
        return 0;
    }
    for (uint32_t k = 0; k < n; k++) {
        if (!((p[k] >= '0' && p[k] <= '9') || p[k] == '_')) {
            return 0;
        }
    }
    return p[0] != '_';
}

static yep_tag_id compat11(void* ctx, const char* p, uint32_t n) {
    (void)ctx;
    if (n == 0 || eq(p, n, "~") || ci_eq3(p, n, "null")) {
        return 4; /* null: psych nil for ""/"~"/null (any case) */
    }
    if (ci_eq3(p, n, "yes") || ci_eq3(p, n, "true") || ci_eq3(p, n, "on") || ci_eq3(p, n, "y")) {
        return 3;
    }
    if (ci_eq3(p, n, "no") || ci_eq3(p, n, "false") || ci_eq3(p, n, "off") || ci_eq3(p, n, "n")) {
        return 3;
    }
    if (eq(p, n, "=")) {
        return 10; /* value */
    }
    if (eq(p, n, "<<")) {
        return 9; /* merge */
    }
    uint32_t i = 0;
    if (p[0] == '-' || p[0] == '+') {
        i = 1;
    }
    if (i >= n) {
        return 0;
    }
    /* timestamps: full production first (psych TIME), then plain dates */
    {
        /* -?dddd-dd-dd([Tt ]dd:dd:dd(.d*)?(Z|+dd(:dd)?|+d)?)? — the
         * date-only form handled after; conservative core check */
        uint32_t k = i;
        int digs = 0;
        while (k < n && p[k] >= '0' && p[k] <= '9' && digs < 4) {
            k++;
            digs++;
        }
        if (digs == 4 && k + 1 < n && p[k] == '-') {
            k++;
            int m1 = 0;
            while (k < n && p[k] >= '0' && p[k] <= '9' && m1 < 2) {
                k++;
                m1++;
            }
            if (m1 >= 1 && k + 1 < n && p[k] == '-') {
                k++;
                int m2 = 0;
                while (k < n && p[k] >= '0' && p[k] <= '9' && m2 < 2) {
                    k++;
                    m2++;
                }
                if (m2 >= 1) {
                    if (k == n) {
                        return 5; /* date */
                    }
                    if (p[k] == 'T' || p[k] == 't' || p[k] == ' ') {
                        /* time part: dd:dd:dd required */
                        k++; /* consume the separator */
                        int ok = 1;
                        int f[3] = {0, 0, 0};
                        int fi = 0;
                        int seen_colon = 0;
                        while (k < n) {
                            char c = p[k];
                            if (c == ':') {
                                if (f[fi] < 1 || f[fi] > 2 || fi >= 2) {
                                    ok = 0;
                                    break;
                                }
                                seen_colon = 1;
                                fi++;
                                k++;
                                continue;
                            }
                            if (c >= '0' && c <= '9' && fi <= 2) {
                                f[fi]++;
                                if (f[fi] > 2) {
                                    ok = 0;
                                    break;
                                }
                                k++;
                                continue;
                            }
                            break;
                        }
                        if (ok && seen_colon && fi == 2 && f[2] == 2) {
                            /* optional .frac, Z or offset */
                            if (k < n && p[k] == '.') {
                                k++;
                                int fd = 0;
                                while (k < n && p[k] >= '0' && p[k] <= '9') {
                                    k++;
                                    fd++;
                                }
                                if (fd == 0) {
                                    ok = 0;
                                }
                            }
                            while (ok && k < n && p[k] == ' ') {
                                k++; /* psych: \s* before the zone */
                            }
                            if (ok && k < n) {
                                if (p[k] == 'Z') {
                                    k++;
                                } else if (p[k] == '+' || p[k] == '-') {
                                    k++;
                                    int od = 0;
                                    while (k < n && ((p[k] >= '0' && p[k] <= '9') ||
                                                     (od == 2 && p[k] == ':'))) {
                                        if (p[k] == ':') {
                                            if (od != 2) {
                                                ok = 0;
                                                break;
                                            }
                                        } else {
                                            od++;
                                        }
                                        k++;
                                    }
                                    if (od < 1 || od > 4) {
                                        ok = 0;
                                    }
                                } else {
                                    ok = 0;
                                }
                            }
                            if (ok && k == n) {
                                return 5; /* timestamp */
                            }
                        }
                    }
                }
            }
        }
    }
    /* .inf/.nan (psych, with sign on inf) */
    {
        const char* r = p + i;
        uint32_t rn = n - i;
        if (rn > 1 && r[0] == '.') {
            if (ci_eq3(r, rn, ".inf") || ci_eq3(r, rn, ".nan")) {
                return 2;
            }
        }
    }
    /* sexagesimal: [-+]?d[d_]*(:[0-5]?d){1,2}(.d*)? */
    {
        uint32_t k = i;
        if (k < n && p[k] >= '0' && p[k] <= '9') {
            k++;
            while (k < n && ((p[k] >= '0' && p[k] <= '9') || p[k] == '_')) {
                k++;
            }
            if (k < n && p[k] == ':') {
                int groups = 0;
                int ok = 1;
                int is_float = 0;
                while (k < n && p[k] == ':') {
                    k++;
                    groups++;
                    int d = 0;
                    while (k < n && p[k] >= '0' && p[k] <= '9') {
                        k++;
                        d++;
                    }
                    if (d < 1 || d > 2 || groups > 2) {
                        ok = 0;
                        break;
                    }
                    if (d == 2 && p[k - 2] > '5') {
                        ok = 0; /* [0-5]?d */
                        break;
                    }
                }
                if (ok && k < n && p[k] == '.') {
                    is_float = 1;
                    k++;
                    while (k < n && ((p[k] >= '0' && p[k] <= '9') || p[k] == '_')) {
                        k++;
                    }
                }
                if (ok && k == n && groups >= 1) {
                    return is_float ? 2 : 1;
                }
            }
        }
    }
    /* 0b / 0x / leading-0 octal / decimal int with underscores */
    if (n - i > 2 && p[i] == '0' && (p[i + 1] == 'b' || p[i + 1] == 'B')) {
        return is_digits_us(p + i + 2, n - i - 2) ? 1 : 0;
    }
    if (n - i > 2 && p[i] == '0' &&
        p[i + 1] == 'x') { /* lowercase only — psych keeps "0X1A" a String */
        uint32_t k = i + 2;
        if (p[k] == '_') {
            k++;
        }
        int d = 0;
        for (; k < n; k++) {
            char c = p[k];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                c == '_') {
                if (c != '_') {
                    d++;
                }
                continue;
            }
            return 0;
        }
        return d ? 1 : 0;
    }
    if (n - i >= 2 && p[i] == '0') {
        /* leading-0 octal (psych: 0[_]*[0-7][0-7_]*) */
        uint32_t k = i + 1;
        while (k < n && p[k] == '_') {
            k++;
        }
        int d = 0;
        for (; k < n; k++) {
            if (p[k] >= '0' && p[k] <= '7') {
                d++;
            } else if (p[k] == '_') {
                continue;
            } else {
                break;
            }
        }
        if (d && k == n) {
            return 1;
        }
        if (d == 0 && k == n && p[n - 1] == '0') {
            return 1; /* plain 0 */
        }
    }
    /* decimal int / float with underscores and commas (psych legacy).
     * The float law is psych's scanner (and libyaml's 1.1 resolver): a
     * DOT is required, and an exponent only counts when SIGNED —
     * "1e0" and "1.5e2" stay strings (psych tokenize; libyaml's 1.1
     * float regex agrees), "1.5" / ".5" / "1.5e+2" are floats. The
     * unqualified-exponent float leaked host strings into number
     * typing (yeptris-ruby #293's CBOR dump). */
    {
        int digits = 0, dot = 0, e = 0, ed = 0, esigned = 0;
        for (uint32_t k = i; k < n; k++) {
            char c = p[k];
            if (c >= '0' && c <= '9') {
                if (e) {
                    ed = 1;
                } else {
                    digits = 1;
                }
            } else if ((c == '_' || c == ',') && !e) {
                /* separators */
            } else if (c == '.' && !e) {
                if (dot) {
                    return 0;
                }
                dot = 1;
            } else if ((c == 'e' || c == 'E') && digits && !e) {
                e = 1;
                if (k + 1 < n && (p[k + 1] == '-' || p[k + 1] == '+')) {
                    esigned = 1;
                    k++;
                }
            } else {
                return 0;
            }
        }
        if (digits && dot && (!e || (ed && esigned))) {
            return 2;
        }
        if (digits && !dot && !e) {
            return 1;
        }
    }
    return 0; /* str */
}

static const yep_resolver k_compat11 = {compat11, NULL, NULL};

const yep_resolver* yep_resolver_compat11(void) {
    return &k_compat11;
}
