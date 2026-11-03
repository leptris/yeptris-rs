/* nametab.h — open-addressing view→id interner (TODO.impl/18A).
 *
 * The single primitive for name-keyed registries: engine anchor names,
 * DOM anchor binding, mapping-key interning. Keys are borrowed views —
 * the referenced storage must outlive the table (parse input or a
 * document-owned pool). No deletes, so no tombstones; upsert of an
 * existing key overwrites in place (last definition wins).
 */
#ifndef YEP_NAMETAB_H
#define YEP_NAMETAB_H

#include <stdint.h>

#include "common/string_view.h"
#include "memory/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* get() miss sentinel; never a stored value (ids are array indices). */
#define YEP_NAMETAB_NIL UINT32_MAX

typedef struct yep_nt_slot {
    uint64_t prefix; /* the key's first 8 bytes, zero-padded */
    uint32_t val;    /* <=8B keys: the value INLINE; longer: entry index+1 */
    uint32_t klen1;  /* key length + 1; 0 = empty slot */
} yep_nt_slot;       /* 16 bytes: one cache line per probe */

typedef struct yep_nametab {
    const yep_allocator* sys;
    yep_nt_slot* slots;
    yep_view* keys; /* >8-byte keys only, insertion-ordered */
    uint32_t* values;
    uint32_t ext_count; /* live entries in keys/values */
    uint32_t ext_cap;
    uint32_t count; /* live entries (inline + external) */
    uint32_t cap;   /* slot count, power of two */
} yep_nametab;

/* Returns 0 only on allocation failure (table unchanged). */
int yep_nametab_init(yep_nametab* t, const yep_allocator* sys);
void yep_nametab_free(yep_nametab* t);

/* Insert or overwrite. Returns 0 on allocation failure. */
int yep_nametab_set(yep_nametab* t, yep_view key, uint32_t value);

/* Drops all entries, keeping the allocations. */
void yep_nametab_clear(yep_nametab* t);

/* Pre-sizes for n inserts: no rehash while count stays <= n. The slots
 * table is rebuilt (ids keep their insertion order); the keys/values
 * arrays are only grown. Returns 0 only on allocation failure. */
int yep_nametab_reserve(yep_nametab* t, uint32_t n);

/* YEP_NAMETAB_NIL when absent. */
uint32_t yep_nametab_get(const yep_nametab* t, yep_view key);

/* The table's hash (FNV-1a + murmur-style finalizer): the ONE view
 * hash — the map index and any future name-keyed structure reuse it,
 * never a re-derivation. */
uint32_t yep_view_hash(yep_view s);

#ifdef __cplusplus
}
#endif

#endif /* YEP_NAMETAB_H */
