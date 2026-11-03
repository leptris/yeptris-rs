/* dom.h — minimal event-built DOM (TODO.impl/11 v1).
 *
 * Nodes are dense records in a pool; children are sibling-linked
 * (first_child/last_child/next_sibling) — safe when parent and child
 * collections interleave. Anchors bind to node ids. The builder is a
 * sink over the engine (MECE: the DOM never parses). v1 deviation from
 * the board (recorded there): one dom.c with a kind enum — per-kind
 * vtables land with mutation + emitter integration, where polymorphic
 * behavior actually appears.
 *
 * Lifetime: borrowed event values (from the caller's input) stay views
 * into that input; engine-pool values (folded/escaped) are copied into
 * the DOM's pool. The input buffer must outlive the document.
 */
#ifndef YEP_DOM_H
#define YEP_DOM_H

#include <stdint.h>

#include <yeptris/api.h> /* YEPTRIS_CT_ASSERT (the layout gates) */
#include <yeptris/tape.h> /* yeptris_json_tape (dom_from_tape) */ /* YEPTRIS_CT_ASSERT (the layout gates) */

#include "../common/simd_text.h"

#include "common/nametab.h"
#include "dom/mapindex.h"
#include "memory/allocator.h"
#include "memory/pool.h"
#include "parse/events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YEP_DOM_SCALAR = 0,
    YEP_DOM_SEQUENCE,
    YEP_DOM_MAPPING,
    YEP_DOM_ALIAS,
} yep_dom_kind;

#define YEP_DOM_MAX_DEPTH 1000

/* Compact node strings: (region-tagged offset, length) instead of
 * pointers — the node drops to <= 64 B (TODO.impl/11's gate). Two
 * regions: the borrowed parse input (offset from its base) and the
 * DOM's own contiguous string arena (offset from its start; the
 * arena grows by realloc, which MOVES bytes but never invalidates
 * OFFSETS — the leptris arena discipline). Absence = len 0. */
#define YEP_SV_INPUT 0x80000000u /* region bit: 1 = arena, 0 = input */
#define YEP_SV_OFF 0x7FFFFFFFu   /* 31-bit offsets (2 GiB ceiling) */

typedef struct yep_sview {
    uint32_t off; /* YEP_SV_INPUT | byte offset into the region */
    uint32_t len;
} yep_sview;

/* Compact header (TODO.restructure/64-2a): kind/style/flow/implicit
 * pack into one bitfield byte. The mutation-only attached/depth
 * fields left the record — they live in the DOM's lazily-built side
 * tables (parse pays nothing; dom.c owns them). */
typedef struct yep_dnode {
    uint8_t kind : 2;      /* yep_dom_kind (0-3) */
    uint8_t style : 3;     /* yep_scalar_style (0-5) */
    uint8_t flow : 1;      /* collection opened in flow style */
    uint8_t implicit : 1;  /* scalar written without quotes/tag */
    uint8_t no_exotic : 1; /* value verified clean of the dq-forced
                            * byte set (#352 lane 2): the serializer's
                            * sc_route skips its rescan. Set at every
                            * value assignment; 0 = unknown = scan. */
    uint8_t plain_ok;      /* value bytes verified yep_scan_plain_safe
                            * (#352 lane 3): the emitter skips its
                            * plain-safety walk (the block kernel's 35%
                            * frame). Same invariant as no_exotic: set
                            * at every dom_set_value; 0 = unknown =
                            * derive. */
    uint8_t tag_id;        /* resolved tag (resolve/resolver.h) */
    uint32_t first_child;  /* child link (mappings: key,value,key,value…) */
    uint32_t last_child;
    uint32_t next_sibling;
    uint32_t count;  /* children (mappings: 2 × pairs) */
    uint32_t target; /* alias: target node id */
    yep_sview value; /* scalar content / alias name */
    yep_sview tag;
    yep_sview anchor;
} yep_dnode; /* 48 B: line/col never left the event stream (64-2c) —
                the pull/push/recorder APIs remain the source */

/* 11's node-size gate, tightened by 64-2c: <= 48 B. */
YEPTRIS_CT_ASSERT(sizeof(yep_dnode) <= 48);

