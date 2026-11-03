/* full12.c — the FULL YAML 1.2 family resolver (#906 slice 3).
 *
 * The spec's schema chapter names two families (10.2 JSON, 10.3 core)
 * and leaves the rest to 10.4 "Other Schemas": interoperable schemas
 * "make use of global tags", may add their own resolution regexes, and
 * are "strongly recommended ... based on the core schema". The
 * historical full schema is the yaml.org type repository carried
 * forward in practice — libyaml implements nothing of it; PyYAML's
 * SafeLoader resolves timestamps, and that is the de-facto full
 * behavior the ecosystem knows. This resolver is that family:
 *
 *   core12's productions verbatim (the spec-refereed base — NOT
 *     psych's numeric quirks: underscores, sexagesimal, leading-0
 *     octal and y/n booleans stay in compat11, which is the 1.1
 *     practice lane, not a 1.2 family);
 * + timestamps on untagged plains: spec example 2.22's four shapes —
 *     canonical (2001-12-15T02:59:43.1Z), iso8601
 *     (2001-12-14t21:59:43.10-05:00), spaced (2001-12-14 21:59:43.10
 *     -5), date (2002-12-14) — classified by PyYAML 6.0.2's
 *     timestamp regex, the practice oracle (see pyyaml_timestamp
 *     below for the production-by-production account);
 * + the merge/value words ('<<' / '='), the 1.1 full schema's
 *     grammar-adjacent tags PyYAML also resolves implicitly;
 * - binary: NEVER implicit (nowhere, in any edition) — the explicit
 *     !!binary tag rides as consumer-defined passthrough (PyYAML's
 *     base64 shaping is a CONSTRUCTION step; keeping shaping out of
 *     resolution is the documented passthrough policy).
 *
 * The validator family-checks explicit !!timestamp content through the
 * same classifier (the resolver's `timestamped` capability), so a
 * resolution change lands once and both move together.
 */

#include <stdint.h>

#include "resolver.h"

/* PyYAML 6.0.2 resolver.py's timestamp_regexp, production by
 * production (re.X whitespace stripped):
 *
 *   [0-9]{4}-[0-9]{2}-[0-9]{2}                      date-only: month
 *                                                   and day are EXACTLY
 *                                                   two digits
 * | [0-9]{4}-[0-9]{1,2}-[0-9]{1,2}                  with a time part:
 *                                                   month/day/hour are
 *                                                   1-2 digits, the
 *     (?:[Tt]|[ \t]+)[0-9]{1,2}                     separator T/t/space-
 *                                                   run, minutes and
 *     :[0-9]{2}:[0-9]{2}(?:\.[0-9]*)?               seconds EXACTLY two,
 *                                                   fraction zero+ digits
 *     (?:[ \t]*(?:Z|[-+][0-9]{1,2}(?::[0-9]{2})?))? tz OPTIONAL: Z or a
 *                                                   signed 1-2-digit
 *                                                   offset, minutes
 *                                                   optional
 *
 * The two-oracle divergences this keeps (PyYAML's resolver is the
 * oracle, deliberately): NO sign on the year (psych allows -?); NO
 * calendar range checks (2001-13-14 resolves timestamp — PyYAML's own
 * resolver agrees, only its constructor raises; resolution is not
 * calendar validation, the consumer's date type owns ranges). The
 * spec's plain-scalar colon law is what keeps the spaced forms
 * unquoted ("a:1" is a plain scalar; "a: 1" is a pair — 5.3.2's note
 * cites timestamps as the canonical case).
 */
