/* error.h — the internal error channel.
 *
 * Codes and their strings are declared together in one X-macro — adding a
 * code is one line (OCP). Public API entry points map these to the pinned
 * YeptrisStatus codes (public error.h); this channel carries the detail:
 * code + line/column/offset + composed message.
 *
 * Two channels (the libleptris contract):
 *  - thread-local slot: yep_error_tls() — most recent failure on the
 *    calling thread;
 *  - per-document slot: any yep_error* the owner (document, 11) embeds.
 */
#ifndef YEP_ERROR_H
#define YEP_ERROR_H

#include <stdarg.h>
#include <stddef.h>

#include "port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YEP_ERROR_X(X)                                                                             \
    X(YEP_ERR_NONE, "no error")                                                                    \
    X(YEP_ERR_UNEXPECTED, "unexpected parser state")                                               \
    X(YEP_ERR_MEMORY, "out of memory")                                                             \
    X(YEP_ERR_DEPTH, "nesting depth exceeds the limit")                                            \
    X(YEP_ERR_ENCODING, "ill-formed or unsupported character encoding")                            \
    X(YEP_ERR_TAB_IN_INDENT, "tab character used as indentation")                                  \
    X(YEP_ERR_UNTERMINATED_QUOTE, "unterminated quoted scalar")                                    \
    X(YEP_ERR_BAD_INDENT, "inconsistent indentation")                                              \
    X(YEP_ERR_KEY_TOO_LONG, "simple key exceeds 1024 characters")                                  \
    X(YEP_ERR_UNDEFINED_ALIAS, "alias references an undefined anchor")                             \
    X(YEP_ERR_INVALID_ESCAPE, "invalid escape sequence in double-quoted scalar")                   \
    X(YEP_ERR_BAD_DIRECTIVE, "invalid or unsupported directive")                                   \
    X(YEP_ERR_INTERNAL, "internal invariant violation")                                            \
    X(YEP_ERR_SCHEMA, "required descriptor node missing")

typedef enum {
#define YEP_ERROR_ENUM(code, str) code,
    YEP_ERROR_X(YEP_ERROR_ENUM)
#undef YEP_ERROR_ENUM
        YEP_ERR_CODE_COUNT
} yep_err_code;

#define YEP_ERROR_MSG_MAX 192

typedef struct yep_error {
    yep_err_code code;
    uint32_t line; /* 1-based; 0 = unknown */
    uint32_t col;  /* 1-based; 0 = unknown */
    size_t offset; /* byte offset in the input */
    char msg[YEP_ERROR_MSG_MAX];
} yep_error;

/* The string for a code (X-macro table; never NULL). */
const char* yep_error_string(yep_err_code code);

/* Clears a slot to the no-error state. */
void yep_error_clear(yep_error* slot);

/* True when the slot holds no error. */
int yep_error_is_none(const yep_error* slot);

/* Sets slot (printf-style message, truncated to fit). Returns 0 so callers
 * can `return yep_error_set(...)` where convenient. */
int yep_error_set(yep_error* slot, yep_err_code code, uint32_t line, uint32_t col, size_t offset,
                  const char* fmt, ...);

/* The thread-local slot (one per thread; no cross-talk by construction). */
yep_error* yep_error_tls(void);

#ifdef __cplusplus
}
#endif

#endif /* YEP_ERROR_H */
