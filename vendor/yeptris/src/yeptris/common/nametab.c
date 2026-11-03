/* nametab.c — open-addressing view→id interner (TODO.impl/18A).
 *
 * FNV-lineage word hash (murmur-style finalizer — FNV alone has weak
 * low bits, fatal under power-of-two masking); linear probing. The
 * slot is 16 bytes and carries the key's zero-padded first-8-byte
 * prefix plus its length: for keys of <=8 bytes — the dominant
 * anchor-name shape — prefix+length identify the key EXACTLY, the
 * caller's value sits INLINE in the slot, and a probe touches ONE
 * cache line (the previous layout paid a slot line plus a random
 * keys[] line per get — nametab-GET misses were ~14% of anchor-heavy
 * parse). Longer keys keep insertion-ordered keys[]/values[] side
 * tables and confirm via a full compare, distinguished by klen1.
 * No deletes; upsert of an existing key overwrites (last wins).
 */

#include "common/nametab.h"

#include <string.h>

#define NT_MIN_CAP 64u
#define NT_LOAD_PCT 70u

static uint64_t hash_round(uint64_t h, uint64_t k) {
    h ^= k;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return h;
}

/* Word-at-a-time, constant-size loads only (a variable-length memcpy
 * lowers to a memmove call — one per probe cost ~3% of anchor-heavy
 * parse). Hash values are build-internal: nothing persists them. */
static uint64_t load_small(const char* p, uint32_t n) {
    uint64_t k = 0;
    if (n & 4) {
        uint32_t v;
        memcpy(&v, p, 4);
        k = v;
        p += 4;
    }
    if (n & 2) {
        uint16_t v;
        memcpy(&v, p, 2);
        k = (k << 16) | v;
        p += 2;
    }
    if (n & 1) {
        k = (k << 8) | (unsigned char)*p;
    }
    return k;
}

uint32_t yep_view_hash(yep_view s) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)s.len;
    if (s.len <= 8) {
        h = hash_round(h, load_small(s.p, s.len));
    } else if (s.len <= 16) {
        uint64_t k0;
        memcpy(&k0, s.p, 8);
        h = hash_round(hash_round(h, k0), load_small(s.p + 8, s.len - 8));
    } else {
        uint64_t k0, kn;
        memcpy(&k0, s.p, 8);
        memcpy(&kn, s.p + s.len - 8, 8);
        h = hash_round(hash_round(hash_round(h, k0), kn), (uint64_t)s.len * 0x85EBCA77ull);
    }
    h ^= h >> 32;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return (uint32_t)(h ^ (h >> 32));
}

/* The zero-padded first-8-byte prefix: with the length it identifies
 * keys of <=8 bytes EXACTLY (no confirmation ever needed). */
static uint64_t key_prefix(yep_view k) {
    return k.len >= 8 ? load_small(k.p, 8) : load_small(k.p, k.len);
}

int yep_nametab_init(yep_nametab* t, const yep_allocator* sys) {
    if (t == NULL || sys == NULL) {
        return 0;
    }
    t->sys = sys;
    t->slots = yep_alloc(sys, NT_MIN_CAP * sizeof(*t->slots));
    t->keys = NULL;
    t->values = NULL;
    if (t->slots == NULL) {
        return 0;
    }
    memset(t->slots, 0, NT_MIN_CAP * sizeof(*t->slots));
    t->ext_count = 0;
    t->ext_cap = 0;
    t->count = 0;
    t->cap = NT_MIN_CAP;
    return 1;
}

void yep_nametab_free(yep_nametab* t) {
    if (t == NULL) {
        return;
    }
    yep_free(t->sys, t->slots);
    yep_free(t->sys, t->keys);
    yep_free(t->sys, t->values);
    memset(t, 0, sizeof(*t));
}

static int nt_ext_grow(yep_nametab* t, uint32_t need) {
    if (need <= t->ext_cap) {
        return 1;
    }
    uint32_t cap = t->ext_cap ? t->ext_cap * 2 : 16;
    while (cap < need) {
        cap *= 2;
    }
    yep_view* keys = yep_alloc(t->sys, cap * sizeof(*keys));
    uint32_t* values = yep_alloc(t->sys, cap * sizeof(*values));
    if (keys == NULL || values == NULL) {
        yep_free(t->sys, keys);
        yep_free(t->sys, values);
        return 0;
    }
    if (t->ext_count > 0) {
        memcpy(keys, t->keys, (size_t)t->ext_count * sizeof(*keys));
        memcpy(values, t->values, (size_t)t->ext_count * sizeof(*values));
    }
    yep_free(t->sys, t->keys);
    yep_free(t->sys, t->values);
    t->keys = keys;
    t->values = values;
    t->ext_cap = cap;
    return 1;
}

/* Replaces every entry into newcap slots. Inline entries rebuild
 * their key bytes from the prefix (lossless for <=8-byte keys);
 * external entries re-hash from keys[]. */
