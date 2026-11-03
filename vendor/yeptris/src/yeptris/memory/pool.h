/* pool.h — O(1) bump pool for small, same-lifetime objects.
 *
 * Allocation is pointer-bump within the current block; oversized requests
 * get a dedicated block. Destruction frees every block in one walk. The
 * pool never frees individual objects (arena-of-record semantics for
 * transient same-lifetime work, e.g. per-parse scratch).
 */
#ifndef YEP_POOL_H
#define YEP_POOL_H

#include <stddef.h>

#include "allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yep_pool yep_pool;

/* block_size 0 selects the 4 KiB default; rounded up to at least 256 B. */
yep_pool* yep_pool_create(const yep_allocator* sys, size_t block_size);
void yep_pool_destroy(yep_pool* pool);

/* Bump-allocates size bytes with power-of-two alignment. Returns NULL on
 * allocation failure or absurd align. */
void* yep_pool_alloc(yep_pool* pool, size_t size, size_t align);

/* Number of blocks allocated so far (observability for tests + ledger). */
size_t yep_pool_block_count(const yep_pool* pool);

#ifdef __cplusplus
}
#endif

#endif /* YEP_POOL_H */