typedef struct yep_dom {
    const yep_allocator* sys;
    /* Memory guard (the bounded-parse law): when input_len > 0 (set by
     * the parse seams), every growth path is capped by the input size —
     * a parse over N bytes allocates O(N), so a grammar/loop bug fails
     * YEP_ERR_MEMORY instead of ballooning. The mutation API (no input)
     * leaves it 0 and is uncapped. */
    size_t input_len;
    /* mutation side tables (64-2a): attached bitmap + depths, grown
     * lazily on first mutation — parse-built docs never pay */
    uint8_t* mut_att;
    uint16_t* mut_depth;
    uint32_t mut_tbl_cap;
    /* the strict-JSON route's fused walk (TODO.restructure/81): when
     * set, dom_on_flow_build's walker enforces RFC 8259 keys — the
     * engine's flow path leaves it 0 (the lenient YAML flow class) */
    int flow_strict;
    /* fused-flow staging (TODO.restructure/53): scratch nodes live at
     * [stage_base, ncount) until commit; rollback restores the counts */
    int flow_staged;
    uint32_t flow_stage_base, flow_stage_str;
    uint32_t flow_stage_root, flow_stage_anchor;
    const struct yep_resolver* resolver; /* implicit typing for direct
        builders (NULL = core12); parse_impl sets it beside the engine's
        so the two paths resolve identically (the typing SSOT) */
    yep_pool* pool;                      /* node/docs arrays (strings live in str) */
    /* string arena: contiguous, realloc-grown; node strings are
     * OFFSETS into it, so growth never dangles (dom_str_put) */
    char* str;
    uint32_t str_len, str_cap;
    /* an arena write failed (str_put under OOM): the value would have
     * silently become the empty view — doc corruption. Sticky; every
     * builder path checks it at dom_place/dom_open_node and fails the
     * parse with the memory status. */
    int oom;
    const char* input_base; /* borrowed parse input (offset base) */
    /* handle arena: thread-safe (atomic bump) — query APIs allocate
     * YeptrisNode wrappers here, so read-only document sharing across
     * threads is safe; parse-path pools stay single-threaded */
    struct yep_hpool* handles;
    /* #377: the lazy indexed-children cache — child ids of the LAST
     * container indexed via seq_at/map_at. Built once per container
     * under the lazy-init mutex (read-only sharing), O(1) after;
     * invalidated by every child-list mutation. NULL when idle. */
    uint32_t* child_cache;
    uint32_t child_cache_id; /* UINT32_MAX = empty */
    uint32_t child_cache_len;
    yep_dnode* nodes;
    uint32_t ncount, ncap;
    uint32_t* docs; /* document root node ids */
    uint32_t dcount, dcap;
    /* anchor ordinal -> binding node id, indexed directly (the engine
     * stamps 1-based ordinals into the events; no name hashing here) */
    uint32_t* anchor_nodes;
    uint32_t anchor_nodes_cap;  /* slots allocated */
    uint32_t anchor_max;        /* highest ordinal seen (clear prefix) */
    struct yep_midx_state midx; /* lazy per-map lookup index (mapindex.c) */
    /* builder state */
    uint32_t stack[YEP_DOM_MAX_DEPTH];
    int depth;
    int doc_open;
    int map_pending_key[YEP_DOM_MAX_DEPTH]; /* per-frame: key seen, value next */
    uint32_t pending_key_id[YEP_DOM_MAX_DEPTH];
    uint32_t pending_key;
} yep_dom;

/* Decodes to the pointer view callers use (defined after yep_dom).
 * Absent views (len 0) decode to a valid base pointer with length
 * 0 — callers test len. */
static inline yep_view yep_dom_view(const yep_dom* d, yep_sview sv) {
    yep_view v = {NULL, 0};
    if (sv.len == 0) {
        return v; /* NULL+0 would be UB on a regionless document */
    }
    v.len = sv.len;
    v.p = (sv.off & YEP_SV_INPUT ? d->str : d->input_base) + (sv.off & YEP_SV_OFF);
    return v;
}

/* Input-region encoding; arena copies go through dom_str_put. */
static inline yep_sview yep_sv_input(const yep_dom* d, yep_view v) {
    yep_sview sv = {(uint32_t)((const char*)v.p - d->input_base), v.len};
    return sv;
}

yep_dom* yep_dom_create(const yep_allocator* sys);

/* Pre-sizes the node array and string arena from content-derived
 * hints (TODO.impl/06: one SIMD count pass -> reserves -> at most
 * logarithmic growth remains instead of doubling from empty).
 * Hints are advisory: overshoot wastes memory, undershoot falls
 * back to normal growth. 0 hint skips that reserve. */
