/* mutate.c — the from-scratch DOM builder (TODO.impl/11 phase 3).
 *
 * Synthesis mirrors what parse produces: runtime values are copied
 * into the DOM pool (nothing is borrowed), typing runs through the
 * core12 resolver (the SSOT — a quoted "12" is a string here exactly
 * as at parse), and links form only through dom_link. Invariants:
 * one parent per node, mapping children always balanced key/value
 * pairs, depth capped at YEP_DOM_MAX_DEPTH — so the emitter's
 * guarantees hold for synthesized trees unchanged.
 */

#include <string.h>

#include <yeptris/resolve.h>

#include "dom/dom.h"
#include "memory/allocator.h"
#include "resolve/resolver.h"

/* status codes (dom.h) */
#define MUT_ERR -1
#define MUT_DUP -2
#define MUT_DEPTH -3
#define MUT_ATTACHED -4

static yep_view mut_copy(yep_dom* d, const char* s, size_t len) {
    yep_view v = {NULL, 0};
    if (len == 0) {
        return v;
    }
    char* cp = yep_pool_alloc(d->pool, len, 16);
    if (cp == NULL) {
        return v;
    }
    memcpy(cp, s, len);
    v.p = cp;
    v.len = (uint32_t)len;
    return v;
}

uint32_t yep_mut_scalar(yep_dom* d, const char* value, size_t len, uint8_t style) {
    if (d == NULL || (value == NULL && len > 0)) {
        return UINT32_MAX;
    }
    uint32_t id = dom_new_node(d, NULL, YEP_DOM_SCALAR);
    if (id == UINT32_MAX) {
        return UINT32_MAX;
    }
    yep_dnode* n = &d->nodes[id];
    yep_view v = mut_copy(d, value, len);
    if (len > 0 && v.p == NULL) {
        d->ncount--;
        return UINT32_MAX;
    }
    n->value = yep_dom_str_put(d, v.p ? (const char*)v.p : "", v.len);
    n->style = style;
    n->tag_id =
        style == YEP_STYLE_PLAIN ? yep_dom_resolver(d)->resolve(NULL, v.p, v.len) : YEPTRIS_TAG_STR;
    return id;
}

uint32_t yep_mut_seq(yep_dom* d, uint8_t flow) {
    if (d == NULL) {
        return UINT32_MAX;
    }
    uint32_t id = dom_new_node(d, NULL, YEP_DOM_SEQUENCE);
    if (id == UINT32_MAX) {
        return UINT32_MAX;
    }
    d->nodes[id].flow = flow;
    d->nodes[id].tag_id = YEPTRIS_TAG_SEQ;
    return id;
}

uint32_t yep_mut_map(yep_dom* d, uint8_t flow) {
    if (d == NULL) {
        return UINT32_MAX;
    }
    uint32_t id = dom_new_node(d, NULL, YEP_DOM_MAPPING);
    if (id == UINT32_MAX) {
        return UINT32_MAX;
    }
    d->nodes[id].flow = flow;
    d->nodes[id].tag_id = YEPTRIS_TAG_MAP;
    return id;
}

/* Subtree height with a depth-safe stop (defensive: synthesized trees
 * are capped before links form, so the walk never exceeds the cap). */
static int mut_height(const yep_dom* d, uint32_t id, uint32_t* height) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return -1;
    }
    uint32_t child_h = 0;
    uint32_t c = n->first_child;
    while (c != UINT32_MAX) {
        const yep_dnode* cn = yep_dom_node(d, c);
        if (cn == NULL) {
            return -1;
        }
        uint32_t h = 0;
        if (mut_height(d, c, &h) != 0) {
            return -1;
        }
        if (h + 1 > child_h) {
            child_h = h + 1;
        }
        c = cn->next_sibling;
    }
    *height = child_h;
    return 0;
}

/* Descendant depths after a move/relink (one walk); also the
 * stream-end fixup: builder links form bottom-up, so local depths are
 * relative until the tree completes (dom.c calls this per root). */
/* Mutation links carry the side-table facts parse no longer writes
 * (64-2a): the attached flag and the child's depth under its parent. */
