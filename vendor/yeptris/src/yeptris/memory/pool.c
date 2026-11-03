/* pool.c — bump pool implementation. */

#include <stdint.h>

#include "pool.h"

#define YEP_POOL_DEFAULT_BLOCK (4u * 1024u)
#define YEP_POOL_MIN_BLOCK 256u

typedef struct yep_block {
    struct yep_block* next;
    size_t size; /* payload bytes (excludes the header) */
    size_t used;
} yep_block;

struct yep_pool {
    const yep_allocator* sys;
    yep_block* head;
    size_t block_size;
    size_t blocks;
};

static size_t yep_round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static int yep_align_ok(size_t align) {
    return align != 0 && (align & (align - 1)) == 0;
}

static yep_block* yep_block_new(const yep_allocator* sys, size_t payload) {
    yep_block* b = yep_alloc(sys, sizeof(yep_block) + payload);
    if (b == NULL) {
        return NULL;
    }
    b->next = NULL;
    b->size = payload;
    b->used = 0;
    return b;
}

void* yep_pool_alloc(yep_pool* pool, size_t size, size_t align) {
    if (pool == NULL || size == 0 || !yep_align_ok(align)) {
        return NULL;
    }
    /* mirror of the arena guard: the fit test's start+size and the
     * payload sums must not wrap on a hostile size */
    if (size > (SIZE_MAX - align) / 2) {
        /* half the pointer space: both the fit test's start+size and
         * the new-block payload sums stay wrap-free below this */
        return NULL;
    }

    yep_block* b = pool->head;
    if (b == NULL) { /* the lazy first block (#157) */
        b = yep_block_new(pool->sys, pool->block_size);
        if (b == NULL) {
            return NULL;
        }
        b->next = NULL;
        pool->head = b;
        pool->blocks = 1;
    }
    uintptr_t base = (uintptr_t)b + sizeof(yep_block);
    uintptr_t start = yep_round_up(base + b->used, align);

    /* the fit test in subtraction form — the arena twin carries the
     * full comment (wrap-free comparisons, round_up overshoot covered
     * by the limit-first clause) */
    uintptr_t limit = base + b->size;
    if (start >= limit || size > limit - start) {
        /* Dedicated-or-default new block, sized so the aligned request
         * always fits (malloc returns at least 16-byte-aligned memory, so
         * the correction is < align). */
        size_t payload = pool->block_size;
        if (payload < size + align + sizeof(yep_block)) {
            payload = size + align + sizeof(yep_block);
        }
        b = yep_block_new(pool->sys, payload);
        if (b == NULL) {
            return NULL;
        }
        b->next = pool->head;
        pool->head = b;
        pool->blocks++;
        base = (uintptr_t)b + sizeof(yep_block);
        start = yep_round_up(base, align);
    }

    b->used = start + size - base;
    return (void*)start;
}

yep_pool* yep_pool_create(const yep_allocator* sys, size_t block_size) {
    if (sys == NULL) {
        return NULL;
    }
    if (block_size == 0) {
        block_size = YEP_POOL_DEFAULT_BLOCK;
    }
    if (block_size < YEP_POOL_MIN_BLOCK) {
        block_size = YEP_POOL_MIN_BLOCK;
    }

    yep_pool* p = yep_alloc(sys, sizeof(yep_pool));
    if (p == NULL) {
        return NULL;
    }
    p->sys = sys;
    p->block_size = block_size;
    /* #157: the first block is lazy — one fewer eager allocation per
     * document; pool_alloc materializes it on first use */
    p->head = NULL;
    p->blocks = 0;
    return p;
}

void yep_pool_destroy(yep_pool* pool) {
    if (pool == NULL) {
        return;
    }
    yep_block* b = pool->head;
    while (b != NULL) {
        yep_block* next = b->next;
        yep_free(pool->sys, b);
        b = next;
    }
    yep_free(pool->sys, pool);
}

size_t yep_pool_block_count(const yep_pool* pool) {
    return pool ? pool->blocks : 0;
}