static int pyyaml_timestamp(const char* p, uint32_t n) {
    uint32_t k = 0;
    for (int d = 0; d < 4; d++) { /* year: exactly four digits, no sign */
        if (k >= n || p[k] < '0' || p[k] > '9') {
            return 0;
        }
        k++;
    }
    if (k >= n || p[k] != '-') {
        return 0;
    }
    k++;
    uint32_t mdig = 0, ddig = 0;
    for (int seg = 0; seg < 2; seg++) {
        const uint32_t start = k;
        while (k < n && k - start < 2 && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
        if (seg == 0) {
            mdig = k - start;
            if (mdig == 0 || k >= n || p[k] != '-') {
                return 0;
            }
            k++;
        } else {
            ddig = k - start;
            if (ddig == 0) {
                return 0;
            }
        }
    }
    if (k == n) {
        /* date-only: the branch demands EXACTLY two-digit month/day
         * ("2001-1-1" is a string — PyYAML's own verdict) */
        return mdig == 2 && ddig == 2;
    }
    /* the time part is MANDATORY on the second branch */
    if (p[k] == 'T' || p[k] == 't') {
        k++;
    } else if (p[k] == ' ' || p[k] == '\t') {
        while (k < n && (p[k] == ' ' || p[k] == '\t')) {
            k++;
        }
    } else {
        return 0;
    }
    const uint32_t hstart = k; /* hour: 1-2 digits */
    while (k < n && k - hstart < 2 && p[k] >= '0' && p[k] <= '9') {
        k++;
    }
    if (k == hstart) {
        return 0;
    }
    for (int seg = 0; seg < 2; seg++) { /* :dd:dd — exactly two each */
        if (k >= n || p[k] != ':') {
            return 0;
        }
        k++;
        const uint32_t start = k;
        while (k < n && k - start < 2 && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
        if (k - start != 2) {
            return 0;
        }
    }
    if (k < n && p[k] == '.') { /* fraction: dot then zero+ digits */
        k++;
        while (k < n && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
    }
    /* tz: greedy, OPTIONAL — a malformed tail backtracks to empty and
     * the overall match then fails at the tail byte (regex semantics) */
    const uint32_t save = k;
    while (k < n && (p[k] == ' ' || p[k] == '\t')) {
        k++;
    }
    if (k < n && p[k] == 'Z') {
        k++;
    } else if (k < n && (p[k] == '+' || p[k] == '-')) {
        k++;
        const uint32_t ostart = k; /* offset hour: 1-2 digits */
        while (k < n && k - ostart < 2 && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
        int ok = k - ostart > 0;
        if (ok && k < n && p[k] == ':') { /* optional :mm — exactly two */
            const uint32_t mstart = k + 1;
            ok = mstart + 1 < n && p[mstart] >= '0' && p[mstart] <= '9' && p[mstart + 1] >= '0' &&
                 p[mstart + 1] <= '9';
            if (ok) {
                k = mstart + 2;
            }
        }
        if (!ok) {
            k = save;
        }
    } else {
        k = save;
    }
    return k == n;
}

static yep_tag_id full12(void* ctx, const char* p, uint32_t n) {
    (void)ctx;
    /* the merge/value words: exact compares, ahead of everything (the
     * 1.1 full schema's grammar-adjacent tags, PyYAML-resolved) */
    if (n == 1 && p[0] == '=') {
        return 10; /* value */
    }
    if (n == 2 && p[0] == '<' && p[1] == '<') {
        return 9; /* merge */
    }
    /* the core base, verbatim */
    yep_tag_id core = yep_core12_plain(p, n);
    if (core != 0) {
        return core;
    }
    /* timestamps: gate on the shape (four digits, '-', >= 10 bytes) —
     * every accepted form starts with four digits */
    if (n >= 10 && p[0] >= '0' && p[0] <= '9' && pyyaml_timestamp(p, n)) {
        return 5;
    }
    return 0;
}

static yep_tag_id full12_number(void* ctx, int is_float) {
    (void)ctx;
    return is_float ? 2 : 1; /* full numbers are core numbers */
}

static const yep_resolver k_full12 = {full12, full12_number, NULL, 1};

const yep_resolver* yep_resolver_full12(void) {
    return &k_full12;
}