static void mut_link(yep_dom* d, uint32_t parent, uint32_t child) {
    dom_link(d, parent, child);
    dom_mut_set_att(d, child, 1);
    dom_mut_set_depth(d, child, (uint16_t)(dom_mut_depth(d, parent) + 1));
}

void yep_mut_set_depths(yep_dom* d, uint32_t id, uint16_t depth) {
    dom_mut_set_depth(d, id, depth);
    uint32_t c = d->nodes[id].first_child;
    while (c != UINT32_MAX) {
        yep_mut_set_depths(d, c, (uint16_t)(depth + 1));
        c = d->nodes[c].next_sibling;
    }
}

static int mut_attach_ok(yep_dom* d, uint32_t parent, uint32_t child) {
    if (child >= d->ncount || dom_mut_att(d, child)) {
        return MUT_ATTACHED;
    }
    uint32_t h = 0;
    if (mut_height(d, child, &h) != 0) {
        return MUT_ERR;
    }
    if ((uint32_t)dom_mut_depth(d, parent) + 1 + h > YEP_DOM_MAX_DEPTH) {
        return MUT_DEPTH;
    }
    return 0;
}

int yep_mut_seq_add(yep_dom* d, uint32_t seq, uint32_t child) {
    if (d == NULL || seq >= d->ncount || d->nodes[seq].kind != YEP_DOM_SEQUENCE) {
        return MUT_ERR;
    }
    int rc = mut_attach_ok(d, seq, child);
    if (rc != 0) {
        return rc;
    }
    mut_link(d, seq, child);
    return 0;
}

static uint32_t map_find(const yep_dom* d, uint32_t map, const char* key, size_t klen) {
    const yep_dnode* m = yep_dom_node(d, map);
    if (m == NULL) {
        return UINT32_MAX;
    }
    uint32_t c = m->first_child;
    while (c != UINT32_MAX) {
        const yep_dnode* k = yep_dom_node(d, c);
        if (k == NULL) {
            return UINT32_MAX;
        }
        yep_view kv = yep_dom_view(d, k->value);
        if (kv.len == klen && (klen == 0 || memcmp(kv.p, key, klen) == 0)) {
            return c;
        }
        c = k->next_sibling; /* the value node follows the key */
        if (c == UINT32_MAX) {
            break;
        }
        k = yep_dom_node(d, c);
        if (k == NULL) {
            return UINT32_MAX;
        }
        c = k->next_sibling;
    }
    return UINT32_MAX;
}

/* Appends the (key,value) pair; assumes the dup check already ran. */
static int map_place_pair(yep_dom* d, uint32_t map, const char* key, size_t klen, uint32_t value) {
    uint32_t k = yep_mut_scalar(d, key, klen, YEP_STYLE_PLAIN);
    if (k == UINT32_MAX) {
        return MUT_ERR;
    }
    int rc = mut_attach_ok(d, map, value);
    if (rc != 0) {
        d->ncount--; /* the key node never linked: retract */
        return rc;
    }
    mut_link(d, map, k);
    mut_link(d, map, value);
    yep_midx_invalidate(d, map);
    return 0;
}

int yep_mut_map_add(yep_dom* d, uint32_t map, const char* key, size_t klen, uint32_t value) {
    if (d == NULL || key == NULL || map >= d->ncount || d->nodes[map].kind != YEP_DOM_MAPPING) {
        return MUT_ERR;
    }
    if (map_find(d, map, key, klen) != UINT32_MAX) {
        return MUT_DUP;
    }
    return map_place_pair(d, map, key, klen, value);
}

