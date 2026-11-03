/* visit.h — language-agnostic value visitor (TODO.restructure/22/24).
 *
 * A push-style sink over the parsed value stream. Hosts that build
 * native objects (Ruby C-API, CPython, etc.) implement the vtable and
 * receive one callback per scalar/container — no intermediate records,
 * no DOM, no host-side walk. The JSON entry is a fused RFC 8259 scan
 * that never allocates a tree; the YAML entry rides the engine; the
 * node entry walks an existing DOM subtree.
 *
 * OCP: a new host is a new vtable, never a core edit. MECE: scanning
 * stays in scan/; this header is only the sink contract. */
#ifndef YEPTRIS_VISIT_H
#define YEPTRIS_VISIT_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/dom.h>
#include <yeptris/error.h>
#include <yeptris/resolve.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return 0 to continue, nonzero to abort (propagated as the status
 * detail when non-zero; YEPTRIS_ERROR_INTERNAL is the library-side
 * stand-in when the visitor doesn't set a more specific code). */
typedef struct YeptrisVisitVTable {
    int (*on_null)(void* ctx);
    int (*on_bool)(void* ctx, int truthy);
    int (*on_int)(void* ctx, int64_t v);
    int (*on_float)(void* ctx, double v);
    /* bytes are borrowed from the input (or a short-lived decode
     * scratch for escaped JSON strings — valid only for the duration
     * of the callback). */
    int (*on_string)(void* ctx, const char* p, size_t len);
    int (*on_seq_start)(void* ctx);
    int (*on_seq_end)(void* ctx);
    int (*on_map_start)(void* ctx);
    /* A map key is always a string callback immediately followed by
     * the value; hosts that need the key/value pairing push the key
     * on on_string when is_key is set. */
    int (*on_map_end)(void* ctx);
    /* Optional: 1 when the next scalar/container is a map key. NULL
     * means the visitor does not distinguish (JSON keys are always
     * strings fired via on_string before their value). */
    int (*on_key)(void* ctx, const char* p, size_t len);
    /* Optional document boundary (YAML multi-doc). NULL = ignore. */
    int (*on_doc)(void* ctx);
    /* Optional anchor/alias. NULL = materializer does not support
     * identity (JSON never fires these). */
    int (*on_anchor)(void* ctx, const char* name, size_t len);
    int (*on_alias)(void* ctx, const char* name, size_t len);
} YeptrisVisitVTable;

/* Fused strict-JSON scan → visit. No DOM, no records. On success the
 * visitor saw exactly one value (plus optional ws). */
YEPTRIS_API YeptrisStatus yeptris_visit_json(const char* data, size_t len,
                                             const YeptrisVisitVTable* vt, void* ctx);

/* YAML parse → visit through the engine (schema selects the resolver).
 * Multi-doc streams fire on_doc before each document's root value. */
YEPTRIS_API YeptrisStatus yeptris_visit(const char* data, size_t len, YeptrisSchema schema,
                                        const YeptrisVisitVTable* vt, void* ctx);

/* Existing DOM subtree → visit (Node#to_ruby bulk path without
 * records/Marshal). */
YEPTRIS_API YeptrisStatus yeptris_visit_node(YeptrisNode node, const YeptrisVisitVTable* vt,
                                             void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_VISIT_H */
