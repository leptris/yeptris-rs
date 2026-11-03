/* cbor.h — the CBOR (RFC 8949) codec's internal interface
 * (TODO.cbor/01). The public surface is <yeptris/cbor.h>. */
#ifndef YEP_CBOR_H
#define YEP_CBOR_H

#include <stdint.h>

#include "../dom/dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The semantic-tag chain law: decode renders the chain into a stack
 * buffer; encode splits that text back into tag heads. ONE pair of
 * constants governs both ends — the nightly's 23-nested-tags artifact
 * (23 links decoded fine, encode-side tags[16] refused) was the two
 * ends disagreeing. 64 links is already pathological (RFC 8949's
 * registry is <20); a link renders up to 20 digits + separator, so
 * the buffer holds a full 64-link chain. */
#define YEP_CBOR_TAGBUF 1408
#define YEP_CBOR_TAG_MAX_LINKS 64

/* one data item: bytes -> the shared DOM (the plan's item 01) */
/* One data item. consumed (may be NULL): when given, returns the
 * item's byte extent and TRAILING BYTES ARE NOT an error (the
 * sequence loop steps on them); NULL keeps the one-item contract. */
int yep_cbor_decode_dom(yep_dom* d, const unsigned char* p, size_t len, int strict,
                        size_t* consumed);

#ifdef __cplusplus
}
#endif

#endif /* YEP_CBOR_H */
