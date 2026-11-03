/* sink.h — the CBOR decode seam (#157).
 *
 * The grammar loop (decode.c's cbor_item machinery) is the single
 * source of CBOR semantics; a sink implements the item constructors
 * on top of it. Two sinks exist: the DOM builder (the default, the
 * yeptris_cbor_decode public path) and the native ext's VALUE builder
 * (one pass, no intermediate). The sink vtable IS the public SAX face
 * (yeptris_cbor_sax, <yeptris/cbor.h> — #742): the alias keeps the
 * decoder driving the same layout the public entry hands over, so
 * yeptris_cbor_decode_sax is a straight passthrough. Callback/field
 * contracts are documented there; sinks receive the resolver's tag_id
 * and the pending semantic-tag chain in its text form ("N N N",
 * outermost first — NULL when the item carries no chain).
 */
#ifndef YEP_CBOR_SINK_H
#define YEP_CBOR_SINK_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h> /* YEPTRIS_API (the ext links these) */
#include <yeptris/cbor.h>

typedef yeptris_cbor_sax yep_cbor_sink;

/* The generic engine: the grammar, driving `sink`. Same reject/
 * encoding/depth semantics as the DOM entry. */
YEPTRIS_API int yep_cbor_decode_gen(const unsigned char* p, size_t len, int strict,
                                    size_t* consumed, const yep_cbor_sink* sink);

/* The DOM sink: pairs yep_cbor_decode_gen with a yep_dom* (the
 * yep_cbor_decode_dom public path's driver; d is a yep_dom*). */
YEPTRIS_API int yep_cbor_decode_dom_sink(void* d, const unsigned char* p, size_t len, int strict,
                                         size_t* consumed);

#endif /* YEP_CBOR_SINK_H */
