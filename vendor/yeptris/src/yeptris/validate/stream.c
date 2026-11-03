/* stream.c — the event-route validator (#902, TODO.validate.md's
 * stream.c): the family lanes riding the EVENT walk itself.
 *
 * The engine's event stream feeds a streaming state machine — the same
 * family checks the DOM walk runs (through internal.h's fact cores),
 * the same diag machinery, the same report contract — and NO node tree
 * is ever built. The state machine holds per-collection frames (path
 * law, duplicate-key tables, canonical digests) and reproduces the
 * DOM walk's exact observable order:
 *
 *  - DIAGNOSTIC ORDER. The DOM walk emits a mapping's own tag check,
 *    then ALL of its duplicate-key misses, then the pairs' content —
 *    while events deliver duplicates last (a miss is only known when
 *    the equal key arrives). Each mapping therefore stages its
 *    diagnostics ([own tag][duplicates][pair content]) and flushes at
 *    MAP_END; nested collections flush into the parent's content slot
 *    at their own close, so chronological order inside a slot is the
 *    walk order, and a collection KEY's own law lands at the pair's
 *    head position (an insert at the mark the key started at). The
 *    report cap applies at the replay — the flush order IS the walk
 *    order, so the LIMIT sentinel rides the same slot the DOM walk
 *    would have written it in; nothing is dropped before the order is
 *    fixed (the staging holds O(violations), the DOM route's own
 *    input-bounded class).
 *
 *  - STREAM ATTRIBUTION (#885 item 4). A multi-document stream
 *    addresses its documents as array elements ("/1/k"), but the
 *    qualification (more than one document) is known only when the
 *    SECOND DOCUMENT_START arrives — after document 0's diagnostics.
 *    Those carry bare pointers until then; the prefix is prepended
 *    retroactively (with the path slot's exact truncation), and every
 *    later document pushes its index live. Single-document streams
 *    keep the bare pointers, exactly like the DOM route.
 *
 *  - POSITIONS. The DOM route positions a diagnostic at the node's
 *    INPUT-BORROWED value bytes (the DOM builder's region law)
 *    resolved through the line index; collections position at their
 *    first leaf, a mapping's own tag check included. The event facts
 *    carry `borrowed` and the value view, so the offset law is the
 *    same predicate the DOM builder applies — the offsets (and thus
 *    line/col/offset) agree bit for bit; pooled content (folded,
 *    escaped, empty) stays unpositioned (0/0). A collection's tag
 *    check defers until its first leaf arrives (or the collection
 *    closes empty).
 *
 *  - DUPLICATE KEYS. Per-mapping open addressing over canonical
 *    digests (spec 3.2.1.3), the digest fold identical to the DOM
 *    hash's law (yep_validate_hash_mix + the seed constants). Scalar
 *    keys re-compare under the DOM's exact equality law over stored
 *    byte snapshots; collection keys compare digests (equal structures
 *    fold equally; unequal ones practically do — the DOM's structural
 *    equality remains the reference, this is the documented bound of
 *    the streaming twin). Alias keys resolve their target through an
 *    anchor table filled at subtree close (an alias to an OPEN
 *    collection — legal under the engine's compose law — has no digest
 *    yet; the fingerprint falls back to the unbound form, which the
 *    DOM's capped recursive fold cannot reproduce: documented corner).
 *
 * MECE: no validation logic lives here beyond the fact plumbing — the
 * verdicts come from the fact cores in validate.c (a verdict change
 * lands once and both routes move together); the parse rides
 * parse.c's sink-entry seam (the DOM route's front-end and statuses,
 * one home); the sink sets `compose` so the engine's undefined-alias
 * law matches the DOM route's parse statuses. The compiled JSON
 * Schema lane keeps the DOM route (its evaluator needs random access
 * — uniqueItems, contains, required; a streaming twin waits for
 * #901's annotation-model seam, exactly as the issue sequences it).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yeptris/validate.h>

#include "../common/error.h"
#include "../doc.h"
#include "../memory/allocator.h"
#include "../parse/events.h"
#include "../parse/numbers.h"
#include "../resolve/resolver.h"
#include "internal.h"

/* ---- the facts ---------------------------------------------------------- */

/* The five facts the family checks read, per node — the DOM walk
 * extracts them from yep_dnode, this walk from the engine's events. */
typedef struct {
    int kind;       /* yep_dom_kind */
    int has_tag;    /* an explicit tag rode the event */
    uint8_t tag_id; /* the resolver's verdict (emit_now's SSOT) */
    int plainish;   /* PLAIN style or the implicit flag */
    yep_view value; /* scalar content / alias name ({NULL, 0} when empty) */
    uint32_t off;   /* the input-borrowed value offset; UINT32_MAX =
                     * unpositioned (the DOM route's node_off law) */
    uint32_t anchor_id;
} yep_vs_facts;

/* A duplicate-table entry: the 32-bit probe hash gates, the canonical
 * digest identifies; scalar keys keep a byte snapshot so the equality
 * re-runs the DOM's exact law. Collection keys compare digests. */
typedef struct {
    uint32_t h32;
    uint64_t digest;
    uint8_t used;
    uint8_t is_scalar;
    uint8_t tag;
    uint32_t blen;
    char* bytes; /* scalar snapshot (heap; freed with the table) */
    uint32_t off;
} yep_vs_keyent;

