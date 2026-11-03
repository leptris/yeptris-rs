/* string_view.h — YepView: the single string representation (SSOT law).
 *
 * Every string in the library is a YepView — either borrowed into the
 * caller's input buffer, or borrowed into the document string arena after
 * a fold/escape copy. No module stores (char*, int) pairs or NUL-
 * terminated strings internally.
 *
 * Views are NOT NUL-terminated; length is authoritative.
 */
#ifndef YEP_STRING_VIEW_H
#define YEP_STRING_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yep_view {
    const char* p;
    uint32_t len;
} yep_view;

static inline yep_view yep_view_empty(void) {
    yep_view v = {NULL, 0};
    return v;
}

/* Wraps a NUL-terminated string (test/convenience paths only). */
yep_view yep_view_from_cstr(const char* s);

static inline bool yep_view_is_empty(yep_view v) {
    return v.len == 0;
}

/* True if a and b are the same bytes. Word-at-a-time: interner probes
 * compare tiny keys millions of times per parse and a libc memcmp call
 * per probe cost ~5% of anchor-heavy parse. */
static inline bool yep_view_eq(yep_view a, yep_view b) {
    if (a.len != b.len) {
        return false;
    }
    uint32_t n = a.len;
    const char* p = a.p;
    const char* q = b.p;
    while (n >= 8) {
        uint64_t x, y;
        memcpy(&x, p, 8);
        memcpy(&y, q, 8);
        if (x != y) {
            return false;
        }
        p += 8;
        q += 8;
        n -= 8;
    }
    if (n >= 4) {
        uint32_t x, y;
        memcpy(&x, p, 4);
        memcpy(&y, q, 4);
        if (x != y) {
            return false;
        }
        p += 4;
        q += 4;
        n -= 4;
    }
    while (n--) {
        if (*p++ != *q++) {
            return false;
        }
    }
    return true;
}

/* True if a equals the NUL-terminated s (without including its NUL). */
bool yep_view_eq_cstr(yep_view a, const char* s);

/* True if v begins with the NUL-terminated prefix. */
bool yep_view_starts_with(yep_view v, const char* prefix);

/* Sub-view [off, off + len). Invalid bounds yield the empty view. */
yep_view yep_view_slice(yep_view v, uint32_t off, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* YEP_STRING_VIEW_H */
