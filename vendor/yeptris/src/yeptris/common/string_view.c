/* string_view.c — YepView primitives. */

#include <string.h>

#include "string_view.h"

yep_view yep_view_from_cstr(const char* s) {
    yep_view v = {s, s ? (uint32_t)strlen(s) : 0};
    return v;
}

bool yep_view_eq_cstr(yep_view a, const char* s) {
    if (s == NULL) {
        return a.len == 0;
    }
    size_t n = strlen(s);
    return a.len == n && (n == 0 || memcmp(a.p, s, n) == 0);
}

bool yep_view_starts_with(yep_view v, const char* prefix) {
    if (prefix == NULL) {
        return true;
    }
    size_t n = strlen(prefix);
    return v.len >= n && memcmp(v.p, prefix, n) == 0;
}

yep_view yep_view_slice(yep_view v, uint32_t off, uint32_t len) {
    if (off > v.len || len > v.len - off) {
        return yep_view_empty();
    }
    yep_view out = {v.p + off, len};
    return out;
}
