/* core12.c — the YAML 1.2 core schema (TODO.impl/10).
 *
 * null: "", "~", "null", "Null", "NULL"
 * bool: true/True/TRUE, false/False/FALSE
 * int:  [-+]?[0-9]+ | [-+]?0o[0-7]+ | [-+]?0x[0-9a-fA-F]+
 * float: core float production, [-+]?(\.inf|\.Inf|\.INF), \.nan/\.NaN/\.NAN
 * else str
 *
 * Shape (TODO.restructure/71): the first-byte gate, then ONE branch —
 * word lead letters do the null/true/false compares and return, digits
 * and signs take the number walk. The old chain tried the word memcmps
 * for every length-4/5 scalar before ruling (anchor-heavy: 42 cycles
 * per call on gate-passing strings).
 */

#include <stdint.h>
#include <string.h>

#include "resolver.h"

static yep_tag_id core12(void* ctx, const char* p, uint32_t n) {
    (void)ctx;
    if (n == 0) {
        return 4; /* null */
    }
    unsigned char c0 = (unsigned char)p[0];
    unsigned l0 = c0 | 0x20u;

    /* The gate (profile: 6.5% of deep-nesting was the reject chain):
     * a first byte that can begin no core production is a string. */
    if (!((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+' || c0 == '.' || l0 == 'n' ||
          l0 == 't' || l0 == 'f' || l0 == 'y' || c0 == '~')) {
        return 0;
    }

    /* Word leads: the exact casings only; every other spelling (and
     * every y-word — compat owns those) is a string, no walk. */
    if (l0 == 'n' || l0 == 't' || l0 == 'f' || l0 == 'y') {
        if (n == 4 && l0 == 'n') {
            return (memcmp(p, "null", 4) == 0 || memcmp(p, "Null", 4) == 0 ||
                    memcmp(p, "NULL", 4) == 0)
                       ? 4
                       : 0;
        }
        if (n == 4 && l0 == 't') {
            return (memcmp(p, "true", 4) == 0 || memcmp(p, "True", 4) == 0 ||
                    memcmp(p, "TRUE", 4) == 0)
                       ? 3
                       : 0;
        }
        if (n == 5 && l0 == 'f') {
            return (memcmp(p, "false", 5) == 0 || memcmp(p, "False", 5) == 0 ||
                    memcmp(p, "FALSE", 5) == 0)
                       ? 3
                       : 0;
        }
        return 0;
    }
    if (c0 == '~') {
        return n == 1 ? 4 : 0;
    }

    /* Number path: optional sign, then the productions in frequency
     * order (.inf/.nan, hex, octal, decimal int/float). */
    uint32_t i = (c0 == '-' || c0 == '+') ? 1 : 0;
    if (i >= n) {
        return 0; /* lone sign */
    }
    {
        const char* r = p + i;
        uint32_t rn = n - i;
        if (rn == 4 && r[0] == '.') {
            if (memcmp(r, ".inf", 4) == 0 || memcmp(r, ".Inf", 4) == 0 ||
                memcmp(r, ".INF", 4) == 0 ||
                (i == 0 && (memcmp(r, ".nan", 4) == 0 || memcmp(r, ".NaN", 4) == 0 ||
                            memcmp(r, ".NAN", 4) == 0))) {
                return 2; /* float; NaN carries no sign */
            }
        }
    }
    if (n - i > 2 && p[i] == '0' &&
        p[i + 1] == 'x') { /* lowercase only: the core schema's int regexp; "0X1A" rides a string
                              (psych agrees) */
        for (uint32_t k = i + 2; k < n; k++) {
            char c = p[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return 0;
            }
        }
        return 1; /* int */
    }
    if (n - i > 2 && p[i] == '0' && p[i + 1] == 'o') {
        for (uint32_t k = i + 2; k < n; k++) {
            if (p[k] < '0' || p[k] > '7') {
                return 0;
            }
        }
        return 1; /* int */
    }
    int digits = 0, dot = 0, e = 0, edigits = 0;
    for (uint32_t k = i; k < n; k++) {
        char c = p[k];
        if (c >= '0' && c <= '9') {
            if (e) {
                edigits = 1;
            } else {
                digits = 1;
            }
            continue;
        }
        if (c == '.' && !e) {
            if (dot) {
                return 0;
            }
            dot = 1;
            continue;
        }
        if ((c == 'e' || c == 'E') && digits && !e) {
            e = 1;
            if (k + 1 < n && (p[k + 1] == '-' || p[k + 1] == '+')) {
                k++;
            }
            continue;
        }
        return 0;
    }
    if (digits && (dot || e) && (!e || edigits)) {
        return 2; /* float */
    }
    if (digits && !dot && !e) {
        return 1; /* int */
    }
    return 0;
}

static yep_tag_id core12_number(void* ctx, int is_float) {
    (void)ctx;
    return is_float ? 2 : 1;
}

static const yep_resolver k_core12 = {core12, core12_number, NULL};

const yep_resolver* yep_resolver_core12(void) {
    return &k_core12;
}