/* One open collection. */
typedef struct {
    uint8_t kind;      /* YEP_DOM_SEQUENCE / YEP_DOM_MAPPING */
    uint8_t role;      /* 0 = value/root; 1 = the direct KEY of a
                        * mapping (transparent: no own tag check, no
                        * path segment — the DOM's walk_children law) */
    uint8_t pair_slot; /* mapping: 0 = key expected, 1 = value expected
                        * (the key's path segment is on the scratch) */
    uint32_t seq_index;
    size_t path_mark;
    size_t pair_tail_mark; /* role 1: the parent tail's length at the
                            * key's start (the pair-head insert point) */
    /* deferred own-tag check: collections position at their first leaf */
    int has_tag;
    uint8_t tag_id;
    int first_seen;
    uint32_t first_off;
    /* the emission stage ([own tag][duplicates][pair content]) */
    yeptris_diagnostic* head;
    uint32_t head_n;
    size_t head_cap;
    yeptris_diagnostic* dups;
    uint32_t dups_n;
    size_t dups_cap;
    yeptris_diagnostic* tail;
    uint32_t tail_n;
    size_t tail_cap;
    /* the duplicate table (opens on the second key — a one-key mapping
     * can never miss, the DOM walk's pairs<2 law) */
    int have_pending;
    yep_vs_keyent pending;
    yep_vs_keyent* tbl;
    uint32_t tbl_slots; /* power of two; 0 = unopened */
    uint32_t tbl_n;
    /* the canonical digest (spec 3.2.1.3) — keys and anchor records */
    uint64_t digest;
    uint64_t pair_key_digest; /* valid while pair_slot == 1 */
    uint32_t anchor_id;
} yep_vs_frame;

/* An anchor's target: opened when the anchored node starts (the kind),
 * completed when its subtree closes (digest + the key-check facts).
 * The value view borrows the run's bytes — read only during the run. */
typedef struct {
    uint8_t kind;
    uint8_t closed;
    uint8_t has_tag;
    uint8_t tag_id;
    uint8_t style;
    uint8_t implicit;
    yep_view value;
    uint32_t off; /* the node_off law: own value or first leaf */
    uint64_t digest;
    int digest_known;
} yep_vs_anchor;

typedef struct yep_vstream {
    yep_vctx ctx;
    const yep_resolver* fam;
    int json_family;
    const char* input_base; /* the run's borrowed-view base (dies with
                             * the run; nothing reads it after) */
    yep_vs_frame* frames;
    int depth, frames_cap;
    int maps_open;  /* the stage hook rides this (diagnostics stage
                     * exactly while a mapping is open) */
    int depth_diag; /* the walk guard's one-shot */
    uint32_t docs_seen;
    int qualified; /* a second document arrived: prefixes ride live */
    yep_vs_anchor* anchors;
    uint32_t anchors_cap; /* slots; ordinal i lives at i-1 */
} yep_vstream;

/* The stage hooks. yep_validate_emit composes and then hands the
 * diagnostic to ctx.stage_put; three writers exist:
 *   - vs_stage_put:  the innermost open mapping's CONTENT slot
 *                    (chronological = the walk order inside it);
 *   - vs_capture_*:  a SPECIFIC frame's own-tag (head) or duplicates
 *                    slot, for the deferred tag fire and dup misses.
 * With no mapping open the hook is NULL and emit takes the report
 * path — one home for the cap law (the DOM route's exact shape). */
/* The stage hooks' capture: a frame plus, for the pair-head insert,
 * the parent tail mark the pair's key started at. */
typedef struct {
    yep_vstream* v;
    yep_vs_frame* f;
    size_t mark;
} yep_vs_capture;

/* ---- stage arrays (alloc + copy + free; no realloc) ---------------------- */

static int vs_stage_put_array(yep_vctx* c, yeptris_diagnostic** arr, uint32_t n, size_t* cap,
                              const yeptris_diagnostic* d) {
    if ((size_t)n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        yeptris_diagnostic* na = yep_alloc(yep_system_allocator(), ncap * sizeof(*na));
        if (na == NULL) {
            c->oom = 1;
            return -1;
        }
        if (*arr != NULL) {
            memcpy(na, *arr, (size_t)n * sizeof(*na));
            yep_free(yep_system_allocator(), *arr);
        }
        *arr = na;
        *cap = ncap;
    }
    (*arr)[n] = *d;
    return 0;
}

/* The default hook: the diagnostic joins the INNERMOST OPEN mapping's
 * content slot (everything inside it, chronologically = walk order). */
static void vs_stage_put(void* stage, const yeptris_diagnostic* d) {
    yep_vstream* v = stage;
    yep_vs_frame* m = NULL;
    for (int i = v->depth - 1; i >= 0; i--) {
        if (v->frames[i].kind == YEP_DOM_MAPPING) {
            m = &v->frames[i];
            break;
        }
    }
    if (m == NULL) {
        return; /* unreachable: the hook is up only under a mapping */
    }
    if (vs_stage_put_array(&v->ctx, &m->tail, m->tail_n, &m->tail_cap, d) == 0) {
        m->tail_n++;
    }
}

static void vs_capture_head(void* stage, const yeptris_diagnostic* d) {
    yep_vs_capture* cap = stage;
    if (vs_stage_put_array(&cap->v->ctx, &cap->f->head, cap->f->head_n, &cap->f->head_cap, d) ==
        0) {
        cap->f->head_n++;
    }
}

static void vs_capture_dups(void* stage, const yeptris_diagnostic* d) {
    yep_vs_capture* cap = stage;
    if (vs_stage_put_array(&cap->v->ctx, &cap->f->dups, cap->f->dups_n, &cap->f->dups_cap, d) ==
        0) {
        cap->f->dups_n++;
    }
}

/* A collection KEY's own json12 law lands at the pair's HEAD position:
 * the DOM walk emits it before the key subtree's walk, whose content
 * already sits in the parent's tail — the insert re-creates that
 * order. The mark rides the capture (the parent tail's length at the
 * key's start). */
static void vs_capture_pairhead(void* stage, const yeptris_diagnostic* d) {
    yep_vs_capture* cap = stage;
    yep_vs_frame* m = cap->f;
    size_t mark = cap->mark;
    if (mark > m->tail_n) {
        return; /* defensive: the anchor point vanished */
    }
    if ((size_t)m->tail_n + 1 > m->tail_cap) {
        size_t ncap = m->tail_cap ? m->tail_cap * 2 : 8;
        yeptris_diagnostic* na = yep_alloc(yep_system_allocator(), ncap * sizeof(*na));
        if (na == NULL) {
            cap->v->ctx.oom = 1;
            return;
        }
        if (m->tail != NULL) {
            memcpy(na, m->tail, (size_t)m->tail_n * sizeof(*na));
            yep_free(yep_system_allocator(), m->tail);
        }
        m->tail = na;
        m->tail_cap = ncap;
    }
    memmove(m->tail + mark + 1, m->tail + mark, (size_t)(m->tail_n - mark) * sizeof(*m->tail));
    m->tail[mark] = *d;
    m->tail_n++;
}

