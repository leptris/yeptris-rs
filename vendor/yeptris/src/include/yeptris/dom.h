/* document.h / node.h surface — DOM accessors (TODO.impl/11).
 * Pinned enums: bindings hard-code these values (test_abi). */

#ifndef YEPTRIS_DOM_PUBLIC_H
#define YEPTRIS_DOM_PUBLIC_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>
#include <yeptris/resolve.h> /* YeptrisSchema */
#include <yeptris/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YEPTRIS_NODE_SCALAR = 0,
    YEPTRIS_NODE_SEQUENCE = 1,
    YEPTRIS_NODE_MAPPING = 2,
    YEPTRIS_NODE_ALIAS = 3,
} YeptrisNodeKind;

typedef enum {
    YEPTRIS_STYLE_ANY = 0,
    YEPTRIS_STYLE_PLAIN = 1,
    YEPTRIS_STYLE_SINGLE_QUOTED = 2,
    YEPTRIS_STYLE_DOUBLE_QUOTED = 3,
    YEPTRIS_STYLE_LITERAL = 4,
    YEPTRIS_STYLE_FOLDED = 5,
} YeptrisScalarStyle;

/* Releases the document and everything reachable from it (one call).
 * Handles are invalid afterwards. NULL is accepted. */
YEPTRIS_API void yeptris_document_free(YeptrisDocument doc);

/* Number of documents in the parsed stream. */
YEPTRIS_API size_t yeptris_document_count(YeptrisDocument doc);

/* Root node of document i (0-based); NULL when out of range. */
YEPTRIS_API YeptrisNode yeptris_document_root(YeptrisDocument doc, size_t index);

YEPTRIS_API YeptrisNodeKind yeptris_node_kind(YeptrisNode node);

/* Stable identity of the node within its document (bindings key
 * wrapper caches on it; query handles are transient, ids are not).
 * Only for equality — not an index into any public structure. */
YEPTRIS_API uint32_t yeptris_node_id(YeptrisNode node);

/* Scalar content / alias name. Returns NULL for collections.
 * Memory: borrowed from the input (or document-owned when the value was
 * folded/escaped) — valid until yeptris_document_free. */
YEPTRIS_API const char* yeptris_node_value(YeptrisNode node, size_t* len);

YEPTRIS_API YeptrisScalarStyle yeptris_node_style(YeptrisNode node);

/* Node properties; empty (NULL, 0) when absent. */
YEPTRIS_API const char* yeptris_node_tag(YeptrisNode node, size_t* len);
YEPTRIS_API const char* yeptris_node_anchor(YeptrisNode node, size_t* len);

/* Alias target node; NULL for non-aliases. */
YEPTRIS_API YeptrisNode yeptris_node_alias_target(YeptrisNode node);

/* Sequence entry count / entry i. */
YEPTRIS_API size_t yeptris_node_seq_count(YeptrisNode node);
YEPTRIS_API YeptrisNode yeptris_node_seq_at(YeptrisNode node, size_t index);

/* Mapping pair count / value for a key (linear; interning lands with the
 * full DOM item). The first matching key wins. */
YEPTRIS_API size_t yeptris_node_map_count(YeptrisNode node);
YEPTRIS_API YeptrisNode yeptris_node_map_get(YeptrisNode node, const char* key, size_t key_len);

/* Ordered pair access: key and value of pair i (0-based). Returns 0
 * and sets the out-params when the index is in range, -1 otherwise. */
YEPTRIS_API int yeptris_node_map_at(YeptrisNode node, size_t index, YeptrisNode* key,
                                    YeptrisNode* value);

/* Bulk children drain — the O(n) iteration primitive (ruby #168: the
 * per-index seq_at/map_at walks make an 80k-row sequence O(n^2)). One
 * call walks the child list once and fills out[0..cap) with handles:
 * sequence elements in order; mapping KEY,VALUE pairs interleaved.
 * Returns the total child count (mapping pairs count twice); out==NULL
 * or cap==0 returns the count without allocating handles. Handles come
 * from the document's pool — document_free reclaims them. */
YEPTRIS_API size_t yeptris_node_children(YeptrisNode node, YeptrisNode* out, size_t cap);

/* ---- Construction (TODO.impl/11 phase 3) ----
 *
 * Build documents from scratch and mutate them; the emitter and every
 * query API above work on synthesized documents unchanged. All values
 * are COPIED into the document — nothing is borrowed from the caller.
 * Nodes belong to exactly one parent (or one document root); a node
 * already attached, a duplicate map key (add), cross-document links,
 * and trees deeper than the nesting cap (YEPTRIS_ERROR_DEPTH) are
 * rejected. Deletion unlinks but keeps arena lifetime: handles stay
 * valid until yeptris_document_free. */

/* An empty document with no input; build nodes and set the root. */
YEPTRIS_API YeptrisDocument yeptris_document_new(void);

/* The same, with the schema resolver plain scalars type through
 * (YEPTRIS_SCHEMA_12_CORE is yeptris_document_new's default). Additive
 * ABI. */