int yep_mut_map_set(yep_dom* d, uint32_t map, const char* key, size_t klen, uint32_t value) {
    if (d == NULL || key == NULL || map >= d->ncount || d->nodes[map].kind != YEP_DOM_MAPPING) {
        return MUT_ERR;
    }
    uint32_t k = map_find(d, map, key, klen);
    if (k == UINT32_MAX) {
        return map_place_pair(d, map, key, klen, value);
    }
    int rc = mut_attach_ok(d, map, value);
    if (rc != 0) {
        return rc;
    }
    /* replace the value in place: position preserved (json-c) */
    yep_dnode* m = &d->nodes[map];
    yep_dnode* kn = &d->nodes[k];
    uint32_t old = kn->next_sibling;
    if (old == UINT32_MAX) {
        return MUT_ERR; /* unbalanced pair: impossible by construction */
    }
    dom_mut_set_att(d, old, 0);
    d->nodes[value].next_sibling = d->nodes[old].next_sibling;
    kn->next_sibling = value;
    if (m->last_child == old) {
        m->last_child = value;
    }
    dom_mut_set_att(d, value, 1);
    yep_mut_set_depths(d, value, (uint16_t)(dom_mut_depth(d, k) + 1));
    yep_midx_invalidate(d, map);
    return 1;
}

/* Unlinks ONE node from a parent's child chain (pairs call it twice). */
static int unlink_child(yep_dom* d, uint32_t parent, uint32_t child) {
    yep_dnode* p = &d->nodes[parent];
    if (child == UINT32_MAX || p->first_child == UINT32_MAX) {
        return MUT_ERR;
    }
    if (p->first_child == child) {
        p->first_child = d->nodes[child].next_sibling;
        if (p->last_child == child) {
            p->last_child = UINT32_MAX;
        }
    } else {
        uint32_t c = p->first_child;
        while (c != UINT32_MAX && d->nodes[c].next_sibling != child) {
            c = d->nodes[c].next_sibling;
        }
        if (c == UINT32_MAX) {
            return MUT_ERR;
        }
        d->nodes[c].next_sibling = d->nodes[child].next_sibling;
        if (p->last_child == child) {
            p->last_child = c;
        }
    }
    d->child_cache_id = UINT32_MAX; /* #377: the list changed */
    d->child_cache_len = 0;
    d->nodes[child].next_sibling = UINT32_MAX;
    dom_mut_set_att(d, child, 0);
    p->count--;
    return 0;
}

int yep_mut_seq_del(yep_dom* d, uint32_t seq, uint32_t index) {
    if (d == NULL || seq >= d->ncount || d->nodes[seq].kind != YEP_DOM_SEQUENCE) {
        return MUT_ERR;
    }
    yep_dnode* s = &d->nodes[seq];
    if (index >= s->count) {
        return MUT_ERR;
    }
    uint32_t c = s->first_child;
    for (uint32_t i = 0; i < index && c != UINT32_MAX; i++) {
        c = d->nodes[c].next_sibling;
    }
    return unlink_child(d, seq, c);
}

int yep_mut_set_anchor(yep_dom* d, uint32_t node, const char* name, size_t len) {
    if (d == NULL || node >= d->ncount || name == NULL) {
        return MUT_ERR;
    }
    d->nodes[node].anchor = yep_dom_str_put(d, name, (uint32_t)len);
    return 0;
}

int yep_mut_set_tag(yep_dom* d, uint32_t node, const char* tag, size_t len) {
    if (d == NULL || node >= d->ncount || tag == NULL) {
        return MUT_ERR;
    }
    d->nodes[node].tag = yep_dom_str_put(d, tag, (uint32_t)len);
    return 0;
}

uint32_t yep_mut_new_alias(yep_dom* d, uint32_t target, const char* name, size_t len) {
    if (d == NULL || name == NULL) {
        return UINT32_MAX;
    }
    uint32_t id = dom_new_node(d, NULL, YEP_DOM_ALIAS);
    if (id == UINT32_MAX) {
        return UINT32_MAX;
    }
    d->nodes[id].target = target;
    d->nodes[id].value = yep_dom_str_put(d, name, (uint32_t)len);
    return id;
}