/* Replays one stage slot through the report path (the hook is down;
 * the staged diagnostics are RESOLVED — they were composed at walk
 * time — so they append verbatim through the cap-law writer). */
static void vs_replay(yep_vstream* v, const yeptris_diagnostic* arr, uint32_t n) {
    v->ctx.stage_put = NULL;
    for (uint32_t i = 0; i < n; i++) {
        yep_validate_report_put(&v->ctx, &arr[i]);
    }
    v->ctx.stage_put = vs_stage_put;
}

/* Flushes the mapping's stage: into the ENCLOSING mapping's content
 * slot (the flattened [own tag][duplicates][content] joins the parent
 * tail at this close position — chronological = walk order), or to
 * the report when no mapping encloses it. */
static void vs_flush_stage(yep_vstream* v, yep_vs_frame* f) {
    yep_vs_frame* m = NULL;
    for (int i = v->depth - 2; i >= 0; i--) {
        if (v->frames[i].kind == YEP_DOM_MAPPING) {
            m = &v->frames[i];
            break;
        }
    }
    if (m == NULL) {
        vs_replay(v, f->head, f->head_n);
        vs_replay(v, f->dups, f->dups_n);
        vs_replay(v, f->tail, f->tail_n);
        v->ctx.staged -= (size_t)f->head_n + f->dups_n + f->tail_n;
    } else {
        const yeptris_diagnostic* slots[3] = {f->head, f->dups, f->tail};
        uint32_t ns[3] = {f->head_n, f->dups_n, f->tail_n};
        for (int s = 0; s < 3; s++) {
            for (uint32_t i = 0; i < ns[s]; i++) {
                if (vs_stage_put_array(&v->ctx, &m->tail, m->tail_n, &m->tail_cap, &slots[s][i]) !=
                    0) {
                    break;
                }
                m->tail_n++;
            }
        }
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, f->head);
    yep_free(sys, f->dups);
    yep_free(sys, f->tail);
    f->head = NULL;
    f->dups = NULL;
    f->tail = NULL;
    f->head_n = f->dups_n = f->tail_n = 0;
    f->head_cap = f->dups_cap = f->tail_cap = 0;
}

/* ---- frames -------------------------------------------------------------- */

static yep_vs_frame* vs_push(yep_vstream* v, uint8_t kind, uint8_t role, const yep_event* ev) {
    if (v->depth == v->frames_cap) {
        int ncap = v->frames_cap ? v->frames_cap * 2 : 32;
        yep_vs_frame* nf = yep_alloc(yep_system_allocator(), (size_t)ncap * sizeof(*nf));
        if (nf == NULL) {
            v->ctx.oom = 1;
            return NULL;
        }
        if (v->frames != NULL) {
            memcpy(nf, v->frames, (size_t)v->depth * sizeof(*nf));
            yep_free(yep_system_allocator(), v->frames);
        }
        v->frames = nf;
        v->frames_cap = ncap;
    }
    if (v->depth >= YEP_DOM_MAX_DEPTH) {
        /* the engine's depth cap fires first (same constant): the
         * walk guard's defensive twin, one diagnostic like the DOM */
        if (!v->depth_diag) {
            v->depth_diag = 1;
            yep_validate_emit(&v->ctx, YEPTRIS_DIAG_DEPTH, UINT32_MAX,
                              "validation walk exceeded the nesting limit");
        }
        return NULL;
    }
    yep_vs_frame* f = &v->frames[v->depth++];
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    f->role = role;
    f->path_mark = v->ctx.path_len;
    f->first_off = UINT32_MAX;
    f->has_tag = role == 0 && ev->tag.len > 0; /* a KEY-root collection
                                                * never checks its own
                                                * tag (the DOM law) */
    f->tag_id = ev->tag_id;
    f->anchor_id = ev->anchor_id;
    f->digest = kind == YEP_DOM_SEQUENCE ? YEP_VAL_DIGEST_SEQ_SEED : YEP_VAL_DIGEST_MAP_SEED;
    if (kind == YEP_DOM_MAPPING) {
        v->maps_open++;
    }
    return f;
}

static void vs_pop(yep_vstream* v) {
    yep_vs_frame* f = &v->frames[--v->depth];
    if (f->kind == YEP_DOM_MAPPING) {
        v->maps_open--;
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, f->head);
    yep_free(sys, f->dups);
    yep_free(sys, f->tail);
    /* the entries live at their hash slots: sweep the whole table */
    for (uint32_t i = 0; i < f->tbl_slots; i++) {
        yep_free(sys, f->tbl[i].bytes);
    }
    yep_free(sys, f->pending.bytes);
    yep_free(sys, f->tbl);
}

/* ---- anchors ------------------------------------------------------------- */

static yep_vs_anchor* vs_anchor(yep_vstream* v, uint32_t ordinal) {
    if (ordinal == 0) {
        return NULL;
    }
    if (ordinal > v->anchors_cap) {
        /* more anchors than input bytes is a bug, not a document (the
         * DOM table's law); the cap keeps a hostile stream bounded */
        if ((size_t)ordinal > (size_t)v->ctx.input_len + 16) {
            return NULL;
        }
        uint32_t ncap = v->anchors_cap ? v->anchors_cap * 2 : 64;
        while (ncap < ordinal) {
            ncap *= 2;
        }
        yep_vs_anchor* na = yep_alloc(yep_system_allocator(), (size_t)ncap * sizeof(*na));
        if (na == NULL) {
            v->ctx.oom = 1;
            return NULL;
        }
        if (v->anchors != NULL) {
            memcpy(na, v->anchors, (size_t)v->anchors_cap * sizeof(*na));
            yep_free(yep_system_allocator(), v->anchors);
        }
        v->anchors = na;
        v->anchors_cap = ncap;
    }
    yep_vs_anchor* a = &v->anchors[ordinal - 1];
    memset(a, 0, sizeof(*a));
    return a;
}