YEPTRIS_API YeptrisDocument yeptris_document_new_schema(YeptrisSchema schema);

/* Records a detached node as document i's root. */
YEPTRIS_API int yeptris_document_set_root(YeptrisDocument doc, YeptrisNode root);

YEPTRIS_API YeptrisNode yeptris_node_new_mapping(YeptrisDocument doc);
YEPTRIS_API YeptrisNode yeptris_node_new_sequence(YeptrisDocument doc);

/* Memory: value copied (document lifetime). Plain style types the copy
 * through the schema resolver exactly like parse; quoted/block styles
 * force string. */
YEPTRIS_API YeptrisNode yeptris_node_new_scalar(YeptrisDocument doc, const char* value, size_t len,
                                                YeptrisScalarStyle style);

/* YEPTRIS_OK, or ERROR_ARG (cross-document/detached misuse),
 * ERROR_MEMORY, ERROR_DEPTH, ERROR_PARSE (duplicate key). */
YEPTRIS_API int yeptris_node_map_add(YeptrisNode map, const char* key, size_t key_len,
                                     YeptrisNode value);

/* Replace-or-add: an existing key keeps its position and swaps the
 * value; a new key appends. */
YEPTRIS_API int yeptris_node_map_set(YeptrisNode map, const char* key, size_t key_len,
                                     YeptrisNode value);

YEPTRIS_API int yeptris_node_seq_add(YeptrisNode seq, YeptrisNode value);
YEPTRIS_API int yeptris_node_seq_del(YeptrisNode seq, size_t index);

/* Replace the entry at index (same position kept). */
YEPTRIS_API int yeptris_node_seq_set(YeptrisNode seq, size_t index, YeptrisNode value);
YEPTRIS_API int yeptris_node_map_del(YeptrisNode map, const char* key, size_t key_len);

/* ---- bulk build (TODO.impl/15 phase D: the dump-side mirror of the
 * recorder's bulk drain — ONE call builds a whole document, so
 * bindings never pay per-node FFI) ------------------------------------- */

typedef enum {
    YEPTRIS_BUILD_SCALAR = 1, /* push a scalar (value bytes in the blob) */
    YEPTRIS_BUILD_SEQ = 2,    /* open a sequence */
    YEPTRIS_BUILD_MAP = 3,    /* open a mapping */
    YEPTRIS_BUILD_END = 4,    /* close the innermost open container */
    YEPTRIS_BUILD_TAG = 5,    /* tag the last-placed node (tag bytes in
                               * the blob) — #300: nil mapping keys need
                               * Psych's explicit `!` tag */
} YeptrisBuildOp;

/* 12 bytes, ABI-pinned: op, style (scalar: YeptrisScalarStyle;
 * ignored for containers), reserved, then the value slice. */
typedef struct {
    uint8_t op;
    uint8_t style;
    uint16_t reserved;
    uint32_t off;
    uint32_t len;
} YeptrisBuildEntry;
#if defined(__cplusplus)
static_assert(sizeof(YeptrisBuildEntry) == 12, "build entry layout pinned");
#else
_Static_assert(sizeof(YeptrisBuildEntry) == 12, "build entry layout pinned");
#endif

/* Walks the entries in order — the same document-order grammar as the
 * event stream: scalars link into the innermost open container (in a
 * mapping, entries alternate key/value), SEQ/MAP open a child
 * container, END closes it; after the last entry exactly one root
 * must remain open-free and becomes the document root. Scalars copy
 * their bytes from the blob (nothing is borrowed). Duplicate keys
 * keep the LAST value, exactly as the parser accepts them (host
 * mappings cannot produce one; the check would cost O(n^2) on wide
 * maps). Returns YEPTRIS_OK, ERROR_ARG (NULL inputs),
 * ERROR_PARSE (imbalanced walk: END on an empty stack, entries after
 * the root closed, an unclosed container, a dangling key, a bad op
 * or out-of-range slice), ERROR_DEPTH, or ERROR_MEMORY. */
YEPTRIS_API YeptrisStatus yeptris_document_build(YeptrisDocument doc,
                                                 const YeptrisBuildEntry* entries, size_t count,
                                                 const char* blob, size_t blob_len);

/* Appends a pre-built key node (duplicate rejected). */
YEPTRIS_API int yeptris_node_map_add_node(YeptrisNode map, YeptrisNode key, YeptrisNode value);

/* Props on synthesized nodes (copied in). An alias node carries its
 * display name and resolves to `target` for queries/serialization. */
YEPTRIS_API int yeptris_node_set_anchor(YeptrisNode node, const char* name, size_t len);
YEPTRIS_API int yeptris_node_set_tag(YeptrisNode node, const char* tag, size_t len);
YEPTRIS_API YeptrisNode yeptris_node_new_alias(YeptrisDocument doc, YeptrisNode target,
                                               const char* name, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_DOM_PUBLIC_H */
