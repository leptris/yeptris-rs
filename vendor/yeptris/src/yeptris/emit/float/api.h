/* api.h — the emitter-facing float printer boundary (TODO.impl/14).
 *
 * Thin adaptation over the clean-room float printer TUs
 * (TODO.impl/14 — no third-party code). Non-finite
 * values print as the YAML core-schema words (.inf/-.inf/.nan), never
 * ryu's "Infinity"/"NaN" — the resolver and the printer agree.
 *
 * Buffer contracts (caller provides; asserted by tests):
 *   shortest: 32 bytes; fixed: 2 + 309 + 1 + precision.
 */
#ifndef YEP_FLOAT_API_H
#define YEP_FLOAT_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shortest round-trip decimal for a double. Returns the length written. */
int yep_d2s_shortest(double d, char* buf);

/* Shortest round-trip decimal for a float. Returns the length written. */
int yep_f2s_shortest(float f, char* buf);

/* Fixed-notation output with exactly `precision` fraction digits
 * (Psych Float#to_s / explicit-precision emission). Returns length. */
int yep_d2fixed(double d, uint32_t precision, char* buf);

/* YAML words for non-finite values (finite input returns 0). */
int yep_d2s_nonfinite(double d, char* buf);

/* The json-gem float byte shape (grisu2 digits + fpconv emit
 * windows, vendored in fpconv.c) for JSON-mode canonical scalars
 * (yeptris#651). 32-byte buffer. Returns the length written. */
int yep_d2s_json(double d, char* buf);

#ifdef __cplusplus
}
#endif

#endif /* YEP_FLOAT_API_H */