static yep_vs_anchor* vs_anchor_at(const yep_vstream* v, uint32_t ordinal) {
    if (ordinal == 0 || ordinal > v->anchors_cap) {
        return NULL;
    }
    return &v->anchors[ordinal - 1];
}

/* ---- facts --------------------------------------------------------------- */

/* The DOM route's region law (the DOM builder's dom_str_in predicate),
 * applied to the event's value view: input-borrowed, non-empty, inside
 * the run's base. The offset (and therefore line/col/offset) matches
 * the DOM walk's bit for bit; pooled content stays unpositioned. */
static uint32_t vs_off_of(const yep_vstream* v, const yep_view* value) {
    if (value->len > 0 && value->p != NULL && v->input_base != NULL &&
        (const char*)value->p >= v->input_base) {
        return (uint32_t)((const char*)value->p - v->input_base);
    }
    return UINT32_MAX;
}

static void vs_facts_of(const yep_vstream* v, const yep_event* ev, yep_vs_facts* f) {
    memset(f, 0, sizeof(*f));
    f->kind = ev->type == YEP_EV_SCALAR ? (int)YEP_DOM_SCALAR : (int)YEP_DOM_ALIAS;
    f->has_tag = ev->tag.len > 0;
    f->tag_id = ev->tag_id;
    f->plainish = ev->implicit || ev->style == YEP_STYLE_PLAIN;
    f->value.p = ev->value.len ? ev->value.p : NULL;
    f->value.len = ev->value.len;
    f->off = vs_off_of(v, &ev->value);
    f->anchor_id = ev->anchor_id;
}

/* ---- the first-leaf law -------------------------------------------------- */

/* The DFS-first leaf of every open collection is this leaf (stream
 * order is document order): mark the ancestors that have not seen a
 * leaf yet and fire their pending tag checks, outermost first — the
 * DOM walk's enter order. A mapping's fire lands in its own head slot
 * (capture); a sequence's rides the current stage (chronological:
 * nothing of any enclosing content precedes the shared first leaf). */
static void vs_first_leaf(yep_vstream* v, const yep_vs_facts* f) {
    for (int i = 0; i < v->depth; i++) {
        yep_vs_frame* fr = &v->frames[i];
        if (fr->first_seen) {
            continue;
        }
        fr->first_seen = 1;
        fr->first_off = f->off;
        if (!fr->has_tag) {
            continue;
        }
        fr->has_tag = 0;
        if (fr->kind == YEP_DOM_MAPPING) {
            yep_vs_capture cap = {v, fr, 0};
            void (*save_put)(void*, const yeptris_diagnostic*) = v->ctx.stage_put;
            void* save_stage = v->ctx.stage;
            v->ctx.stage_put = vs_capture_head;
            v->ctx.stage = &cap;
            yep_check_tag_facts(&v->ctx, (int)fr->kind, 1, fr->tag_id, (yep_view){NULL, 0}, f->off,
                                v->fam, v->json_family);
            v->ctx.stage_put = save_put;
            v->ctx.stage = save_stage;
        } else {
            yep_check_tag_facts(&v->ctx, (int)fr->kind, 1, fr->tag_id, (yep_view){NULL, 0}, f->off,
                                v->fam, v->json_family);
        }
    }
}

/* ---- checks --------------------------------------------------------------- */

static void vs_checks_scalar(yep_vstream* v, const yep_vs_facts* f) {
    if (v->ctx.oom) {
        return;
    }
    yep_check_tag_facts(&v->ctx, f->kind, f->has_tag, f->tag_id, f->value, f->off, v->fam,
                        v->json_family);
    if (v->json_family) {
        yep_check_json_scalar_facts(&v->ctx, f->has_tag, f->tag_id, f->plainish, f->value, f->off);
    }
}

/* A scalar KEY's checks at the mapping's path (the DOM pair order:
 * the json12 key law first, then the tag check). */
static void vs_checks_key_scalar(yep_vstream* v, const yep_vs_facts* f) {
    if (v->ctx.oom) {
        return;
    }
    if (v->json_family) {
        yep_check_json_key_facts(&v->ctx, f->has_tag, f->tag_id, f->plainish, f->value, f->off);
    }
    yep_check_tag_facts(&v->ctx, (int)YEP_DOM_SCALAR, f->has_tag, f->tag_id, f->value, f->off,
                        v->fam, v->json_family);
}

/* ---- the canonical digests ----------------------------------------------- */

static uint64_t vs_digest_scalar(const yep_vs_facts* f) {
    return yep_validate_scalar_digest(yep_validate_tag_of(f->tag_id, f->has_tag, f->plainish),
                                      f->value);
}

/* ---- duplicate keys ------------------------------------------------------- */

/* The DOM walk's scalar equality law over stored facts (the number
 * arms compare by value across the int/float tags; everything else
 * wants the same tag and equal bytes). */
static int vs_key_equal(const yep_vs_keyent* e, uint8_t tag, const char* p, uint32_t len) {
    int numa = e->tag == YEPTRIS_TAG_INT || e->tag == YEPTRIS_TAG_FLOAT;
    int numb = tag == YEPTRIS_TAG_INT || tag == YEPTRIS_TAG_FLOAT;
    if (numa != numb) {
        return 0;
    }
    if (numa) {
        int64_t ia, ib;
        if (yep_num_i64(e->bytes, e->blen, &ia) == 0 && yep_num_i64(p, len, &ib) == 0) {
            return ia == ib;
        }
        double fa, fb;
        if (yep_num_f64(e->bytes, e->blen, &fa) == 0 && yep_num_f64(p, len, &fb) == 0) {
            return fa == fb;
        }
        return 0;
    }
    if (e->tag != tag) {
        return 0;
    }
    return e->blen == len && (len == 0 || memcmp(e->bytes, p, len) == 0);
}

/* The dup diagnostic rides the mapping's duplicates slot (the DOM walk
 * emits the whole table before the pairs). */
