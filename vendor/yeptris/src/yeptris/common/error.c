/* error.c — error channel implementation. printf-composition happens here,
 * once, at error-creation time; nothing on any hot path formats strings.
 */

#include <stdio.h>

#include "error.h"

const char* yep_error_string(yep_err_code code) {
    switch (code) {
#define YEP_ERROR_CASE(code, str)                                                                  \
    case code:                                                                                     \
        return str;
        YEP_ERROR_X(YEP_ERROR_CASE)
#undef YEP_ERROR_CASE
    default:
        return "unknown error code";
    }
}

void yep_error_clear(yep_error* slot) {
    if (slot == NULL) {
        return;
    }
    slot->code = YEP_ERR_NONE;
    slot->line = 0;
    slot->col = 0;
    slot->offset = 0;
    slot->msg[0] = '\0';
}

int yep_error_is_none(const yep_error* slot) {
    return slot == NULL || slot->code == YEP_ERR_NONE;
}

static void yep_error_vset(yep_error* slot, yep_err_code code, uint32_t line, uint32_t col,
                           size_t offset, const char* fmt, va_list ap) {
    slot->code = code;
    slot->line = line;
    slot->col = col;
    slot->offset = offset;
    if (fmt == NULL) {
        snprintf(slot->msg, sizeof(slot->msg), "%s", yep_error_string(code));
        return;
    }
    vsnprintf(slot->msg, sizeof(slot->msg), fmt, ap);
}

int yep_error_set(yep_error* slot, yep_err_code code, uint32_t line, uint32_t col, size_t offset,
                  const char* fmt, ...) {
    if (slot == NULL) {
        return 0;
    }
    va_list ap;
    va_start(ap, fmt);
    yep_error_vset(slot, code, line, col, offset, fmt, ap);
    va_end(ap);
    return 0;
}

static _Thread_local yep_error yep_tls_error = {YEP_ERR_NONE, 0, 0, 0, ""};

yep_error* yep_error_tls(void) {
    return &yep_tls_error;
}
