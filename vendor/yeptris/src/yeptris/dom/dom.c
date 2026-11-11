/* dom.c — event-built DOM (builder sink + storage). */

#include <stdio.h>
#include <string.h>

#include "common/simd_text.h"

#include "common/mutex.h"

#include "dom.h"

#include "../scan/scan.h" /* the dq-forced SSOT (the fact bit) */

#include <yeptris/resolve.h> /* the tag-id SSOT (the strict literal arm) */

/* The bounded-parse caps: each node consumes at least one input byte
 * (the grammar guarantees it), so len + 1024 is airtight for valid
 * documents; docs and anchors are sparser still. The arena copies
 * content the input already paid for (folds, tags, anchors) — generous
 * 128x covers pathological tag expansion while still bounding any
 * loop bug linearly in the input. */
static int cap_nodes(const yep_dom* d) {
    return d->input_len == 0 || (size_t)d->ncount <= d->input_len + 1024;
}
static int cap_arena(const yep_dom* d, uint32_t need) {
    return d->input_len == 0 || (size_t)(d->str_len + need) <= d->input_len * 128 + (1u << 20);
}

/* Doubling with the uint32 ceiling (the audit's wrap finding): the old
 * `while (cap < target) cap *= 2` wrapped to 0 and span forever once a
 * cap crossed 2^31 with a target above it. A cap that cannot double
 * inside uint32 — or a target past the 32-bit space at all — is an
 * alloc failure, never a wrapped capacity. */
static int dom_cap_next(uint32_t cap, size_t target, uint32_t* out) {
    if (target > UINT32_MAX) {
        return 0;
    }
    while (cap < target) {
        if (cap > UINT32_MAX / 2) {
            return 0;
        }
        cap *= 2;
    }
    *out = cap;
    return 1;
}

int dom_grow_nodes(yep_dom* d, uint32_t need) {
    if ((size_t)d->ncount + need <= d->ncap) {
        return 1;
    }
    if (!cap_nodes(d)) {
        return 0;
    }
    uint32_t ncap;
    if (!dom_cap_next(d->ncap ? d->ncap : 64, (size_t)d->ncount + need, &ncap)) {
        return 0;
    }
    yep_dnode* nd = yep_pool_alloc(d->pool, (size_t)ncap * sizeof(yep_dnode), 16);
    if (nd == NULL) {
        return 0;
    }
    if (d->ncount > 0) {
        memcpy(nd, d->nodes, (size_t)d->ncount * sizeof(yep_dnode));
    }
    d->nodes = nd;
    d->ncap = ncap;
    return 1;
}

int dom_grow_docs(yep_dom* d, uint32_t need) {
    if ((size_t)d->dcount + need <= d->dcap) {
        return 1;
    }
    if (d->input_len != 0 && (size_t)d->dcount > d->input_len / 2 + 16) {
        return 0;
    }
    uint32_t ncap;
    if (!dom_cap_next(d->dcap ? d->dcap : 16, (size_t)d->dcount + need, &ncap)) {
        return 0;
    }
    uint32_t* nd = yep_pool_alloc(d->pool, (size_t)ncap * sizeof(uint32_t), 16);
    if (nd == NULL) {
        return 0;
    }
    if (d->dcount > 0) {
        memcpy(nd, d->docs, (size_t)d->dcount * sizeof(uint32_t));
    }
    d->docs = nd;
    d->dcap = ncap;
    return 1;
}

/* Doubles the arena until cap bytes fit; 0 on OOM. Offsets stored in
 * nodes stay valid across the move. */
static int str_grow(yep_dom* d, uint32_t need) {
    if ((size_t)d->str_len + need <= d->str_cap) {
        return 1;
    }
    if (!cap_arena(d, need)) {
        return 0;
    }
    uint32_t cap;
    if (!dom_cap_next(d->str_cap ? d->str_cap : 256, (size_t)d->str_len + need, &cap)) {
        return 0;
    }
    char* ns = yep_alloc(d->sys, cap);
    if (ns == NULL) {
        return 0;
    }
    if (d->str_len > 0) {
        memcpy(ns, d->str, d->str_len);
    }
    yep_free(d->sys, d->str);
    d->str = ns;
    d->str_cap = cap;
    return 1;
}

yep_sview yep_dom_str_put(yep_dom* d, const char* p, uint32_t len) {
    yep_sview sv = {YEP_SV_INPUT, 0}; /* empty: arena offset 0 */
    if (len == 0) {
        return sv;
    }
    if (!str_grow(d, len)) {
        /* OOM: an empty view here would silently corrupt the document
         * (the value reads as ""). Record it; dom_place/dom_open_node
         * fail the parse with the memory status. */
        d->oom = 1;
        return sv;
    }
    memcpy(d->str + d->str_len, p, len);
    sv.off = YEP_SV_INPUT | d->str_len;
    sv.len = len;
    d->str_len += len;
    return sv;
}

char* yep_dom_str_tail(yep_dom* d, uint32_t cap) {
    if (!str_grow(d, cap)) {
        return NULL;
    }
    return d->str + d->str_len;
}

yep_sview yep_dom_str_commit(yep_dom* d, uint32_t written) {
    yep_sview sv = {YEP_SV_INPUT | d->str_len, written};
    d->str_len += written;
    return sv;
}

/* Node string: borrowed views stay INPUT offsets (zero copy);
 * everything the engine produced (folded, escaped, resolved tags,
 * anchor names) is copied into the arena — the finish pool is then
 * free to die with the engine. */
static yep_sview dom_str_in(yep_dom* d, const yep_view* v, int borrowed) {
    if (v == NULL || v->len == 0) {
        yep_sview sv = {0, 0};
        return sv;
    }
    /* The zero-copy law: borrow ONLY a view demonstrably inside the
     * recorded input window. The old heap-order probe (v->p >=
     * input_base) misfiled every engine-owned buffer that happened to
     * sit above the caller's input — the r171 normalizer copy path
     * (norm_buf) hands exactly such views to the pair/event builders —
     * and the resulting input-relative offset was then layout luck per
     * platform/build (the ubuntu-gcc / MSVC-arm / TSan / UBSan CI
     * divergences of PR #1042). The upper bound makes the decision a
     * pure function of the view's location class: both engines of a
     * differential run now store identically on every platform. */
    if (borrowed && v->p != NULL && d->input_base != NULL &&
        (size_t)((const char*)v->p - d->input_base) + v->len <= d->input_len) {
        return yep_sv_input(d, *v);
    }
    return yep_dom_str_put(d, (const char*)v->p, v->len);
}

