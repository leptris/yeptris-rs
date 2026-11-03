/* encode_priv.h — the CBOR encoder's shared kernels (internal).
 *
 * encode.c owns the DOM walk; from_marshal.c (the binding dump face)
 * appends the same item bytes from a Marshal stream. The minimal-head
 * table, the preferred-float width rule and the reserved diagnostic
 * words are ONE law each — both writers call them from here. */

#ifndef YEPTRIS_CBOR_ENCODE_PRIV_H
#define YEPTRIS_CBOR_ENCODE_PRIV_H

#include <stddef.h>
#include <stdint.h>

/* RFC 8949 s3: the encoded size of argument arg. */
size_t yep_cbor_arg_bytes(uint64_t arg);

/* RFC 8949 s3: one head (major type + minimally encoded argument). */
void yep_cbor_put_head(uint8_t* out, size_t* pos, uint8_t mt, uint64_t arg);

/* s4.2.2: the smallest of half/single/double that round-trips. */
uint8_t yep_cbor_float_width(double d);

/* s4.2.2: one float at the requested width (NaN canonicalizes). */
void yep_cbor_put_float(uint8_t* out, size_t* pos, double d, uint8_t width);

/* The diagnostic round-trip ledger: decode renders undefined and
 * simple(N) as text, so the encoder recognizes those words back — with
 * the >9-byte gate that leaves single-digit simple(N) as text. Returns
 * the simple value, or -1 when the text is not a reserved word. */
int yep_cbor_simple_of(const char* p, size_t len);

#endif /* YEPTRIS_CBOR_ENCODE_PRIV_H */
