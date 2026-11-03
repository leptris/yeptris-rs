/* arena.h — the contiguous per-document arena (libleptris TODO 183
 * pattern): an initial block sized from scan-derived hints so a typical
 * document needs no further system allocations, plus amortized-doubling
 * growth for underestimated inputs. All document-reachable memory lives
 * here or in a pool; yeptris_document_free frees the arena and the world.
 */
#ifndef YEP_ARENA_H
#define YEP_ARENA_H

#include <stddef.h>

#include "allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Content-derived sizing hints. Counts come from the scan layer's fused
 * SIMD counters (TODO.impl/06). The multipliers are heuristic placeholders
 * until 06 wires measured densities; they only affect the initial block
 * size, never correctness. */
typedef struct yep_sizing_hints {
    size_t min_capacity;  /* explicit floor */
    size_t newline_count; /* '\n' occurrences in the input */
    size_t colon_count;   /* ':' occurrences */
    size_t quote_count;   /* '"' occurrences */
} yep_sizing_hints;

typedef struct yep_arena_stats {
    size_t blocks;
    size_t capacity;   /* total payload bytes across blocks */
    size_t used;       /* bytes handed out (excludes alignment loss) */
    size_t sys_allocs; /* system allocation count (the 0-after-reserve gate) */
} yep_arena_stats;

typedef struct yep_arena yep_arena;

/* hints may be NULL (16 KiB default). */
yep_arena* yep_arena_create(const yep_allocator* sys, const yep_sizing_hints* hints);
void yep_arena_destroy(yep_arena* arena);

/* Allocates size bytes with power-of-two alignment. Returns NULL on
 * allocation failure or absurd align. */
void* yep_arena_alloc(yep_arena* arena, size_t size, size_t align);

yep_arena_stats yep_arena_get_stats(const yep_arena* arena);

#ifdef __cplusplus
}
#endif

#endif /* YEP_ARENA_H */
