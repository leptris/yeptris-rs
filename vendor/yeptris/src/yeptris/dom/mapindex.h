/* mapindex.h — lazy per-mapping key index (TODO.impl/11).
 *
 * O(1) map lookup over the sibling-linked pair list: on a mapping's
 * first lookup an open-addressing table (view hash -> key child id)
 * is built from its pairs; probes compare the full view bytes on
 * hash matches. First key wins on duplicates (the linear scan's
 * contract). Only SCALAR keys are indexed — collection keys never
 * match a string lookup, exactly like the scan.
 *
 * Tables and the per-node slot array live on the system allocator
 * (NOT the DOM pool): mutation invalidates a table by freeing it,
 * and pool memory cannot be individually released. Everything is
 * freed at document destruction.
 *
 * Threads: table builds and mutations serialize on the document's
 * index mutex; built tables are immutable, so concurrent probes are
 * safe — the read-only sharing contract holds.
 */
#ifndef YEP_MAPINDEX_H
#define YEP_MAPINDEX_H

#include <stdint.h>

#include "common/mutex.h"

#include "common/nametab.h"
#include "memory/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-mapping table: hash -> key node id (opaque here; mapindex.c). */
typedef struct yep_midx_tab yep_midx_tab;

struct yep_midx_state {
    yep_midx_tab** tabs; /* per node id: table or NULL (lazy) */
    uint32_t tabs_cap;   /* slots allocated (grown with the node set) */
    yep_mutex_raw mu;
    int mu_ready;
};

/* The key node id for `key` in `map`, or UINT32_MAX when absent.
 * Builds the mapping's table on first use. UINT32_MAX also on
 * allocation failure (the caller falls back to a linear scan if it
 * needs to distinguish — callers treat both as "not found"). */
struct yep_dom;
uint32_t yep_midx_lookup(struct yep_dom* d, uint32_t map, yep_view key);

/* Drops `map`'s table (mutation changed the pairs). Cheap no-op when
 * no table was built. */
void yep_midx_invalidate(struct yep_dom* d, uint32_t map);

/* Frees every table and the slot array (document destruction). */
void yep_midx_destroy(struct yep_dom* d);

#ifdef __cplusplus
}
#endif

#endif /* YEP_MAPINDEX_H */