/* The node-init law (every builder's SSOT): creates one node with the
 * given metadata; tag/anchor views encode through dom_str_in (tags and
 * event-path anchors copy to the arena; input-backed views borrow).
 * tag_id stays 0 — the caller resolves (scalars) or leaves 0. */
/* Value assign + the emit-side fact bits (issues #352 lanes 2-3):
 * no_exotic is "verified clean" of the dq-forced byte set (sc_route
 * skips its rescan); plain_ok is the full plain-safety verdict (the
 * emitter skips its walk — the block kernel's 35% frame). Both facts
 * ride the scan/'s SSOT predicates; every value assignment routes
 * through here so no path can leave a dirty scalar marked clean. */
static void dom_set_value(yep_dom* d, uint32_t id, const yep_view* v, int borrowed) {
    d->nodes[id].value = dom_str_in(d, v, borrowed);
    d->nodes[id].no_exotic = !yep_scan_dq_forced(v->p, v->len);
    d->nodes[id].plain_ok = (uint8_t)yep_scan_plain_safe(v->p, v->len);
}

uint32_t dom_open_node(yep_dom* d, uint8_t kind, const yep_view* tag, const yep_view* anchor,
                       int anchor_borrowed, uint8_t style, uint8_t implicit, uint8_t flow) {
    if (!dom_grow_nodes(d, 1)) {
        return UINT32_MAX;
    }
    yep_dnode* n = &d->nodes[d->ncount];
    /* template copy: one 48 B move replaces memset + four sentinel
     * patches (TODO.restructure/73) — the hot per-node init */
    static const yep_dnode k_init = {
        .first_child = UINT32_MAX,
        .last_child = UINT32_MAX,
        .next_sibling = UINT32_MAX,
        .target = UINT32_MAX,
    };
    *n = k_init;
    n->kind = kind;
    n->tag = dom_str_in(d, tag, 0);
    n->anchor = dom_str_in(d, anchor, anchor_borrowed);
    if (d->oom) {
        /* the tag/anchor arena write failed: refuse the node (the slot
         * is uncommitted — the next open re-initializes it) */
        return UINT32_MAX;
    }
    n->style = style;
    n->implicit = implicit;
    n->flow = flow;
    return d->ncount++;
}

uint32_t dom_new_node(yep_dom* d, const yep_event* ev, uint8_t kind) {
    if (ev == NULL) {
        return dom_open_node(d, kind, NULL, NULL, 0, 0, 0, 0);
    }
    /* event-path anchors ride the VALUE's borrowedness (historical
     * law, kept byte-identical) */
    uint32_t id = dom_open_node(d, kind, &ev->tag, &ev->anchor, ev->borrowed, ev->style,
                                ev->implicit, ev->flow);
    if (id == UINT32_MAX) {
        return UINT32_MAX;
    }
    d->nodes[id].tag_id = ev->tag_id;
    return id;
}

static int dom_anchor_set(yep_dom* d, uint32_t ordinal, uint32_t node) {
    if (ordinal == 0) {
        return -1;
    }
    if (d->input_len != 0 && ordinal > d->input_len + 16) {
        return -1; /* more anchors than input bytes: a bug, not a doc */
    }
    if (ordinal > d->anchor_nodes_cap) {
        /* First allocation sizes from the input (TODO.restructure/71):
         * growth from a bare 64 re-copied the table ~18 times on
         * anchor-heavy (one entry per '&x…' line). len/32 overshoots
         * only pathological one-anchor-per-2-bytes documents, which
         * then double as before. */
        uint32_t cap = d->anchor_nodes_cap ? d->anchor_nodes_cap * 2 : 64;
        if (d->anchor_nodes_cap == 0 && d->input_len > 2048) {
            uint32_t sized = (uint32_t)(d->input_len / 32) + 64;
            if (sized > cap) {
                cap = sized;
            }
        }
        if (!dom_cap_next(cap, ordinal, &cap)) {
            return -1; /* the wrap-checked doubling (audit round 5) */
        }
        uint32_t* na = yep_alloc(d->sys, cap * sizeof(*na));
        if (na == NULL) {
            return -1;
        }
        if (d->anchor_nodes != NULL) {
            memcpy(na, d->anchor_nodes, d->anchor_nodes_cap * sizeof(*na));
            yep_free(d->sys, d->anchor_nodes);
            /* ONLY the anchor table: the mut side tables belong to
             * dom_mut_grow alone (it allocates, copies and frees them
             * together with the cap that tracks their size). These two
             * frees were copy-paste debris from that path -- harmless
             * today only because the mutation tables cannot exist
             * during a parse -- but the ownership must not rely on
             * that accident (audit 2026-10-31). */
        }
        d->anchor_nodes = na;
        d->anchor_nodes_cap = cap;
    }
    d->anchor_nodes[ordinal - 1] = node;
    if (ordinal > d->anchor_max) {
        d->anchor_max = ordinal;
    }
    return 0;
}

static uint32_t dom_anchor_get(const yep_dom* d, uint32_t ordinal) {
    if (ordinal == 0 || ordinal > d->anchor_max) {
        return UINT32_MAX;
    }
    return d->anchor_nodes[ordinal - 1];
}

/* The one place links form (builder and mutation both): records
 * attachment and depth so mutation can reject double-parents and cap
 * nesting without parent pointers. */
void dom_link(yep_dom* d, uint32_t parent, uint32_t child) {
    yep_dnode* p = &d->nodes[parent];
    if (p->count == 0) {
        p->first_child = child;
    } else {
        d->nodes[p->last_child].next_sibling = child;
    }
    p->last_child = child;
    p->count++;
    /* every link changes SOME container's child list (#377's cache) */
    d->child_cache_id = UINT32_MAX;
    d->child_cache_len = 0;
}

/* ---- mutation side tables (64-2a) ---- */