static void vs_dup_emit(yep_vstream* v, yep_vs_frame* f, const yep_vs_facts* kf) {
    yep_vs_capture cap = {v, f, 0};
    void (*save_put)(void*, const yeptris_diagnostic*) = v->ctx.stage_put;
    void* save_stage = v->ctx.stage;
    v->ctx.stage_put = vs_capture_dups;
    v->ctx.stage = &cap;
    char key[40];
    size_t kl = kf->value.len < sizeof(key) - 8 ? kf->value.len : sizeof(key) - 8;
    if (kl > 0 && kf->value.p != NULL) {
        memcpy(key, kf->value.p, kl);
    }
    key[kl] = '\0';
    yep_validate_emit(&v->ctx, YEPTRIS_DIAG_DUPLICATE_KEY, kf->off,
                      "duplicate mapping key (\"%s\")", key);
    v->ctx.stage_put = save_put;
    v->ctx.stage = save_stage;
}

/* Opens the probe table on the second key (the pending first entry
 * rides along); NULL on allocation failure (ctx.oom set). The pending
 * entry's snapshot moves into the table (the frame's copy is zeroed —
 * one owner per snapshot). */
static yep_vs_keyent* vs_tbl_open(yep_vstream* v, yep_vs_frame* f) {
    f->tbl_slots = 8;
    f->tbl = yep_alloc(yep_system_allocator(), f->tbl_slots * sizeof(*f->tbl));
    if (f->tbl == NULL) {
        v->ctx.oom = 1;
        return NULL;
    }
    memset(f->tbl, 0, f->tbl_slots * sizeof(*f->tbl));
    f->tbl[f->pending.h32 & (f->tbl_slots - 1)] = f->pending;
    memset(&f->pending, 0, sizeof(f->pending));
    f->tbl_n = 1;
    f->have_pending = 0;
    return f->tbl;
}

/* Probes (and inserts) one key entry. The first key parks in the
 * frame slot; the second opens the table (the pending entry moves in)
 * and the probe runs against it — a one-key mapping can never miss,
 * and once the table exists every key probes it. */
static void vs_dup_probe(yep_vstream* v, yep_vs_frame* f, const yep_vs_keyent* e,
                         const yep_vs_facts* kf) {
    if (f->tbl == NULL) {
        if (!f->have_pending) {
            f->pending = *e; /* the snapshot moves with the entry */
            f->have_pending = 1;
            return;
        }
        if (vs_tbl_open(v, f) == NULL) {
            yep_free(yep_system_allocator(), e->bytes);
            return;
        }
    }
    uint32_t i = e->h32 & (f->tbl_slots - 1);
    while (f->tbl[i].used) {
        const yep_vs_keyent* s = &f->tbl[i];
        int hit = 0;
        if (s->h32 == e->h32) {
            if (s->is_scalar && e->is_scalar) {
                hit = vs_key_equal(s, e->tag, e->bytes, e->blen);
            } else if (!s->is_scalar && !e->is_scalar) {
                hit = s->digest == e->digest;
            }
            /* mixed kinds: the DOM's kind mismatch answers 0 */
        }
        if (hit) {
            /* enumerate EVERY miss; the equal key does not insert */
            vs_dup_emit(v, f, kf);
            yep_free(yep_system_allocator(), e->bytes);
            return;
        }
        i = (i + 1) & (f->tbl_slots - 1);
    }
    /* grow at 3/4 (the DOM walk's cap<4*pairs shape) */
    if ((f->tbl_n + 1) * 4 > f->tbl_slots * 3) {
        uint32_t nslots = f->tbl_slots * 2;
        yep_vs_keyent* nt = yep_alloc(yep_system_allocator(), nslots * sizeof(*nt));
        if (nt == NULL) {
            yep_free(yep_system_allocator(), e->bytes);
            v->ctx.oom = 1;
            return;
        }
        memset(nt, 0, nslots * sizeof(*nt));
        for (uint32_t j = 0; j < f->tbl_slots; j++) {
            if (f->tbl[j].used) {
                uint32_t k = f->tbl[j].h32 & (nslots - 1);
                while (nt[k].used) {
                    k = (k + 1) & (nslots - 1);
                }
                nt[k] = f->tbl[j];
            }
        }
        yep_free(yep_system_allocator(), f->tbl);
        f->tbl = nt;
        f->tbl_slots = nslots;
        i = e->h32 & (nslots - 1);
        while (f->tbl[i].used) {
            i = (i + 1) & (f->tbl_slots - 1);
        }
    }
    f->tbl[i] = *e;
    f->tbl_n++;
}

/* Builds a scalar key's table entry from the facts. */
static void vs_dup_scalar(yep_vstream* v, yep_vs_frame* f, const yep_vs_facts* kf,
                          uint64_t digest) {
    yep_vs_keyent e;
    memset(&e, 0, sizeof(e));
    e.h32 = (uint32_t)(digest ^ (digest >> 32));
    e.digest = digest;
    e.used = 1;
    e.is_scalar = 1;
    e.tag = yep_validate_tag_of(kf->tag_id, kf->has_tag, kf->plainish);
    e.blen = kf->value.len;
    e.off = kf->off;
    if (kf->value.len > 0) {
        e.bytes = yep_alloc(yep_system_allocator(), kf->value.len);
        if (e.bytes == NULL) {
            v->ctx.oom = 1;
            return;
        }
        memcpy(e.bytes, kf->value.p, kf->value.len);
    }
    vs_dup_probe(v, f, &e, kf);
}

/* Alias keys resolve their target through the anchor table (the DOM
 * hash follows the target node). An OPEN target has no digest yet —
 * the documented corner (see the file comment). */
static void vs_dup_alias(yep_vstream* v, yep_vs_frame* f, const yep_vs_facts* kf) {
    yep_vs_anchor* a = vs_anchor_at(v, kf->anchor_id);
    uint64_t digest = a != NULL && a->digest_known
                          ? a->digest
                          : yep_validate_hash_mix(YEP_VAL_DIGEST_ALIAS, kf->value.len);
    yep_vs_keyent e;
    memset(&e, 0, sizeof(e));
    e.h32 = (uint32_t)(digest ^ (digest >> 32));
    e.digest = digest;
    e.used = 1;
    e.is_scalar = 0;
    e.off = kf->off;
    vs_dup_probe(v, f, &e, kf);
}