void yep_dom_reserve(yep_dom* d, uint32_t node_hint, uint32_t str_hint);

/* One-shot sizing from the input (count passes + reserve above) —
 * the single home of the heuristic; parse_impl and the memory bench
 * both call it so the measure and the product can't drift. */
void yep_dom_prepare(yep_dom* d, const yep_text_stats* st);

/* Length-heuristic pre-sizing (TODO.restructure/66): real corpora
 * run ~0.2-0.35 nodes/byte (the legal worst, deep bracket chains,
 * reaches ~1/byte and regrows once). No input pre-pass needed. */
void yep_dom_prepare_len(yep_dom* d, size_t len);

/* builder helpers shared with mutate.c (dom-internal) */
int dom_grow_nodes(yep_dom* d, uint32_t need);
int dom_grow_docs(yep_dom* d, uint32_t need);
uint32_t dom_new_node(yep_dom* d, const yep_event* ev, uint8_t kind);
/* Node-init law shared by the event sink, the mutation builder and the
 * direct builders (no yep_event plumbing): creates and initializes a
 * node; UINT32_MAX = OOM. */
uint32_t dom_open_node(yep_dom* d, uint8_t kind, const yep_view* tag, const yep_view* anchor,
                       int anchor_borrowed, uint8_t style, uint8_t implicit, uint8_t flow);
void dom_link(yep_dom* d, uint32_t parent, uint32_t child);

/* Places a built node: at depth 0 it becomes a document root, inside a
 * sequence it links as a child, inside a map the pairing law applies
 * (first placement = key, second = value). Shared by the event sink,
 * the direct builders, and the CBOR decoder. 0 ok, -1 OOM. */
int dom_place(yep_dom* d, uint32_t id);

/* Mutation-only facts (64-2a): the attached flag and node depth read
 * through lazily-grown side tables; parse-built nodes are unattached
 * with depth 0 until a mutation touches them. */
int dom_mut_att(const yep_dom* d, uint32_t id);
void dom_mut_set_att(yep_dom* d, uint32_t id, int v);
uint16_t dom_mut_depth(const yep_dom* d, uint32_t id);
void dom_mut_set_depth(yep_dom* d, uint32_t id, uint16_t depth);

/* thread-safe handle arena (see hpool.c) */
struct yep_hpool* yep_hpool_create(const yep_allocator* sys);
/* Lazy handle-pool acquisition (#157): NULL dom or OOM stays NULL. */
struct yep_hpool* yep_dom_handles(yep_dom* d);
/* #377: the lazy indexed-children cache (seq_at/map_at's O(1) leg) */
void dom_invalidate_child_cache(yep_dom* d);
uint32_t dom_indexed_child(yep_dom* d, uint32_t cid, size_t index, uint32_t* count_out);
void yep_hpool_destroy(struct yep_hpool* p);
void* yep_hpool_alloc(struct yep_hpool* p, size_t size, size_t align);

/* mutate.c — the from-scratch builder (TODO.impl/11 phase 3).
 *
 * Values passed at runtime have no input buffer to borrow from, so
 * they are copied into the DOM pool. Plain scalars are typed by the
 * core12 resolver (the typing SSOT — mutation never invents types);
 * quoted/block styles are strings. Synthesized documents resolve with
 * the default schema; a compat-mode constructor arrives with 15.
 *
 * Returns: node ids (UINT32_MAX = OOM); statuses 0 ok, -1 invalid
 * argument / OOM, -2 duplicate key (add), -3 would exceed
 * YEP_DOM_MAX_DEPTH, -4 node already attached (would alias a parent).
 */
uint32_t yep_mut_scalar(yep_dom* d, const char* value, size_t len, uint8_t style);
uint32_t yep_mut_seq(yep_dom* d, uint8_t flow);
uint32_t yep_mut_map(yep_dom* d, uint8_t flow);
int yep_mut_seq_add(yep_dom* d, uint32_t seq, uint32_t child);
int yep_mut_map_add(yep_dom* d, uint32_t map, const char* key, size_t klen, uint32_t value);
/* Replace-in-place (json-c semantics: position kept); 1 = replaced. */
int yep_mut_map_set(yep_dom* d, uint32_t map, const char* key, size_t klen, uint32_t value);
int yep_mut_seq_del(yep_dom* d, uint32_t seq, uint32_t index);

/* Node props on synthesized trees (15's YAMLTree): values are copied
 * into the arena. set_alias_target links an alias node at the target
 * (the emitter resolves and re-emits the name). */