static int dom_mut_grow(yep_dom* d, uint32_t need) {
    if (need <= d->mut_tbl_cap) {
        return 1;
    }
    uint32_t cap;
    if (!dom_cap_next(d->mut_tbl_cap ? d->mut_tbl_cap : 64, need, &cap)) {
        return 0; /* the wrap-checked doubling (audit round 5) */
    }
    uint8_t* na = yep_alloc(d->sys, (size_t)cap / 8 + 1);
    uint16_t* nd = yep_alloc(d->sys, (size_t)cap * sizeof(uint16_t));
    if (na == NULL || nd == NULL) {
        yep_free(d->sys, na);
        yep_free(d->sys, nd);
        return 0;
    }
    size_t old_bytes = ((size_t)d->mut_tbl_cap + 7) / 8;
    if (d->mut_att != NULL) {
        memcpy(na, d->mut_att, old_bytes);
        memcpy(nd, d->mut_depth, (size_t)d->mut_tbl_cap * sizeof(uint16_t));
    }
    /* the grown region is FRESH facts: unattached, depth 0 (a CI
     * failure traced to garbage bits here — local mallocs were zero) */
    memset(na + old_bytes, 0, (size_t)cap / 8 + 1 - old_bytes);
    memset(nd + d->mut_tbl_cap, 0, (size_t)(cap - d->mut_tbl_cap) * sizeof(uint16_t));
    yep_free(d->sys, d->mut_att);
    yep_free(d->sys, d->mut_depth);
    d->mut_att = na;
    d->mut_depth = nd;
    d->mut_tbl_cap = cap;
    return 1;
}

int dom_mut_att(const yep_dom* d, uint32_t id) {
    yep_dom* m = (yep_dom*)d; /* lazy growth is benign bookkeeping */
    if (id >= m->mut_tbl_cap && !dom_mut_grow(m, id + 1)) {
        return 0;
    }
    return (m->mut_att[id >> 3] >> (id & 7)) & 1;
}

void dom_mut_set_att(yep_dom* d, uint32_t id, int v) {
    if (id >= d->mut_tbl_cap && !dom_mut_grow(d, id + 1)) {
        return;
    }
    uint8_t bit = (uint8_t)(1u << (id & 7));
    if (v) {
        d->mut_att[id >> 3] |= bit;
    } else {
        d->mut_att[id >> 3] &= (uint8_t)~bit;
    }
}

uint16_t dom_mut_depth(const yep_dom* d, uint32_t id) {
    yep_dom* m = (yep_dom*)d;
    if (id >= m->mut_tbl_cap && !dom_mut_grow(m, id + 1)) {
        return 0;
    }
    return m->mut_depth[id];
}

void dom_mut_set_depth(yep_dom* d, uint32_t id, uint16_t depth) {
    if (id >= d->mut_tbl_cap && !dom_mut_grow(d, id + 1)) {
        return;
    }
    d->mut_depth[id] = depth;
}

/* Places a completed node: value for a pending key, child of the top
 * collection, or document root. Every builder path ends here, so it is
 * also the OOM choke: an arena write that failed (d->oom) must fail
 * the node, never leave a silently-empty value in the tree. */
int dom_place(yep_dom* d, uint32_t id) {
    if (d->oom) {
        return -1;
    }
    if (d->depth == 0) {
        if (!dom_grow_docs(d, 1)) {
            return -1;
        }
        d->docs[d->dcount++] = id;
        return 0;
    }
    uint32_t top = d->stack[d->depth - 1];
    yep_dnode* p = &d->nodes[top];
    if (p->kind == YEP_DOM_MAPPING) {
        /* per-frame pairing: a map nested as a KEY consumes this frame's
         * slot without disturbing the parent's (global state corrupted
         * complex keys) */
        if (!d->map_pending_key[d->depth - 1]) {
            d->pending_key_id[d->depth - 1] = id;
            d->map_pending_key[d->depth - 1] = 1;
            return 0;
        }
        d->map_pending_key[d->depth - 1] = 0;
        dom_link(d, top, d->pending_key_id[d->depth - 1]); /* key */
        dom_link(d, top, id);                              /* value */
        return 0;
    }
    dom_link(d, top, id);
    return 0;
}

int yep_dom_on_event(void* ctx, const yep_event* ev) {
    yep_dom* d = (yep_dom*)ctx;
    switch (ev->type) {
    case YEP_EV_STREAM_START:
    case YEP_EV_DOCUMENT_START:
    case YEP_EV_DOCUMENT_END:
        /* bindings are document-scoped, like the engine's names */
        if (d->anchor_max != 0) {
            memset(d->anchor_nodes, 0, d->anchor_max * sizeof(*d->anchor_nodes));
            d->anchor_max = 0;
        }
        return 0;
    case YEP_EV_STREAM_END:
        /* no depth walk: the parse links TOP-DOWN (every child is
         * linked the moment it starts, under a parent whose depth is
         * final), so dom_link's O(1) assignment already globalizes
         * them. The walk remains the MUTATION builder's job — it
         * attaches pre-built subtrees whose descendants keep local
         * depths (yep_mut_set_depths after attach). */
        return 0;

    case YEP_EV_SEQ_START:
    case YEP_EV_MAP_START: {
        if (d->depth >= YEP_DOM_MAX_DEPTH) {
            return -1;
        }
        uint8_t kind = (ev->type == YEP_EV_SEQ_START) ? YEP_DOM_SEQUENCE : YEP_DOM_MAPPING;
        uint32_t id = dom_new_node(d, ev, kind);
        if (id == UINT32_MAX) {
            return -1;
        }
        if (ev->anchor_id != 0 && dom_anchor_set(d, ev->anchor_id, id) != 0) {
            return -1;
        }
        if (dom_place(d, id) != 0) {
            return -1;
        }
        d->map_pending_key[d->depth] = 0;
        d->stack[d->depth++] = id;
        return 0;
    }

    case YEP_EV_SEQ_END:
    case YEP_EV_MAP_END:
        if (d->depth == 0) {
            return -1;
        }
        if (ev->type == YEP_EV_MAP_END && d->map_pending_key[d->depth - 1]) {
            /* an odd child reached the mapping: the unpaired node —
             * possibly an ANCHORED one — used to be dropped here, its
             * anchor lost, and a later alias then emitted a dangling
             * reference the re-parse rejected (the nightly's round
             * 102). Pair it with a null: "k:" with nothing is a null
             * value in every other position. */
            yep_event nv;
            memset(&nv, 0, sizeof(nv));
            nv.type = YEP_EV_SCALAR;
            nv.style = YEP_STYLE_PLAIN;
            nv.implicit = 1;
            uint32_t nid = dom_new_node(d, &nv, YEP_DOM_SCALAR);
            if (nid == UINT32_MAX) {
                return -1;
            }
            uint32_t top = d->stack[d->depth - 1];
            dom_link(d, top, d->pending_key_id[d->depth - 1]);
            dom_link(d, top, nid);
            d->map_pending_key[d->depth - 1] = 0;
        }
        d->depth--;
        return 0;

    case YEP_EV_SCALAR: {
        uint32_t id = dom_new_node(d, ev, YEP_DOM_SCALAR);
        if (id == UINT32_MAX) {
            return -1;
        }
        dom_set_value(d, id, &ev->value, ev->borrowed);
        if (ev->anchor_id != 0 && dom_anchor_set(d, ev->anchor_id, id) != 0) {
            return -1;
        }
        return dom_place(d, id);
    }

    case YEP_EV_ALIAS: {
        uint32_t target = dom_anchor_get(d, ev->anchor_id);
        if (target == UINT32_MAX) {
            return -1;
        }
        uint32_t id = dom_new_node(d, ev, YEP_DOM_ALIAS);
        if (id == UINT32_MAX) {
            return -1;
        }
        dom_set_value(d, id, &ev->value, ev->borrowed);
        d->nodes[id].target = target;
        return dom_place(d, id);
    }

    default:
        return -1;
    }
}

