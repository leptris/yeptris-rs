/* arena.c — hint-sized contiguous arena with doubling growth. */

#include <stdint.h>

#include "arena.h"

#define YEP_ARENA_DEFAULT_BLOCK (16u * 1024u)

typedef struct yep_arena_block {
    struct yep_arena_block* next;
    size_t size;
    size_t used;
} yep_arena_block;

struct yep_arena {
    const yep_allocator* sys;
    yep_arena_block* head; /* newest block; next pointers run newest → oldest */
    size_t last_block_size;
    yep_arena_stats stats;
};

static size_t yep_round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static int yep_align_ok(size_t align) {
    return align != 0 && (align & (align - 1)) == 0;
}

static size_t yep_initial_capacity(const yep_sizing_hints* hints) {
    size_t cap = YEP_ARENA_DEFAULT_BLOCK;
    if (hints != NULL) {
        size_t derived = hints->min_capacity + hints->newline_count * 48 +
                         (hints->colon_count + hints->quote_count) * 8;
        if (derived > cap) {
            cap = derived;
        }
    }
    return cap;
}

static yep_arena_block* yep_arena_block_new(const yep_allocator* sys, size_t payload,
                                            size_t* sys_allocs) {
    yep_arena_block* b = yep_alloc(sys, sizeof(yep_arena_block) + payload);
    if (b == NULL) {
        return NULL;
    }
    b->next = NULL;
    b->size = payload;
    b->used = 0;
    (*sys_allocs)++;
    return b;
}

yep_arena* yep_arena_create(const yep_allocator* sys, const yep_sizing_hints* hints) {
    if (sys == NULL) {
        return NULL;
    }

    yep_arena* a = yep_alloc(sys, sizeof(yep_arena));
    if (a == NULL) {
        return NULL;
    }
    a->sys = sys;
    a->head = NULL;
    a->last_block_size = 0;
    a->stats.blocks = 0;
    a->stats.capacity = 0;
    a->stats.used = 0;
    a->stats.sys_allocs = 1; /* this struct */

    size_t cap = yep_initial_capacity(hints);
    a->head = yep_arena_block_new(sys, cap, &a->stats.sys_allocs);
    if (a->head == NULL) {
        yep_free(sys, a);
        return NULL;
    }
    a->last_block_size = cap;
    a->stats.blocks = 1;
    a->stats.capacity = cap;
    return a;
}

void* yep_arena_alloc(yep_arena* arena, size_t size, size_t align) {
    if (arena == NULL || size == 0 || !yep_align_ok(align)) {
        return NULL;
    }
    /* a hostile size near SIZE_MAX must FAIL the fit test, not wrap
     * start+size past the pointer space and pass it (the sizing sums
     * below stay inside the guard's slack) */
    if (size > (SIZE_MAX - align) / 2) {
        /* half the pointer space: both the fit test's start+size and
         * the new-block payload sums stay wrap-free below this */
        return NULL;
    }

    yep_arena_block* b = arena->head;
    uintptr_t base = (uintptr_t)b + sizeof(yep_arena_block);
    uintptr_t start = yep_round_up(base + b->used, align);

    /* the fit test in subtraction form (the audit's finding): the old
     * `start + size > base + b->size` wraps when size rides past the
     * pointer space and the request "fits". The limit-first clause
     * covers the round_up overshoot — start can sit up to align-1
     * bytes past the limit, where `limit - start` would underflow — so
     * both comparisons are wrap-free without leaning on the
     * hostile-size guard above. */
    uintptr_t limit = base + b->size;
    if (start >= limit || size > limit - start) {
        /* Amortized doubling: each new block is at least twice the last,
         * unless the request itself is larger. */
        size_t payload = arena->last_block_size * 2;
        if (payload < size + align + sizeof(yep_arena_block)) {
            payload = size + align + sizeof(yep_arena_block);
        }
        b = yep_arena_block_new(arena->sys, payload, &arena->stats.sys_allocs);
        if (b == NULL) {
            return NULL;
        }
        b->next = arena->head; /* newest at head; tail keeps destroy order */
        arena->head = b;
        arena->last_block_size = payload;
        arena->stats.blocks++;
        arena->stats.capacity += payload;
        base = (uintptr_t)b + sizeof(yep_arena_block);
        start = yep_round_up(base, align);
    }

    b->used = start + size - base;
    arena->stats.used += size;
    return (void*)start;
}

void yep_arena_destroy(yep_arena* arena) {
    if (arena == NULL) {
        return;
    }
    yep_arena_block* b = arena->head;
    while (b != NULL) {
        yep_arena_block* next = b->next;
        yep_free(arena->sys, b);
        b = next;
    }
    yep_free(arena->sys, arena);
}

yep_arena_stats yep_arena_get_stats(const yep_arena* arena) {
    yep_arena_stats zero = {0, 0, 0, 0};
    return arena ? arena->stats : zero;
}