static int nt_rehash(yep_nametab* t, uint32_t newcap) {
    yep_nt_slot* slots = yep_alloc(t->sys, newcap * sizeof(*slots));
    if (slots == NULL) {
        return 0;
    }
    memset(slots, 0, newcap * sizeof(*slots));
    uint32_t mask = newcap - 1;
    for (uint32_t i = 0; i < t->cap; i++) {
        yep_nt_slot src = t->slots[i];
        if (src.klen1 == 0) {
            continue;
        }
        uint32_t h;
        if (src.klen1 > 9) {
            h = yep_view_hash(t->keys[src.val - 1]);
        } else {
            char buf[8];
            uint64_t p = src.prefix;
            uint32_t len = src.klen1 - 1;
            memcpy(buf, &p, 8); /* same little-endian round-trip as load_small */
            yep_view kb = {buf, len};
            h = yep_view_hash(kb);
        }
        uint32_t j = h & mask;
        while (slots[j].klen1 != 0) {
            j = (j + 1) & mask;
        }
        slots[j] = src;
    }
    yep_free(t->sys, t->slots);
    t->slots = slots;
    t->cap = newcap;
    return 1;
}

uint32_t yep_nametab_get(const yep_nametab* t, yep_view key) {
    if (t == NULL || t->cap == 0) {
        return YEP_NAMETAB_NIL;
    }
    uint32_t klen1 = key.len + 1;
    uint64_t prefix = key_prefix(key);
    uint32_t mask = t->cap - 1;
    uint32_t j = yep_view_hash(key) & mask;
    for (;;) {
        const yep_nt_slot* s = &t->slots[j];
        if (s->klen1 == 0) {
            return YEP_NAMETAB_NIL;
        }
        if (s->klen1 == klen1 && s->prefix == prefix) {
            if (klen1 <= 9) {
                return s->val; /* inline: prefix+len identify the key */
            }
            /* external: confirm the full key (prefix collisions) */
            if (yep_view_eq(t->keys[s->val - 1], key)) {
                return t->values[s->val - 1];
            }
        }
        j = (j + 1) & mask;
    }
}

void yep_nametab_clear(yep_nametab* t) {
    if (t == NULL || t->cap == 0) {
        return;
    }
    memset(t->slots, 0, (size_t)t->cap * sizeof(*t->slots));
    t->count = 0;
    t->ext_count = 0;
}

int yep_nametab_set(yep_nametab* t, yep_view key, uint32_t value) {
    if (t == NULL || t->cap == 0) {
        return 0;
    }
    uint32_t klen1 = key.len + 1;
    uint64_t prefix = key_prefix(key);
    uint32_t mask = t->cap - 1;
    uint32_t j = yep_view_hash(key) & mask;
    for (;;) {
        yep_nt_slot* s = &t->slots[j];
        if (s->klen1 == 0) {
            break; /* insert below */
        }
        if (s->klen1 == klen1 && s->prefix == prefix) {
            if (klen1 <= 9) {
                s->val = value; /* last definition wins */
                return 1;
            }
            if (yep_view_eq(t->keys[s->val - 1], key)) {
                t->values[s->val - 1] = value;
                return 1;
            }
        }
        j = (j + 1) & mask;
    }
    if ((uint64_t)(t->count + 1) * 100 > (uint64_t)t->cap * NT_LOAD_PCT) {
        if (!nt_rehash(t, t->cap * 2)) {
            return 0;
        }
        mask = t->cap - 1;
        j = yep_view_hash(key) & mask;
        while (t->slots[j].klen1 != 0) {
            j = (j + 1) & mask;
        }
    }
    if (klen1 > 9) {
        if (!nt_ext_grow(t, t->ext_count + 1)) {
            return 0;
        }
        t->keys[t->ext_count] = key;
        t->values[t->ext_count] = value;
        t->ext_count++;
        t->slots[j].prefix = prefix;
        t->slots[j].val = t->ext_count; /* entry index + 1 */
        t->slots[j].klen1 = klen1;
    } else {
        t->slots[j].prefix = prefix;
        t->slots[j].val = value; /* inline */
        t->slots[j].klen1 = klen1;
    }
    t->count++;
    return 1;
}

int yep_nametab_reserve(yep_nametab* t, uint32_t n) {
    if (t == NULL || t->cap == 0) {
        return 0;
    }
    if ((uint64_t)n * 100 <= (uint64_t)t->cap * NT_LOAD_PCT) {
        return 1;
    }
    uint64_t need = ((uint64_t)n * 100 + NT_LOAD_PCT - 1) / NT_LOAD_PCT;
    uint32_t newcap = NT_MIN_CAP;
    while ((uint64_t)newcap < need) {
        newcap *= 2;
    }
    return nt_rehash(t, newcap);
}