void yep_dom_reserve(yep_dom* d, uint32_t node_hint, uint32_t str_hint) {
    if (d == NULL) {
        return;
    }
    if (node_hint > d->ncount) {
        /* capacity for node_hint TOTAL nodes; growth remains on OOM */
        (void)dom_grow_nodes(d, node_hint - d->ncount);
    }
    if (str_hint > d->str_cap) {
        (void)str_grow(d, str_hint);
    }
}

/* Content-derived sizing (TODO.impl/06): three SIMD count passes,
 * then reserves. Every structural byte bounds a node boundary; the
 * arena reserve only pays when the content copies (quoted scalars
 * escape, block scalars fold) — borrowed-only documents must not
 * allocate an arena at all. Hints are advisory. */
void yep_dom_prepare(yep_dom* d, const yep_text_stats* st) {
    if (d == NULL || st == NULL) {
        return;
    }
    /* a block line carries at least TWO nodes (a key and its value,
     * or a dash and its entry): nl-colons cancels to ~zero on plain
     * scalar maps, so the old hint undershot by 200k nodes on
     * anchor-heavy (the growth memmove profiled at 5% of parse) */
    size_t structural = st->comma + st->dash + st->bracket + st->brace + 8;
    size_t by_line = st->nl * 2 + 8;
    uint32_t node_hint = (uint32_t)(structural > by_line ? structural : by_line);
    uint32_t str_hint = (st->dq + st->sq + st->pipe) > 0
                            ? (uint32_t)(st->dq * 16 + st->sq * 8 + st->pipe * 128 + 64)
                            : 0;
    yep_dom_reserve(d, node_hint, str_hint);
}

void yep_dom_prepare_len(yep_dom* d, size_t len) {
    if (d == NULL) {
        return;
    }
    yep_dom_reserve(d, (uint32_t)(len / 2 + 64), (uint32_t)(len / 16 + 256));
}

