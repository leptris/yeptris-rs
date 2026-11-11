/* tags.c — the tag identity SSOT (TODO.impl/10).
 *
 * Dense ids for the core set; canonical URIs live here once. Custom
 * tags keep their string (the DOM already stores the view) and use
 * YEPTRIS_TAG_CUSTOM.
 */

#include "resolver.h"

#include <string.h>

typedef struct {
    yep_tag_id id;
    const char* uri;
} yep_tag_entry;

static const yep_tag_entry k_core[] = {
    {0, "tag:yaml.org,2002:str"},    {1, "tag:yaml.org,2002:int"},
    {2, "tag:yaml.org,2002:float"},  {3, "tag:yaml.org,2002:bool"},
    {4, "tag:yaml.org,2002:null"},   {5, "tag:yaml.org,2002:timestamp"},
    {6, "tag:yaml.org,2002:seq"},    {7, "tag:yaml.org,2002:map"},
    {8, "tag:yaml.org,2002:binary"}, {9, "tag:yaml.org,2002:merge"},
    {10, "tag:yaml.org,2002:value"},
};

const char* yep_tag_uri(yep_tag_id id) {
    if (id < sizeof(k_core) / sizeof(k_core[0])) {
        return k_core[id].uri;
    }
    return NULL;
}

yep_tag_id yep_tag_from_uri(const char* p, uint32_t len) {
    for (size_t i = 0; i < sizeof(k_core) / sizeof(k_core[0]); i++) {
        if (strlen(k_core[i].uri) == len && memcmp(k_core[i].uri, p, len) == 0) {
            return k_core[i].id;
        }
    }
    return 11; /* YEPTRIS_TAG_CUSTOM */
}

const char* yep_tag_float_word(const char* p, uint32_t len) {
    static const struct {
        const char* word;
        const char* canon;
    } words[] = {
        {".inf", ".inf"},   {".Inf", ".inf"},   {".INF", ".inf"},  {"-.inf", "-.inf"},
        {"-.Inf", "-.inf"}, {"-.INF", "-.inf"}, {"+.inf", ".inf"}, {"+.Inf", ".inf"},
        {"+.INF", ".inf"},  {".nan", ".nan"},   {".NaN", ".nan"},  {".NAN", ".nan"},
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        uint32_t wl = (uint32_t)strlen(words[i].word);
        if (len == wl && memcmp(p, words[i].word, wl) == 0) {
            return words[i].canon;
        }
    }
    return NULL;
}

static int ci_eq(const char* p, uint32_t n, const char* s) {
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

int yep_tag_bool_value(const char* p, uint32_t len) {
    static const char* const words[8] = {"y", "yes", "true", "on", "n", "no", "false", "off"};
    for (unsigned i = 0; i < 8; i++) {
        if (ci_eq(p, len, words[i])) {
            return i < 4 ? 1 : 0;
        }
    }
    return -1;
}

/* One base-denominated digit with overflow detection (uint64 keeps
 * the INT64_MIN magnitude representable). */
static int acc_digit(uint64_t* acc, int base, int dv) {
    uint64_t na = *acc * (uint64_t)base;
    if (na / (uint64_t)base != *acc) {
        return 1;
    }
    na += (uint64_t)dv;
    if (na < *acc) {
        return 1;
    }
    *acc = na;
    return 0;
}

int yep_tag_int64(const char* p, uint32_t n, int compat, int64_t* out) {
    uint32_t i = 0;
    int neg = 0;
    if (n == 0) {
        return 1;
    }
    if (p[0] == '-' || p[0] == '+') {
        neg = p[0] == '-';
        i = 1;
        if (i >= n) {
            return 1;
        }
    }
    /* radix prefixes first (both families, lowercase only — "0X1A"
     * rides a string in core AND compat; 0o is core's spelling, 0b
     * compat's), then compat's leading-0 octal, then the plain
     * decimal/sexagesimal walk */
    uint64_t acc = 0;
    int digits = 0;
    int base = 10;
    uint32_t d = i;
    if (n - i > 2 && p[i] == '0' && p[i + 1] == 'x') {
        base = 16;
        d = i + 2;
        if (compat && p[d] == '_') {
            d++;
            if (p[d] == '_') {
                return 1;
            }
        }
    } else if (n - i > 2 && p[i] == '0' && p[i + 1] == 'o') {
        if (compat) {
            return 1; /* 0o is not a compat form (psych) */
        }
        base = 8;
        d = i + 2;
    } else if (compat && n - i > 2 && p[i] == '0' && p[i + 1] == 'b') {
        base = 2;
        d = i + 2;
        if (p[d] == '_') {
            d++;
            if (p[d] == '_') {
                return 1;
            }
        }
    } else if (compat && p[i] == '0' && n - i >= 2) {
        /* leading-0 octal (psych): 0[_]*[0-7][0-7_]* — the radix
         * prefixes above never reach this arm */
        base = 8;
        d = i + 1;
        while (d < n && p[d] == '_') {
            d++;
        }
        if (d >= n) {
            *out = 0; /* plain "0" / "000" */
            return 0;
        }
    } else if (compat && p[i] == '0' && n - i == 1) {
        *out = 0;
        return 0;
    } else if (compat && p[i] >= '0' && p[i] <= '9') {
        /* sexagesimal int (compat only): d[d_,]*(:[0-5]?d){1,2} — the
         * fractional form is float-tagged and never reaches here, but
         * a '.' still refuses (the decode must not guess). '_' and ','
         * are the compat separators (psych legacy). */
        uint32_t k = i;
        while (k < n && ((p[k] >= '0' && p[k] <= '9') || p[k] == '_' || p[k] == ',')) {
            if (p[k] != '_' && p[k] != ',') {
                if (acc_digit(&acc, 10, p[k] - '0')) {
                    return 1;
                }
                digits = 1;
            }
            k++;
        }
        if (k < n && p[k] == ':') {
            while (k < n && p[k] == ':') {
                k++;
                int gd = 0;
                int g = 0;
                while (k < n && p[k] >= '0' && p[k] <= '9' && gd < 2) {
                    g = g * 10 + (p[k] - '0');
                    gd++;
                    k++;
                }
                if (gd < 1 || g > 59) {
                    return 1;
                }
                if (acc_digit(&acc, 60, g)) {
                    return 1;
                }
            }
            if (k == n) {
                *out = neg ? -(int64_t)acc : (int64_t)acc;
                return 0;
            }
            return 1;
        }
        /* a ':'-less digit span: the plain decimal path below would
         * re-read it — answer directly */
        if (k == n && digits) {
            *out = neg ? -(int64_t)acc : (int64_t)acc;
            return 0;
        }
        return 1;
    }
    acc = 0;
    digits = 0;
    for (uint32_t k = d; k < n; k++) {
        char c = p[k];
        int dv;
        if (c >= '0' && c <= '9') {
            dv = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            dv = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            dv = c - 'A' + 10;
        } else if (compat && (c == '_' || c == ',')) {
            continue;
        } else {
            return 1;
        }
        if (base != 16 && dv >= base) {
            return 1;
        }
        if (acc_digit(&acc, base, dv)) {
            return 1;
        }
        digits = 1;
    }
    if (!digits) {
        return 1;
    }
    if (neg && acc == (uint64_t)INT64_MAX + 1u) {
        *out = INT64_MIN;
        return 0;
    }
    if (acc > (uint64_t)INT64_MAX) {
        return 1;
    }
    *out = neg ? -(int64_t)acc : (int64_t)acc;
    return 0;
}