int yep_mut_set_anchor(yep_dom* d, uint32_t node, const char* name, size_t len);
int yep_mut_set_tag(yep_dom* d, uint32_t node, const char* tag, size_t len);
uint32_t yep_mut_new_alias(yep_dom* d, uint32_t target, const char* name, size_t len);
/* Replace the entry at index (json-c array_put_idx): unlinks the old
 * subtree and splices the new node in at the SAME position. */
int yep_mut_seq_set(yep_dom* d, uint32_t seq, uint32_t index, uint32_t value);
int yep_mut_map_del(yep_dom* d, uint32_t map, const char* key, size_t klen);

/* Appends a pre-built (key,value) pair (the generator path: keys are
 * constructed nodes, not caller strings). Duplicate keys are
 * REJECTED like map_add. */
int yep_mut_map_add_node(yep_dom* d, uint32_t map, uint32_t key, uint32_t value);
int yep_mut_map_append(yep_dom* d, uint32_t map, uint32_t key, uint32_t value);
int yep_mut_add_root(yep_dom* d, uint32_t node);
void yep_mut_set_depths(yep_dom* d, uint32_t id, uint16_t depth);

/* The sink's flow fast path (TODO.restructure/50): builds the
 * strictly-validated span [open, close] directly through the DOM's own
 * placement/link/anchor laws — node-for-node identical to the event
 * path (the flow-direct-diff ctest pins it). 1 = built, <0 = abort. */
/* dom_from_tape — the lazy-DOM seam (#342 slice 1): builds the node
 * tree from the tape's records, mirroring dom_on_flow_build's arms
 * (borrowed spans for clean strings, an arena unescape for escaped
 * ones, typed literals by kind). The records are pre-validated, so
 * the walk is placement only. Returns 0, or -1 (allocation/depth/an
 * unexpected record). STRICT tapes only — YEP_T_NUM (the lenient
 * route's deferred numbers) is rejected. */
int dom_from_tape(yep_dom* d, const yeptris_json_tape* t);

int dom_on_flow_build(void* ctx, const char* p, size_t open, size_t len, uint32_t line,
                      size_t line_start, yep_view anchor, yep_view tag, uint32_t anchor_id,
                      int max_depth, size_t* close);
int dom_on_flow_commit(void* ctx);
void dom_on_flow_rollback(void* ctx);
struct yep_block_value;

/* The direct scalar sink path (TODO.restructure/79). */
int dom_on_scalar(void* ctx, const yep_view* value, const yep_view* tag, const yep_view* anchor,
                  uint32_t anchor_id, uint8_t tag_id, uint8_t style, uint8_t implicit, uint8_t flow,
                  int borrowed);

int dom_on_block_pair(void* ctx, const yep_view* key, const struct yep_block_value* v,
                      uint32_t line, uint32_t key_col, uint32_t val_col);
int dom_on_block_open(void* ctx, const yep_view* key, uint32_t line, uint32_t key_col);
int dom_on_block_item(void* ctx, const yep_block_value* v);

/* Direct DOM construction from a strict-validated JSON buffer
 * (TODO.impl/27): no engine, no event pipeline. The caller validated
 * with yep_json_document; 0 on success, -1 OOM, -2 grammar surprise
 * (defensive). */
int yep_dom_build_json(yep_dom* d, const char* buf, size_t len);

/* Appends len bytes to the string arena; returns the ARENA-offset
 * encoding (input_base-relative encodings use yep_sv_input). */
yep_sview yep_dom_str_put(yep_dom* d, const char* p, uint32_t len);

/* Reserve/commit for in-place decoding (the JSON builder's escape
 * path): tail ensures capacity and returns the write position at
 * str_len; commit advances by the written length and returns the
 * arena-encoded view. */
char* yep_dom_str_tail(yep_dom* d, uint32_t cap);
yep_sview yep_dom_str_commit(yep_dom* d, uint32_t written);
void yep_dom_destroy(yep_dom* d);

/* The document's implicit-typing resolver (its own, or the core-12
 * default) — the SSOT the mutation and build paths must share. */
struct yep_resolver;
const struct yep_resolver* yep_dom_resolver(const yep_dom* d);
int yep_dom_on_event(void* ctx, const yep_event* ev);

/* Node id accessor for the public API layer. */
const yep_dnode* yep_dom_node(const yep_dom* d, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* YEP_DOM_H */
