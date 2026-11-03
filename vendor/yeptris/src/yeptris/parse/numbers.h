/* numbers.h — the typed-value conversion kernels (TODO.impl/08B).
 *
 * ONE home for text->int64/double/bool: the node typed accessors,
 * the value stream (values.c), and any future consumer ride these.
 * Pure functions over (pointer, length) — no handles, no state. */
#ifndef YEP_NUMBERS_H
#define YEP_NUMBERS_H

#include <stdint.h>
#include <yeptris/api.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Separator-carrying YAML number text (digits, '_', ','; radix
 * forms 0x/0o/0b/leading-0; sexagesimal for the compat schema).
 * Returns 0 and sets *out on success, nonzero otherwise. */
YEPTRIS_API int yep_num_i64(const char* p, uint32_t len, int64_t* out);
YEPTRIS_API int yep_num_f64(const char* p, uint32_t len, double* out);

/* The resolver's true-word set (true/True/TRUE/y/Y/yes/Yes/YES/on/
 * On/ON); anything else the BOOL tag accepted is false. */
YEPTRIS_API int yep_num_bool(const char* p, uint32_t len);

/* Case-insensitive truth (the compat resolver's matching rule). */
YEPTRIS_API int yep_num_bool_ci(const char* p, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* YEP_NUMBERS_H */