/* ---- path ----------------------------------------------------------------- */

/* Pushes the key's path segment (its raw value bytes; the alias's
 * name; collections contribute the empty segment — the DOM's law). */
static int vs_push_key_segment(yep_vstream* v, const yep_vs_facts* kf) {
    return yep_validate_path_push(&v->ctx, kf->value.p != NULL ? kf->value.p : "", kf->value.len);
}

/* ---- events ---------------------------------------------------------------- */

static void vs_event_scalar(yep_vstream* v, const yep_event* ev) {
    yep_vs_facts f;
    vs_facts_of(v, ev, &f);
    vs_first_leaf(v, &f);
    if (v->ctx.oom) {
        return;
    }
    uint64_t digest = vs_digest_scalar(&f);
    yep_vs_frame* top = v->depth ? &v->frames[v->depth - 1] : NULL;
    if (top == NULL) {
        /* document root */
        vs_checks_scalar(v, &f);
    } else if (top->kind == YEP_DOM_SEQUENCE) {
        /* a KEY-root sequence's children walk flat (no index) */
        int pushed = top->role == 0 && yep_validate_path_push_index(&v->ctx, top->seq_index++) == 0;
        vs_checks_scalar(v, &f);
        if (pushed) {
            yep_validate_path_pop(&v->ctx, top->path_mark);
        }
        top->digest = yep_validate_hash_mix(top->digest, digest);
    } else if (top->pair_slot == 0) {
        /* the KEY. A KEY-root mapping (role 1) walks its children the
         * flat walk_children way: the value-arm checks, no key law,
         * no dup table (its map arm never runs in the DOM), no path
         * segments, and the pair folds still account. */
        if (top->role == 1) {
            vs_checks_scalar(v, &f);
        } else {
            vs_checks_key_scalar(v, &f);
            vs_dup_scalar(v, top, &f, digest);
            if (vs_push_key_segment(v, &f) != 0) {
                /* OOM flagged; the value still streams at the same depth */
            }
        }
        top->pair_slot = 1;
        top->pair_key_digest = digest;
    } else {
        /* the VALUE: checks at the key's path, then the pair folds */
        vs_checks_scalar(v, &f);
        uint64_t pair = yep_validate_hash_mix(YEP_VAL_DIGEST_PAIR_SEED, top->pair_key_digest);
        pair = yep_validate_hash_mix(pair, digest);
        top->digest ^= pair;
        if (top->role == 0) {
            yep_validate_path_pop(&v->ctx, top->path_mark);
        }
        top->pair_slot = 0;
    }
    if (ev->anchor_id != 0) {
        yep_vs_anchor* a = vs_anchor(v, ev->anchor_id);
        if (a != NULL) {
            a->kind = YEP_DOM_SCALAR;
            a->closed = 1;
            a->has_tag = (uint8_t)f.has_tag;
            a->tag_id = f.tag_id;
            a->style = (uint8_t)ev->style;
            a->implicit = (uint8_t)ev->implicit;
            a->value = f.value;
            a->off = f.off;
            a->digest = digest;
            a->digest_known = 1;
        }
    }
}

static void vs_event_alias(yep_vstream* v, const yep_event* ev) {
    yep_vs_facts f;
    vs_facts_of(v, ev, &f);
    vs_first_leaf(v, &f);
    if (v->ctx.oom) {
        return;
    }
    yep_vs_anchor* a = vs_anchor_at(v, f.anchor_id);
    uint64_t digest = a != NULL && a->digest_known
                          ? a->digest
                          : yep_validate_hash_mix(YEP_VAL_DIGEST_ALIAS, f.value.len);
    yep_vs_frame* top = v->depth ? &v->frames[v->depth - 1] : NULL;
    if (top == NULL) {
        /* alias root: the DOM walk checks nothing at the alias */
    } else if (top->kind == YEP_DOM_SEQUENCE) {
        int pushed = top->role == 0 && yep_validate_path_push_index(&v->ctx, top->seq_index++) == 0;
        if (pushed) {
            yep_validate_path_pop(&v->ctx, top->path_mark);
        }
        top->digest = yep_validate_hash_mix(top->digest, digest);
    } else if (top->pair_slot == 0) {
        /* the KEY. Role 1 (inside a KEY-root collection) walks flat:
         * the ALIAS arm checks nothing, no segment, folds still run. */
        if (top->role == 0) {
            /* the json12 law resolves the TARGET's facts (the DOM
             * recursion lands on the target node); no tag check */
            if (v->json_family && a != NULL) {
                if (a->kind != YEP_DOM_SCALAR) {
                    yep_validate_emit(&v->ctx, YEPTRIS_DIAG_KEY_NOT_STRING, a->off,
                                      "mapping key is a %s",
                                      a->kind == YEP_DOM_SEQUENCE ? "sequence" : "mapping");
                } else if (a->closed) {
                    yep_check_json_key_facts(&v->ctx, a->has_tag, a->tag_id,
                                             a->implicit || a->style == YEP_STYLE_PLAIN, a->value,
                                             a->off);
                }
            }
            vs_dup_alias(v, top, &f);
            if (vs_push_key_segment(v, &f) != 0) {
                /* OOM flagged; the value still streams */
            }
        }
        top->pair_slot = 1;
        top->pair_key_digest = digest;
    } else {
        /* the VALUE: nothing (the target is validated at its place) */
        uint64_t pair = yep_validate_hash_mix(YEP_VAL_DIGEST_PAIR_SEED, top->pair_key_digest);
        pair = yep_validate_hash_mix(pair, digest);
        top->digest ^= pair;
        if (top->role == 0) {
            yep_validate_path_pop(&v->ctx, top->path_mark);
        }
        top->pair_slot = 0;
    }
}

