/* marshal.h — Ruby Marshal 4.8 emission (TODO.restructure/21).
 *
 * The binding materialization fast path: the C side converts the
 * value records directly into Ruby's Marshal wire format, so the host
 * materializes the whole object graph with ONE core-C call
 * (Marshal.load) instead of walking per-value records in Ruby.
 *
 * Semantics are pinned to the binding walks (the ':sym' plain scan,
 * Psych's single-char y/n and dot-required floats, alias identity
 * via object links). Constructs the format cannot express return
 * YEPTRIS_ERROR_UNSUPPORTED — the host falls back to the record walk.
 *
 * Memory: *out is a single malloc'd buffer; free with
 * yeptris_marshal_free exactly once. */
#ifndef YEPTRIS_MARSHAL_H
#define YEPTRIS_MARSHAL_H

#include <stddef.h>

#include <yeptris/api.h>
#include <yeptris/dom.h>
#include <yeptris/resolve.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* the whole stream as one Ruby Array of documents (load_all) */
    YEPTRIS_MARSHAL_ALL_DOCS = 0,
    /* just the first document's root (load) */
    YEPTRIS_MARSHAL_FIRST_DOC = 1,
} YeptrisMarshalMode;

/* Parses `data` and emits Marshal 4.8 bytes of the object graph the
 * value stream describes. Returns YEPTRIS_OK, YEPTRIS_ERROR_PARSE /
 * _MEMORY / _ARG, or YEPTRIS_ERROR_UNSUPPORTED (merge keys or
 * timestamps: materialize those through the record walk). */
YEPTRIS_API YeptrisStatus yeptris_marshal(const char* data, size_t len, YeptrisSchema schema,
                                          YeptrisMarshalMode mode, char** out, size_t* out_len);

/* The same emission for one node subtree of a built document — the
 * bulk path behind Node#to_ruby (one call, not one per node). */
YEPTRIS_API YeptrisStatus yeptris_marshal_node(YeptrisNode node, char** out, size_t* out_len);

#define YEPTRIS_MARSHAL_STRICT_DUP_KEYS 1u

/* yeptris_marshal_node with flags. STRICT_DUP_KEYS raises
 * YEPTRIS_ERROR_PARSE ("duplicate key in object") when any map carries
 * the same key twice — json 3's default contract; the JSON DOM keeps
 * both pairs, and the non-strict stream stays last-wins (json 2.x).
 * Memory: *out is malloc'd, free with yeptris_marshal_free exactly
 * once; on failure nothing is allocated. */
YEPTRIS_API YeptrisStatus yeptris_marshal_node_ex(YeptrisNode node, uint32_t flags, char** out,
                                                  size_t* out_len);

YEPTRIS_API void yeptris_marshal_free(char* out);

/* yeptris-ruby#322: the dump face's marshal reader — Ruby
 * Marshal.dump bytes IN, JSON text OUT, one C call (no per-scalar
 * crossings, no object reconstruction). Handles the JSON-relevant
 * subset (nil/bool/int/string/float/array/hash + symbols/links/ivars);
 * anything else — objects, structs, user marshal, bignums beyond i64,
 * non-finite floats, cycles — returns YEPTRIS_ERROR_UNSUPPORTED and
 * the binding falls back to its per-node walk. *out is malloc'd, free
 * with yeptris_marshal_free exactly once; on failure nothing is
 * allocated. compact selects the no-space separators. */
YEPTRIS_API YeptrisStatus yeptris_json_from_marshal(const char* m, size_t m_len, int compact,
                                                    char** out, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_MARSHAL_H */
