/* allocator.h — the single indirection point for system memory.
 *
 * Every consumer (pool, arena, document) takes a yep_allocator explicitly;
 * yep_system_allocator() is the default. No process-global override —
 * injection is just passing a different allocator (TODO.impl/19 tests
 * count and fail allocations this way).
 *
 * Memory law: nothing outside memory/ calls malloc/free directly.
 */
#ifndef YEP_ALLOCATOR_H
#define YEP_ALLOCATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* (*yep_alloc_fn)(void* ctx, size_t size);
typedef void (*yep_free_fn)(void* ctx, void* ptr);

typedef struct yep_allocator {
    yep_alloc_fn alloc; /* returns NULL on failure; size > 0 */
    yep_free_fn free;   /* accepts NULL */
    void* ctx;
} yep_allocator;

/* The system allocator (malloc/free). Valid forever. */
const yep_allocator* yep_system_allocator(void);

static inline void* yep_alloc(const yep_allocator* a, size_t size) {
    return a->alloc(a->ctx, size);
}

static inline void yep_free(const yep_allocator* a, void* ptr) {
    a->free(a->ctx, ptr);
}

#ifdef __cplusplus
}
#endif

#endif /* YEP_ALLOCATOR_H */
