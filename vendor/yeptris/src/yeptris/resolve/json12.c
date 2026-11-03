/* json12.c — the spec 10.2 JSON schema resolver (#882).
 *
 * The productions are the spec 10.2.2 table (1.2.2 erratum kept: the
 * float regex needs NO digits after the dot — "0." is a float):
 *   null:  null
 *   bool:  true | false
 *   int:   -? ( 0 | [1-9] [0-9]* )
 *   float: -? ( 0 | [1-9] [0-9]* ) ( \. [0-9]* )? ( [eE] [-+]? [0-9]+ )?
 *   *  -> Error (the validator's business; the resolver stays str)
 *
 * Deliberately NOT accepted (the core schema's extensions): "True"/
 * "False"/"NULL" casings, "~"/empty nulls, +signs, 0o/0x radix, .inf/
 * .nan, underscores. The example-10.8 Invalid list pins each one.
 *
 * Shape: yep_json12_plain is the ONE classifier — the resolver wraps
 * it, and the validator re-asks it per plain scalar (no second truth
 * table anywhere). Parse never errors here: a scalar outside the
 * productions resolves str so the DOM stays family-neutral; the
 * validation face turns the spec's Error row into diagnostics.
 */

#include <stdint.h>
#include <string.h>

#include "resolver.h"

/* JSON int: -?(0|[1-9][0-9]*); optionally continues as the float
 * production (the int regex is a prefix of the float one). Returns 1
 * (int), 2 (float), 0 (no production). */
int yep_json12_plain(const char* p, uint32_t n) {
    if (n == 0) {
        return 0;
    }
    if (n == 4 && memcmp(p, "null", 4) == 0) {
        return 4; /* null */
    }
    if (n == 4 && memcmp(p, "true", 4) == 0) {
        return 3; /* bool */
    }
    if (n == 5 && memcmp(p, "false", 5) == 0) {
        return 3;
    }
    uint32_t i = (p[0] == '-') ? 1 : 0;
    if (i >= n) {
        return 0; /* lone "-" (or empty after the prefix check) */
    }
    /* int part: 0 | [1-9][0-9]* — no +sign, no leading zeros */
    if (p[i] == '0') {
        i++;
    } else if (p[i] >= '1' && p[i] <= '9') {
        i++;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            i++;
        }
    } else {
        return 0;
    }
    if (i == n) {
        return 1; /* bare int */
    }
    /* float tail: (\.[0-9]*)? ([eE][-+]?[0-9]+)? — at least one of the
     * two must be present (a bare int already returned) */
    int floaty = 0;
    if (p[i] == '.') {
        floaty = 1;
        i++;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            i++; /* digits optional after the dot (1.2.2 erratum) */
        }
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) {
            i++;
        }
        int ed = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            i++;
            ed = 1;
        }
        if (!ed) {
            return 0; /* exponent sign(s) with no digits */
        }
        floaty = 1;
    }
    return (floaty && i == n) ? 2 : 0;
}

static yep_tag_id json12(void* ctx, const char* p, uint32_t n) {
    (void)ctx;
    return (yep_tag_id)yep_json12_plain(p, n);
}

static yep_tag_id json12_number(void* ctx, int is_float) {
    (void)ctx;
    return is_float ? 2 : 1;
}

static const yep_resolver k_json12 = {json12, json12_number, NULL};

const yep_resolver* yep_resolver_json12(void) {
    return &k_json12;
}