static void vs_event_collection_start(yep_vstream* v, const yep_event* ev, uint8_t kind) {
    yep_vs_frame* top = v->depth ? &v->frames[v->depth - 1] : NULL;
    uint8_t role = 0;
    size_t pair_mark = 0;
    if (top != NULL && top->kind == YEP_DOM_SEQUENCE) {
        /* walk_children children (inside a KEY-root collection) walk
         * flat: no index segments — the seq arm never runs for it */
        if (top->role == 0 && yep_validate_path_push_index(&v->ctx, top->seq_index++) != 0) {
            /* OOM flagged; the frame still opens (the walk stops) */
        }
    } else if (top != NULL && top->pair_slot == 0) {
        role = 1;                /* the direct KEY of a mapping: transparent */
        pair_mark = top->tail_n; /* the pair-head insert point */
    }
    yep_vs_frame* f = vs_push(v, kind, role, ev);
    if (f == NULL) {
        return;
    }
    f->pair_tail_mark = pair_mark;
    /* the anchor's OPEN record (the DOM binds at node creation) */
    if (ev->anchor_id != 0) {
        yep_vs_anchor* a = vs_anchor(v, ev->anchor_id);
        if (a != NULL) {
            a->kind = kind;
            a->closed = 0;
            a->off = UINT32_MAX;
        }
    }
}

static void vs_event_collection_end(yep_vstream* v, const yep_event* ev, uint8_t kind) {
    (void)ev;
    if (v->depth == 0) {
        return;
    }
    yep_vs_frame* f = &v->frames[v->depth - 1];
    if (f->kind != kind) {
        return; /* defensive: the engine's streams are balanced */
    }
    /* the pending tag check of an EMPTY collection (no first leaf) */
    if (!f->first_seen) {
        f->first_seen = 1;
        if (f->has_tag) {
            f->has_tag = 0;
            if (f->kind == YEP_DOM_MAPPING) {
                yep_vs_capture cap = {v, f, 0};
                void (*save_put)(void*, const yeptris_diagnostic*) = v->ctx.stage_put;
                void* save_stage = v->ctx.stage;
                v->ctx.stage_put = vs_capture_head;
                v->ctx.stage = &cap;
                yep_check_tag_facts(&v->ctx, (int)f->kind, 1, f->tag_id, (yep_view){NULL, 0},
                                    UINT32_MAX, v->fam, v->json_family);
                v->ctx.stage_put = save_put;
                v->ctx.stage = save_stage;
            } else {
                yep_check_tag_facts(&v->ctx, (int)f->kind, 1, f->tag_id, (yep_view){NULL, 0},
                                    UINT32_MAX, v->fam, v->json_family);
            }
        }
    }
    uint64_t digest = f->digest;
    uint32_t anchor_id = f->anchor_id;
    uint8_t child_kind = f->kind;

    if (kind == YEP_DOM_MAPPING) {
        /* the DOM's odd-child law: an unpaired key pairs with a
         * synthesized null (plain, implicit, empty, tag-free) */
        if (f->pair_slot == 1) {
            yep_vs_facts nf;
            memset(&nf, 0, sizeof(nf));
            nf.kind = (int)YEP_DOM_SCALAR;
            nf.plainish = 1;
            nf.off = UINT32_MAX;
            vs_checks_scalar(v, &nf);
            uint64_t vd = vs_digest_scalar(&nf);
            uint64_t pair = yep_validate_hash_mix(YEP_VAL_DIGEST_PAIR_SEED, f->pair_key_digest);
            pair = yep_validate_hash_mix(pair, vd);
            f->digest ^= pair;
            yep_validate_path_pop(&v->ctx, f->path_mark);
            f->pair_slot = 0;
        }
        vs_flush_stage(v, f);
    }

    /* digest + pair delivery to the parent; the parent's segment for
     * this child pops here (the seq index / the pair's key segment) */
    yep_vs_frame* p = v->depth >= 2 ? &v->frames[v->depth - 2] : NULL;
    if (p != NULL) {
        if (p->kind == YEP_DOM_SEQUENCE) {
            yep_validate_path_pop(&v->ctx, p->path_mark); /* the index */
            p->digest = yep_validate_hash_mix(p->digest, digest);
        } else if (p->pair_slot == 0) {
            /* the child was the KEY collection: transparent on the
             * path (no own segment). Its dup-table probe runs here
             * (the DOM's table covers collection keys; the detail's
             * key text is the empty view, its position the key's
             * first leaf). The json12 key law lands at the pair's
             * HEAD position (the DOM emits it before the key
             * subtree's walk). */
            if (p->role == 0) {
                yep_vs_keyent e;
                memset(&e, 0, sizeof(e));
                e.h32 = (uint32_t)(digest ^ (digest >> 32));
                e.digest = digest;
                e.used = 1;
                e.is_scalar = 0;
                yep_vs_facts kf;
                memset(&kf, 0, sizeof(kf));
                kf.off = f->first_off;
                vs_dup_probe(v, p, &e, &kf);
            }
            if (v->json_family) {
                yep_vs_capture cap = {v, p, f->pair_tail_mark};
                void (*save_put)(void*, const yeptris_diagnostic*) = v->ctx.stage_put;
                void* save_stage = v->ctx.stage;
                v->ctx.stage_put = vs_capture_pairhead;
                v->ctx.stage = &cap;
                yep_validate_emit(&v->ctx, YEPTRIS_DIAG_KEY_NOT_STRING, f->first_off,
                                  "mapping key is a %s",
                                  child_kind == YEP_DOM_SEQUENCE ? "sequence" : "mapping");
                v->ctx.stage_put = save_put;
                v->ctx.stage = save_stage;
            }
            p->pair_key_digest = digest;
            if (yep_validate_path_push(&v->ctx, "", 0) != 0) {
                /* OOM flagged */
            }
            p->pair_slot = 1;
        } else {
            uint64_t pair = yep_validate_hash_mix(YEP_VAL_DIGEST_PAIR_SEED, p->pair_key_digest);
            pair = yep_validate_hash_mix(pair, digest);
            p->digest ^= pair;
            yep_validate_path_pop(&v->ctx, p->path_mark);
            p->pair_slot = 0;
        }
    }

    /* the anchor's subtree closed */
    if (anchor_id != 0) {
        yep_vs_anchor* a = vs_anchor_at(v, anchor_id);
        if (a != NULL) {
            a->kind = child_kind;
            a->closed = 1;
            a->off = f->first_off;
            a->digest = digest;
            a->digest_known = 1;
        }
    }

    vs_pop(v);
}

