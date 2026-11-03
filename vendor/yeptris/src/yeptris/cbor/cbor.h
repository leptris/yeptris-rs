/* cbor.h — the CBOR (RFC 8949) codec's internal interface
 * (TODO.cbor/01). The public surface is <yeptris/cbor.h>. */
#ifndef YEP_CBOR_H
#define YEP_CBOR_H

#include <stdint.h>

#include "../dom/dom.h"

#ifdef __cplusplus
extern "C" {
#endif

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
