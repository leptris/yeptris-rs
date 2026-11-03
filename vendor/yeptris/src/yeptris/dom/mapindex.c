/* mapindex.c — lazy per-mapping key index (TODO.impl/11). */

#include "dom/mapindex.h"

#include <stdlib.h>
#include <string.h>

#include "dom/dom.h"

typedef struct yep_midx_slot {
    uint32_t hash;      /* yep_view_hash of the key view */
    uint32_t key_child; /* key node id; UINT32_MAX = empty */
} yep_midx_slot;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C99 flexible array member */
#endif
typedef struct yep_midx_tab {
    uint32_t cap; /* slot count, power of two */
    yep_midx_slot slots[];
} yep_midx_tab;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* Table capacity: power of two, >= 2x pairs (load factor <= 50%). */
static uint32_t tab_cap_for(uint32_t pairs) {
    uint32_t cap = 8;
    while (cap < pairs * 2) {
        cap *= 2;
    }
    return cap;
}

static yep_midx_tab* tab_build(const struct yep_dom* d, uint32_t map) {
    const yep_dnode* m = yep_dom_node(d, map);
    if (m == NULL || m->kind != YEP_DOM_MAPPING) {
        return NULL;
    }
    uint32_t pairs = m->count / 2;
    uint32_t cap = tab_cap_for(pairs);
    yep_midx_tab* t = yep_alloc(d->sys, sizeof(*t) + cap * sizeof(*t->slots));
    if (t == NULL) {
        return NULL;
    }
    t->cap = cap;
    for (uint32_t i = 0; i < cap; i++) {
        t->slots[i].key_child = UINT32_MAX;
    }
    uint32_t mask = cap - 1;
    uint32_t id = m->first_child;
    int want_key = 1;
    while (id != UINT32_MAX) {
        const yep_dnode* cur = yep_dom_node(d, id);
        if (cur == NULL) {
            break;
        }
        if (want_key && cur->kind == YEP_DOM_SCALAR) {
            /* |1: the empty marker is key_child, not hash — this
             * just avoids the all-zero-hash clustering of 1-char keys */
            uint32_t h = yep_view_hash(yep_dom_view(d, cur->value)) | 1u;
            uint32_t i = h & mask;
            while (t->slots[i].key_child != UINT32_MAX) {
                i = (i + 1) & mask; /* first key wins: occupied slots keep their entry */
            }
            t->slots[i].hash = h;
            t->slots[i].key_child = id;
        }
        want_key = !want_key;
        id = cur->next_sibling;
    }
    return t;
}

static int tabs_grow(struct yep_dom* d, uint32_t need) {
    struct yep_midx_state* x = &d->midx;
    if (need <= x->tabs_cap) {
        return 0;
    }
    uint32_t cap = x->tabs_cap ? x->tabs_cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    yep_midx_tab** grown = yep_alloc(d->sys, cap * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    if (x->tabs_cap > 0) {
        memcpy(grown, x->tabs, x->tabs_cap * sizeof(*grown));
        memset(grown + x->tabs_cap, 0, (cap - x->tabs_cap) * sizeof(*grown));
        yep_free(d->sys, x->tabs);
    } else {
        memset(grown, 0, cap * sizeof(*grown));
    }
    x->tabs = grown;
    x->tabs_cap = cap;
    return 0;
}

uint32_t yep_midx_lookup(struct yep_dom* d, uint32_t map, yep_view key) {
    if (d == NULL || map >= d->ncount) {
        return UINT32_MAX;
    }
    struct yep_midx_state* x = &d->midx;
    if (x->mu_ready == 0) {
        return UINT32_MAX; /* document without a live index (paranoia) */
    }
    yep_mutex_lock(&x->mu);
    if (tabs_grow(d, d->ncount) != 0) {
        yep_mutex_unlock(&x->mu);
        return UINT32_MAX;
    }
    if (x->tabs[map] == NULL) {
        x->tabs[map] = tab_build(d, map);
        if (x->tabs[map] == NULL) {
            yep_mutex_unlock(&x->mu);
            return UINT32_MAX;
        }
    }
    yep_midx_tab* t = x->tabs[map];
    uint32_t mask = t->cap - 1;
    uint32_t h = yep_view_hash(key) | 1u;
    uint32_t i = h & mask;
    uint32_t found = UINT32_MAX;
    for (;;) {
        uint32_t child = t->slots[i].key_child;
        if (child == UINT32_MAX) {
            break; /* empty slot: absent */
        }
        if (t->slots[i].hash == h) {
            const yep_dnode* kn = yep_dom_node(d, child);
            if (kn != NULL && yep_view_eq(yep_dom_view(d, kn->value), key)) {
                found = child;
                break;
            }
        }
        i = (i + 1) & mask;
    }
    yep_mutex_unlock(&x->mu);
    return found;
}

void yep_midx_invalidate(struct yep_dom* d, uint32_t map) {
    if (d == NULL || !d->midx.mu_ready || d->midx.tabs == NULL || map >= d->midx.tabs_cap) {
        return;
    }
    yep_mutex_lock(&d->midx.mu);
    yep_midx_tab* t = d->midx.tabs[map];
    if (t != NULL) {
        d->midx.tabs[map] = NULL;
        yep_free(d->sys, t);
    }
    yep_mutex_unlock(&d->midx.mu);
}

void yep_midx_destroy(struct yep_dom* d) {
    if (d == NULL) {
        return;
    }
    if (d->midx.tabs != NULL) {
        for (uint32_t i = 0; i < d->midx.tabs_cap; i++) {
            yep_free(d->sys, d->midx.tabs[i]);
        }
        yep_free(d->sys, d->midx.tabs);
        d->midx.tabs = NULL;
        d->midx.tabs_cap = 0;
    }
    if (d->midx.mu_ready) {
        yep_mutex_destroy(&d->midx.mu);
        d->midx.mu_ready = 0;
    }
}