int yep_mut_seq_set(yep_dom* d, uint32_t seq, uint32_t index, uint32_t value) {
    if (d == NULL || seq >= d->ncount || d->nodes[seq].kind != YEP_DOM_SEQUENCE) {
        return MUT_ERR;
    }
    yep_dnode* s = &d->nodes[seq];
    if (index >= s->count) {
        return MUT_ERR;
    }
    uint32_t cur = s->first_child;
    for (uint32_t i = 0; i < index && cur != UINT32_MAX; i++) {
        cur = d->nodes[cur].next_sibling;
    }
    int rc = mut_attach_ok(d, seq, value);
    if (rc != 0) {
        return rc;
    }
    /* splice: value takes cur's position (first-child or prev's
     * next_sibling), cur is detached — same shape as map_set */
    if (s->first_child == cur) {
        s->first_child = value;
    } else {
        uint32_t prev = s->first_child;
        while (d->nodes[prev].next_sibling != cur) {
            prev = d->nodes[prev].next_sibling;
        }
        d->nodes[prev].next_sibling = value;
    }
    if (s->last_child == cur) {
        s->last_child = value;
    }
    d->nodes[value].next_sibling = d->nodes[cur].next_sibling;
    d->nodes[cur].next_sibling = UINT32_MAX;
    dom_mut_set_att(d, cur, 0);
    dom_mut_set_att(d, value, 1);
    yep_mut_set_depths(d, value, (uint16_t)(dom_mut_depth(d, seq) + 1));
    return 0;
}

int yep_mut_map_add_node(yep_dom* d, uint32_t map, uint32_t key, uint32_t value) {
    if (d == NULL || map >= d->ncount || d->nodes[map].kind != YEP_DOM_MAPPING) {
        return MUT_ERR;
    }
    yep_view kv = yep_dom_view(d, d->nodes[key].value);
    if (map_find(d, map, (const char*)kv.p, kv.len) != UINT32_MAX) {
        return MUT_DUP;
    }
    int rc = mut_attach_ok(d, map, value);
    if (rc != 0) {
        return rc;
    }
    mut_link(d, map, key);
    mut_link(d, map, value);
    yep_midx_invalidate(d, map);
    return 0;
}

/* The bulk-build pairing twin of map_add_node: same invariants,
 * without the duplicate scan — document-order entries from a host
 * mapping cannot carry a duplicate, and the linear scan is O(n^2)
 * on wide maps (187 ms of a 20k-node build). Last wins, as at
 * parse. Depths come from mut_link (top-down links, O(1) each). */
int yep_mut_map_append(yep_dom* d, uint32_t map, uint32_t key, uint32_t value) {
    if (d == NULL || map >= d->ncount || d->nodes[map].kind != YEP_DOM_MAPPING) {
        return MUT_ERR;
    }
    int rc = mut_attach_ok(d, map, value);
    if (rc != 0) {
        return rc;
    }
    mut_link(d, map, key);
    mut_link(d, map, value);
    yep_midx_invalidate(d, map);
    return 0;
}

int yep_mut_map_del(yep_dom* d, uint32_t map, const char* key, size_t klen) {
    if (d == NULL || key == NULL || map >= d->ncount || d->nodes[map].kind != YEP_DOM_MAPPING) {
        return MUT_ERR;
    }
    uint32_t k = map_find(d, map, key, klen);
    if (k == UINT32_MAX) {
        return MUT_ERR;
    }
    uint32_t v = d->nodes[k].next_sibling;
    if (unlink_child(d, map, k) != 0) {
        return MUT_ERR;
    }
    int rc = unlink_child(d, map, v);
    yep_midx_invalidate(d, map);
    return rc;
}

int yep_mut_add_root(yep_dom* d, uint32_t node) {
    if (d == NULL || node >= d->ncount || dom_mut_att(d, node)) {
        return MUT_ATTACHED;
    }
    uint32_t h = 0;
    if (mut_height(d, node, &h) != 0 || h + 1 > YEP_DOM_MAX_DEPTH) {
        return MUT_DEPTH;
    }
    if (!dom_grow_docs(d, 1)) {
        return MUT_ERR;
    }
    dom_mut_set_att(d, node, 1);
    dom_mut_set_depth(d, node, 0);
    yep_mut_set_depths(d, node, 0);
    d->docs[d->dcount++] = node;
    return 0;
}
