/* types.h — opaque public handles.
 *
 * The single canonical source of public types (TODO.impl/02 extends this
 * file; internal types never appear here). Callers never see struct
 * definitions: every handle is opaque and pointer-sized, asserted by the
 * ABI pinning test (test/unit/test_abi.cpp).
 */
#ifndef YEPTRIS_TYPES_H
#define YEPTRIS_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A parsed document; the sole owner of all memory reachable from it.
 * Memory: freed exactly once with yeptris_document_free (TODO.impl/11). */
typedef struct yeptris_document* YeptrisDocument;

/* A node (document, mapping, sequence, scalar, alias) inside a document.
 * Memory: borrowed from its document — valid until the document is freed. */
typedef struct yeptris_node* YeptrisNode;

/* Streaming/streaming-adjacent handles, introduced with their items:
 * parser engine (TODO.impl/07), pull/recorder/iterparse (TODO.impl/12),
 * emitter (TODO.impl/13). Declared now so the handle vocabulary is stable. */
typedef struct yeptris_parser* YeptrisParser;
typedef struct yeptris_pull* YeptrisPullParser;
typedef struct yeptris_recorder* YeptrisRecorder;
typedef struct yeptris_iterparse* YeptrisIterparse;
typedef struct yeptris_emitter* YeptrisEmitter;

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_TYPES_H */