static void vs_doc_start(yep_vstream* v) {
    v->docs_seen++;
    if (v->docs_seen == 2 && !v->qualified) {
        /* qualification arrives with the SECOND document: document 0's
         * diagnostics (all flushed — its root closed before the
         * DOCUMENT_END) gain the prefix retroactively, with the path
         * slot's exact truncation */
        v->qualified = 1;
        yeptris_validate_report* r = v->ctx.rep;
        for (uint32_t i = 0; i < r->count; i++) {
            char* p = r->diags[i].path;
            size_t plen = strlen(p);
            size_t keep = plen < YEPTRIS_DIAG_PATH_MAX - 3 ? plen : YEPTRIS_DIAG_PATH_MAX - 3;
            memmove(p + 2, p, keep);
            p[0] = '/';
            p[1] = '0';
            p[2 + keep] = '\0';
        }
    }
    v->ctx.path_len = 0; /* root-relative per document */
    if (v->qualified) {
        if (yep_validate_path_push_index(&v->ctx, v->docs_seen - 1) != 0) {
            v->ctx.oom = 1;
        }
    }
}

static void vs_doc_end(yep_vstream* v) {
    v->ctx.path_len = 0;
}

static int vs_on_event(void* ctx, const yep_event* ev) {
    yep_vstream* v = ctx;
    /* the stage hook rides the mapping state: diagnostics stage while
     * a mapping is open, otherwise they take the report path (one
     * home for the cap law) */
    v->ctx.stage_put = v->maps_open > 0 ? vs_stage_put : NULL;
    v->ctx.stage = v;
    switch (ev->type) {
    case YEP_EV_SCALAR:
        vs_event_scalar(v, ev);
        break;
    case YEP_EV_ALIAS:
        vs_event_alias(v, ev);
        break;
    case YEP_EV_SEQ_START:
        vs_event_collection_start(v, ev, YEP_DOM_SEQUENCE);
        break;
    case YEP_EV_MAP_START:
        vs_event_collection_start(v, ev, YEP_DOM_MAPPING);
        break;
    case YEP_EV_SEQ_END:
        vs_event_collection_end(v, ev, YEP_DOM_SEQUENCE);
        break;
    case YEP_EV_MAP_END:
        vs_event_collection_end(v, ev, YEP_DOM_MAPPING);
        break;
    case YEP_EV_DOCUMENT_START:
        vs_doc_start(v);
        break;
    case YEP_EV_DOCUMENT_END:
        vs_doc_end(v);
        break;
    default:
        break; /* STREAM_START / STREAM_END frame only */
    }
    return v->ctx.oom ? -1 : 0; /* the DOM sink's OOM abort law */
}

/* ---- the face -------------------------------------------------------------- */

static void vs_free(yep_vstream* v) {
    if (v == NULL) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    while (v->depth > 0) {
        vs_pop(v);
    }
    yep_free(sys, v->frames);
    yep_free(sys, v->anchors);
    yep_free(sys, v->ctx.line_off);
    yep_free(sys, v->ctx.path);
    yeptris_validate_report_free(v->ctx.rep);
    yep_free(sys, v);
}

static void vs_on_input(void* ctx, const char* base, size_t len) {
    yep_vstream* v = ctx;
    v->input_base = base;
    v->ctx.input = base;
    v->ctx.input_len = len;
}

YEPTRIS_API YeptrisStatus yeptris_validate_stream(const char* buf, size_t len, YeptrisSchema schema,
                                                  yeptris_validate_report** out) {
    if (out == NULL || (buf == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    const yep_resolver* fam;
    int json_family;
    if (!yep_validate_schema_family(schema, &fam, &json_family)) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "schema %d is not a validation family", (int)schema);
        return YEPTRIS_ERROR_ARG;
    }
    yep_vstream* v = yep_alloc(yep_system_allocator(), sizeof(*v));
    if (v == NULL) {
        return YEPTRIS_ERROR_MEMORY;
    }
    memset(v, 0, sizeof(*v));
    v->fam = fam;
    v->json_family = json_family;
    v->ctx.stage_put = vs_stage_put;
    v->ctx.stage = v;
    v->ctx.rep = yep_validate_report_new();
    if (v->ctx.rep == NULL) {
        vs_free(v);
        return YEPTRIS_ERROR_MEMORY;
    }
    yep_sink sink;
    memset(&sink, 0, sizeof(sink));
    sink.on_event = vs_on_event;
    sink.ctx = v;
    sink.compose = 1; /* the engine's undefined-alias law: the DOM
                       * route's parse statuses ride along */
    YeptrisStatus st = yep_parse_events(buf, len, schema, &sink, vs_on_input, v);
    v->input_base = NULL; /* the run's bytes died with the run */
    v->ctx.input = NULL;
    v->ctx.input_len = 0;
    if (st != YEPTRIS_OK) {
        vs_free(v);
        return st; /* the parse's own error, verbatim */
    }
    /* defensive: a clean run leaves no stage open (documents are
     * balanced); drain anything anyway so the cap law stays the last
     * writer */
    while (v->depth > 0) {
        yep_vs_frame* f = &v->frames[v->depth - 1];
        if (f->kind == YEP_DOM_MAPPING) {
            vs_flush_stage(v, f);
        }
        vs_pop(v);
    }
    v->maps_open = 0;
    /* the shared tail (the DOM walk's law): OOM frees the report */
    if (v->ctx.oom) {
        vs_free(v);
        return YEPTRIS_ERROR_MEMORY;
    }
    yeptris_validate_report* r = v->ctx.rep;
    st = r->count > 0 ? YEPTRIS_ERROR_SCHEMA : YEPTRIS_OK;
    *out = r;
    v->ctx.rep = NULL;
    vs_free(v);
    return st;
}