yep_dom* yep_dom_create(const yep_allocator* sys) {
    if (sys == NULL) {
        return NULL;
    }
    yep_pool* pool = yep_pool_create(sys, 8192);
    if (pool == NULL) {
        return NULL;
    }
    yep_dom* d = yep_alloc(sys, sizeof(yep_dom));
    if (d == NULL) {
        yep_pool_destroy(pool);
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    d->sys = sys;
    d->pool = pool;
    /* #157: the handle pool is lazy — two allocations and a mutex per
     * document that decode-only consumers (CBOR load, serialize, free)
     * never touch; yep_dom_handles() materializes it on first use */
    d->handles = NULL;
    d->child_cache = NULL;
    d->child_cache_id = UINT32_MAX;
    d->child_cache_len = 0;
    if (yep_mutex_init(&d->midx.mu) != 0) {
        yep_pool_destroy(pool);
        yep_free(sys, d);
        return NULL;
    }
    d->midx.mu_ready = 1;
    return d;
}

/* Lazy handle pool, mirroring the map-index's discipline (mapindex.c):
 * EVERY access rides the dom's lazy-init mutex — read-only sharing
 * means multiple threads may race the FIRST handle creation on a
 * shared document (Threads.ReadOnlySharing; the TSAN catch on the
 * first cut). The uncontended lock costs less than the hpool's own
 * per-allocation lock that follows. */
struct yep_hpool* yep_dom_handles(yep_dom* d) {
    if (d == NULL || !d->midx.mu_ready) {
        return NULL;
    }
    yep_mutex_lock(&d->midx.mu);
    if (d->handles == NULL) {
        d->handles = yep_hpool_create(d->sys);
    }
    struct yep_hpool* h = d->handles;
    yep_mutex_unlock(&d->midx.mu);
    return h;
}

/* #377 (ruby #168): seq_at/map_at were O(i) sibling walks — the
 * bindings' per-element loops made an 80k-row sequence quadratic
 * (the relaton index: 238-328s where stdlib takes ~3s). The bulk
 * drain (yeptris_node_children) fixed the bindings; THIS fixes the
 * accessor: the last indexed container's child ids cache under the
 * lazy-init mutex, O(1) after the first call. Invalidated by every
 * child-list mutation (dom_invalidate_child_cache). */
void dom_invalidate_child_cache(yep_dom* d) {
    if (d == NULL) {
        return;
    }
    d->child_cache_id = UINT32_MAX;
    d->child_cache_len = 0;
}

/* Returns child id at index (UINT32_MAX when out of range); the
 * container's child count rides *count_out. */
uint32_t dom_indexed_child(yep_dom* d, uint32_t cid, size_t index, uint32_t* count_out) {
    const yep_dnode* n = yep_dom_node(d, cid);
    if (n == NULL) {
        *count_out = 0;
        return UINT32_MAX;
    }
    /* the midx discipline (TSAN, Threads.ReadOnlySharing): READS of
     * the cache ride the mutex too — a concurrent builder publishes
     * under it, so the old unlocked hit path raced the build's
     * stores. Uncontended in the single-threaded hot path. */
    yep_mutex_lock(&d->midx.mu);
    uint32_t out;
    *count_out = n->count;
    if (d->child_cache_id == cid) {
        out = index < d->child_cache_len ? d->child_cache[index] : UINT32_MAX;
    } else {
        /* miss: build once under the lazy-init mutex (concurrent
         * first calls race here exactly like the handle pool's) */
        yep_free(d->sys, d->child_cache);
        d->child_cache = NULL;
        d->child_cache_len = 0;
        if (n->count > 0) {
            uint32_t* arr = yep_alloc(d->sys, n->count * sizeof(uint32_t));
            if (arr != NULL) {
                uint32_t k = 0;
                for (uint32_t id = n->first_child; id != UINT32_MAX && k < n->count;) {
                    const yep_dnode* cn = yep_dom_node(d, id);
                    if (cn == NULL) {
                        break;
                    }
                    arr[k++] = id;
                    id = cn->next_sibling;
                }
                d->child_cache = arr;
                d->child_cache_len = k;
            }
        }
        d->child_cache_id = cid;
        out = index < d->child_cache_len ? d->child_cache[index] : UINT32_MAX;
    }
    yep_mutex_unlock(&d->midx.mu);
    return out;
}

void yep_dom_destroy(yep_dom* d) {
    if (d == NULL) {
        return;
    }
    yep_free(d->sys, d->str);
    yep_midx_destroy(d);
    yep_free(d->sys, d->anchor_nodes);
    yep_free(d->sys, d->mut_att);
    yep_free(d->sys, d->mut_depth);
    yep_hpool_destroy(d->handles);
    yep_free(d->sys, d->child_cache);
    yep_pool_destroy(d->pool);
    yep_free(d->sys, d);
}

/* ------------------------------------------------- direct builders */
/* Two direct builders, one set of laws: yep_dom_build_json walks a
 * whole strictly-validated JSON buffer (TODO.impl/27, the parse_json
 * seam); dom_on_flow_json walks an engine-validated span mid-document
 * (TODO.restructure/50, the sink fast path). Both create nodes through
 * dom_open_node and place them through dom_place on the DOM's own
 * stack — the event sink's pairing/link/anchor laws, shared, so the
 * trees cannot diverge (the flow-direct-diff ctest pins it). */

#include "parse/scalars.h"
#include "resolve/resolver.h"
#include "scan/json.h"
#include "scan/scan.h"

const yep_resolver* yep_dom_resolver(const yep_dom* d) {
    return d->resolver != NULL ? d->resolver : yep_resolver_core12();
}

typedef struct {
    yep_dom* d;
    const char* p;
    size_t len;
    size_t i;
} jbuilder;

/* Whole-buffer JSON: value-first recursion (the grammar is validated,
 * the walk only advances). */
static int jb_value(jbuilder* b);

static int jb_string_node(jbuilder* b) {
    uint32_t id = dom_open_node(b->d, YEP_DOM_SCALAR, NULL, NULL, 0, 0, 0, 0);
    if (id == UINT32_MAX) {
        return -1;
    }
    yep_dnode* n = &b->d->nodes[id];
    size_t close;
    int has_esc = 0;
    size_t start = b->i;
    if (!yep_json_string(b->p, b->len, &b->i, &close, &has_esc)) {
        return -2;
    }
    if (has_esc) {
        uint32_t span = (uint32_t)(close - start - 1);
        char* dst = yep_dom_str_tail(b->d, span);
        if (dst == NULL) {
            return -1;
        }
        n->value = yep_dom_str_commit(
            b->d, yep_finish_double_into(b->p, (uint32_t)(start + 1), (uint32_t)close, dst, span));
    } else {
        yep_view v = {b->p + start + 1, (uint32_t)(close - start - 1)};
        n->value = dom_str_in(b->d, &v, 1);
    }
    n->style = YEP_STYLE_DOUBLE_QUOTED;
    n->tag_id = 0; /* str */
    return dom_place(b->d, id) == 0 ? 0 : -1;
}

static int jb_scalar_node(jbuilder* b) {
    uint32_t id = dom_open_node(b->d, YEP_DOM_SCALAR, NULL, NULL, 0, 0, 0, 0);
    if (id == UINT32_MAX) {
        return -1;
    }
    yep_dnode* n = &b->d->nodes[id];
    size_t start = b->i;
    size_t i = b->i;
    const yep_resolver* r = yep_dom_resolver(b->d);
    if (b->p[i] == 't') {
        if (!yep_json_literal(b->p, b->len, &i, "true")) {
            return -2;
        }
    } else if (b->p[i] == 'f') {
        if (!yep_json_literal(b->p, b->len, &i, "false")) {
            return -2;
        }
    } else if (b->p[i] == 'n') {
        if (!yep_json_literal(b->p, b->len, &i, "null")) {
            return -2;
        }
    } else {
        if (!yep_json_number(b->p, b->len, &i)) {
            return -2;
        }
    }
    b->i = i;
    yep_view v = {b->p + start, (uint32_t)(i - start)};
    n->value = dom_str_in(b->d, &v, 1);
    n->style = YEP_STYLE_PLAIN;
    n->implicit = 1;
    n->tag_id = r->resolve(NULL, v.p, v.len);
    return dom_place(b->d, id) == 0 ? 0 : -1;
}

static void jb_ws(jbuilder* b) {
    while (b->i < b->len) {
        char c = b->p[b->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            b->i++;
        } else {
            break;
        }
    }
}

static int jb_value(jbuilder* b) {
    yep_dom* d = b->d;
    jb_ws(b);
    if (b->i >= b->len) {
        return -2;
    }
    char c = b->p[b->i];
    if (c == '{' || c == '[') {
        if (d->depth >= YEP_DOM_MAX_DEPTH) {
            return -2;
        }
        int map = (c == '{');
        b->i++;
        uint32_t id =
            dom_open_node(d, map ? YEP_DOM_MAPPING : YEP_DOM_SEQUENCE, NULL, NULL, 0, 0, 0, 1);
        if (id == UINT32_MAX) {
            return -1;
        }
        if (dom_place(d, id) != 0) {
            return -1;
        }
        d->map_pending_key[d->depth] = 0;
        d->stack[d->depth++] = id;
        jb_ws(b);
        if (b->i < b->len && b->p[b->i] == (map ? '}' : ']')) {
            b->i++;
            d->depth--;
            return 0;
        }
        for (;;) {
            size_t member_start = b->i;
            int rc = jb_value(b);
            if (rc != 0) {
                return rc;
            }
            jb_ws(b);
            if (map) {
                /* a value must be followed by , or } — the map's key
                 * slot is pending until the ':' value completes */
                if (b->i < b->len && b->p[b->i] == ':' && d->map_pending_key[d->depth - 1] != 0) {
                    /* libyaml's simple-key law rides the JSON-class
                     * route too: a key whose colon distance crosses
                     * 1024 characters defers to the engine's clean
                     * error (run 38143486175) */
                    if (b->i - member_start > 1024 &&
                        yep_simple_key_chars(b->p + member_start, b->i - member_start) > 1024) {
                        return -2;
                    }
                    b->i++;
                    rc = jb_value(b);
                    if (rc != 0) {
                        return rc;
                    }
                    jb_ws(b);
                }
            }
            if (b->i >= b->len) {
                return -2;
            }
            if (b->p[b->i] == ',') {
                b->i++;
                continue;
            }
            if (b->p[b->i] == (map ? '}' : ']')) {
                b->i++;
                d->depth--;
                return 0;
            }
            return -2;
        }
    }
    if (c == '"') {
        return jb_string_node(b);
    }
    return jb_scalar_node(b);
}

int yep_dom_build_json(yep_dom* d, const char* buf, size_t len) {
    jbuilder b = {d, buf, len, 0};
    int rc = jb_value(&b);
    if (rc != 0) {
        return rc;
    }
    jb_ws(&b);
    if (b.i != len) {
        return -2;
    }
    return 0;
}

/* --- the flow fast path (TODO.restructure/50) --- */

/* --- the fused flow build (TODO.restructure/53) --- */

/* One walk validates AND builds: the shared grammar walker drives,
 * nodes are created through the DOM's own laws (dom_open_node,
 * dom_place on the live stack above the entry depth). Staging: the
 * root's placement and anchor binding defer to commit; scratch
 * nodes live past the caller's ncount until then — ANY deviation
 * restores both counters with zero live-state mutation, so the
 * engine's fallback paths are exactly as before. */
static void dom_flow_stage_reset(yep_dom* d) {
    d->ncount = d->flow_stage_base;
    d->str_len = d->flow_stage_str;
    d->flow_staged = 0;
}

int dom_on_flow_build(void* ctx, const char* p, size_t open, size_t len, uint32_t line,
                      size_t line_start, yep_view anchor, yep_view tag, uint32_t anchor_id,
                      int max_depth, size_t* close) {
    yep_dom* d = (yep_dom*)ctx;
    if (d->depth >= YEP_DOM_MAX_DEPTH) {
        return 0; /* the event path owns the depth-limit error shape */
    }
    int entry_depth = d->depth;
    d->flow_stage_base = d->ncount;
    d->flow_stage_str = d->str_len;

    uint32_t root = dom_open_node(d, p[open] == '[' ? YEP_DOM_SEQUENCE : YEP_DOM_MAPPING, &tag,
                                  &anchor, 0, 0, 0, 1);
    if (root == UINT32_MAX) {
        goto fail;
    }
    d->map_pending_key[d->depth] = 0;
    d->stack[d->depth++] = root;

    const yep_resolver* r = yep_dom_resolver(d);
    yep_json_walk w;
    yep_json_tok t;
    yep_json_walk_init(&w, p, len, open, max_depth);
    w.strict = 1; /* the same byte-for-byte JSON gate as the engine's
                   * pass 1: a lenient walk split number/literal keys
                   * across the fast/slow paths (whole {9->1} vs chunked
                   * {"9:1"->null}); flow_strict keeps owning the TAG
                   * route only (TODO.restructure/81) */
    for (;;) {
        yep_jw_status st = yep_json_walk_next(&w, &t);
        if (st == YEP_JW_REJECT) {
            d->depth = entry_depth;
            dom_flow_stage_reset(d);
            return 0; /* not JSON-class: the general kernel */
        }
        if (st == YEP_JW_DONE) {
            break;
        }
        switch (t.cls) {
        case '[':
        case '{': {
            if (d->depth >= YEP_DOM_MAX_DEPTH) {
                goto fail;
            }
            uint32_t cid = dom_open_node(d, t.cls == '[' ? YEP_DOM_SEQUENCE : YEP_DOM_MAPPING, NULL,
                                         NULL, 0, 0, 0, 1);
            if (cid == UINT32_MAX) {
                goto fail;
            }
            if (dom_place(d, cid) != 0) {
                goto fail;
            }
            d->map_pending_key[d->depth] = 0;
            d->stack[d->depth++] = cid;
            break;
        }
        case ']':
        case '}':
            d->depth--;
            break;
        case '"': {
            uint32_t sid =
                dom_open_node(d, YEP_DOM_SCALAR, NULL, NULL, 0, YEP_STYLE_DOUBLE_QUOTED, 0, 0);
            if (sid == UINT32_MAX) {
                goto fail;
            }
            if (t.has_escape) {
                uint32_t span = (uint32_t)(t.end - t.at - 2);
                char* dst = yep_dom_str_tail(d, span);
                if (dst == NULL) {
                    goto fail;
                }
                d->nodes[sid].value =
                    yep_dom_str_commit(d, yep_finish_double_into(p, (uint32_t)(t.at + 1),
                                                                 (uint32_t)(t.end - 1), dst, span));
                d->nodes[sid].no_exotic = 1; /* JSON-class strings: the
                    walker rejected every control byte */
            } else {
                yep_view v = {p + t.at + 1, (uint32_t)(t.end - t.at - 2)};
                dom_set_value(d, sid, &v, 1);
            }
            if (dom_place(d, sid) != 0) {
                goto fail;
            }
            break;
        }
        default: { /* number or literal: the walker validated the span */
            uint32_t sid = dom_open_node(d, YEP_DOM_SCALAR, NULL, NULL, 0, YEP_STYLE_PLAIN, 1, 0);
            if (sid == UINT32_MAX) {
                goto fail;
            }
            yep_view v = {p + t.at, (uint32_t)(t.end - t.at)};
            d->nodes[sid].value = dom_str_in(d, &v, 1);
            d->nodes[sid].no_exotic = 1; /* the walker validated the
                span as a JSON number or literal */
            if (t.cls == '#') {
                d->nodes[sid].tag_id = yep_resolve_number(r, t.is_float);
            } else if (d->flow_strict) {
                /* strict JSON: the walker validated the span is exactly
                 * one of the three RFC 8259 words — no resolver dispatch */
                d->nodes[sid].tag_id = (v.p[0] == 'n') ? YEPTRIS_TAG_NULL : YEPTRIS_TAG_BOOL;
            } else {
                d->nodes[sid].tag_id = r->resolve(NULL, v.p, v.len);
            }
            if (dom_place(d, sid) != 0) {
                goto fail;
            }
            break;
        }
        }
    }
    d->depth = entry_depth;
    if (w.long_key) {
        dom_flow_stage_reset(d);
        return 2; /* grammar-valid; pass 2 owns the simple-key error */
    }
    d->flow_staged = 1;
    d->flow_stage_root = root;
    d->flow_stage_anchor = anchor_id;
    *close = w.close;
    return 1;
fail:
    d->depth = entry_depth;
    dom_flow_stage_reset(d);
    return -1;
}

int dom_on_flow_commit(void* ctx) {
    yep_dom* d = (yep_dom*)ctx;
    if (!d->flow_staged) {
        return 0;
    }
    d->flow_staged = 0;
    if (d->flow_stage_anchor != 0 &&
        dom_anchor_set(d, d->flow_stage_anchor, d->flow_stage_root) != 0) {
        return -1;
    }
    if (dom_place(d, d->flow_stage_root) != 0) {
        return -1;
    }
    return 1;
}

int dom_from_tape(yep_dom* d, const yeptris_json_tape* t) {
    if (d == NULL || t == NULL || t->_src == NULL) {
        return -1;
    }
    /* the columns are the one representation valid on every route
     * (strict: native; lenient fused: lazily materialized from the
     * records; scalar-root lenient: native column writes) */
    if (yeptris_tape_columns((yeptris_json_tape*)t) != 0) {
        return -1;
    }
    if (t->kinds == NULL) {
        return -1;
    }
    d->input_base = (const char*)t->_src;
    d->input_len = t->_srclen;
    const char* src = (const char*)t->_src;
    d->depth = 0;
    for (uint32_t i = 0; i < t->count; i++) {
        const uint8_t kind = t->kinds[i];
        const uint32_t len = t->lens[i];
        const uint32_t off = t->offs[i];
        switch (kind) {
        case YEP_T_DOC:
            break; /* the stream boundary record */
        case YEP_T_SEQ_OPEN:
        case YEP_T_MAP_OPEN: {
            if (d->depth >= YEP_DOM_MAX_DEPTH) {
                return -1;
            }
            uint32_t id =
                dom_open_node(d, kind == YEP_T_SEQ_OPEN ? YEP_DOM_SEQUENCE : YEP_DOM_MAPPING, NULL,
                              NULL, 0, 0, 0, 1);
            if (id == UINT32_MAX || dom_place(d, id) != 0) {
                return -1;
            }
            d->map_pending_key[d->depth] = 0;
            d->stack[d->depth++] = id;
            break;
        }
        case YEP_T_CLOSE:
            if (d->depth == 0) {
                return -1;
            }
            if (d->nodes[d->stack[d->depth - 1]].kind == YEP_DOM_MAPPING &&
                d->map_pending_key[d->depth - 1]) {
                /* an odd child reached the mapping (see the event-path
                 * MAP_END): pair it with a null instead of dropping it */
                yep_event nv;
                memset(&nv, 0, sizeof(nv));
                nv.type = YEP_EV_SCALAR;
                nv.style = YEP_STYLE_PLAIN;
                nv.implicit = 1;
                uint32_t nid = dom_new_node(d, &nv, YEP_DOM_SCALAR);
                if (nid == UINT32_MAX) {
                    return -1;
                }
                dom_link(d, d->stack[d->depth - 1], d->pending_key_id[d->depth - 1]);
                dom_link(d, d->stack[d->depth - 1], nid);
                d->map_pending_key[d->depth - 1] = 0;
            }
            d->depth--;
            break;
        case YEP_T_NULL:
        case YEP_T_TRUE:
        case YEP_T_FALSE:
        case YEP_T_INT:
        case YEP_T_FLOAT:
        case YEP_T_NUM:
        case YEP_T_STR: {
            yep_tag_id tag = YEPTRIS_TAG_STR;
            uint8_t style = YEP_STYLE_PLAIN;
            int implicit = 0;
            switch (kind) {
            case YEP_T_NULL:
                tag = YEPTRIS_TAG_NULL;
                implicit = 1;
                break;
            case YEP_T_TRUE:
            case YEP_T_FALSE:
                tag = YEPTRIS_TAG_BOOL;
                implicit = 1;
                break;
            case YEP_T_INT:
            case YEP_T_FLOAT:
            case YEP_T_NUM:
                tag = YEPTRIS_TAG_INT;
                implicit = 1;
                if (kind == YEP_T_NUM) {
                    /* the lenient route's deferred contract: the span
                     * carried NO number grammar at parse — classify
                     * now (the tape convert's own authority) */
                    size_t adv = 0;
                    int flt = 0;
                    if (yep_json_number_shape(src + off, len, &adv, &flt) == 0 || adv != len) {
                        return -1; /* malformed: the strict route rejected at parse */
                    }
                    tag = flt ? YEPTRIS_TAG_FLOAT : YEPTRIS_TAG_INT;
                } else if (kind == YEP_T_FLOAT) {
                    tag = YEPTRIS_TAG_FLOAT;
                }
                break;
            default:
                style = YEP_STYLE_DOUBLE_QUOTED;
                break;
            }
            uint32_t id =
                dom_open_node(d, YEP_DOM_SCALAR, NULL, NULL, 0, style, (uint8_t)implicit, 0);
            if (id == UINT32_MAX) {
                return -1;
            }
            if (kind == YEP_T_STR) {
                /* the record's span is the INNER bytes (quotes off) */
                if (memchr(src + off, '\\', len) != NULL) {
                    /* escaped: unescape into the arena (the fused
                     * builder's arm, byte for byte) */
                    char* dst = yep_dom_str_tail(d, len);
                    if (dst == NULL) {
                        return -1;
                    }
                    d->nodes[id].value = yep_dom_str_commit(
                        d, yep_finish_double_into(src, off, off + len, dst, len));
                    d->nodes[id].no_exotic = 1; /* JSON-class: escapes
                        only, the walker's law */
                } else {
                    yep_view v = {src + off, len};
                    dom_set_value(d, id, &v, 1);
                }
            } else {
                yep_view v = {src + off, len};
                dom_set_value(d, id, &v, 1);
            }
            d->nodes[id].tag_id = tag;
            if (dom_place(d, id) != 0) {
                return -1;
            }
            break;
        }
        default:
            return -1; /* a CONT run or an unknown kind: neither reaches a
                          strict-legal tape (CONT only past 16 MiB tokens) */
        }
    }
    return d->depth == 0 ? 0 : -1;
}

void dom_on_flow_rollback(void* ctx) {
    yep_dom* d = (yep_dom*)ctx;
    if (d->flow_staged) {
        dom_flow_stage_reset(d);
    }
}

/* The sink's block-open fast path (TODO.restructure/57): a `key:` line
 * whose value follows becomes the map node plus its pending key in one
 * call — the DOM stack mirrors what the MAP_START event would have
 * pushed. 0 only for the depth cap (the engine then takes the events,
 * which own the error). */
int dom_on_block_open(void* ctx, const yep_view* key, uint32_t line, uint32_t key_col) {
    yep_dom* d = (yep_dom*)ctx;
    if (d->depth >= YEP_DOM_MAX_DEPTH) {
        return 0;
    }
    const yep_resolver* r = yep_dom_resolver(d);
    uint32_t mid = dom_open_node(d, YEP_DOM_MAPPING, NULL, NULL, 0, 0, 0, 0);
    if (mid == UINT32_MAX) {
        return -1;
    }
    if (dom_place(d, mid) != 0) {
        return -1;
    }
    d->map_pending_key[d->depth] = 0;
    d->stack[d->depth++] = mid;
    uint32_t kid = dom_open_node(d, YEP_DOM_SCALAR, NULL, NULL, 0, YEP_STYLE_PLAIN, 1, 0);
    if (kid == UINT32_MAX) {
        d->depth--;
        return -1;
    }
    dom_set_value(d, kid, key, 1);
    d->nodes[kid].tag_id = r->resolve(NULL, key->p, key->len);
    return dom_place(d, kid) == 0 ? 1 : -1;
}

/* The sink's dash-item fast path (TODO.restructure/78): one node
 * through the node-init law, placed into the sequence the engine
 * opened before offering — node-for-node the event path's item. */
int dom_on_block_item(void* ctx, const yep_block_value* v) {
    yep_dom* d = (yep_dom*)ctx;
    const yep_resolver* r = yep_dom_resolver(d);
    uint32_t vid = dom_open_node(d, YEP_DOM_SCALAR, NULL, NULL, 0, YEP_STYLE_PLAIN, 1, 0);
    if (vid == UINT32_MAX) {
        return -1;
    }
    dom_set_value(d, vid, &v->value, v->borrowed);
    d->nodes[vid].tag_id = r->resolve(NULL, v->value.p, v->value.len);
    return dom_place(d, vid) == 0 ? 1 : -1;
}

/* The sink's block fast path (TODO.restructure/54): one classified
 * `key: value` line becomes two nodes — the key scalar resolved like
 * every implicit key event, the value through its class — placed with
 * the shared pairing law. The engine offers the line BEFORE emitting;
 * returning 0 here (only for an unbound alias target, which cannot
 * happen when the engine resolved the name) falls back to the two
 * events with nothing built. */
int dom_on_block_pair(void* ctx, const yep_view* key, const yep_block_value* v, uint32_t line,
                      uint32_t key_col, uint32_t val_col) {
    yep_dom* d = (yep_dom*)ctx;
    uint32_t target = 0;
    if (v->cls == YEP_LVAL_ALIAS) {
        target = dom_anchor_get(d, v->anchor_id);
        if (target == UINT32_MAX) {
            return 0;
        }
    }
    const yep_resolver* r = yep_dom_resolver(d);
    uint32_t kid = dom_open_node(d, YEP_DOM_SCALAR, NULL, NULL, 0, YEP_STYLE_PLAIN, 1, 0);
    if (kid == UINT32_MAX) {
        return -1;
    }
    dom_set_value(d, kid, key, 1);
    d->nodes[kid].tag_id = r->resolve(NULL, key->p, key->len);
    if (dom_place(d, kid) != 0) {
        return -1;
    }
    if (v->cls == YEP_LVAL_ALIAS) {
        uint32_t vid = dom_open_node(d, YEP_DOM_ALIAS, NULL, NULL, 0, 0, 0, 0);
        if (vid == UINT32_MAX) {
            return -1;
        }
        dom_set_value(d, vid, &v->value, 1); /* borrowed — the
            event path stamps it too now (73); the gates compare decoded
            views, not arena placement */
        d->nodes[vid].target = target;
        return dom_place(d, vid) == 0 ? 1 : -1;
    }
    int anchored = (v->cls == YEP_LVAL_ANCHOR_PLAIN);
    uint32_t vid = dom_open_node(d, YEP_DOM_SCALAR, NULL, anchored ? &v->anchor : NULL, v->borrowed,
                                 YEP_STYLE_PLAIN, 1, 0);
    if (vid == UINT32_MAX) {
        return -1;
    }
    dom_set_value(d, vid, &v->value, v->borrowed);
    d->nodes[vid].tag_id = r->resolve(NULL, v->value.p, v->value.len);
    if (anchored && v->anchor_id != 0 && dom_anchor_set(d, v->anchor_id, vid) != 0) {
        return -1;
    }
    return dom_place(d, vid) == 0 ? 1 : -1;
}

/* The direct scalar path (TODO.restructure/79): the YEP_EV_SCALAR
 * case's node build, minus the event seam. The engine resolved
 * tag_id (the typing SSOT); everything else is the case body. */
int dom_on_scalar(void* ctx, const yep_view* value, const yep_view* tag, const yep_view* anchor,
                  uint32_t anchor_id, uint8_t tag_id, uint8_t style, uint8_t implicit, uint8_t flow,
                  int borrowed) {
    yep_dom* d = (yep_dom*)ctx;
    uint32_t id = dom_open_node(d, YEP_DOM_SCALAR, tag, anchor, borrowed, style, implicit, flow);
    if (id == UINT32_MAX) {
        return -1;
    }
    dom_set_value(d, id, value, borrowed);
    d->nodes[id].tag_id = tag_id;
    if (anchor_id != 0 && dom_anchor_set(d, anchor_id, id) != 0) {
        return -1;
    }
    return dom_place(d, id);
}

const yep_dnode* yep_dom_node(const yep_dom* d, uint32_t id) {
    if (d == NULL || id >= d->ncount) {
        return NULL;
    }
    return &d->nodes[id];
}
