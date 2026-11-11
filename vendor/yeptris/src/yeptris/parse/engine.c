/* engine.c — the block machine + flow kernel (TODO.impl/07).
 *
 * One engine, two kernels: block context is a line/indent loop over an
 * explicit frame stack; '[' or '{' switches to the iterative flow kernel.
 * No recursion anywhere (depth = frame stack depth, guarded).
 *
 * Key invariants:
 *  - MAP_START always precedes its key events, including flow keys
 *    (a flow node that turns out to be a key is pre-scanned with
 *    e_skip_flow before its events are emitted);
 *  - a collection frame is pushed exactly once: sibling entries at the
 *    frame's column reuse the open frame;
 *  - anchors defined before use (engine tracks names; the DOM binds nodes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/chartype.h"
#include "common/nametab.h"
#include "common/simd_text.h"
#include "encoding/encoding.h"
#include "engine.h"
#include "memory/allocator.h"
#include "memory/pool.h"
#include "parse/scalars.h"
#include "resolve/resolver.h"
#include "scan/json.h"
#include "scan/scan.h"
#include <yeptris/resolve.h> /* YEPTRIS_TAG_* names (#168) */

#define YEP_MAX_DEPTH 1000
#define YEP_MAX_FOLD_LINES 8192
#define YEP_MAX_SIMPLE_KEY                                                                         \
    1024 /* YAML 1.2: simple keys are one line,                                                    \
          * at most 1024 characters (libyaml parity) */

typedef enum { YEP_FRAME_SEQ = 0, YEP_FRAME_MAP } yep_frame_kind;

typedef enum {
    YEP_CTX_FRESH = 0,   /* fresh line entry: may open collections */
    YEP_CTX_AFTER_DASH,  /* value of a seq entry: may open collections (compact) */
    YEP_CTX_AFTER_COLON, /* value of a map key: collections via following lines only */
    YEP_CTX_AFTER_Q,     /* explicit-key content: may ALIGN with the '?' column */
    YEP_CTX_VALUE_LINE,  /* value of "key:" on a FOLLOWING line: indentless
                          * sequences allowed, same-line prohibitions lifted */
} yep_ctx;

/* One open collection frame (the engine's indentation stack). */
typedef struct {
    uint32_t col;
    uint8_t kind;
} yep_frame;

/* One repeat-alias memo slot: the name is an input-relative span
 * (alias names are always raw input views), so at+len re-derive it. */
typedef struct e_alias_slot {
    uint32_t at;  /* offset of the name inside e->p */
    uint32_t len; /* 0 = slot empty */
    uint32_t ord; /* the ordinal the nametab answered */
} e_alias_slot;

struct yep_engine {
    const yep_allocator* sys;
    yep_pool* pool; /* finish pool: folded/escaped scalar content */
    const char* p;
    size_t len;
    size_t pos;
    uint32_t line;              /* 1-based current line */
    size_t line_start;          /* offset of the current line start */
    yep_line_info li_cache;     /* per-line scan_line memo: every site
           positioned at line_start reads it (plain continuation checks,
           literal blocks, the main loop), so a line is scanned once */
    uint32_t li_cache_line;     /* 0 = no entry (line numbers are 1-based) */
    yep_line_shape shape_cache; /* per-line shape memo (TODO.restructure/49):
                                   the dispatch decision is made once */
    uint32_t shape_cache_line;  /* 0 = no entry */
    uint32_t line_base;         /* lines consumed by earlier stepped runs */
    yep_error err;
    const yep_sink* sink;

    yep_frame frames[YEP_MAX_DEPTH];
    int depth;

    int front_done;         /* the encoding front-end ran on this engine:
                             * parse/values pre-strip and set it; later
                             * chunked runs never re-sniff (mid-stream
                             * U+FEFF is content, not a mark) */
    yep_nametab anchors;    /* name interning: O(1) define/lookup at any scale */
    uint32_t anchor_serial; /* 1-based ordinals; the value IS the id */
    uint8_t* anchor_placed; /* per-ordinal: the anchor's NODE has been
                             * placed in the output (an alias to an
                             * unplaced anchor would precede its
                             * definition and the re-parse would reject
                             * the document — the nightly's run
                             * 38541356075) */
    uint32_t anchor_placed_cap;
    /* repeat-alias memo (TODO.restructure/71): offset/len/ordinal of
     * the last alias resolved — a hit answers by an input-span word
     * compare (yep_view_eq; no libc call), not by the table.
     * anchor_define drops the slot when the name being (re)defined is
     * the memo's: the table's last-definition-wins overwrite would
     * otherwise leave the memo serving a STALE ordinal (and skip the
     * new ordinal's unplaced check). */
    e_alias_slot alias_memo;
    /* the line-facts memo (TODO.restructure/76): one kernel pass per
     * line feeds both the line info and the shape */
    yep_line_facts facts_cache;
    uint32_t facts_cache_line;

    yep_fold_line fold[YEP_MAX_FOLD_LINES];
    size_t fold_n;

    /* properties that belong to a value parsed on following lines */
    yep_view pend_anchor, pend_tag;
    uint32_t pend_anchor_id; /* the pend anchor's ordinal */

    int doc_content;       /* any node emitted for the current document */
    int parsing_final;     /* engine_run_impl's emit_end: the buffer is the
                            * complete input (pre-scan errors are real) vs a
                            * partial step feed (an unclosed flow is
                            * incomplete, not malformed) */
    int q_key_pending;     /* an explicit '?' key awaits its ':' value line */
    uint32_t cv_line;      /* the line whose colon-value is being parsed: a
                            * key indicator on it sits where a value belongs
                            * (": ?") — libyaml rejects */
    int qmd_stack[16];     /* the '?' pending pairs' map depths — a STACK:
                            * nested '? ?' pairs stack two pendings and a
                            * scalar slot let the inner clobber the
                            * outer's level (the nightly's run 38387614146) */
    uint32_t qmd_line[16]; /* each pending's open line: a LATER-line '?'
                            * at the same level completes the previous
                            * pair (libyaml's same-indent KEY rule) */
    int qmd_n;
    int q_value_pending; /* an explicit key was emitted; its value is null
                            until a ':' line supplies one */
    /* %TAG handle map for the current document (engine = tag SSOT) */
    struct {
        yep_view handle, prefix;
    } tagmap[8];
    int tagmap_n;
    int saw_yaml;            /* %YAML seen for the pending document */
    size_t doc_begin;        /* #179: first directive line of the pending doc
                              * (SIZE_MAX = none) — libyaml's DOCUMENT_START
                              * start_mark includes directives. An offset into
                              * THIS run's buffer. */
    uint32_t doc_begin_line; /* the same directive line as a stream-
                              * absolute LINE: offsets die at a step cut
                              * (the tail is memmoved), but a directive
                              * line split from its --- must still mark
                              * the DOCUMENT_START (#645's neighbor:
                              * whole 85:1 vs chunked 86:1) */
    void* step;              /* yep_stepstate: resumable stepping (07) */

    /* Flow single-pair deferral: a sequence entry's events are buffered
     * until we know whether ':' follows ("[a: b]" needs MAP_START before
     * the key's events, but ':' is only seen after them). */
    yep_event* ev_buf;
    uint32_t ev_buf_n, ev_buf_cap;
    uint32_t ev_scopes; /* active entry-buffer scopes */
    char* norm_buf;
    size_t norm_buf_len; /* norm_buf's byte length (NULL when no copy) */
    unsigned char* enc_buf;
    /* owned UTF-16/32 transcode scratch (run-owned) */ /* owned input copy with exotic breaks
                                                           normalized */
    const struct yep_resolver* resolver;                /* implicit-typing schema (10) */
    int max_depth;                                      /* runtime nesting limit (options) */
    uint16_t rec;                                       /* e_node<->e_parse_value call depth:
                                                           the props/value-line chain opens NO
                                                           container frame, so e->depth cannot
                                                           bound it (the nightly's stack
                                                           overflow crasher) */
    int last_root_flow;                                 /* a root-level flow node just completed */
    int tag_undef;                                      /* an unresolved !handle! was seen (QLJ7) */
    int doc_inline;      /* the node opens on the --- line: block collections
                          * refuse there (the marker-line law, #900) */
    uint32_t flow_floor; /* parent column of the current block-level flow */
    int flow_enforce;    /* flow continuation lines must out-indent it */
    int compat_grammar;  /* 11-COMPAT: libyaml/psych grammar parity —
                          * the flow indent floor (9C9N/VJP3) is a
                          * yaml-test-suite rule libyaml never
                          * enforced, and real-world locale files
                          * close multi-line flows at the parent's
                          * column; the floor stays on for the
                          * strict 1.2 engine the suite runs */
};

/* ------------------------------------------------------------ forwards */

static int e_node(yep_engine* e, yep_ctx ctx, uint32_t floor_col);
static int e_parse_value(yep_engine* e, yep_ctx ctx, uint32_t floor_col);

/* ---------------------------------------------------------------- util */

static uint32_t e_col(const yep_engine* e, size_t at) {
    return at >= e->line_start ? (uint32_t)(at - e->line_start) : 0;
}

/* #179: the (1-based line, 1-based col) of a byte offset. Unlike
 * e_col this handles offsets on PRIOR lines — event ends whose scan
 * already advanced past them (block scalars, collection closes).
 * The backward walk covers only the span's own lines, so the total
 * across a document stays linear in its size. */
static void e_pos_mark(const yep_engine* e, size_t at, uint32_t* line, uint32_t* col) {
    if (at >= e->line_start) {
        *line = e->line;
        *col = (uint32_t)(at - e->line_start) + 1;
        return;
    }
    uint32_t l = e->line;
    size_t ls = e->line_start;
    while (at < ls && ls > 0) {
        /* p[ls-1] is the newline ENDED the previous line: start the
         * walk below it, or ls never decreases (the infinite-loop bug
         * the Stepping.Directive hang found) */
        size_t q = ls - 1;
        while (q > 0 && e->p[q - 1] != '\n' && e->p[q - 1] != '\r') {
            q--;
        }
        ls = q; /* line_start(l-1) */
        l--;
    }
    *line = l;
    *col = (uint32_t)(at - ls) + 1;
}

static int e_fail(yep_engine* e, yep_err_code code, size_t at) {
    yep_error_set(&e->err, code, e->line, e_col(e, at) + 1, at, NULL);
    return -1;
}

/* e_fail with an explicit message: the document-boundary diagnostics
 * spell libyaml's exact words (issue #900 — the oracle's message
 * surface, not the generic default-arm string). */
static int e_fail_msg(yep_engine* e, yep_err_code code, size_t at, const char* msg) {
    yep_error_set(&e->err, code, e->line, e_col(e, at) + 1, at, "%s", msg);
    return -1;
}

/* A simple key (no explicit '?') is limited to one line and 1024
 * characters. The span runs from the node's first byte (opening
 * quote, alias star — not its anchor/tag properties) to the ':'. */
static int e_simple_key_ok(yep_engine* e, size_t key_at, size_t colon_at) {
    if (colon_at - key_at > YEP_MAX_SIMPLE_KEY) {
        return e_fail(e, YEP_ERR_KEY_TOO_LONG, key_at);
    }
    return 0;
}

static void e_event_init(yep_event* ev, yep_event_type t) {
    memset(ev, 0, sizeof(*ev));
    ev->type = t;
}

static void e_anchor_mark(yep_engine* e, uint32_t id);

static int emit_now(yep_engine* e, const yep_event* ev) {
    e_anchor_mark(e, ev->anchor_id);
    if (ev->tag.len > 0) {
        /* Explicit-tag resolution (the typing SSOT for SCALARS AND
         * COLLECTIONS): an event's tag_id names the explicit tag's id,
         * so the family walk's consistency check can tell `!!map` on a
         * mapping, `!!seq` on a sequence and a consumer-defined custom
         * tag (CUSTOM, no family verdict) apart — a collection event's
         * id used to stay 0 ("str"), which made EVERY tagged collection
         * read as `!!str` and diagnose a false mismatch (#906 slice 4's
         * tag-driven lane needs the true id). Both routes read this
         * field, so the DOM and event walks stay in lockstep. */
        yep_tag_id tid = yep_tag_from_uri((const char*)ev->tag.p, ev->tag.len);
        /* psych's own dumped form is the LOCAL !binary shorthand,
         * and stdlib to_ruby accepts it beside the core URI (#168)
         * — compat schemas resolve it to BINARY as well */
        if (ev->type == YEP_EV_SCALAR && tid == YEPTRIS_TAG_CUSTOM && ev->tag.len == 7 &&
            memcmp(ev->tag.p, "!binary", 7) == 0 && e->resolver == yep_resolver_compat11()) {
            tid = YEPTRIS_TAG_BINARY;
        }
        ((yep_event*)ev)->tag_id = tid;
    } else if (ev->type == YEP_EV_SCALAR) {
        /* implicit typing: scalars only (a collection's id stays 0,
         * its "no tag" value) */
        if (ev->implicit && e->resolver != NULL) {
            ((yep_event*)ev)->tag_id = e->resolver->resolve(
                e->resolver->ctx, (const char*)ev->value.p, (uint32_t)ev->value.len);
        } else {
            ((yep_event*)ev)->tag_id = 0; /* str: quoted or tag-free */
        }
    }
    if (ev->type == YEP_EV_SCALAR && e->ev_scopes == 0 && e->sink != NULL &&
        e->sink->on_scalar != NULL) {
        return e->sink->on_scalar(e->sink->ctx, &ev->value, &ev->tag, &ev->anchor, ev->anchor_id,
                                  ev->tag_id, ev->style, ev->implicit, ev->flow, ev->borrowed);
    }
    if (e->ev_scopes > 0) {
        if (e->ev_buf_n == e->ev_buf_cap) {
            uint32_t cap = e->ev_buf_cap ? e->ev_buf_cap * 2 : 16;
            yep_event* nb = yep_pool_alloc(e->pool, (size_t)cap * sizeof(yep_event), 16);
            if (nb == NULL) {
                e_fail(e, YEP_ERR_MEMORY, e->pos);
                return -2;
            }
            if (e->ev_buf_n > 0) {
                memcpy(nb, e->ev_buf, (size_t)e->ev_buf_n * sizeof(yep_event));
            }
            e->ev_buf = nb;
            e->ev_buf_cap = cap;
        }
        e->ev_buf[e->ev_buf_n++] = *ev;
        return 0;
    }
    return e->sink ? e->sink->on_event(e->sink->ctx, ev) : 0;
}

/* Emits buffered events from `from` to the sink and truncates. */
static int e_buf_send(yep_engine* e, uint32_t from) {
    for (uint32_t i = from; i < e->ev_buf_n; i++) {
        if (e->sink && e->sink->on_event(e->sink->ctx, &e->ev_buf[i]) != 0) {
            return -2;
        }
    }
    e->ev_buf_n = from;
    return 0;
}

/* Inserts a flow MAP_START at `from`, wrapping the buffered entry. */
static int e_buf_wrap(yep_engine* e, uint32_t from) {
    if (e->ev_buf_n >= e->ev_buf_cap) {
        uint32_t cap = e->ev_buf_cap ? e->ev_buf_cap * 2 : 16;
        yep_event* nb = yep_pool_alloc(e->pool, (size_t)cap * sizeof(yep_event), 16);
        if (nb == NULL) {
            e_fail(e, YEP_ERR_MEMORY, e->pos);
            return -2;
        }
        if (e->ev_buf_n > 0) {
            memcpy(nb, e->ev_buf, (size_t)e->ev_buf_n * sizeof(yep_event));
        }
        e->ev_buf = nb;
        e->ev_buf_cap = cap;
    }
    memmove(&e->ev_buf[from + 1], &e->ev_buf[from],
            (size_t)(e->ev_buf_n - from) * sizeof(yep_event));
    e_event_init(&e->ev_buf[from], YEP_EV_MAP_START);
    e->ev_buf[from].flow = 1;
    e->ev_buf_n++;
    return 0;
}

static void e_line_done(yep_engine* e, size_t at) {
    size_t br = yep_scan_break_len(e->p, e->len, at);
    if (br > 0) {
        e->pos = at + br;
        e->line++;
        e->line_start = e->pos;
        e->li_cache_line = 0;
        e->shape_cache_line = 0;
        e->facts_cache_line = 0;
    } else {
        e->pos = at;
    }
}

/* The line-facts memo (TODO.restructure/76): ONE kernel pass per
 * line; li and the shape both derive from it. */
static const yep_line_facts* e_facts_here(yep_engine* e) {
    if (e->facts_cache_line != e->line) {
        yep_scan_facts(e->p, e->len, e->line_start, &e->facts_cache);
        e->facts_cache_line = e->line;
        e->li_cache_line = 0; /* derived caches are stale now */
        e->shape_cache_line = 0;
    }
    return &e->facts_cache;
}

/* scan_line for the current line, memoized for the flow loop (the
 * loop head and the post-ws check each need it every line). */
static yep_line_info e_line_info_here(yep_engine* e) {
    const yep_line_facts* f = e_facts_here(e);
    if (e->li_cache_line != e->line) {
        yep_scan_line_f(e->p, e->len, e->line_start, f, &e->li_cache);
        e->li_cache_line = e->line;
    }
    return e->li_cache;
}

/* The line's classified shape, memoized beside the line facts: the
 * engine decides dispatch ONCE per line (TODO.restructure/49). */
static const yep_line_shape* e_shape_here(yep_engine* e) {
    const yep_line_facts* f = e_facts_here(e);
    e_line_info_here(e);
    if (e->shape_cache_line != e->line) {
        yep_scan_shape_f(e->p, e->len, &e->li_cache, f, &e->shape_cache);
        e->shape_cache_line = e->line;
    }
    return &e->shape_cache;
}

static void e_skip_inline_space(yep_engine* e) {
    while (e->pos < e->len && (e->p[e->pos] == ' ' || e->p[e->pos] == '\t')) {
        e->pos++;
    }
}

/* '#' only opens a comment when a blank (or nothing) precedes it. */
static int e_comment_at(const yep_engine* e) {
    return e->p[e->pos] == '#' &&
           (e->pos == 0 || e->p[e->pos - 1] == ' ' || e->p[e->pos - 1] == '\t' ||
            e->p[e->pos - 1] == '\n' || e->p[e->pos - 1] == '\r');
}

static int e_at_eol(yep_engine* e) {
    return e->pos >= e->len || e->p[e->pos] == '\n' || e->p[e->pos] == '\r' || e_comment_at(e) ||
           yep_scan_mb_break_len(e->p, e->len, e->pos) > 0;
}

static int e_skip_to_eol(yep_engine* e) {
    while (e->pos < e->len && e->p[e->pos] != '\n' && e->p[e->pos] != '\r') {
        e->pos++;
    }
    return 0;
}

/* ':' terminates a node here (blank/EOL after; flow indicators are
 * handled by the flow kernel's own check). */
static int e_colon_at(yep_engine* e, size_t at) {
    if (at >= e->len || e->p[at] != ':') {
        return 0;
    }
    size_t n = at + 1;
    return n >= e->len || e->p[n] == ' ' || e->p[n] == '\n' || e->p[n] == '\r';
}

/* -------------------------------------------------------------- anchors */

/* The ordinal the nametab assigned at define time (0 when absent). */
static uint32_t anchor_id_of(yep_engine* e, yep_view name) {
    /* Repeat-alias memo (TODO.restructure/71): merge-key YAML re-uses
     * the same alias thousands of times in a row; the memo answers
     * those with one input compare, skipping the hash + a likely
     * cache-missing probe into the (multi-MB, reserved) slots table. */
    if (e->alias_memo.len == name.len && name.len != 0 &&
        yep_view_eq((yep_view){e->p + e->alias_memo.at, name.len}, name)) {
        return e->alias_memo.ord;
    }
    uint32_t id = yep_nametab_get(&e->anchors, name);
    if (id == YEP_NAMETAB_NIL) {
        return 0;
    }
    e->alias_memo.at = (uint32_t)(name.p - e->p);
    e->alias_memo.len = name.len;
    e->alias_memo.ord = id;
    return id;
}

/* The anchor's node has been PLACED (an event emitted, or the node
 * handed to a fused sink callback): aliases to it may parse. */
static void e_anchor_mark(yep_engine* e, uint32_t id) {
    if (id != 0 && id <= e->anchor_serial && e->anchor_placed != NULL) {
        e->anchor_placed[id - 1] = 1;
    }
}

/* Grows the per-ordinal placed flags so flags[id-1] is writable.
 * Returns 0 only on allocation failure (table unchanged). */
static int anchor_placed_fit(yep_engine* e, uint32_t id) {
    if (id <= e->anchor_placed_cap) {
        return 1;
    }
    uint32_t cap = e->anchor_placed_cap ? e->anchor_placed_cap : 64;
    while (cap < id) {
        cap *= 2;
    }
    uint8_t* nb = yep_alloc(e->sys, cap);
    if (nb == NULL) {
        return 0;
    }
    if (e->anchor_placed != NULL) {
        memcpy(nb, e->anchor_placed, e->anchor_placed_cap);
        yep_free(e->sys, e->anchor_placed);
    }
    memset(nb + e->anchor_placed_cap, 0, cap - e->anchor_placed_cap);
    e->anchor_placed = nb;
    e->anchor_placed_cap = cap;
    return 1;
}

static uint32_t anchor_define(yep_engine* e, yep_view name) {
    /* Last definition wins (libyaml parity); OOM leaves the anchor
     * undefined so its alias errors. The VALUE is the anchor's 1-based
     * ordinal — sinks key on it and skip the name hash entirely. */
    uint32_t id = ++e->anchor_serial;
    if (!anchor_placed_fit(e, id)) {
        e->anchor_serial--;
        return 0; /* OOM: the anchor stays undefined, its alias errors */
    }
    yep_nametab_set(&e->anchors, name, id);
    /* A (re)definition changes the ordinal the table answers for THIS
     * name: a memo still naming it would serve the OLD ordinal
     * (violating last-definition-wins) and skip the new ordinal's
     * unplaced check. */
    if (e->alias_memo.len == name.len && name.len != 0 &&
        yep_view_eq((yep_view){e->p + e->alias_memo.at, name.len}, name)) {
        e->alias_memo.len = 0;
    }
    return id;
}

/* --------------------------------------------------------------- frames */

/* Opens a collection frame unless the top frame already continues at
 * exactly this column (sibling entries reuse it). anchor/tag ride the
 * START event. Returns 0 on success. */
static int e_open_seq(yep_engine* e, uint32_t col, uint32_t line, uint32_t coln, yep_view anchor,
                      yep_view tag, uint32_t anchor_id) {
    if (e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_SEQ &&
        e->frames[e->depth - 1].col == col) {
        return 0;
    }
    if (e->doc_inline) {
        /* the marker-line law (issue #900): a block collection cannot
         * open on the --- line. libyaml's scanner disables block
         * entries after the document-start indicator until a line
         * break; libyaml 0.2.5/90a56d4, Psych 5 and PyYAML (pure and
         * C) all reject ("block sequence entries are not allowed in
         * this context"). The suite has no seq-class pin — its
         * 9KBC/CXX2 fail-pins cover the mapping twin in e_open_map —
         * so the oracle's law governs here. The cursor sits at the
         * dash, which is libyaml's problem mark. */
        return e_fail_msg(e, YEP_ERR_DOC_BOUNDARY, e->pos,
                          "block sequence entries are not allowed in this context");
    }
    if (e->depth >= e->max_depth) {
        return e_fail(e, YEP_ERR_DEPTH, e->pos);
    }
    e->frames[e->depth].col = col;
    e->frames[e->depth].kind = YEP_FRAME_SEQ;
    e->depth++;
    yep_event ev;
    e_event_init(&ev, YEP_EV_SEQ_START);
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.tag = tag;
    ev.line = line;
    ev.col = coln;
    ev.end_line = line; /* block opens are zero-span (libyaml) — except
                         * the sequence at its parent mapping's column
                         * ("k:\n- a"), which ends after the dash */
    ev.end_col = coln;
    for (int i = e->depth - 1; i >= 0; i--) {
        if (e->frames[i].kind == YEP_FRAME_MAP) {
            if (col == e->frames[i].col) {
                ev.end_col = coln + 1;
            }
            break;
        }
    }
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* silent: the sink's on_block_open built the node — push the frame
 * without emitting (TODO.restructure/57). */
static int e_open_map(yep_engine* e, uint32_t col, uint32_t line, uint32_t coln, yep_view anchor,
                      yep_view tag, uint32_t anchor_id, int silent) {
    if (e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
        e->frames[e->depth - 1].col == col) {
        return 0;
    }
    if (e->doc_inline) {
        /* the marker-line law (issue #900): a block mapping cannot open
         * on the --- line. libyaml's scanner keeps simple keys disabled
         * after the document-start indicator until a line break, so the
         * pair separator reads as "mapping values are not allowed in
         * this context" (the explicit-key form: "mapping keys ...");
         * libyaml 0.2.5/90a56d4, Psych 5 and PyYAML (pure and C) all
         * reject, and the suite pins the class INVALID (9KBC/CXX2).
         * The mark is the pair separator itself — libyaml's problem
         * mark ("--- a: 1" marks 1:6, the colon). */
        size_t at = e->pos;
        const char* msg;
        if (at < e->len && e->p[at] == '?') {
            msg = "mapping keys are not allowed in this context";
        } else {
            msg = "mapping values are not allowed in this context";
            while (at < e->len && e->p[at] != '\n' && e->p[at] != '\r') {
                if (e->p[at] == ':' &&
                    (at + 1 >= e->len || e->p[at + 1] == ' ' || e->p[at + 1] == '\t' ||
                     e->p[at + 1] == '\n' || e->p[at + 1] == '\r')) {
                    break;
                }
                at++;
            }
        }
        return e_fail_msg(e, YEP_ERR_DOC_BOUNDARY, at, msg);
    }
    if (e->depth >= e->max_depth) {
        return e_fail(e, YEP_ERR_DEPTH, e->pos);
    }
    e->frames[e->depth].col = col;
    e->frames[e->depth].kind = YEP_FRAME_MAP;
    e->depth++;
    if (silent) {
        return 0;
    }
    yep_event ev;
    e_event_init(&ev, YEP_EV_MAP_START);
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.tag = tag;
    ev.line = line;
    ev.col = coln;
    ev.end_line = line; /* block opens are zero-span (libyaml) */
    ev.end_col = coln;
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* Emits the null value for an explicit key that never got its ':' line. */
static int e_flush_q_value(yep_engine* e) {
    if (!e->q_value_pending || e->qmd_n == 0) {
        return 0;
    }
    int top = e->qmd_stack[e->qmd_n - 1];
    if (e->depth != top) {
        return 0; /* the pending pair's map is not the current level */
    }
    e->qmd_n--;
    /* nested '?' pairs stack one pending per level — the flag stays up
     * while the stack still holds one, so the outer null flushes at its
     * own level (the nightly's run 38387614146: `? ? k : a` lost it) */
    e->q_value_pending = (e->qmd_n > 0);
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* The pair-value completion: every `key: value` arm funnels its value
 * parse through here. Under an AFTER_Q parse (a nested '?' pair whose
 * value arrived inline) the innermost '?' pending resolves — without
 * this pop the stack keeps a stale entry and the outer pair's null
 * flushes inside the wrong map (the nightly's run 38387614146). */
static int e_pair_value_done(yep_engine* e, yep_ctx ctx, uint32_t key_col) {
    /* the pair's own level, BEFORE its value parse: the value may open
     * deeper frames ("? a:\n   ? b<EOF>") and a post-parse depth read
     * then matches the DEEPER pair's pending slot, popping the wrong
     * entry and stranding the outer null (the nightly's round 24,
     * crash-bcc0f52c) */
    int pre_depth = e->depth;
    int rc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
    if (rc >= 0 && ctx == YEP_CTX_AFTER_Q && e->qmd_n > 0 &&
        pre_depth == e->qmd_stack[e->qmd_n - 1]) {
        e->qmd_n--; /* the inline value resolved the innermost '?' pending */
    }
    return rc;
}

static int e_close_to(yep_engine* e, int to_depth) {
    while (e->depth > to_depth) {
        /* the pending '?' value flushes once the key's structure closed */
        int rc = e_flush_q_value(e);
        if (rc != 0) {
            return rc;
        }
        if (e->q_value_pending && e->qmd_n > 0 && e->depth == e->qmd_stack[e->qmd_n - 1]) {
            continue; /* flushed the null; this frame closes next round */
        }
        e->depth--;
        yep_event ev;
        e_event_init(&ev,
                     e->frames[e->depth].kind == YEP_FRAME_SEQ ? YEP_EV_SEQ_END : YEP_EV_MAP_END);
        { /* libyaml: a block END's mark is the next token's position
           * (zero-span) — the first non-space at the cursor, or EOF */
            size_t at = e->pos;
            while (at < e->len && e->p[at] == ' ') {
                at++;
            }
            e_pos_mark(e, at, &ev.line, &ev.col);
            ev.end_line = ev.line;
            ev.end_col = ev.col;
        }
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }
    return 0;
}

/* ------------------------------------------------------------- scalars */

/* Quoted scalar at the opening quote; spans lines when needed. */
static int e_quoted_floor(yep_engine* e, yep_event* ev, uint32_t min_indent, int enforce) {
    unsigned char q = (unsigned char)e->p[e->pos];
    size_t content_start = e->pos + 1;
    int has_esc = 0;
    size_t close;
    {
        const yep_text_kernels* k = yep_text_active();
        ptrdiff_t r =
            k->quote_scan(e->p + content_start, e->len - content_start, (char)q, &has_esc);
        if (r >= 0) {
            close = content_start + (size_t)r;
        } else {
            /* unterminated on this span: manual multi-line scan */
            size_t i = content_start;
            close = e->len + 1;
            while (i < e->len) {
                if (q == '"' && e->p[i] == '\\') {
                    i += 2;
                    has_esc = 1;
                    continue;
                }
                if ((unsigned char)e->p[i] == q) {
                    if (q == '\'' && i + 1 < e->len && e->p[i + 1] == '\'') {
                        i += 2;
                        continue;
                    }
                    close = i;
                    break;
                }
                i++;
            }
            if (close == e->len + 1) {
                return e_fail(e, YEP_ERR_UNTERMINATED_QUOTE, e->pos);
            }
        }
    }

    uint32_t start = (uint32_t)content_start;
    uint32_t end = (uint32_t)close;
    int multiline = 0;
    uint32_t breaks = 0; /* '\n' occurrences, folded with detection */
    {
        const yep_text_kernels* k = yep_text_active();
        const yep_stopset* brk = &yep_break_stopset; /* scan's SSOT */
        size_t i = start;
        while (i < end) {
            ptrdiff_t r = k->stopset_find(brk, e->p + i, end - i);
            if (r < 0) {
                break; /* single-line span: one SIMD call decided it */
            }
            size_t at = i + (size_t)r;
            multiline = 1;
            if (e->p[at] == '\n') {
                breaks++;
            }
            /* each following line must be content, not a document marker,
             * and in block context must out-indent the parent (QB6E) */
            size_t br = yep_scan_break_len(e->p, e->len, at);
            yep_line_info ql = yep_scan_line(e->p, e->len, at + br);
            if (ql.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END | YEP_LF_DIRECTIVE)) {
                return e_fail(e, YEP_ERR_UNTERMINATED_QUOTE, at + br);
            }
            if (enforce && !(ql.flags & YEP_LF_BLANK) && ql.indent <= min_indent) {
                return e_fail(e, YEP_ERR_BAD_INDENT, at + br + ql.indent);
            }
            i = at + br;
        }
    }
    /* escape pre-validation (double quotes only: ' has no escapes);
     * quote_scan already reported whether any backslash exists */
    for (size_t i = (q == '"' && has_esc) ? start : end; i < end; i++) {
        if (e->p[i] != '\\' || i + 1 >= end) {
            continue;
        }
        unsigned char esc = (unsigned char)e->p[i + 1];
        if (esc == 'x' || esc == 'u' || esc == 'U') {
            /* hex escapes carry exact digit counts: 2/4/8 */
            size_t need = (esc == 'x') ? 2 : (esc == 'u') ? 4 : 8;
            if (i + 1 + need >= end) {
                return e_fail(e, YEP_ERR_INVALID_ESCAPE, i); /* truncated "\uDd" */
            }
            for (size_t k = 1; k <= need; k++) {
                if (!yep_ct_is((unsigned char)e->p[i + 1 + k], YEP_CT_HEXDIGIT)) {
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i + 1 + k);
                }
            }
            if (esc == 'u') {
                uint32_t cp = 0;
                for (size_t k = 1; k <= 4; k++) {
                    cp = (cp << 4) | (uint32_t)hexval((unsigned char)e->p[i + 1 + k]);
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* high surrogate: the low escape must follow */
                    size_t j = i + 6; /* at the low escape's backslash */
                    uint32_t lo = 0;
                    if (j + 6 <= end && e->p[j] == '\\' && e->p[j + 1] == 'u') {
                        int lok = 1;
                        for (int k = 0; k < 4; k++) {
                            int hv = hexval((unsigned char)e->p[j + 2 + (size_t)k]);
                            if (hv < 0) {
                                lok = 0;
                                break;
                            }
                            lo = (lo << 4) | (uint32_t)hv;
                        }
                        if (lok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            i = j + 5; /* at the low escape's last digit */
                            continue;
                        }
                    }
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i);
                }
                if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i); /* lone low */
                }
            }
            if (esc == 'U') {
                /* 8 hex digits reach far past U+10FFFF — the nightly's
                 * "\UA66DA66D" crasher: parse accepted an unencodable
                 * code point and the emitted bytes never re-parsed */
                uint32_t cp = 0;
                for (size_t k = 1; k <= 8; k++) {
                    cp = (cp << 4) | (uint32_t)hexval((unsigned char)e->p[i + 1 + k]);
                }
                if (cp > 0x10FFFF) {
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i);
                }
            }
            i += 1 + need; /* the digits are not escapes themselves */
            continue;
        }
        if (esc == '\n' || esc == '\r') {
            continue; /* line continuation: checked by finish_double */
        }
        switch (esc) {
        case '0':
        case 'a':
        case 'b':
        case 't':
        case '\t':
        case 'n':
        case 'v':
        case 'f':
        case 'r':
        case 'e':
        case ' ':
        case '"':
        case '/':
        case '\\':
        case 'N':
        case '_':
        case 'L':
        case 'P':
            break;
        default:
            return e_fail(e, YEP_ERR_INVALID_ESCAPE, i);
        }
        i++; /* skip the escaped byte */
    }

    ev->style = (q == '\'') ? YEP_STYLE_SINGLE_QUOTED : YEP_STYLE_DOUBLE_QUOTED;
    if (q == '\'') {
        ev->multiline = (uint8_t)multiline;
        char* out = yep_finish_single(e->p, start, end, multiline, e->pool, &ev->value.len);
        if (out == NULL) {
            ev->value.p = e->p + start;
            ev->value.len = end - start; /* finish_single returns before writing len */
            ev->borrowed = 1;
        } else {
            ev->value.p = out;
            ev->borrowed = 0;
        }
    } else if (!has_esc && !multiline) {
        ev->value.p = e->p + start;
        ev->value.len = end - start;
        ev->borrowed = 1;
    } else {
        ev->multiline = (uint8_t)multiline;
        char* out = yep_finish_double(e->p, start, end, multiline, e->pool, &ev->value.len);
        if (out == NULL) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        ev->value.p = out;
        ev->borrowed = 0;
    }

    size_t after = close + 1;
    e->line += breaks;
    /* approximate: after a multi-line scalar the column resets */
    if (multiline) {
        size_t ls = close;
        while (ls > start && e->p[ls - 1] != '\n') {
            ls--;
        }
        e->line_start = ls;
    }
    e->pos = after;
    e_pos_mark(e, after, &ev->end_line, &ev->end_col);
    e_skip_inline_space(e);
    return 0;
}

/* Block scalar (| or >) with header, auto/explicit indent, chomping. */
static int e_block_scalar(yep_engine* e, yep_event* ev, int parent_col) {
    int folded = (e->p[e->pos] == '>');
    e->pos++;
    int chomp = 0;
    int explicit_indent = 0;
    int saw_digit = 0;
    int saw_blank = 0;
    while (e->pos < e->len && e->p[e->pos] != '\n' && e->p[e->pos] != '\r') {
        char c = e->p[e->pos];
        if (c == ' ' || c == '\t') {
            saw_blank = 1;
            e->pos++;
            continue;
        }
        if (e_comment_at(e)) {
            e_skip_to_eol(e);
            break;
        }
        if (saw_blank || (c == '-' && chomp != 0) || (c == '+' && chomp != 0) ||
            (c >= '1' && c <= '9' && saw_digit)) {
            /* libyaml: the params ride the indicator DIRECTLY (a
             * spaced "| 2" is trailing content, not a param — "did
             * not find expected comment or line break"), one
             * indentation digit and one chomp sign at most (the
             * nightly's run 38119775008 family) */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (c == '-') {
            chomp = 1;
        } else if (c == '+') {
            chomp = 2;
        } else if (c >= '1' && c <= '9') {
            explicit_indent = c - '0';
            saw_digit = 1;
        } else {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        e->pos++;
    }
    e_line_done(e, e->pos);

    int content_indent =
        explicit_indent ? (parent_col < 0 ? explicit_indent : parent_col + explicit_indent) : -1;
    e->fold_n = 0;
    uint32_t pending_breaks = 0;
    uint32_t trailing_breaks = 0;
    int saw_content = 0;
    int blank_tab = 0;
    int max_blank_indent = -1;

    while (e->pos < e->len) {
        /* trailing spaces after "|-" leave pos mid-line: scan from pos
         * (the header tail), never the memo's full line */
        yep_line_info li =
            e->pos == e->line_start ? e_line_info_here(e) : yep_scan_line(e->p, e->len, e->pos);
        int blank = (li.flags & YEP_LF_BLANK) != 0;
        if (!blank) {
            if (parent_col < 0 && (li.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END))) {
                /* col-0 ---/... are document boundaries ALWAYS — even
                 * with no content yet: libyaml scans the marker as a
                 * document token regardless, so '>\n--- ' is an EMPTY
                 * block scalar plus a second (empty) document, not a
                 * block whose content is the marker line (the nightly's
                 * run 38544774709: whole saw one doc, the chunked cut
                 * at the marker saw two) */
                break;
            }
            if (saw_content && parent_col < 0 && (li.flags & YEP_LF_DIRECTIVE)) {
                /* a %-line after content ends the root block; as the
                 * FIRST line it is content (M7A3/W4TN: the block's
                 * value is '%!PS-Adobe-2.0 ...') */
                break;
            }
            if (content_indent < 0) {
                content_indent = li.indent;
                if (content_indent <= parent_col) {
                    break; /* no block content */
                }
            } else if ((int)li.indent <= parent_col) {
                break; /* dedent ends the block */
            } else if ((int)li.indent < content_indent && (int)li.indent <= parent_col) {
                break;
            }
            if ((int)li.indent < content_indent) {
                break;
            }
        }
        if (blank) {
            if (li.indent > 0 && memchr(e->p + li.offset, '\t', li.end - li.offset) != NULL) {
                /* a tab after leading spaces is CONTENT (Y79Y#2) */
                if (content_indent < 0) {
                    content_indent = li.indent;
                    if (content_indent <= parent_col) {
                        break;
                    }
                } else if ((int)li.indent < content_indent) {
                    break;
                }
                if ((int)li.indent >= content_indent) {
                    uint32_t from2 = li.offset + (uint32_t)content_indent;
                    if (e->fold_n >= YEP_MAX_FOLD_LINES) {
                        return e_fail(e, YEP_ERR_MEMORY, e->pos);
                    }
                    e->fold[e->fold_n].content.p = e->p + from2;
                    e->fold[e->fold_n].content.len = li.end > from2 ? li.end - from2 : 0;
                    e->fold[e->fold_n].breaks_before =
                        saw_content ? pending_breaks + 1 : pending_breaks;
                    e->fold[e->fold_n].more_indented = 1;
                    e->fold_n++;
                    saw_content = 1;
                    pending_breaks = 0;
                    trailing_breaks = 0;
                    /* the line's OWN break counts toward trailing (the
                     * content-line arm and the spaces-past-indent arm
                     * scan it below): consuming it silently made keep
                     * spell one break short whenever a tab-carrying
                     * content line ENDED the block ("|+\n\n >\t\n
                     *   \t \n" — libyaml keeps the final break, the
                     * nightly's round 37) */
                    {
                        size_t at = li.end;
                        size_t br = yep_scan_break_len(e->p, e->len, at);
                        if (br > 0) {
                            trailing_breaks = 1;
                            e->pos = at + br;
                            e->line++;
                            e->line_start = e->pos;
                            e->li_cache_line = 0;
                            e->shape_cache_line = 0;
                        } else {
                            e->pos = at;
                        }
                    }
                    continue;
                }
            }
            /* a blank line whose spaces extend past the content indent is
             * CONTENT (kept verbatim); otherwise it is a pure break */
            if (content_indent >= 0 && (int)li.indent > content_indent) {
                if (e->fold_n >= YEP_MAX_FOLD_LINES) {
                    return e_fail(e, YEP_ERR_MEMORY, e->pos);
                }
                e->fold[e->fold_n].content.p = e->p + li.offset + (uint32_t)content_indent;
                e->fold[e->fold_n].content.len =
                    li.end > li.offset + content_indent ? li.end - li.offset - content_indent : 0;
                e->fold[e->fold_n].breaks_before =
                    saw_content ? pending_breaks + 1 : pending_breaks;
                e->fold[e->fold_n].more_indented = 1;
                e->fold_n++;
                saw_content = 1;
                pending_breaks = 0;
                trailing_breaks = 0;
                {
                    size_t at = li.end;
                    size_t br = yep_scan_break_len(e->p, e->len, at);
                    if (br > 0) {
                        trailing_breaks = 1;
                        e->pos = at + br;
                        e->line++;
                        e->line_start = e->pos;
                        e->li_cache_line = 0;
                        e->shape_cache_line = 0;
                    } else {
                        e->pos = at;
                    }
                }
                continue;
            }
            if (!saw_content) {
                if (memchr(e->p + li.offset, '\t', li.end - li.offset) != NULL) {
                    blank_tab = 1;
                }
                if ((int)li.indent > max_blank_indent) {
                    max_blank_indent = (int)li.indent;
                }
            }
            pending_breaks++;
            e_line_done(e, li.end);
            continue;
        }
        uint32_t from = li.offset + (uint32_t)content_indent;
        if (e->fold_n >= YEP_MAX_FOLD_LINES) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        e->fold[e->fold_n].content.p = e->p + from;
        e->fold[e->fold_n].content.len = li.end > from ? li.end - from : 0;
        /* leading blank lines belong to the block content */
        e->fold[e->fold_n].breaks_before = saw_content ? pending_breaks + 1 : pending_breaks;
        e->fold[e->fold_n].more_indented =
            (int)li.indent > content_indent ||
            (from < li.end && (e->p[from] == ' ' || e->p[from] == '\t'));
        e->fold_n++;
        saw_content = 1;
        pending_breaks = 0;
        trailing_breaks = 0;
        /* the break after this content line counts toward trailing */
        size_t at = li.end;
        size_t br = yep_scan_break_len(e->p, e->len, at);
        if (br > 0) {
            trailing_breaks = 1;
            e->pos = at + br;
            e->line++;
            e->line_start = e->pos;
        } else {
            e->pos = at;
        }
    }
    trailing_breaks += pending_breaks;
    if (!saw_content && trailing_breaks > 0 && e->pos == e->len && e->len > 0 &&
        e->p[e->len - 1] != '\n' && e->p[e->len - 1] != '\r') {
        /* keep-chomping counts COMPLETE blank lines only: the final
         * blank line carries no break at EOF, so there is nothing to
         * keep ("|+\n " is "" where the pre-fix kept "\n"; two
         * complete blanks stay "\n\n" -- libyaml/psych oracle) */
        trailing_breaks--;
    }
    if (saw_content ? (max_blank_indent > content_indent) : blank_tab) {
        /* a leading blank line out-indents the first content line; a
         * tab-only line before any content is an error (5LLU/Y79Y) */
        return e_fail(e, YEP_ERR_BAD_INDENT, e->pos);
    }

    char* out = yep_finish_block(e->fold, e->fold_n, folded, chomp, trailing_breaks, e->pool,
                                 &ev->value.len);
    ev->value.p = out; /* may be NULL for an empty block; len 0 */
    ev->borrowed = 0;
    ev->style = folded ? YEP_STYLE_FOLDED : YEP_STYLE_LITERAL;
    e_pos_mark(e, e->pos, &ev->end_line, &ev->end_col);
    return 0;
}

/* Multi-line plain scalar continuation in block context. */
static int e_plain_multiline(yep_engine* e, yep_span s0, uint32_t block_floor, yep_event* ev,
                             int root_ctx) {
    /* the caller already scanned this exact span for its colon/term
     * decision — the rescan doubled every plain value's scan cost */
    e->fold_n = 0;
    {
        e->fold[0].content.p = e->p + s0.start;
        e->fold[0].content.len = s0.end - s0.start;
        e->fold[0].breaks_before = 0;
        e->fold_n = 1;
        if (s0.term == YEP_TERM_COMMENT) {
            e->pos = s0.end;
            e_skip_to_eol(e);
            return 0; /* the comment ends the scalar here */
        }
    }
    uint32_t breaks = 0;
    for (;;) {
        if (e->pos >= e->len) {
            break;
        }
        /* trailing spaces/tabs after a piece are stripped before the
         * break — they must not inflate the break count */
        while (e->pos < e->len && (e->p[e->pos] == ' ' || e->p[e->pos] == '\t')) {
            e->pos++;
        }
        if (e->pos >= e->len) {
            break;
        }
        if (e_comment_at(e)) {
            e_skip_to_eol(e);
            break; /* a comment terminates the plain scalar */
        }
        if (e->pos >= e->len) {
            break;
        }
        breaks++;
        e_line_done(e, e->pos);
        yep_line_info li = e_line_info_here(e);
        if (li.flags & YEP_LF_BLANK) {
            /* Park at the line end: the loop's next break consumption
             * counts this blank line's own break. */
            e->pos = li.end;
            continue;
        }
        /* At document root there is no parent block, so continuation
         * lines may sit at column 0. */
        int too_shallow = root_ctx ? 0 : (li.indent <= block_floor);
        if (too_shallow || (li.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END))) {
            break; /* a directive-LIKE line is content (XLQ9) */
        }
        size_t cs = e->pos + li.indent;
        while (cs < e->len && (e->p[cs] == ' ' || e->p[cs] == '\t')) {
            cs++; /* continuation leading white space is stripped */
        }
        if (cs < e->len && e->p[cs] == '#') {
            /* a '#' opening a continuation line is a comment: the
             * plain ends here and nothing folds -- the scan below
             * returned an empty piece that still took a fold slot,
             * adding a trailing fold-space ("a\n#" parsed as "a ",
             * libyaml "a": the nightly's combined-build round) */
            e_skip_to_eol(e);
            break;
        }
        yep_span s = yep_scan_plain(e->p, e->len, cs, 0);
        if (s.term == YEP_TERM_COLON) {
            return e_fail(e, YEP_ERR_UNEXPECTED, s.end);
        }
        if (s.end == s.start && s.term == YEP_TERM_EOL) {
            /* The continuation line opens with a raw multibyte break
             * (NEL/LS/PS): scan stopped on it before any content, and
             * the line-facts kernel (\n/\r law) could not park it as a
             * blank line. No piece: the break rides the loop's own
             * tally, so a run of breaks folds to the (n-1) newline law
             * -- one fold-space per break made "nnn]\n!<"+NEL^2+"NNN"
             * read "nnn] !<  NNN" where libyaml folds "nnn] !<\nNNN"
             * (the r102 nightly, yeptris#870). */
            continue;
        }
        if (e->fold_n >= YEP_MAX_FOLD_LINES) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        e->fold[e->fold_n].content.p = e->p + s.start;
        e->fold[e->fold_n].content.len = s.end - s.start;
        e->fold[e->fold_n].breaks_before = breaks;
        e->fold_n++;
        breaks = 0;
        e->pos = s.end;
        if (s.term == YEP_TERM_COMMENT) {
            e_skip_to_eol(e);
            break; /* the comment ends the scalar; later lines are new */
        }
    }
    { /* #179: the source end = the last piece's content end */
        const yep_view* last = &e->fold[e->fold_n - 1].content;
        size_t lend = (size_t)(last->p - e->p) + last->len;
        e_pos_mark(e, lend, &ev->end_line, &ev->end_col);
    }
    if (e->fold_n == 1) {
        return 0; /* single line: the borrowed view stands */
    }
    char* out = yep_fold_plain(e->fold, e->fold_n, e->pool, &ev->value.len);
    if (out == NULL) {
        return e_fail(e, YEP_ERR_MEMORY, e->pos);
    }
    ev->value.p = out;
    ev->borrowed = 0;
    return 0;
}

/* ---------------------------------------------------------------- props */

/* Resolves a scanned tag against the document's %TAG map. !! is the
 * YAML shorthand prefix; !<...> is verbatim; matched handles compose
 * prefix+suffix in the finish pool. */
static int e_hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Tags carry URI escapes (%XX) in every form (verbatim, !!, %TAG). */
static yep_view e_tag_uri_decode(yep_engine* e, yep_view v) {
    const char* p = v.p;
    /* every '%' must be followed by two hex digits — libyaml rejects
     * the family ("did not find URI escaped octet"); a stray '%' left
     * raw would decode differently on re-parse of the re-encoded form
     * (the nightly's tag-escape byte-instability) */
    for (uint32_t i = 0; i < v.len; i++) {
        if (p[i] == '%' && (i + 2 >= v.len || e_hex_val((unsigned char)p[i + 1]) < 0 ||
                            e_hex_val((unsigned char)p[i + 2]) < 0)) {
            e->tag_undef = 1; /* e_props reports it */
            return v;
        }
    }
    for (uint32_t i = 0; i + 2 < v.len; i++) {
        if (p[i] == '%' && e_hex_val((unsigned char)p[i + 1]) >= 0 &&
            e_hex_val((unsigned char)p[i + 2]) >= 0) {
            char* out = yep_pool_alloc(e->pool, v.len, 16);
            if (out == NULL) {
                return v;
            }
            uint32_t o = 0;
            for (uint32_t j = 0; j < v.len;) {
                if (j + 2 < v.len && p[j] == '%' && e_hex_val((unsigned char)p[j + 1]) >= 0 &&
                    e_hex_val((unsigned char)p[j + 2]) >= 0) {
                    out[o++] = (char)(e_hex_val((unsigned char)p[j + 1]) * 16 +
                                      e_hex_val((unsigned char)p[j + 2]));
                    j += 3;
                } else {
                    out[o++] = p[j++];
                }
            }
            yep_view d = {out, o};
            return d;
        }
    }
    return v;
}

static yep_view e_resolve_tag_raw(yep_engine* e, yep_view tag) {
    if (tag.len >= 3 && tag.p[0] == '!' && tag.p[1] == '<' && tag.p[tag.len - 1] == '>') {
        yep_view v = {tag.p + 2, tag.len - 3};
        return v;
    }
    /* the BARE '!' is the non-specific tag and is exempt from %TAG
     * directives (spec 7.3.3): it stays '!' here, and the plain-scalar
     * drop happens at emit_now (the nightly's round 22, crash-d6be3347:
     * '%TAG ! ...' + '- ! tag:clarkevans.com,2002:' resolved the
     * non-specific tag through the directive where libyaml drops it) */
    if (tag.len == 1 && tag.p[0] == '!') {
        return tag;
    }
    /* longest matching handle wins; a redefined PRIMARY ('%TAG ! ...')
     * cannot capture a '!!'-led tag — the secondary handle resolves
     * through its own entry or the fixed tag:yaml.org,2002: default
     * (spec 6.8.2.2; libyaml: "%TAG ! !p/" + "!!i" is
     * tag:yaml.org,2002:i, the nightly's round 52, crash-763bb852) */
    int secondary = tag.len >= 2 && tag.p[0] == '!' && tag.p[1] == '!';
    int best = -1;
    uint32_t best_len = 0;
    for (int i = 0; i < e->tagmap_n; i++) {
        if (secondary && e->tagmap[i].handle.len == 1) {
            continue;
        }
        if (tag.len >= e->tagmap[i].handle.len && e->tagmap[i].handle.len > best_len &&
            memcmp(tag.p, e->tagmap[i].handle.p, e->tagmap[i].handle.len) == 0) {
            best = i;
            best_len = e->tagmap[i].handle.len;
        }
    }
    if (tag.len >= 2 && tag.p[0] == '!' && tag.p[1] == '!' && best < 0) {
        static const char yns[] = "tag:yaml.org,2002:";
        size_t plen = sizeof(yns) - 1;
        char* out = yep_pool_alloc(e->pool, plen + tag.len - 2, 16);
        if (out == NULL) {
            return tag;
        }
        memcpy(out, yns, plen);
        memcpy(out + plen, tag.p + 2, tag.len - 2);
        yep_view v = {out, (uint32_t)(plen + tag.len - 2)};
        return v;
    }
    if (best >= 0) {
        const yep_view* h = &e->tagmap[best].handle;
        const yep_view* pfx = &e->tagmap[best].prefix;
        size_t n = pfx->len + tag.len - h->len;
        char* out = yep_pool_alloc(e->pool, n ? n : 1, 16);
        if (out == NULL) {
            return tag;
        }
        memcpy(out, pfx->p, pfx->len);
        memcpy(out + pfx->len, tag.p + h->len, tag.len - h->len);
        yep_view v = {out, (uint32_t)n};
        return v;
    }
    /* "!word!suffix" with no %TAG entry for the handle is undefined */
    for (uint32_t k = 1; k + 1 < tag.len; k++) {
        if (tag.p[k] == '!') {
            yep_view h = {tag.p, k + 1};
            char* m = yep_pool_alloc(e->pool, h.len, 16);
            if (m != NULL) {
                memcpy(m, h.p, h.len);
                yep_view hv = {m, h.len};
                yep_view res = e_resolve_tag_raw(e, hv);
                if (res.p == hv.p) {
                    e->tag_undef = 1; /* e_props reports it */
                }
            }
            break;
        }
    }
    return tag;
}

static yep_view e_resolve_tag(yep_engine* e, yep_view tag) {
    return e_tag_uri_decode(e, e_resolve_tag_raw(e, tag));
}

/* Parses &anchor / !tag runs at the cursor into *anchor / *tag. A node
 * carries at most one of each (c-ns-properties): a second property of
 * the same kind is the suite's 4JVG error. Keeping the last one let a
 * mutated anchor run read differently from libyaml (catch 156).
 * *have_anchor / *have_tag carry that across CALLS — the flow node
 * parse re-runs the scan when a property rides the next line, and the
 * duplicate law must hold across the gap. */
static void e_props_ex(yep_engine* e, yep_view* anchor, yep_view* tag, int* have_anchor,
                       int* have_tag) {
    for (;;) {
        e_skip_inline_space(e);
        if (e->pos >= e->len) {
            return;
        }
        unsigned char c = (unsigned char)e->p[e->pos];
        if (c == '&') {
            if (*have_anchor) {
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                return;
            }
            *have_anchor = 1;
            size_t start = e->pos + 1;
            e->pos = yep_scan_prop_end(e->p, e->len, start);
            anchor->p = e->p + start;
            anchor->len = (uint32_t)(e->pos - start);
            if (anchor->len == 0) {
                /* libyaml requires one or more anchor characters
                 * ("did not find expected alphabetic or numeric
                 * character"); the nightly fuzz_roundtrip crasher,
                 * a lone '&' */
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                return;
            }
            /* The flow-follower colon law (the nightly's round 29,
             * crash-47eb4054; the block-header twin is round 31,
             * crash-4cbeede1): a name ending at a riding colon whose
             * follower is a FLOW collection or a BLOCK SCALAR HEADER
             * un-consumes the colon. libyaml ACCEPTS "&k2: [1,\n2]"
             * and "&kk: |\n  x" — the anchor names the implicit-null
             * KEY and the collection/scalar is the value — so the
             * riding law (2SXE) must yield exactly where its result
             * is observable in the differential; the plain-led twins
             * keep it (libyaml rejects those: "&a: key: v", W5VH's
             * value position — suite-VALID, ledgered). The colon
             * re-lexes as the key indicator and the existing colon
             * arm builds the anchored null key.
             *
             * The r228 nightly (run 42645063770, crash-8100227a)
             * caught the PROPS-LED follower: "&k2: ! {\n}" — the run
             * between the separator and the collection is the VALUE's
             * own properties, so libyaml still reads the anchored null
             * key (tag "<!>" on the flow map); yeptris instead rode
             * the colon into the name and stamped "k2:" + tag on the
             * map itself, and the anchor-led twin "&k2: &v {...}"
             * outright REJECTED (duplicate anchor) where libyaml
             * accepts. The follower scan now crosses ONE props run
             * ("!tag" / "&anch", verbatim "!<!..>" included —
             * scan_prop_end ends at a blank/break like the name scan)
             * before the indicator check, and a crossed run
             * un-consumes regardless of what follows: the tag/plain
             * member "&k2: ! t" reads the same pair in libyaml. The
             * plain-led twins (no props run — "&a: key", W5VH) never
             * cross one and keep riding. */
            if (e->p[e->pos - 1] == ':') {
                /* the follower scan crosses blanks, breaks AND
                 * comments: the pair separator at line end still opens
                 * the value on a later line ("&k:\n {\n}" — the
                 * nightly's round 31, crash-c2232e17 — rode the colon
                 * and DROPPED the flow map; "&: #c\n|\nA" — round
                 * 32's crash-d580b467 — stopped at the comment, rode
                 * the colon into the anchor name and pass1/pass2
                 * spelled different trees). A comment is
                 * blank-equivalent here: skip it to its break and keep
                 * scanning. */
                size_t t = e->pos;
                int rode_props = 0;
                for (;;) {
                    while (t < e->len && (e->p[t] == ' ' || e->p[t] == '\t' || e->p[t] == '\n' ||
                                          e->p[t] == '\r')) {
                        t++;
                    }
                    if (t < e->len && e->p[t] == '#') {
                        while (t < e->len && e->p[t] != '\n' && e->p[t] != '\r') {
                            t++;
                        }
                        continue;
                    }
                    /* the r228 props-led law: one props run between
                     * the separator and the follower belongs to the
                     * VALUE node — a second keeps the first's verdict
                     * ("&k2: !t &a {" still un-consumes on the "!") */
                    if (!rode_props && t < e->len && (e->p[t] == '!' || e->p[t] == '&')) {
                        t = yep_scan_prop_end(e->p, e->len, t + 1);
                        rode_props = 1;
                        continue;
                    }
                    break;
                }
                if (t < e->len &&
                    (e->p[t] == '[' || e->p[t] == '{' || e->p[t] == '|' || e->p[t] == '>')) {
                    anchor->len -= 1;
                    e->pos -= 1;
                } else if (rode_props) {
                    /* a crossed props run is the value's own: libyaml
                     * reads the pair whatever follows the run — the
                     * tag-led plain ("&k2: ! t") and the bare-tag/EOF
                     * tail ("&k2: !") included */
                    anchor->len -= 1;
                    e->pos -= 1;
                }
            }
            if (e->pos < e->len && e->p[e->pos] == '#') {
                /* the name ends at a blank or flow indicator; a glued
                 * '#' is junk on the name (libyaml: "did not find
                 * expected alphabetic or numeric character") — the
                 * nightly's run 38121449804, "&f#o": this scan kept
                 * the anchor and DROPPED the bytes after '#',
                 * mangling the node while the runner kept a different
                 * shape */
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                return;
            }
        } else if (c == '!') {
            if (*have_tag) {
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                return;
            }
            *have_tag = 1;
            size_t start = e->pos;
            e->pos++;
            if (e->pos < e->len && e->p[e->pos] == '<') {
                /* a verbatim tag closes '>' on the same line, with no
                 * interior space and non-empty content — libyaml errors
                 * otherwise ("did not find expected '>'"; the nightly's
                 * fuzz_roundtrip byte-instability crasher, "!<") */
                size_t gt = e->pos + 1;
                while (gt < e->len && e->p[gt] != '>' && e->p[gt] != '\n' && e->p[gt] != '\r' &&
                       e->p[gt] != ' ' && e->p[gt] != '\t') {
                    gt++;
                }
                if (gt >= e->len || e->p[gt] != '>' || gt == e->pos + 1) {
                    e_fail(e, YEP_ERR_UNEXPECTED, start);
                    return;
                }
                e->pos = gt + 1;
            } else {
                /* libyaml scans URI chars only: bytes like '>' or '|'
                 * end the suffix and surface as the follower, which
                 * the check below rejects */
                while (e->pos < e->len && yep_scan_tag_char((unsigned char)e->p[e->pos])) {
                    e->pos++;
                }
            }
            if (e->pos < e->len) {
                unsigned char after = (unsigned char)e->p[e->pos];
                if (!(after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
                      after == ',' || after == '[' || after == ']' || after == '{' ||
                      after == '}')) {
                    e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                    return;
                }
            }
            yep_view raw = {e->p + start, (uint32_t)(e->pos - start)};
            *tag = e_resolve_tag(e, raw);
            if (e->tag_undef) {
                e->tag_undef = 0;
                e_fail(e, YEP_ERR_UNEXPECTED, start);
                return;
            }
        } else {
            return;
        }
    }
}

/* The one-call form: fresh duplicate state (the block node parse
 * consumes the whole run in a single scan). */
static void e_props(yep_engine* e, yep_view* anchor, yep_view* tag) {
    int have_anchor = 0, have_tag = 0;
    e_props_ex(e, anchor, tag, &have_anchor, &have_tag);
}

/* ------------------------------------------------------------- aliases */

static int e_alias(yep_engine* e, yep_event* ev) {
    size_t star = e->pos;
    size_t start = ++e->pos;
    while (e->pos < e->len && yep_scan_prop_char((unsigned char)e->p[e->pos])) {
        if (e->p[e->pos] == ':') {
            /* ':' separates only before blank or flow indicators; at
             * EOL it belongs to the name ("*a:" — 2SXE), and interior
             * colons always do ("*an:chor" — Y2GN) */
            size_t n2 = e->pos + 1;
            if (n2 < e->len && (e->p[n2] == ' ' || e->p[n2] == '\t' ||
                                yep_ct_is((unsigned char)e->p[n2], YEP_CT_FLOW_IND))) {
                break;
            }
        }
        e->pos++;
    }
    yep_view name = {e->p + start, (uint32_t)(e->pos - start)};
    if (name.len == 0) {
        return e_fail(e, YEP_ERR_UNEXPECTED, star);
    }
    uint32_t target = anchor_id_of(e, name);
    /* libyaml's scanner/composer split: the scanner emits the ALIAS
     * event for ANY name; the composer rejects what it cannot resolve
     * ("found undefined alias"). Rejecting here unconditionally made
     * event streams (recorder/pull/push) diverge from libyaml's for
     * every dangling alias ({?\n:*c }, the nightly's r101), so the
     * rejection rides the sink's compose bit: document builders (the
     * DOM, the record tape) keep the parse-time error, event
     * consumers carry the name for their host's resolution (the ruby
     * materializer raises; the py loader does since this change too). */
    if (e->sink != NULL && e->sink->compose &&
        (target == 0 || (target <= e->anchor_serial && e->anchor_placed != NULL &&
                         e->anchor_placed[target - 1] == 0))) {
        /* undefined, or defined-but-not-yet-placed: the props scan
         * registers the name before the anchor's node reaches the
         * output, so an alias here would precede its definition and
         * the re-parse would reject the document */
        e->pos = star;
        return e_fail(e, YEP_ERR_UNDEFINED_ALIAS, star);
    }
    if (e->pos < e->len && e->p[e->pos] == '#') {
        /* the name ends at a blank or flow indicator; a glued '#' is
         * junk (the anchor-name follower law, catch 43's family) */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    e_skip_inline_space(e);
    if (e->pos < e->len && (e->p[e->pos] == '&' || e->p[e->pos] == '!' || e->p[e->pos] == '*')) {
        /* an alias node is COMPLETE — it carries no props and nothing
         * can follow it on the line but a separator: "<<: *base &bar"
         * (the nightly's run 38124486657 family) errored in libyaml
         * ("did not find expected key") while the DOM route absorbed
         * the stray props and the value drain rejected — the routes
         * split */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    ev->type = YEP_EV_ALIAS;
    ev->value = name;
    ev->borrowed = 1; /* the name is an input view (TODO.restructure/73):
                         the DOM borrows it like any plain scalar — no
                         arena copy per alias */
    ev->anchor_id = target;
    e_pos_mark(e, e->pos, &ev->end_line, &ev->end_col);
    return 0;
}

/* ---------------------------------------------------------------- flow */

static int e_flow_ws(yep_engine* e) {
    for (;;) {
        while (e->pos < e->len && (e->p[e->pos] == ' ' || e->p[e->pos] == '\t')) {
            e->pos++;
        }
        if (e->pos < e->len && e_comment_at(e)) {
            e_skip_to_eol(e);
            continue;
        }
        if (e->pos < e->len && (e->p[e->pos] == '\n' || e->p[e->pos] == '\r')) {
            e_line_done(e, e->pos);
            if (e->pos < e->len && e->pos == e->line_start) {
                yep_line_info fl = e_line_info_here(e);
                if ((fl.flags & YEP_LF_TAB) && !(fl.flags & YEP_LF_BLANK) && fl.indent == 0) {
                    /* a col-0 tab before flow content errors; tabs after
                     * spaces are separation (6HB6) */
                    size_t t = e->line_start + fl.indent;
                    while (t < fl.end && (e->p[t] == ' ' || e->p[t] == '\t')) {
                        t++;
                    }
                    if (t < fl.end && e->p[t] != '[' && e->p[t] != ']' && e->p[t] != '{' &&
                        e->p[t] != '}' && e->p[t] != ',' && e->p[t] != '#' && e->p[t] != ':') {
                        e_fail(e, YEP_ERR_TAB_IN_INDENT, t); /* Y79Y#4 */
                        return -1;
                    }
                }
            }
            continue;
        }
        return 0;
    }
}

/* Pre-scans a flow collection from its opening bracket, returning 0 with
 * e->pos just past the matching close (no events; for the flow-as-key
 * decision MAP_START must precede the key's events). */
static int e_skip_flow(yep_engine* e) {
    int stk[YEP_MAX_DEPTH];
    int n = 0;
    stk[n++] = (e->p[e->pos] == '[') ? 0 : 1;
    e->pos++;
    /* quotes open only at a TOKEN start: a quote byte inside a tag
     * suffix ("[!'str b]") or a plain ("[a'b]") is content — libyaml
     * accepts both; the pre-scan must not read them as a quoted
     * scalar (the nightly's run 38171171360) */
    int tok_start = 1;
    while (e->pos < e->len && n > 0) {
        unsigned char c = (unsigned char)e->p[e->pos];
        if (tok_start && (c == '!' || c == '&' || c == '*')) {
            /* props/aliases ride the span: skip them the way the real
             * scan does — a verbatim tag to its '>', names over the
             * tag-char run (no quote/bracket bytes in the run) */
            e->pos++;
            if (c == '!' && e->pos < e->len && e->p[e->pos] == '<') {
                while (e->pos < e->len && e->p[e->pos] != '>' && e->p[e->pos] != '\n' &&
                       e->p[e->pos] != '\r' && e->p[e->pos] != ' ' && e->p[e->pos] != '\t') {
                    e->pos++;
                }
                if (e->pos < e->len && e->p[e->pos] == '>') {
                    e->pos++;
                }
                tok_start = 0;
                continue;
            }
            while (e->pos < e->len && yep_scan_tag_char((unsigned char)e->p[e->pos])) {
                e->pos++;
            }
            tok_start = 0;
            continue;
        }
        if (tok_start && (c == '"' || c == '\'')) {
            int esc2 = 0;
            const yep_text_kernels* k = yep_text_active();
            ptrdiff_t r = k->quote_scan(e->p + e->pos + 1, e->len - e->pos - 1, (char)c, &esc2);
            if (r < 0) {
                /* an unterminated quote inside the span: NOT-A-KEY — the
                 * real parse (e_flow) owns the error AFTER it has streamed
                 * the flow's open events. The pre-scan's own error here
                 * fired BEFORE those events, so the whole buffer's stream
                 * lost them while the chunked feed had already emitted
                 * theirs (the nightly's round 106); the unbalanced-span
                 * deferral below defers the same way. */
                return 0;
            }
            e->pos += 1 + (size_t)r + 1;
            tok_start = 0;
            continue;
        }
        if (c == '#' && e_comment_at(e)) {
            e_skip_to_eol(e);
            continue;
        }
        if (c == '\n' || c == '\r') {
            e_line_done(e, e->pos);
            tok_start = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '[' || c == '{' || c == ',') {
            tok_start = 1;
        } else if (c == ':' && e->pos + 1 < e->len &&
                   (e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                    e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
                    e->p[e->pos + 1] == '"' || e->p[e->pos + 1] == '\'' ||
                    yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND))) {
            /* a separating ':' only — including directly before a
             * quoted value: "{\"k\"\t:\"[\"}" is JSON- and YAML-flow-
             * valid, and treating ':' as mid-token left the value's
             * quotes unscanned, so its CLOSE read as an unterminated
             * opener (the nightly's round 103) */
        } else {
            tok_start = 0;
        }
        if (c == '[') {
            if (n >= e->max_depth) {
                /* NOT-A-KEY deferral (the unterminated-quote law
                 * above): the pre-scan fires before the flow's open
                 * events, while the chunked feed had already streamed
                 * them, so the whole stream lost MAP_START/SEQ_START
                 * (the nightly's run 39502647277). The real parse owns
                 * the depth error after the events. */
                return 0;
            }
            stk[n++] = 0;
        } else if (c == '{') {
            if (n >= e->max_depth) {
                return 0; /* the same deferral */
            }
            stk[n++] = 1;
        } else if (c == ']') {
            if (n == 0 || stk[n - 1] != 0) {
                /* a stray close outside any candidate span (or a kind
                 * mismatch) is NOT a flow shape: defer to the general
                 * path -- aborting here discarded the pre-scan's whole
                 * document before a single event emitted, while the
                 * chunked feed had already committed the prefix runs
                 * (the nightly's round 113) */
                return 0;
            }
            n--;
        } else if (c == '}') {
            if (n == 0 || stk[n - 1] != 1) {
                return 0;
            }
            n--;
        }
        e->pos++;
    }
    if (n != 0) {
        /* incomplete: the real parse (e_flow) owns the deferral in BOTH
         * modes. The pre-emptive final error (removed here) fired
         * BEFORE the flow's first events, while the chunked feed had
         * already streamed MAP_START + the first entry — the two routes'
         * event streams diverged at the error boundary (the nightly's
         * round 104). e_flow errors on the unclosed span either way. */
        return 0;
    }
    return 0;
}

/* Parses one flow node at the cursor into *ev. Return: 1 node parsed,
 * 0 no node (separator/close next), 2 nested opener, -1 error. */
static int e_flow_node(yep_engine* e, yep_event* ev, int keyish) {
    (void)keyish;
    if (e_flow_ws(e) != 0) {
        return -1;
    }
    if (e->pos >= e->len) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    unsigned char c = (unsigned char)e->p[e->pos];
    if (c == '[' || c == '{') {
        return 2;
    }
    if (c == ']' || c == '}' || c == ',') {
        return 0; /* separator / close */
    }
    if (c == ':') {
        /* ':' separates when followed by blank/EOL/flow indicator, or
         * directly after a flow close ("{a: b}:c" — JSON-style); ":x" is
         * a plain scalar starting with a colon */
        size_t n2 = e->pos + 1;
        if (n2 >= e->len || e->p[n2] == ' ' || e->p[n2] == '\n' || e->p[n2] == '\r' ||
            yep_ct_is((unsigned char)e->p[n2], YEP_CT_FLOW_IND) ||
            (e->pos > 0 && (e->p[e->pos - 1] == ']' || e->p[e->pos - 1] == '}'))) {
            return 0;
        }
    }

    e_event_init(ev, YEP_EV_SCALAR);
    ev->line = e->line;
    ev->col = e_col(e, e->pos) + 1;
    yep_view anchor = {0}, tag = {0};
    int have_anchor = 0, have_tag = 0;
    e_props_ex(e, &anchor, &tag, &have_anchor, &have_tag);
    if (e->err.code != YEP_ERR_NONE) {
        return -1; /* e_props failed (empty anchor name, unresolved tag) */
    }
    if (!yep_view_is_empty(anchor) &&
        (memchr(anchor.p, '"', anchor.len) != NULL || memchr(anchor.p, '\'', anchor.len) != NULL)) {
        /* anchors have no quoting: a quote byte in a FLOW anchor name
         * has no safe spelling (Psych: "did not find expected
         * alphabetic or numeric character"). Block names ride quotes
         * (the suite's W5VH) — this law is flow-scoped. */
        return (int)e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    ev->anchor = anchor;
    ev->tag = tag;
    if (!yep_view_is_empty(anchor)) {
        ev->anchor_id = anchor_define(e, anchor);
    }
    if (e_flow_ws(e) != 0) {
        return -1;
    }
    if (e->pos >= e->len) {
        /* props at EOF: no node follows the properties (the entry
         * guard above cannot cover this — e_props consumed to the
         * end; found by fuzz_parse, input "[!<<\r\n!]" x8) */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    c = (unsigned char)e->p[e->pos];
    /* Properties may span line breaks in flow (libyaml scans the
     * anchor/tag run across breaks: "&a\n!!t v" carries the tag with
     * value v; "!t\n&a x" carries the anchor). The run above skips
     * INLINE space only, so the cursor stopped at the break and the
     * property byte led the plain branch instead — a plain scalar
     * glued from tag/anchor bytes (the nightly's round 211,
     * crash-925daa54, "&anchor\n !!312    3123]"). Re-lex the riding
     * run with the have_* state threaded so a second property of one
     * kind still errors (the 4JVG law). Each iteration consumes at
     * least one byte or fails, so the loop is bounded. */
    while (c == '&' || c == '!') {
        e_props_ex(e, &anchor, &tag, &have_anchor, &have_tag);
        if (e->err.code != YEP_ERR_NONE) {
            return -1;
        }
        if (!yep_view_is_empty(anchor) && (memchr(anchor.p, '"', anchor.len) != NULL ||
                                           memchr(anchor.p, '\'', anchor.len) != NULL)) {
            return (int)e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        ev->anchor = anchor;
        ev->tag = tag;
        if (!yep_view_is_empty(anchor)) {
            ev->anchor_id = anchor_define(e, anchor);
        }
        if (e_flow_ws(e) != 0) {
            return -1;
        }
        if (e->pos >= e->len) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        c = (unsigned char)e->p[e->pos];
    }
    if ((c == ',' || c == ']' || c == '}' || c == ':') &&
        (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag))) {
        ev->implicit = 1; /* properties with no node: a tagged null */
        ev->style = YEP_STYLE_PLAIN;
        e_pos_mark(e, e->pos, &ev->end_line, &ev->end_col);
        return 1;
    }
    if (c == '"' || c == '\'') {
        ev->implicit = 0;
        return e_quoted_floor(e, ev, 0, 0) == 0 ? 1 : -1;
    }
    if (c == '*') {
        if (e_alias(e, ev) != 0) {
            return -1;
        }
        if (ev->value.len > 0 && (memchr(ev->value.p, '"', ev->value.len) != NULL ||
                                  memchr(ev->value.p, '\'', ev->value.len) != NULL)) {
            /* the alias twin of the flow anchor-name law above */
            return (int)e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return 1;
    }
    if (c == '[' || c == '{') {
        return 2;
    }
    /* plain: may span lines (folded); a leading non-structural ':' is
     * part of the scalar ("::x" per the suite) — keep it in the first
     * piece by scanning from the colon itself */
    const char* piece_start_fix = e->p + e->pos;
    if ((unsigned char)*piece_start_fix == ':') {
        e->pos++;
        if (e_flow_ws(e) != 0) {
            return -1;
        }
        if (e->pos >= e->len) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        /* rescan from the colon so the piece includes it */
        e->pos = (size_t)(piece_start_fix - e->p);
    }
    e->fold_n = 0;
    int crossed_break = 0;
    uint32_t flow_breaks = 1; /* the break count before the next piece */
    for (;;) {
        yep_span s = yep_scan_plain(e->p, e->len, e->pos, 1);
        if (s.end == s.start && e->fold_n == 0) {
            return 0; /* empty token: separator/close comes next */
        }
        if (e->fold_n >= YEP_MAX_FOLD_LINES) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        if (s.end > s.start) {
            e->fold[e->fold_n].content.p = e->p + s.start;
            e->fold[e->fold_n].content.len = s.end - s.start;
            e->fold[e->fold_n].breaks_before = (e->fold_n == 0) ? 0 : flow_breaks;
            e->fold_n++;
        }
        if (s.term == YEP_TERM_EOL || s.term == YEP_TERM_COMMENT) {
            e->pos = s.end;
            if (s.term == YEP_TERM_COMMENT) {
                e_skip_to_eol(e);
                break; /* a comment ends the scalar: the next token is new */
            }
            crossed_break = 1;
            /* Count EVERY break up to the next piece: 1 folds to a
             * space, n>1 folds to n-1 newlines (the plain law —
             * "[0\r\r)]" is "0\n)", the nightly's run 39070140324).
             * The seed is 0 because the loop below consumes the
             * piece-terminating break itself. Trailing spaces of the
             * piece's own line are skipped first — yep_scan_plain can
             * stop before them, and "br == 0 at a space" must never
             * be read as content (that spun forever on "[0 \n )]"). */
            uint32_t nbreaks = 0;
            int ended = 0;
            for (;;) {
                e_skip_inline_space(e);
                yep_line_info cl = e_line_info_here(e);
                if (cl.flags & YEP_LF_COMMENT) {
                    e_line_done(e, cl.end);
                    ended = 1; /* a comment line ends the scalar too */
                    break;
                }
                size_t br = yep_scan_break_len(e->p, e->len, e->pos);
                if (br == 0) {
                    break; /* content on this line */
                }
                nbreaks++;
                e->pos += br;
                e->line++;
                e->line_start = e->pos;
                e->li_cache_line = 0;
                e->shape_cache_line = 0;
                e->facts_cache_line = 0;
                if (e->pos >= e->len) {
                    ended = 1;
                    break;
                }
            }
            flow_breaks = nbreaks;
            if (ended) {
                break;
            }
            if (e->pos < e->len && e->p[e->pos] != ']' && e->p[e->pos] != '}' &&
                e->p[e->pos] != ',' && !e_comment_at(e)) {
                /* a document marker line ends the scalar (and with it
                 * the unclosed flow) — folding across "---" absorbed
                 * the marker and the whole route's scalar grew past the
                 * chunked route's deferral boundary (the nightly's
                 * round 104) */
                yep_line_info ml = e_line_info_here(e);
                if (ml.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END)) {
                    break; /* real document markers end the scalar; a
                            * directive-LIKE '%' line stays content
                            * mid-flow (XLQ9, the UT92 spec example) */
                }
                continue; /* continuation on a following line */
            }
            break; /* at a boundary; pos is on the separator/close */
        }
        e->pos = s.end;
        break;
    }
    if (e->fold_n == 1) {
        ev->value.p = e->fold[0].content.p;
        ev->value.len = e->fold[0].content.len;
        ev->borrowed = 1;
    } else {
        char* out = yep_fold_plain(e->fold, e->fold_n, e->pool, &ev->value.len);
        if (out == NULL) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        ev->value.p = out;
        ev->borrowed = 0;
    }
    if (e->fold_n == 1 && e->fold[0].content.len == 1 && e->fold[0].content.p[0] == '-') {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[-, -]": bare dash entry */
    }
    ev->style = YEP_STYLE_PLAIN;
    ev->implicit = 1;
    ev->multiline = (e->fold_n > 1) || crossed_break;
    { /* #179: the source end = the last fold piece's content end */
        const yep_view* lastp = &e->fold[e->fold_n - 1].content;
        e_pos_mark(e, (size_t)(lastp->p - e->p) + lastp->len, &ev->end_line, &ev->end_col);
    }
    return 1;
}

/* --------------------------------------------------- JSON fast path */

/* The JSON-class fast path (TODO.impl/08A): one validating scan of the
 * span, then bulk event emission — pure-JSON flow collections skip the
 * general kernel's per-node guards. STRICTLY conservative: anything not
 * byte-for-byte JSON (tabs, comments, YAML scalars, trailing commas,
 * indent anomalies under a block parent) returns 0 and the general
 * kernel handles it, so semantics can never diverge — only speed. */

#define YEP_JSON_MAX_DEPTH 256

/* Line/col across a gap: the walk is scan's SSOT (yep_scan_advance_
 * line) — the DOM direct builder shares it so line/col cannot drift
 * between the two paths. */
static void jx_advance_line(yep_engine* e, size_t* from, size_t to, uint32_t* line,
                            size_t* line_start) {
    yep_scan_advance_line(e->p, from, to, line, line_start);
    /* a lone \r sitting at the walk boundary (to - 1) ends the line
     * AT to, but the scan's walk cannot peek at p[to] (it does not
     * know the buffer end) — correct it here with the engine's own
     * length: the fast path's marks after a \r-containing span sat
     * one line low while the general kernel marked correctly (the
     * nightly's runs 38549939326 + 38563005475) */
    if (to > 0 && to <= e->len && e->p[to - 1] == '\r' && (to == e->len || e->p[to] != '\n')) {
        (*line)++;
        *line_start = to;
    }
}

/* A flow node followed by ':' on the same line is a KEY: the caller
 * must wrap it in a mapping first — refuse so the general path's
 * is_key logic runs. Shared by both fast paths (grammar fact). */
static int e_flow_key_after(const char* p, size_t len, size_t close) {
    size_t j = close + 1;
    /* tabs are inline whitespace too: "[]\t\t: |-" defers to the key
     * machinery like its all-spaces sibling, or the fast path commits
     * the flow and then errors on the colon it cannot read as a key
     * separator (the nightly's round 116) */
    while (j < len && (p[j] == ' ' || p[j] == '\t')) {
        j++;
    }
    return j < len && p[j] == ':' &&
           (j + 1 >= len || p[j + 1] == ' ' || p[j + 1] == '\n' || p[j + 1] == '\r' ||
            p[j + 1] == '\t');
}

/* Block-level flow lines must out-indent the parent (9C9N): any line
 * start inside the span at or left of the floor falls back — the
 * closer's own line included ("a: {\n  x: 1\n}\nb: 3" is a suite
 * error; only libyaml accepts it). The scan must cover the closer
 * line: its start sits AT the close offset, outside the loop below,
 * so it is checked explicitly — the general kernel's per-line check
 * rejects it, and the fast path must agree (the nightly's run
 * 38280372447: whole vs chunked split across the fast/slow paths and
 * produced two different verdicts for the same bytes). */
static int e_flow_floor_ok(const char* p, size_t open, size_t close, uint32_t floor) {
    size_t j = open;
    while (j < close) {
        /* the break rule is the scan's: \n, or a \r the next byte does
         * not join into CRLF — a \n-only walk let a CR-broken content
         * line escape the indent check (issue #771: the whole parse
         * took the fast path past a floor violation the windowed
         * parse's general kernel rejected) */
        if (p[j] == '\n' || (p[j] == '\r' && (j + 1 >= close || p[j + 1] != '\n'))) {
            size_t k = j + 1;
            uint32_t ind = 0;
            while (k < close && p[k] == ' ') {
                ind++;
                k++;
            }
            if (k < close && p[k] != '\n' && p[k] != '\r' && ind <= floor) {
                return 0;
            }
        }
        j++;
    }
    if (close > open) {
        size_t from = open;
        uint32_t ln = 0;
        size_t ls = open;
        yep_scan_advance_line(p, &from, close, &ln, &ls);
        /* advance_line cannot peek past its `to`: a lone \r at
         * close - 1 ends the closer's line AT close */
        if (p[close - 1] == '\r' && p[close] != '\n') {
            ls = close;
        }
        uint32_t ind = 0;
        while (ls < close && p[ls] == ' ') {
            ind++;
            ls++;
        }
        if (ind <= floor) {
            return 0;
        }
    }
    return 1;
}

/* Returns 1 = events emitted and consumed (e->pos past the close),
 * 0 = not JSON-class (fall back), -1 = error set, -2 = sink abort. */
static int e_flow_json(yep_engine* e, yep_view anchor, yep_view tag, uint32_t anchor_id) {
    const char* p = e->p;
    size_t len = e->len;
    size_t open_pos = e->pos;
    if (len - open_pos < 24) {
        return 0; /* tiny spans: the general kernel is cheaper */
    }

    /* ---- fused path (TODO.restructure/53): the sink validates AND
     * builds in one walk; the engine's grammar checks run on the
     * builder's close, then commit or roll back. ---- */
    if (e->ev_scopes == 0 && e->sink != NULL && e->sink->on_flow_build != NULL) {
        size_t bclose = 0;
        uint32_t serial_at_open = e->anchor_serial;
        int rc = e->sink->on_flow_build(e->sink->ctx, p, open_pos, len, e->line, e->line_start,
                                        anchor, tag, anchor_id, e->max_depth, &bclose);
        if (rc < 0) {
            return -2;
        }
        if (rc == 0) {
            return 0; /* not JSON-class: the general kernel */
        }
        if (rc == 1) {
            int single = (memchr(p + open_pos, '\n', bclose - open_pos) == NULL) &&
                         (memchr(p + open_pos, '\r', bclose - open_pos) == NULL);
            if (e_flow_key_after(p, len, bclose) ||
                (e->flow_enforce && !single &&
                 !e_flow_floor_ok(p, open_pos, bclose, e->flow_floor))) {
                e->sink->on_flow_rollback(e->sink->ctx);
                return 0;
            }
            if (e->sink->on_flow_commit(e->sink->ctx) != 1) {
                return -2;
            }
            /* the commit places the whole subtree at once: every anchor
             * defined during the flow's parse belongs to it (the builder
             * placed their nodes; the engine never sees the ids
             * individually) — mark the range so a later alias may parse */
            e_anchor_mark(e, anchor_id);
            for (uint32_t aid = serial_at_open + 1; aid <= e->anchor_serial; aid++) {
                e_anchor_mark(e, aid);
            }
            size_t from = open_pos;
            uint32_t ln = e->line;
            size_t ls = e->line_start;
            jx_advance_line(e, &from, bclose + 1, &ln, &ls);
            e->line = ln;
            e->line_start = ls;
            e->pos = bclose + 1;
            e_skip_inline_space(e);
            return 1;
        }
        /* rc == 2: long key — fall to pass 1 + pass 2 for the error */
    }

    /* ---- pass 1: strict-JSON validation via the shared walker ----
     * (the grammar lives in scan/json.c; the engine drives it) */
    uint8_t kind0;
    size_t close;
    {
        yep_json_walk w;
        yep_json_tok t;
        yep_json_walk_init(&w, p, len, open_pos, e->max_depth);
        w.strict = 1; /* the YAML fast path is byte-for-byte JSON only:
                       * a lenient walk accepted number/literal keys
                       * ("{9:1}" -> {9->1}) that the general kernel
                       * reads as the plain scalar "9:1" — whole and
                       * chunked parses took different paths and
                       * diverged (the nightly's run 38513849602) */
        for (;;) {
            yep_jw_status st = yep_json_walk_next(&w, &t);
            if (st == YEP_JW_REJECT) {
                return 0;
            }
            if (st == YEP_JW_DONE) {
                break;
            }
        }
        close = w.close;
        kind0 = w.kind[0];
    }

    /* One memchr decides the dominant shape: a SINGLE-LINE span (one
     * flow collection per block line — the common config/API dump)
     * skips the flow_enforce rescan AND the per-event line bookkeeping
     * below; both are no-ops without a newline, and pass 1 already
     * walked these bytes (TODO.restructure/45, Phase B slice). */
    /* a span carrying ANY \r is multi-line for marking purposes: the
     * lone-\r walk below counts real breaks, and a single-line span
     * skips that walk entirely — a '{\r\r\r\r}' span kept its marks
     * on line 1 while the general kernel (and libyaml) counted four
     * breaks (the nightly's run 38585830922) */
    int single_line = (memchr(p + open_pos, '\n', close - open_pos) == NULL) &&
                      (memchr(p + open_pos, '\r', close - open_pos) == NULL);

    if (e_flow_key_after(p, len, close)) {
        return 0;
    }
    if (e->flow_enforce && !single_line && !e_flow_floor_ok(p, open_pos, close, e->flow_floor)) {
        return 0;
    }

    /* ---- pass 2: emit events from the validated span ---- */
    uint32_t cur_line = e->line;
    size_t cur_ls = e->line_start;
    size_t cur_scan = e->pos; /* unscanned region starts at the span */
    {
        yep_event ev;
        e_event_init(&ev, kind0 ? YEP_EV_MAP_START : YEP_EV_SEQ_START);
        ev.flow = 1;
        ev.anchor = anchor;
        ev.anchor_id = anchor_id;
        ev.tag = tag;
        ev.line = cur_line;
        /* the general kernel's opener marks, mirrored exactly: col =
         * the indicator's 1-based column, end one past it. The old +1
         * made every fused open sit one column right of the same
         * span's general-kernel parse — a <24-byte span cut at a
         * chunk boundary routes general while the whole parse routes
         * fused (the nightly's run 38173499680) */
        ev.col = (uint32_t)(open_pos - cur_ls) + 1;
        ev.end_line = cur_line; /* flow opens end after the indicator */
        ev.end_col = (uint32_t)(open_pos - cur_ls) + 2;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }
    uint8_t stk[YEP_JSON_MAX_DEPTH]; /* 0 seq, 1 map; key-pending per frame */
    uint8_t kpending[YEP_JSON_MAX_DEPTH];
    int sd = 1;
    stk[0] = kind0;
    kpending[0] = 0;
    size_t i = open_pos + 1;
    for (;;) {
        while (i < close && (p[i] == ' ' || p[i] == '\n' || p[i] == '\r')) {
            i++;
        }
        char c = p[i];
        if (c == ']' || c == '}') {
            yep_event ev;
            e_event_init(&ev, stk[sd - 1] ? YEP_EV_MAP_END : YEP_EV_SEQ_END);
            /* flow=1 only on START events (kernel convention: the style
             * belongs to the opening bracket, END events stay plain) */
            if (!single_line)
                jx_advance_line(e, &cur_scan, i, &cur_line, &cur_ls);
            ev.line = cur_line;
            ev.col = (uint32_t)(i - cur_ls) + 1;
            ev.end_line = cur_line;
            ev.end_col = (uint32_t)(i + 1 - cur_ls) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            sd--;
            if (sd == 0) {
                break;
            }
            i++;
            continue;
        }
        if (c == '{' || c == '[') {
            if (stk[sd - 1] && kpending[sd - 1] == 2) {
                kpending[sd - 1] = 0; /* the value of the pending pair */
            }
            yep_event ev;
            e_event_init(&ev, c == '[' ? YEP_EV_SEQ_START : YEP_EV_MAP_START);
            ev.flow = 1;
            if (!single_line)
                jx_advance_line(e, &cur_scan, i, &cur_line, &cur_ls);
            ev.line = cur_line;
            /* the opener's own column, like site 1 and the general
             * kernel: the fused walk's nested open rode the OLD +1
             * (one past the indicator) while the outer opener was
             * already fixed -- whole parses >=24 bytes route fused,
             * the <24-byte chunked prefix routes general, and the
             * nested start sat one column right (the nightly's round
             * 122, "[[]": whole 1:3 vs chunk 1:2) */
            ev.col = (uint32_t)(i - cur_ls) + 1;
            ev.end_line = cur_line; /* flow opens end after the indicator */
            ev.end_col = (uint32_t)(i + 1 - cur_ls) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            stk[sd] = (c == '[') ? 0 : 1;
            kpending[sd] = 0;
            sd++;
            i++;
            continue;
        }
        if (c == ',' || c == ':') {
            i++;
            continue;
        }
        /* scalar: quoted, number, or literal */
        yep_event ev;
        e_event_init(&ev, YEP_EV_SCALAR);
        if (!single_line)
            jx_advance_line(e, &cur_scan, i, &cur_line, &cur_ls);
        ev.line = cur_line;
        ev.col = (uint32_t)(i - cur_ls) + 1;
        size_t vstart = i;
        size_t raw_end = i; /* span end (past the closing quote for strings) */
        if (c == '"') {
            int he = 0;
            size_t vclose;
            if (!yep_json_string(p, close + 1, &i, &vclose, &he)) {
                return -1; /* validated: cannot happen */
            }
            size_t vend = vclose; /* the closing quote */
            raw_end = vclose + 1;
            ev.style = YEP_STYLE_DOUBLE_QUOTED;
            if (he) {
                ev.multiline = 0;
                char* out = yep_finish_double(p, (uint32_t)(vstart + 1), (uint32_t)vend, 0, e->pool,
                                              &ev.value.len);
                if (out == NULL) {
                    return e_fail(e, YEP_ERR_MEMORY, vstart);
                }
                ev.value.p = out;
                ev.borrowed = 0;
            } else {
                ev.value.p = p + vstart + 1;
                ev.value.len = (uint32_t)(vend - vstart - 1);
                ev.borrowed = 1;
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            i++;
            while (i < close && ((p[i] >= '0' && p[i] <= '9') || p[i] == '.' || p[i] == 'e' ||
                                 p[i] == 'E' || p[i] == '+' || p[i] == '-')) {
                i++;
            }
            ev.value.p = p + vstart;
            ev.value.len = (uint32_t)(i - vstart);
            ev.borrowed = 1;
            ev.style = YEP_STYLE_PLAIN;
            ev.implicit = 1;
        } else {
            /* literals: validated in pass 1; walk their exact length */
            i += (c == 't') ? 4 : (c == 'f') ? 5 : 4;
            ev.value.p = p + vstart;
            ev.value.len = (uint32_t)(i - vstart);
            ev.borrowed = 1;
            ev.style = YEP_STYLE_PLAIN;
            ev.implicit = 1;
        }
        if (raw_end == vstart) {
            raw_end = i; /* numbers/literals advance i, not raw_end */
        }
        ev.end_line = cur_line;
        ev.end_col = (uint32_t)(raw_end - cur_ls) + 1;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
        if (stk[sd - 1] && p[i] == ':' && kpending[sd - 1] == 0) {
            /* the simple-key law rides the colon: libyaml keeps a flow
             * key possible while (colon - key start) <= 1024 in
             * characters -- the quotes count against quoted keys and a
             * multibyte code point counts once (the nightly's round 108
             * shrink carries a 2-byte char at byte delta 1025) */
            if (i - vstart > YEP_MAX_SIMPLE_KEY &&
                yep_simple_key_chars(p + vstart, i - vstart) > YEP_MAX_SIMPLE_KEY) {
                return e_fail(e, YEP_ERR_KEY_TOO_LONG, vstart);
            }
            kpending[sd - 1] = 2; /* key seen; the next scalar/collection is the value */
        }
    }

    /* engine position: past the close; line state reflects the span */
    jx_advance_line(e, &cur_scan, close + 1, &cur_line, &cur_ls);
    e->line = cur_line;
    e->line_start = cur_ls;
    e->pos = close + 1;
    e_skip_inline_space(e);
    return 1;
}

/* Iterative flow kernel; emits events for the whole collection. */
/* Initializes one flow frame (SSOT: every field, every push site). */
static void e_flow_frame_init(void* stv, int idx, int kind, int pending_key) {
    typedef struct {
        uint8_t kind;
        uint8_t pending_key;
        uint8_t sep;
        uint8_t just_closed;
        uint8_t pair_open;
        uint8_t pair_value;
        uint8_t pair_q; /* the pair opened on an explicit '?': its
                         * JSON-style ':' separator is still ahead */
        uint8_t pair_wrap;
        uint8_t q_coll;        /* seq: an explicit '?' key collection awaits completion */
        uint8_t q_empty_value; /* seq: the '?' empty-key pair — 1: opened,
                                * 2: its value state (the entry comma was
                                * consumed inside the pair, yeptris#741) */
        uint8_t key_coll;      /* map: the nested collection opened in key position */
        int32_t buf_from;
        uint32_t entry_line;
        uint32_t entries;
    } e_flow_frame;
    e_flow_frame* st = (e_flow_frame*)stv;
    st[idx].kind = (uint8_t)kind;
    st[idx].pending_key = (uint8_t)pending_key;
    st[idx].sep = 0;
    st[idx].just_closed = 0;
    st[idx].key_coll = 0;
    st[idx].pair_open = 0;
    st[idx].pair_value = 0;
    st[idx].pair_q = 0;
    st[idx].pair_wrap = 0;
    st[idx].q_coll = 0;
    st[idx].q_empty_value = 0;
    st[idx].buf_from = -1;
    st[idx].entry_line = 0;
    st[idx].entries = 0;
}

static int e_flow(yep_engine* e, yep_view anchor, yep_view tag, uint32_t anchor_id) {
    struct {
        uint8_t kind;          /* 0 seq, 1 map */
        uint8_t pending_key;   /* map: 1 key seen awaiting ':' value; 2 key emitted w/o ':' */
        uint8_t sep;           /* a ',' was consumed and no entry followed yet */
        uint8_t just_closed;   /* a nested collection closed here (key candidate) */
        uint8_t pair_open;     /* seq: a buffered single-pair awaits its MAP_END */
        uint8_t pair_value;    /* seq: the open pair already has its value */
        uint8_t pair_q;        /* seq: the pair opened on an explicit '?':
                                * its JSON-style ':' separator is still ahead */
        uint8_t pair_wrap;     /* map frame pushed as a seq single-pair */
        uint8_t q_coll;        /* seq: an explicit '?' key collection awaits completion */
        uint8_t q_empty_value; /* seq: the '?' empty-key pair's state (#741) */
        uint8_t key_coll;      /* map: a nested collection opened in key position */
        int32_t buf_from;      /* seq: entry events buffered from this index */
        uint32_t entry_line;   /* seq: line where the buffered entry ended */
        uint32_t entries;      /* completed entries (for trailing-comma detection) */
    } st[YEP_MAX_DEPTH];
    int n = 0;

    unsigned char open = (unsigned char)e->p[e->pos];
    e->pos++;
    yep_event ev;
    e_event_init(&ev, open == '[' ? YEP_EV_SEQ_START : YEP_EV_MAP_START);
    ev.flow = 1;
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.tag = tag;
    ev.line = e->line;
    ev.col = e_col(e, e->pos);
    ev.end_line = e->line; /* flow opens end after the indicator */
    ev.end_col = e_col(e, e->pos) + 1;
    if (emit_now(e, &ev) != 0) {
        return -2;
    }
    e_flow_frame_init(st, n, (open == '[') ? 0 : 1, 0);
    n++;

    for (;;) {
        e_event_init(&ev, YEP_EV_NONE);
        if (e->pos == e->line_start && e->pos < e->len) {
            yep_line_info fl = e_line_info_here(e);
            if ((fl.flags & YEP_LF_TAB) && !(fl.flags & YEP_LF_BLANK)) {
                return e_fail(e, YEP_ERR_TAB_IN_INDENT, e->pos); /* Y79Y#4 */
            }
        }
        if (st[n - 1].kind == 0 && st[n - 1].buf_from < 0 && st[n - 1].pair_open == 0 &&
            e->pos < e->len && e->p[e->pos] != ',' && e->p[e->pos] != ']' && e->p[e->pos] != '}' &&
            e->p[e->pos] != ':') {
            /* a sequence entry may become a single-pair key: defer it */
            st[n - 1].buf_from = (int32_t)e->ev_buf_n;
            st[n - 1].entry_line = e->line;
            e->ev_scopes++;
        }
        int keyish = (st[n - 1].kind == 1 && st[n - 1].pending_key == 0);
        int q_key = 0;
        int q_empty_key = 0; /* the '?' synthesized a null key (#741) */
        int rc;
        if (e_flow_ws(e) != 0) {
            return -1;
        }
        if (e->pos < e->len && e->pos == e->line_start) {
            yep_line_info fl = e_line_info_here(e);
            if (fl.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END | YEP_LF_DIRECTIVE)) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* marker inside flow */
            }
            if (fl.flags & YEP_LF_TAB) {
                return e_fail(e, YEP_ERR_TAB_IN_INDENT, e->pos); /* Y79Y#4 */
            }
            if (e->flow_enforce && !(fl.flags & (YEP_LF_BLANK | YEP_LF_COMMENT)) &&
                fl.indent <= e->flow_floor) {
                /* a block-level flow's lines out-indent its parent (9C9N) */
                return e_fail(e, YEP_ERR_BAD_INDENT, e->pos + fl.indent);
            }
        }
        if (st[n - 1].pair_open && st[n - 1].pair_q && e->pos < e->len && e->p[e->pos] == ':') {
            /* "#671: the explicit key's entry separator may sit flush
             * against the value ("[? &a :b]") -- the JSON-style colon
             * the map frame's pending-key law consumes; an IMPLICIT
             * key's separator is already spent here, so its value's
             * leading ':' stays content ("[s: :v]") */
            size_t n2 = e->pos + 1;
            if (n2 >= e->len ||
                !(e->p[n2] == ' ' || e->p[n2] == '\t' || e->p[n2] == '\n' || e->p[n2] == '\r' ||
                  yep_ct_is((unsigned char)e->p[n2], YEP_CT_FLOW_IND))) {
                st[n - 1].pair_q = 0;
                /* yeptris#741: the consumed ':' is the pair's VALUE
                 * indicator — the '?' empty-key pair's value state ends
                 * here or the value node trips the entry guard */
                st[n - 1].q_empty_value = 0;
                e->pos++;
                continue;
            }
        }
        if (st[n - 1].kind == 1 && st[n - 1].pending_key == 2 && e->pos < e->len &&
            e->p[e->pos] == ':') {
            /* an explicit '?'-key entry's separator may follow on a
             * LATER line (s-separate spans breaks in flow: "{?{a}\n:
             * b}" — the nightly's catch 161); the post-node colon law
             * never sees a colon at the node start */
            st[n - 1].pending_key = 1;
            e->pos++;
            continue;
        }
        if (e->pos < e->len && e->p[e->pos] != ',' && e->p[e->pos] != ']' && e->p[e->pos] != '}' &&
            !(e->p[e->pos] == ':' &&
              ((st[n - 1].kind == 1 && st[n - 1].pending_key == 1) || st[n - 1].just_closed)) &&
            st[n - 1].entries > 0 && st[n - 1].sep == 0 && !st[n - 1].pair_open &&
            !(st[n - 1].kind == 1 && st[n - 1].pending_key == 1)) {
            if (getenv("YEP_TRACE"))
                fprintf(stderr, "[mc] pos=%c kind=%d ent=%u sep=%u pk=%u jc=%u\n", e->p[e->pos],
                        st[n - 1].kind, st[n - 1].entries, st[n - 1].sep, st[n - 1].pending_key,
                        st[n - 1].just_closed);
            /* a node without a preceding ',' or ':' (CML9 missing comma);
             * in a sequence a ':'-led token is a new plain, so it errors */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (st[n - 1].kind == 0 && st[n - 1].q_empty_value == 2 && e->pos < e->len &&
            e->p[e->pos] != ',' && e->p[e->pos] != ']' && e->p[e->pos] != '}' &&
            e->p[e->pos] != ':') {
            /* yeptris#741: a node in the '?' empty-key pair's value
             * state — ENTRY_MAPPING_VALUE wants VALUE/ENTRY/END only
             * ("[?, a]" is the libyaml "did not find expected ',' or
             * ']'" error) */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (e->pos < e->len && e->p[e->pos] == '?' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
             yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND))) {
            e->pos++;
            if (e_flow_ws(e) != 0) {
                return -1;
            }
            if (e->pos >= e->len ||
                (e->p[e->pos] == ':' &&
                 (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                  e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
                  yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND) ||
                  e->p[e->pos + 1] == ',' || e->p[e->pos + 1] == ']' || e->p[e->pos + 1] == '}')) ||
                e->p[e->pos] == ',' || e->p[e->pos] == ']' || e->p[e->pos] == '}') {
                /* '? : v' / '?' at the boundary: an EMPTY explicit key */
                e_event_init(&ev, YEP_EV_SCALAR);
                ev.implicit = 1;
                ev.style = YEP_STYLE_PLAIN;
                ev.line = e->line;
                ev.col = e_col(e, e->pos) + 1;
                rc = 1;
                q_key = 1;
                q_empty_key = (st[n - 1].kind == 0);
            } else {
                rc = e_flow_node(e, &ev, keyish);
                q_key = 1;
            }
        } else {
            rc = e_flow_node(e, &ev, keyish);
        }
        if (rc == -1) {
            return -1;
        }
        if (rc == 2) {
            if (n >= e->max_depth) {
                return e_fail(e, YEP_ERR_DEPTH, e->pos);
            }
            if (q_key) {
                /* the EXPLICIT key is the collection: mark its frame so
                 * the pair completes with a null value when no ':' line
                 * follows ("? [a]" / "? {b: 1}") — without the marker the
                 * key leaked as a plain seq entry and a map-entry key
                 * vanished outright (the nightly's round 101) */
                if (st[n - 1].kind == 1) {
                    st[n - 1].pending_key = 2;
                } else {
                    st[n - 1].q_coll = 1;
                }
            }
            if (!q_key && st[n - 1].pair_open) {
                /* a VALUE collection completes the pending pair; a KEY
                 * collection must not (its pair completes at ':' or the
                 * boundary with a null value). The gate is pair_open:
                 * in a SEQUENCE a plain collection entry has no pair,
                 * and the unguarded write planted a stale pair_value=1
                 * that a LATER '?'-opened pair wrap inherited -- its
                 * comma flush then skipped the implicit null
                 * (crash-8106600e, "[... , ? 20:50 , ...]"). */
                st[n - 1].pair_value = 1;
            }
            unsigned char oc = (unsigned char)e->p[e->pos];
            e->pos++;
            yep_view pa = ev.anchor, pt = ev.tag; /* props before the opener */
            uint32_t paid = ev.anchor_id;
            e_event_init(&ev, oc == '[' ? YEP_EV_SEQ_START : YEP_EV_MAP_START);
            ev.flow = 1;
            ev.anchor = pa;
            ev.anchor_id = paid;
            ev.tag = pt;
            ev.line = e->line;
            ev.col = e_col(e, e->pos);
            ev.end_line = e->line; /* flow opens end after the indicator */
            ev.end_col = e_col(e, e->pos) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            st[n - 1].just_closed = 0;
            /* the nested collection is an entry of the parent frame; when
             * it appears in a map's value position it completes the pair */
            int coll_key = (st[n - 1].kind == 1 && st[n - 1].pending_key == 0 && !q_key);
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 1) {
                st[n - 1].pending_key = 0;
            }
            st[n - 1].key_coll = (uint8_t)coll_key;
            st[n - 1].sep = 0;
            st[n - 1].entries++;
            e_flow_frame_init(st, n, (oc == '[') ? 0 : 1, 0);
            n++;
            continue;
        }
        if (rc == 1) {
            st[n - 1].sep = 0;
            st[n - 1].just_closed = 0;
            st[n - 1].entries++;
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].entry_line = e->line; /* node end (pre-colon ws) */
            }
            if (st[n - 1].pair_open) {
                st[n - 1].pair_value = 1; /* the pair's value arrived */
            }
            /* key detection: ':' after the node (spaces and line breaks
             * may precede it: "unquoted : value", "\"foo\"\n: bar").
             * After a QUOTED node the colon separates JSON-style. */
            size_t save_pos = e->pos;
            uint32_t save_line = e->line;
            size_t save_ls = e->line_start;
            if (e_flow_ws(e) != 0) {
                return -1;
            } /* crosses newlines; restored below when no colon */
            int quoted = (ev.type == YEP_EV_SCALAR && (ev.style == YEP_STYLE_SINGLE_QUOTED ||
                                                       ev.style == YEP_STYLE_DOUBLE_QUOTED));
            int key_pos = (st[n - 1].kind == 1 && st[n - 1].pending_key == 0);
            int cross_line = 0;
            for (size_t k = save_pos; k < e->pos; k++) {
                if (e->p[k] == '\n' || e->p[k] == '\r') {
                    cross_line = 1;
                    break;
                }
            }
            int colon = (e->pos < e->len && e->p[e->pos] == ':' &&
                         ((quoted && !ev.multiline && (!cross_line || st[n - 1].kind == 1)) ||
                          (key_pos && !ev.multiline && !cross_line) || e->pos + 1 >= e->len ||
                          e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                          e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
                          yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND)));
            if (!colon) {
                /* not a key; the caller re-reads — the line state goes
                 * back with the cursor (the ws lookahead crossed the
                 * newline; a stale e->line marked everything after the
                 * flow node one line low) */
                e->pos = save_pos;
                e->line = save_line;
                e->line_start = save_ls;
            }
            if (colon && st[n - 1].kind == 1 && st[n - 1].pending_key == 0) {
                if (!q_key && ((quoted && yep_simple_key_chars(ev.value.p, ev.value.len) + 2 >
                                              YEP_MAX_SIMPLE_KEY) ||
                               (!quoted && yep_simple_key_chars(ev.value.p, ev.value.len) >
                                               YEP_MAX_SIMPLE_KEY))) {
                    return e_fail(e, YEP_ERR_KEY_TOO_LONG, e->pos);
                }
                e->pos++;
                st[n - 1].pending_key = 1;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                /* value next; if none before , or } → null */
                continue;
            }
            if (colon && st[n - 1].kind == 0) {
                if (st[n - 1].pair_open && st[n - 1].pair_value) {
                    /* "[s: 2:]" — the open pair already has its value;
                     * a second single-pair would nest inside the value
                     * slot and unbalance the event stream (the nightly's
                     * run 38168927841). libyaml rejects the ':' mid-
                     * scalar; so does the suite's flow grammar. */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                if (!q_key && ((quoted && yep_simple_key_chars(ev.value.p, ev.value.len) + 2 >
                                              YEP_MAX_SIMPLE_KEY) ||
                               (!quoted && yep_simple_key_chars(ev.value.p, ev.value.len) >
                                               YEP_MAX_SIMPLE_KEY))) {
                    return e_fail(e, YEP_ERR_KEY_TOO_LONG, e->pos);
                }
                if (st[n - 1].buf_from >= 0 && !quoted && !q_key &&
                    (save_pos < e->line_start || ev.multiline)) {
                    /* "[a\n: b]": an IMPLICIT key cannot span lines ("? " may) */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                /* single-pair mapping inside a sequence: the entry's
                 * events are buffered; wrap them in MAP_START .. MAP_END */
                e->pos++;
                {
                    int32_t bf = st[n - 1].buf_from;
                    if (bf >= 0) {
                        st[n - 1].buf_from = -1;
                        e->ev_scopes--;
                        if (e_buf_wrap(e, (uint32_t)bf) != 0) {
                            return -2;
                        }
                        if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                            int brc = e_buf_send(e, 0);
                            if (brc != 0) {
                                return brc;
                            }
                        }
                        if (emit_now(e, &ev) != 0) {
                            return -2;
                        }
                        st[n - 1].pair_open = 1;
                        st[n - 1].pair_q = (uint8_t)q_key;
                        continue;
                    } else {
                        yep_event mk;
                        e_event_init(&mk, YEP_EV_MAP_START);
                        mk.flow = 1;
                        mk.line = ev.line;
                        mk.col = ev.col;
                        mk.end_line = ev.line; /* virtual opener: zero-span
                                                * at the implied position */
                        mk.end_col = ev.col;
                        if (emit_now(e, &mk) != 0) {
                            return -2;
                        }
                        if (emit_now(e, &ev) != 0) {
                            return -2;
                        }
                    }
                    st[n - 1].pair_open = 1;
                }
                continue; /* the value parses as the next regular node */
            }
            if (q_key && st[n - 1].kind == 0) {
                /* "[? x]": an explicit pair with no ':' — null value */
                int32_t bf = st[n - 1].buf_from;
                if (bf >= 0) {
                    st[n - 1].buf_from = -1;
                    e->ev_scopes--;
                    if (e_buf_wrap(e, (uint32_t)bf) != 0) {
                        return -2;
                    }
                    st[n - 1].pair_open = 1;
                    st[n - 1].pair_q = (uint8_t)q_key;
                    if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                        int brc = e_buf_send(e, 0);
                        if (brc != 0) {
                            return brc;
                        }
                    }
                    /* the parsed key rides the slot the empty stand-in
                     * occupied — dropping it lost the key's content AND
                     * its anchor (an alias to it then failed the
                     * placed-anchor check: the nightly's round 101) */
                    if (emit_now(e, &ev) != 0) {
                        return -2;
                    }
                    if (q_empty_key) {
                        st[n - 1].q_empty_value = 1;
                    }
                    continue;
                }
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 0) {
                /* bare entry in a flow map: key with null value */
                st[n - 1].pending_key = 2;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                continue;
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 1) {
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 0;
                continue;
            }
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            continue;
        }

        /* rc == 0: separator or close */
        if (e_flow_ws(e) != 0) {
            return -1;
        }
        if (e->pos >= e->len) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        unsigned char c = (unsigned char)e->p[e->pos];
        if (c == ',') {
            e->pos++;
            if (st[n - 1].kind == 0 && st[n - 1].pair_open && st[n - 1].pair_q &&
                !st[n - 1].pair_value && st[n - 1].q_empty_value == 1) {
                /* yeptris#741: the '?' empty-key pair's first comma is
                 * the entry separator libyaml's ENTRY_MAPPING_KEY
                 * consumed to reach its VALUE state — the pair stays
                 * open and awaits ':' (value), ',' (empty value +
                 * close), or ']' (empty value + close). Closing here
                 * spilled the following ': x' into a second entry
                 * ("[? ,: x , a]" read as two maps). */
                st[n - 1].q_empty_value = 2;
                continue;
            }
            if (st[n - 1].q_coll && st[n - 1].buf_from >= 0) {
                int32_t bf0 = st[n - 1].buf_from;
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e_buf_wrap(e, (uint32_t)bf0) != 0) {
                    return -2;
                }
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
                st[n - 1].q_coll = 0;
                st[n - 1].pair_open = 1;
            }
            if (st[n - 1].pair_open) {
                if (!st[n - 1].pair_value) {
                    yep_event nv;
                    e_event_init(&nv, YEP_EV_SCALAR);
                    nv.implicit = 1;
                    nv.style = YEP_STYLE_PLAIN;
                    if (emit_now(e, &nv) != 0) {
                        return -2;
                    }
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_END);
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                st[n - 1].pair_open = 0;
                st[n - 1].pair_value = 0;
                st[n - 1].pair_q = 0;
                st[n - 1].q_empty_value = 0;
            }
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
            }
            if (st[n - 1].sep) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "{,x}" / "[1,,2]" */
            }
            if (st[n - 1].entries == 0 && !st[n - 1].pair_open) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[, a]" */
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key != 0) {
                /* key without ':' value, or ':' without value: null value */
                yep_event nv;
                e_event_init(&nv, YEP_EV_SCALAR);
                nv.implicit = 1;
                nv.style = YEP_STYLE_PLAIN;
                if (emit_now(e, &nv) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 0;
            }
            if (st[n - 1].kind == 1 && st[n - 1].key_coll && st[n - 1].pending_key == 0) {
                /* "{[a], z: 1}" — a closed collection with no ':' is a
                 * bare key: libyaml emits the implicit null value */
                yep_event nv;
                e_event_init(&nv, YEP_EV_SCALAR);
                nv.implicit = 1;
                nv.style = YEP_STYLE_PLAIN;
                if (emit_now(e, &nv) != 0) {
                    return -2;
                }
                st[n - 1].key_coll = 0;
            }
            st[n - 1].sep = 1;
            st[n - 1].just_closed = 0;
            continue;
        }
        if (c == ']' || c == '}') {
            if (st[n - 1].q_coll && st[n - 1].buf_from >= 0) {
                int32_t bf0 = st[n - 1].buf_from;
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e_buf_wrap(e, (uint32_t)bf0) != 0) {
                    return -2;
                }
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
                st[n - 1].q_coll = 0;
                st[n - 1].pair_open = 1;
            }
            while (st[n - 1].pair_wrap) {
                /* a single-pair wrapper inside a sequence closes on any
                 * bracket ("[ : v ]", CFD4) */
                if (st[n - 1].pending_key != 0) {
                    yep_event nv;
                    e_event_init(&nv, YEP_EV_SCALAR);
                    nv.implicit = 1;
                    nv.style = YEP_STYLE_PLAIN;
                    if (emit_now(e, &nv) != 0) {
                        return -2;
                    }
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_END);
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                st[n - 1].pair_wrap = 0;
                n--;
                if (n == 0) {
                    break; /* wrapper was the last frame: malformed */
                }
                st[n - 1].just_closed = 1;
                st[n - 1].entry_line = e->line;
            }
            unsigned char want = st[n - 1].kind ? '}' : ']';
            if (c != want) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            /* YAML 1.2 allows a trailing separator before the closer
             * ("[1, ]", "{a: 1, }"); the comma handler already rejected
             * doubled commas. */
            if (st[n - 1].kind == 1 && st[n - 1].pending_key != 0) {
                yep_event nv;
                e_event_init(&nv, YEP_EV_SCALAR);
                nv.implicit = 1;
                nv.style = YEP_STYLE_PLAIN;
                if (emit_now(e, &nv) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 0;
            }
            if (st[n - 1].kind == 1 && st[n - 1].key_coll && st[n - 1].pending_key == 0) {
                /* "{{}}" / "{[a]}" — the closed collection is a bare
                 * key: libyaml emits the implicit null value before
                 * the frame's own MAP_END */
                yep_event nv;
                e_event_init(&nv, YEP_EV_SCALAR);
                nv.implicit = 1;
                nv.style = YEP_STYLE_PLAIN;
                if (emit_now(e, &nv) != 0) {
                    return -2;
                }
                st[n - 1].key_coll = 0;
            }
            if (st[n - 1].pair_open) {
                if (!st[n - 1].pair_value) {
                    yep_event nv;
                    e_event_init(&nv, YEP_EV_SCALAR);
                    nv.implicit = 1;
                    nv.style = YEP_STYLE_PLAIN;
                    if (emit_now(e, &nv) != 0) {
                        return -2;
                    }
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_END);
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                st[n - 1].pair_open = 0;
                st[n - 1].pair_value = 0;
                st[n - 1].q_empty_value = 0;
            }
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
            }
            e->pos++;
            e_event_init(&ev, st[n - 1].kind ? YEP_EV_MAP_END : YEP_EV_SEQ_END);
            /* the close indicator's span: e->pos sits just past it */
            ev.line = e->line;
            ev.col = e_col(e, e->pos - 1) + 1;
            ev.end_line = e->line;
            ev.end_col = e_col(e, e->pos) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            n--;
            if (n == 0) {
                if (e->ev_scopes != 0) {
                    return e_fail(e, YEP_ERR_INTERNAL, e->pos);
                }
                if (e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
                e_skip_inline_space(e);
                return 0;
            }
            st[n - 1].just_closed = 1;
            if (st[n - 1].pair_open) {
                st[n - 1].pair_value = 1;
            }
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].entry_line = e->line; /* the nested key ended here */
            }
            continue;
        }
        if (c == ':') {
            e->pos++;
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 2) {
                /* ':' after a bare key completes it (value next) */
                st[n - 1].pending_key = 1;
                continue;
            }
            if (st[n - 1].kind == 1 && st[n - 1].just_closed) {
                /* '[a, b]: v' inside a map — the closed collection is the key */
                st[n - 1].just_closed = 0;
                st[n - 1].key_coll = 0;
                st[n - 1].pending_key = 1;
                continue;
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 0 &&
                (st[n - 1].sep || st[n - 1].entries == 0)) {
                /* '{: x' or '{a, : x' — empty key at a fresh entry */
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.implicit = 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 1;
                st[n - 1].entries = 1;
                continue;
            }
            if (st[n - 1].kind == 0 && st[n - 1].buf_from >= 0) {
                if (st[n - 1].pair_open && st[n - 1].pair_value) {
                    /* "[s: 2\n:]" — the loop-top twin of the inline
                     * colon guard: a buffered value cannot wrap as
                     * another pair once the open pair is complete */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                if (st[n - 1].entry_line != e->line) {
                    /* the key ended on an earlier line */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                /* '[ [a, b]: v' — the buffered collection is the pair key */
                st[n - 1].q_coll = 0;
                int32_t bf0 = st[n - 1].buf_from;
                int was_empty = ((uint32_t)bf0 == e->ev_buf_n);
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e_buf_wrap(e, (uint32_t)bf0) != 0) {
                    return -2;
                }
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
                if (was_empty) {
                    yep_event kv; /* "[ : v" — the empty key (CFD4,
                                   * ledgered suite-VALID) */
                    e_event_init(&kv, YEP_EV_SCALAR);
                    kv.implicit = 1;
                    if (emit_now(e, &kv) != 0) {
                        return -2;
                    }
                }
                st[n - 1].pair_open = 1;
                /* the wrapped key's own open marked pair_value=1 ("a
                 * VALUE collection completes the pending pair"); as a
                 * KEY it completes nothing — the pair awaits a fresh
                 * value, else the boundary flush skips the null ("[[a]:
                 * ]" lost it, the nightly's round 9) */
                st[n - 1].pair_value = 0;
                continue; /* value next */
            }
            if (st[n - 1].kind == 0 && n >= 1) {
                if (st[n - 1].pair_open && st[n - 1].q_empty_value == 2) {
                    /* yeptris#741: the ':' after the '?' empty-key
                     * pair's comma completes THE SAME pair (the pair's
                     * VALUE indicator), not a new entry */
                    size_t p2 = e->pos; /* the ':' byte is already consumed */
                    while (p2 < e->len && (e->p[p2] == ' ' || e->p[p2] == '\t')) {
                        p2++;
                    }
                    if (p2 < e->len && (e->p[p2] == ',' || e->p[p2] == ']' || e->p[p2] == '}')) {
                        /* empty value at the closer: the pair closes
                         * here; the closer dispatches as the separator */
                        yep_event nv;
                        e_event_init(&nv, YEP_EV_SCALAR);
                        nv.implicit = 1;
                        nv.style = YEP_STYLE_PLAIN;
                        if (emit_now(e, &nv) != 0) {
                            return -2;
                        }
                        yep_event mk;
                        e_event_init(&mk, YEP_EV_MAP_END);
                        if (emit_now(e, &mk) != 0) {
                            return -2;
                        }
                        st[n - 1].pair_open = 0;
                        st[n - 1].pair_value = 0;
                        st[n - 1].pair_q = 0;
                        st[n - 1].q_empty_value = 0;
                        st[n - 1].sep = 1;
                        st[n - 1].just_closed = 0;
                        e->pos = p2;
                        continue;
                    }
                    st[n - 1].q_empty_value = 0;
                    e->pos++;
                    continue;
                }
                if (st[n - 1].pair_open) {
                    /* "[s: : v]" — ':' in the open pair's value slot
                     * is not node content (libyaml: "did not find
                     * expected node content") */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                /* '[ : v' — single-pair mapping with an empty key
                 * (CFD4, ledgered suite-VALID: the suite's tree parses
                 * the bare-colon entry; libyaml's rejection is the
                 * ledgered deviation) */
                if (n >= e->max_depth) {
                    return e_fail(e, YEP_ERR_DEPTH, e->pos);
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_START);
                mk.flow = 1;
                e_pos_mark(e, e->pos, &mk.line, &mk.col);
                mk.end_line = mk.line; /* virtual opener: zero-span */
                mk.end_col = mk.col;
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.implicit = 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
                st[n - 1].sep = 0;
                st[n - 1].entries++;
                e_flow_frame_init(st, n, 1, 1);
                st[n].pair_wrap = 1; /* the seq single-pair wrapper */
                n++;
                continue;
            }
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        /* any other stray byte */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
}

/* -------------------------------------------------------- block machine */

/* -------------------------------------------- classified fast arms (49) */
/* Dispatch decided once per line from scan's shape facts. Each arm
 * mirrors the general chain's emission sequence exactly; returns
 * 1 = node complete, 0 = not applicable, <0 error to propagate. */

/* Plain (optionally anchored) value: emit, fold continuations, land at
 * the line end. root_ctx is 0 — every arm has pushed a frame. */
static int e_shape_plain_value(yep_engine* e, const yep_line_shape* sh, uint32_t floor,
                               yep_view anchor, uint32_t anchor_id) {
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.value.p = e->p + sh->val_span.start;
    ev.value.len = sh->val_span.end - sh->val_span.start;
    ev.borrowed = 1;
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    ev.line = e->line;
    ev.col = e_col(e, sh->val_start) + 1;
    e->pos = sh->val_span.end;
    if (e_plain_multiline(e, sh->val_span, floor, &ev, 0) != 0) {
        return -1;
    }
    if (e->fold_n == 1 && sh->val_span.term == YEP_TERM_COMMENT) {
        e_skip_to_eol(e); /* single line ending in a comment */
    }
    /* Dash-item fast path (TODO.restructure/78): the prepared value
     * is offered whole — the same contract as the pair batch; the
     * top frame is the sequence e_open_seq just opened/continued. */
    if (e->sink != NULL && e->sink->on_block_item != NULL) {
        yep_block_value v;
        memset(&v, 0, sizeof(v));
        v.cls = YEP_LVAL_PLAIN;
        v.borrowed = ev.borrowed;
        v.value = ev.value;
        int built = e->sink->on_block_item(e->sink->ctx, &v);
        if (built < 0) {
            return -2;
        }
        if (built == 1) {
            return 1;
        }
    }
    return emit_now(e, &ev) == 0 ? 1 : -2;
}

/* Flow value: the JSON fast path first; anything else restores the
 * cursor and runs the value chain (which re-derives the flow arms). */
static int e_shape_flow_value(yep_engine* e, const yep_line_shape* sh, uint32_t floor,
                              yep_ctx next) {
    yep_view none = {NULL, 0};
    e->pos = sh->val_start;
    e->flow_floor = floor;
    e->flow_enforce = (e->depth > 0) && !e->compat_grammar;
    int fast = e_flow_json(e, none, none, 0);
    e->flow_enforce = 0;
    if (fast == -2) {
        return -2;
    }
    if (fast == 1) {
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "k: [1] x" */
        }
        return 1;
    }
    e->pos = sh->val_start;
    int rc = e_parse_value(e, next, floor);
    return rc == 0 ? 1 : rc;
}

/* Alias value; a ':' after the name is a compact key under a dash and
 * the error the general path raises under a key. */
static int e_shape_alias_value(yep_engine* e, const yep_line_shape* sh, int under_key) {
    yep_view none = {NULL, 0};
    e->pos = sh->val_start;
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.line = e->line;
    ev.col = e_col(e, e->pos) + 1;
    if (e_alias(e, &ev) != 0) {
        return -1;
    }
    if (e_colon_at(e, e->pos)) {
        if (under_key || ev.multiline) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (e_simple_key_ok(e, sh->val_start, e->pos) != 0) {
            return -1;
        }
        uint32_t key_col = e_col(e, sh->val_start);
        int rc = e_open_map(e, key_col, e->line, key_col + 1, none, none, 0, 0);
        if (rc != 0) {
            return rc;
        }
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
        e->pos++; /* ':' */
        rc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
        return rc == 0 ? 1 : rc;
    }
    if (!e_at_eol(e)) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    return emit_now(e, &ev) == 0 ? 1 : -2;
}

/* The classified line's key event — constructed ONLY on paths that
 * emit it: the pair path takes the whole line without events, and
 * building kv up front discarded ~48 stores per classified line
 * (TODO.restructure/63 phase 1). */
static void e_key_event(const yep_engine* e, const yep_line_shape* sh, uint32_t key_col,
                        uint32_t line, yep_event* kv) {
    e_event_init(kv, YEP_EV_SCALAR);
    kv->style = YEP_STYLE_PLAIN;
    kv->implicit = 1;
    kv->value.p = e->p + sh->key_start;
    kv->value.len = sh->key_end - sh->key_start;
    kv->borrowed = 1;
    kv->line = line;
    kv->col = key_col + 1;
    /* keys are single-line by the simple-key law; the cursor may have
     * advanced past the line, so the mark comes from e_pos_mark */
    e_pos_mark(e, sh->key_end, &kv->end_line, &kv->end_col);
}

static int e_classified(yep_engine* e, uint32_t floor_col) {
    yep_line_info li = e_line_info_here(e);
    if (e->pos != e->line_start + li.indent) {
        return 0; /* mid-line entry: the chain keeps its dispatch */
    }
    const yep_line_shape* sh = e_shape_here(e);
    if (sh->val == YEP_LVAL_NONE) {
        return 0; /* classified line, unclassified value: bail whole */
    }
    yep_view none = {NULL, 0};

    if (sh->kind == YEP_LSHAPE_DASH) {
        switch (sh->val) {
        case YEP_LVAL_EMPTY:
        case YEP_LVAL_FLOW:
        case YEP_LVAL_ALIAS:
        case YEP_LVAL_PLAIN:
            break; /* the anchored entry and anything else: general chain */
        default:
            return 0;
        }
        uint32_t col = e_col(e, e->pos);
        int rc = e_open_seq(e, col, e->line, col + 1, none, none, 0);
        if (rc != 0) {
            return rc;
        }
        switch (sh->val) {
        case YEP_LVAL_EMPTY:
            e->pos = sh->dash + 1;
            rc = e_parse_value(e, YEP_CTX_AFTER_DASH, col);
            return rc == 0 ? 1 : rc;
        case YEP_LVAL_FLOW:
            return e_shape_flow_value(e, sh, col, YEP_CTX_AFTER_DASH);
        case YEP_LVAL_ALIAS:
            return e_shape_alias_value(e, sh, 0);
        default:
            return e_shape_plain_value(e, sh, col, none, 0);
        }
    }

    /* KEY: plain key + ':' — the mapping workhorse line */
    if (sh->kind != YEP_LSHAPE_KEY) {
        return 0;
    }
    if (e_simple_key_ok(e, sh->key_start, sh->key_end) != 0) {
        return -1;
    }
    uint32_t key_col = e_col(e, e->pos);
    /* `key:` with the value on following lines, opening a FRESH map:
     * the sink may build map+key in one call (TODO.restructure/57) —
     * the engine then opens its frame silently. Sibling keys (the map
     * already continues at this column) keep the key-only event. */
    if (sh->val == YEP_LVAL_EMPTY && e->sink != NULL && e->sink->on_block_open != NULL &&
        !(e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
          e->frames[e->depth - 1].col == key_col)) {
        yep_view okey = {e->p + sh->key_start, sh->key_end - sh->key_start};
        int built = e->sink->on_block_open(e->sink->ctx, &okey, e->line, key_col);
        if (built < 0) {
            return -2;
        }
        if (built == 1) {
            int orc = e_open_map(e, key_col, e->line, key_col + 1, none, none, 0, 1);
            if (orc != 0) {
                return orc;
            }
            e->pos = sh->colon + 1;
            orc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
            return orc == 0 ? 1 : orc;
        }
    }
    int rc = e_open_map(e, key_col, e->line, key_col + 1, none, none, 0, 0);
    if (rc != 0) {
        return rc;
    }
    /* Whole-line fast path (TODO.restructure/54): the sink may build
     * the key and its classified value as one unit — no events. The
     * value EVENT is assembled ONLY on the fallback path (the sinks
     * that take the pair read the yep_block_value and ignore it): the
     * semantic helpers run unchanged (alias resolution, anchor
     * definition, the fold), the fallback reuses the pair facts
     * captured before the fold — e_plain_multiline advances
     * line/pos across continuations, so post-fold e_col would lie. */
    /* the pair facts are captured BEFORE the fold: e_plain_multiline
     * advances line/pos across continuations */
    uint32_t pair_line = e->line;
    uint32_t pair_val_col = e_col(e, sh->val_start);
    yep_block_value v;
    memset(&v, 0, sizeof(v));
    v.cls = (uint8_t)sh->val;
    yep_event vv; /* the fields the fallback reads are assigned HERE and
                   * overwritten by the arms/helpers — MSVC C4701 cannot
                   * see through the helpers' bypointer writes, so the
                   * zero-init lives in this function, not a memset */
    vv.value.p = NULL;
    vv.value.len = 0;
    vv.borrowed = 0;
    vv.anchor.p = NULL;
    vv.anchor.len = 0;
    vv.anchor_id = 0;
    vv.end_line = 0;
    vv.end_col = 0;
    int prepared = 0;
    if (sh->val == YEP_LVAL_ALIAS) {
        e->pos = sh->val_start;
        if (e_alias(e, &vv) != 0) {
            return -1; /* undefined alias: the chain's error */
        }
        if (e_colon_at(e, e->pos)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        v.value = vv.value;
        v.anchor_id = vv.anchor_id;
        prepared = 1;
    } else if (sh->val == YEP_LVAL_PLAIN || sh->val == YEP_LVAL_ANCHOR_PLAIN) {
        if (sh->val == YEP_LVAL_ANCHOR_PLAIN) {
            v.anchor.p = e->p + sh->val_start + 1;
            v.anchor.len = sh->anchor_end - sh->val_start - 1;
            v.anchor_id = anchor_define(e, v.anchor);
            vv.anchor = v.anchor;
            vv.anchor_id = v.anchor_id;
        }
        vv.value.p = e->p + sh->val_span.start;
        vv.value.len = sh->val_span.end - sh->val_span.start;
        vv.borrowed = 1;
        e->pos = sh->val_span.end;
        if (e_plain_multiline(e, sh->val_span, key_col, &vv, 0) != 0) {
            return -1;
        }
        if (e->fold_n == 1 && sh->val_span.term == YEP_TERM_COMMENT) {
            e_skip_to_eol(e); /* single line ending in a comment */
        }
        v.value = vv.value;
        v.borrowed = vv.borrowed;
        prepared = 1;
    }

    if (prepared) {
        if (e->sink != NULL && e->sink->on_block_pair != NULL) {
            yep_view key = {e->p + sh->key_start, sh->key_end - sh->key_start};
            /* the fused placement places the value node: its anchor
             * rides v — mark it so a later alias may parse (the
             * placement sites are the alias check's ground truth; the
             * classified KEY is always plain — anchored keys ride the
             * general chain) */
            e_anchor_mark(e, v.anchor_id);
            int built =
                e->sink->on_block_pair(e->sink->ctx, &key, &v, pair_line, key_col, pair_val_col);
            if (built < 0) {
                return -2;
            }
            if (built == 1) {
                return 1;
            }
        }
        /* Fallback: assemble the value event with the helper-filled
         * fields preserved — e_alias stamps type=ALIAS + the end marks,
         * e_plain_multiline stamps the fold end marks; the pair facts
         * taken pre-fold (post-fold e_col would read the advanced
         * line/pos). Field-for-field what the pre-fill built. */
        yep_event ve;
        e_event_init(&ve, YEP_EV_SCALAR);
        ve.line = pair_line;
        ve.col = (uint32_t)pair_val_col + 1;
        ve.value = vv.value;
        ve.borrowed = vv.borrowed;
        ve.anchor = vv.anchor;
        ve.anchor_id = vv.anchor_id;
        ve.end_line = vv.end_line;
        ve.end_col = vv.end_col;
        if (sh->val == YEP_LVAL_ALIAS) {
            ve.type = YEP_EV_ALIAS;
        } else {
            ve.style = YEP_STYLE_PLAIN;
            ve.implicit = 1;
        }
        yep_event kv;
        e_key_event(e, sh, key_col, pair_line, &kv);
        if (emit_now(e, &kv) != 0 || emit_now(e, &ve) != 0) {
            return -2;
        }
        return 1;
    }

    /* EMPTY / FLOW: no prepared pair — the value is not on this line */
    switch (sh->val) {
    case YEP_LVAL_EMPTY: {
        yep_event kv;
        e_key_event(e, sh, key_col, pair_line, &kv);
        if (emit_now(e, &kv) != 0) {
            return -2;
        }
        e->pos = sh->colon + 1;
        rc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
        return rc == 0 ? 1 : rc;
    }
    case YEP_LVAL_FLOW: {
        yep_event kv;
        e_key_event(e, sh, key_col, pair_line, &kv);
        if (emit_now(e, &kv) != 0) {
            return -2;
        }
        return e_shape_flow_value(e, sh, key_col, YEP_CTX_AFTER_COLON);
    }
    default:
        return 0; /* PLAIN/ALIAS/ANCHOR_PLAIN handled above (unreachable) */
    }
}

/* Parses the node at the cursor (content position). Returns 0 ok,
 * -1 error, -2 sink abort. Collections may remain open (frames).
 *
 * Property semantics: same-line props belong to the node that follows
 * them (a key scalar when the line turns out to be a mapping entry);
 * pend props (parsed at the parent's value position on an earlier line)
 * belong to the VALUE node as a whole — a collection START event when
 * the value is a collection. */
static int e_node(yep_engine* e, yep_ctx ctx, uint32_t floor_col) {
    size_t node_at = e->pos;
    yep_view pend_a = e->pend_anchor, pend_t = e->pend_tag;
    uint32_t pend_aid = e->pend_anchor_id;
    e->pend_anchor.p = NULL;
    e->pend_anchor.len = 0;
    e->pend_anchor_id = 0;
    e->pend_tag.p = NULL;
    e->pend_tag.len = 0;
    yep_view anchor = {0}, tag = {0};
    e_props(e, &anchor, &tag);
    if (e->err.code != YEP_ERR_NONE) {
        return -1; /* e_props failed (unresolved tag handle) */
    }
    uint32_t anchor_ordinal = 0;
    if (!yep_view_is_empty(anchor)) {
        anchor_ordinal = anchor_define(e, anchor);
    }
    /* Node-level props for scalar/alias/flow VALUE events: same-line
     * wins over pend. */
    yep_view node_a = yep_view_is_empty(anchor) ? pend_a : anchor;
    yep_view node_t = yep_view_is_empty(tag) ? pend_t : tag;
    /* the id rides the SAME precedence as the view: same-line props win
     * over pend — a props-only line followed by its node ("&a\n- x",
     * "&a\n[b]") carried the pend VIEW with a zero id and the DOM never
     * bound the ordinal (found by the self-referencing-structures spec:
     * "--- &id001\n- *id001" failed with an EMPTY error message) */
    uint32_t node_aid = anchor_ordinal != 0 ? anchor_ordinal : pend_aid;

    /* One dispatch decision per line (TODO.restructure/49): a fresh
     * line with no pending props and a classified shape takes the fast
     * arms; every other entry keeps the chain below unchanged. */
    if ((ctx == YEP_CTX_FRESH || ctx == YEP_CTX_VALUE_LINE) && yep_view_is_empty(pend_a) &&
        yep_view_is_empty(pend_t) && pend_aid == 0) {
        int frc = e_classified(e, floor_col);
        if (frc != 0) {
            return frc > 0 ? 0 : frc;
        }
    }

    e_skip_inline_space(e);

    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.anchor = node_a;
    ev.anchor_id = node_aid;
    ev.tag = node_t;
    ev.line = e->line;
    ev.col = e_col(e, node_at) + 1;

    if (e_at_eol(e)) {
        /* properties with the value on following lines */
        if (e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
            e->frames[e->depth - 1].col == e_col(e, node_at) &&
            (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag))) {
            /* a props-only line at an ESTABLISHED pair position: the
             * scanner never finds the simple key's ':' (libyaml:
             * "could not find expected ':' while scanning a simple
             * key"; Psych rejects "k: 1\n!!t: &b\n  n: m" and the
             * anchor-only twins). The old path consumed the line as
             * node props — the pair's key event vanished, the key's
             * tag leaked onto the value node, and a long-tag value
             * then lost its anchor at emit (the nightly's run
             * 38275208009). The FIRST pair of a fresh map ("a:\n
             * !!t: &c\n    n: m") has no frame at the column yet and
             * stays legal. */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (!yep_view_is_empty(pend_a) && !yep_view_is_empty(anchor)) {
            /* a second props line carrying its own ANCHOR displaces the
             * pend anchor (same-line-wins) — the displaced anchor stays
             * in the nametab, so a later alias emits a dangling
             * reference (the nightly's run 38271222289, "&a\n&a1\nF:
             * *a" and its '? &a' explicit-key twin). libyaml rejects
             * every nesting variant ("did not find expected <document
             * start>" / "did not find expected key"); a TAG-only
             * continuation ("&a\n!!str v") stays legal. */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        e->pend_anchor = node_a;
        e->pend_anchor_id = anchor_ordinal;
        e->pend_tag = node_t;
        e->doc_inline = 0; /* the node starts on a later line */
        if (++e->rec >= e->max_depth) {
            e->rec--;
            return e_fail(e, YEP_ERR_DEPTH, e->pos);
        }
        int rc = e_parse_value(e, ctx, floor_col);
        e->rec--;
        return rc;
        /* (ctx stays: AFTER_COLON converts to VALUE_LINE at the break) */
    }

    unsigned char c = (unsigned char)e->p[e->pos];

    if (c == ',' && (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag) ||
                     !yep_view_is_empty(pend_a) || !yep_view_is_empty(pend_t))) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "!!str, xxx" (block) */
    }
    if (c == '-' && (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                     e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
        if (e->pos > 0 && e->p[e->pos - 1] == '\t' && !e->doc_inline) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "-\t-" (Y79Y) */
        }
        if (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "&a - x" */
        }
        uint32_t col = e_col(e, e->pos);
        int rc = e_open_seq(e, col, e->line, col + 1, node_a, node_t, node_aid);
        if (rc != 0) {
            return rc;
        }
        e->pos++; /* '-' */
        return e_parse_value(e, YEP_CTX_AFTER_DASH, col);
    }

    if (c == '?' && (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                     e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
        if (ctx == YEP_CTX_AFTER_COLON || e->cv_line == e->line) {
            /* "key: ? a" and ": ?" — a key indicator where a value
             * belongs (libyaml rejects); accepting it produced an
             * unpairable event stream whose orphaned anchored node
             * the emit dropped (the nightly's round 102). The
             * main-dispatch ':' line rides AFTER_DASH, so the
             * colon's own line marks it. */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag)) {
            /* node properties followed by a second key indicator on
             * the SAME line: libyaml "mapping keys are not allowed in
             * this context" (psych rejects "? &a ? b" and "k: &a ? b";
             * the cross-line twin "? &a\n  ? b" composes and stays
             * legal). The old path opened the nested key map with the
             * props dropped — a later alias then emitted a dangling
             * reference (the nightly's run 38355778230). */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (e->pos > 0 && e->p[e->pos - 1] == '\t' && !e->doc_inline) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "?\t-" (Y79Y) */
        }
        uint32_t col = e_col(e, e->pos);
        int rc = e_open_map(e, col, e->line, col + 1, pend_a, pend_t, pend_aid, 0);
        if (rc != 0) {
            return rc;
        }
        if (e->qmd_n >= 16) {
            return e_fail(e, YEP_ERR_DEPTH, e->pos); /* the depth-guard law */
        }
        /* a pending from an EARLIER line at this level completes with a
         * null value before the new pair opens — without the flush the
         * two pairs share one wrapper and the merge model leaves the
         * later value an unpairable orphan (the nightly's round 102) */
        /* a key indicator at the pending pair's own depth from a LATER
         * line completes that pair with a null value (libyaml's
         * same-indent KEY rule). The pending may still be mid-content
         * (this '?' is being parsed as its content): arm the flag the
         * flush keys on — the outer branch re-arms only if its slot
         * survived. Without the flush the outer pair's value never
         * emits and the DOM drops the unpaired node (the nightly's
         * round 102). */
        if (e->qmd_n > 0 && e->qmd_stack[e->qmd_n - 1] == e->depth &&
            e->qmd_line[e->qmd_n - 1] != e->line) {
            if (!e->q_value_pending) {
                e->q_value_pending = 1;
            }
            int frc = e_flush_q_value(e);
            if (frc != 0) {
                return frc;
            }
        }
        e->qmd_line[e->qmd_n] = e->line;
        e->qmd_stack[e->qmd_n++] = e->depth;
        e->pos++;
        e_skip_inline_space(e);
        if (e->pos < e->len && e->p[e->pos] == ':' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            size_t after = e->pos + 1;
            while (after < e->len && (e->p[after] == ' ' || e->p[after] == '\t')) {
                after++;
            }
            if (after >= e->len || e->p[after] == '\n' || e->p[after] == '\r' ||
                (e->p[after] == '#' && after > e->pos + 1)) {
                /* '? :' at end of line — empty explicit key */
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.style = YEP_STYLE_PLAIN;
                kv.implicit = 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
                e->pos++;
                if (e->qmd_n > 0) {
                    e->qmd_n--; /* the ':' line supplied the innermost value */
                }
                return e_parse_value(e, YEP_CTX_AFTER_COLON, col);
            }
            /* '? : x' — the ':' starts a nested pair: the KEY is that map */
        }
        /* the explicit key is parsed as a value (may be empty → null);
         * its content may ALIGN with the '?' column */
        e->q_key_pending = 1;
        {
            int my_qmd = e->qmd_n; /* this pair's stack slot is my_qmd - 1 */
            int qrc = e_parse_value(e, YEP_CTX_AFTER_Q, col);
            if (qrc != 0) {
                return qrc;
            }
            /* a later-line '?' completed this pair inside the content
             * parse (the flush popped our slot): the flag must not
             * re-arm, or the stale pending emits a stray null */
            if (e->qmd_n >= my_qmd) {
                e->q_value_pending = 1; /* until a ':' line supplies the value */
            }
            return 0;
        }
    }

    if (c == '[' || c == '{') {
        /* pre-scan: is this flow collection a mapping KEY? (a key must
         * begin and end on one line — "[23\n]: 42" is not a key) */
        {
            /* JSON fast path first: its validating scan already finds the
             * close, replacing e_skip_flow's pre-scan when it applies. */
            e->flow_floor = floor_col;
            e->flow_enforce = (e->depth > 0) && !e->compat_grammar;
            int fast = e_flow_json(e, node_a, node_t, anchor_ordinal);
            e->flow_enforce = 0;
            if (fast == 1) {
                if (!e_at_eol(e)) {
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[ a ] ]" */
                }
                if (e->depth == 0) {
                    e->last_root_flow = 1; /* "[23\n]: 42" is not a key */
                }
                return 0;
            }
            if (fast == -2) {
                return -2;
            }
        }
        size_t save = e->pos;
        uint32_t line0 = e->line;
        size_t ls0 = e->line_start;
        if (e_skip_flow(e) != 0) {
            return -1;
        }
        e_skip_inline_space(e);
        int is_key = e_colon_at(e, e->pos) && e->line == line0;
        /* the pre-scan tracks lines while it crosses them; the re-scan
         * below re-derives every mark from the open position, so the
         * line state must go back with the cursor (the nightly's run
         * 38280372447: a multi-line flow's open event marked at the
         * pre-scan's far line, diverging from the JSON fast path) */
        e->pos = save;
        e->line = line0;
        e->line_start = ls0;
        if (is_key && ctx == YEP_CTX_AFTER_COLON) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (is_key) {
            if (e_simple_key_ok(e, node_at, e->pos) != 0) {
                return -1;
            }
            uint32_t key_col = e_col(e, node_at);
            int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
            if (rc != 0) {
                return rc;
            }
            /* the pend props went to the mapping; the key collection
             * keeps only same-line props (Psych: "!t\n[a]: v" tags
             * the map, "!t [a]: v" tags the key) */
            rc = e_flow(e, anchor, tag, anchor_ordinal);
            if (rc != 0) {
                return rc;
            }
            e->pos++; /* ':' */
            return e_pair_value_done(e, ctx, key_col);
        }
        {
            e->flow_floor = floor_col;
            e->flow_enforce = (e->depth > 0) && !e->compat_grammar;
            int rc = e_flow(e, node_a, node_t, node_aid);
            e->flow_enforce = 0;
            if (rc != 0) {
                return rc;
            }
            if (!e_at_eol(e)) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[ a ] ]" */
            }
            if (e->depth == 0) {
                e->last_root_flow = 1; /* "[23\n]: 42" is not a key */
            }
            return 0;
        }
    }

    if (c == '"' || c == '\'') {
        if (e_quoted_floor(e, &ev, floor_col, e->depth > 0) != 0) {
            return -1;
        }
        if (e_colon_at(e, e->pos)) {
            if (ctx == YEP_CTX_AFTER_COLON || ev.multiline) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            if (e_simple_key_ok(e, node_at, e->pos) != 0) {
                return -1;
            }
            uint32_t key_col = e_col(e, node_at);
            int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
            if (rc != 0) {
                return rc;
            }
            /* the pend props went to the mapping; the key keeps only
             * same-line props — ev carries the merged views and would
             * leak the map's tag onto the key (the nightly's
             * byte-instability: "!<!>\n\"\":\n" re-parsed with a
             * tagged first key and never re-stabilized) */
            ev.anchor = anchor;
            ev.anchor_id = anchor_ordinal;
            ev.tag = tag;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            e->pos++; /* ':' */
            return e_pair_value_done(e, ctx, key_col);
        }
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return emit_now(e, &ev) == 0 ? 0 : -2;
    }

    if (c == '*') {
        if (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "&a *b" */
        }
        if (e_alias(e, &ev) != 0) {
            return -1;
        }
        if (e_colon_at(e, e->pos)) {
            if (ctx == YEP_CTX_AFTER_COLON || ev.multiline) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            if (e_simple_key_ok(e, node_at, e->pos) != 0) {
                return -1;
            }
            uint32_t key_col = e_col(e, node_at);
            int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
            if (rc != 0) {
                return rc;
            }
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            e->pos++;
            return e_pair_value_done(e, ctx, key_col);
        }
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return emit_now(e, &ev) == 0 ? 0 : -2;
    }

    if (c == '|' || c == '>') {
        int parent_col = (e->depth == 0) ? -1 : (int)floor_col;
        if (e_block_scalar(e, &ev, parent_col) != 0) {
            return -1;
        }
        return emit_now(e, &ev) == 0 ? 0 : -2;
    }

    /* plain scalar */
    if (!yep_plain_first_ok((unsigned char)e->p[e->pos])) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* lone "]" / "}" / "," */
    }
    yep_span s = yep_scan_plain(e->p, e->len, e->pos, 0);
    if (s.term != YEP_TERM_COLON && !yep_view_is_empty(pend_a) && !yep_view_is_empty(anchor)) {
        /* two anchors on ONE scalar ("&a
&b v"); a colon means the
         * inline anchor belongs to the key and the pend one elsewhere */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    if (s.term != YEP_TERM_COLON && ctx == YEP_CTX_FRESH && e->depth > 0 &&
        e_col(e, node_at) == e->frames[e->depth - 1].col) {
        /* a plain scalar at an open container's column with no ':' —
         * neither a mapping key nor a sequence entry */
        return e_fail(e, YEP_ERR_UNEXPECTED, s.end);
    }
    if (s.term == YEP_TERM_COLON) {
        if (ctx == YEP_CTX_AFTER_COLON) {
            return e_fail(e, YEP_ERR_UNEXPECTED, s.end);
        }
        if (node_at > 0 && e->p[node_at - 1] == '\t' && !e->doc_inline) {
            /* (on the --- line the marker-line gate names the same
             * refusal with libyaml's words — #900) */
            return e_fail(e, YEP_ERR_UNEXPECTED, node_at); /* ":\tkey:" */
        }
        if (e_simple_key_ok(e, s.start, s.end) != 0) {
            return -1;
        }
        /* ("--- &a k: v" rides the e_open_map gate — the marker-line
         * law has one home now; the propped form used to fail here
         * with the generic default-arm diagnostic.) */
        uint32_t key_col = e_col(e, node_at);
        int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
        if (rc != 0) {
            return rc;
        }
        yep_event kv;
        e_event_init(&kv, YEP_EV_SCALAR);
        kv.anchor = anchor;
        kv.anchor_id =
            anchor_ordinal; /* the KEY binds its anchor; the
                             * DOM keys on the ordinal (a key-anchored scalar's aliases
                             * resolved to garbage without it — found by flow-direct-diff) */
        kv.tag = tag;
        kv.style = YEP_STYLE_PLAIN;
        kv.implicit = 1;
        kv.value.p = e->p + s.start;
        kv.value.len = s.end - s.start;
        kv.borrowed = 1;
        kv.line = e->line;
        kv.col = key_col + 1;
        e_pos_mark(e, s.end, &kv.end_line, &kv.end_col);
        if (emit_now(e, &kv) != 0) {
            return -2;
        }
        /* advance from the TRIMMED span end (not the stale pre-scan
         * position — the first ':' in the buffer may be INSIDE the key);
         * spaces may still precede the terminating colon */
        e->pos = s.end;
        while (e->pos < e->len && e->p[e->pos] != ':') {
            e->pos++;
        }
        if (e->pos >= e->len) {
            /* EOF without the ':': the explicit-key pair's value never
             * came ("? b<EOF>") — the pending stays armed and the
             * stream-end flush completes it at the deepest open map
             * (the nightly's round 24, crash-bcc0f52c); popping here
             * resolved a pair that has no value and stranded the OUTER
             * pending's null in the wrong map */
            return 0;
        }
        e->pos++; /* past ':' */
        return e_pair_value_done(e, ctx, key_col);
    }

    ev.value.p = e->p + s.start;
    ev.value.len = s.end - s.start;
    ev.borrowed = 1;
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    e->pos = s.end;
    if (e_plain_multiline(e, s, floor_col, &ev, e->depth == 0) != 0) {
        return -1;
    }
    if (e->fold_n == 1 && s.term == YEP_TERM_COMMENT) {
        e_skip_to_eol(e); /* single line ending in a comment */
    }
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* Value after "key:" / "-" / "?": on this line, or on following lines. */
static int e_parse_value(yep_engine* e, yep_ctx ctx, uint32_t floor_col) {
    e_skip_inline_space(e);
    if (!e_at_eol(e)) {
        if (ctx == YEP_CTX_AFTER_COLON && e->p[e->pos] == '-' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "key: - x" */
        }
        if (ctx == YEP_CTX_AFTER_DASH && e->p[e->pos] == ':' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            /* "- :" — a compact pair with an empty key */
            uint32_t col = e_col(e, e->pos);
            int rc = e_open_map(e, col, e->line, col + 1, e->pend_anchor, e->pend_tag,
                                e->pend_anchor_id, 0);
            if (rc != 0) {
                return rc;
            }
            e->pend_anchor.p = NULL;
            e->pend_anchor.len = 0;
            e->pend_anchor_id = 0;
            e->pend_tag.p = NULL;
            e->pend_tag.len = 0;
            yep_event kv;
            e_event_init(&kv, YEP_EV_SCALAR);
            kv.implicit = 1;
            if (emit_now(e, &kv) != 0) {
                return -2;
            }
            e->pos++;
            return e_parse_value(e, YEP_CTX_AFTER_COLON, col);
        }
        int rc = e_node(e, ctx, floor_col);
        if (rc != 0) {
            return rc;
        }
        /* A plain value with continuations legitimately consumed through
         * the line break — pos sits at the next line's START; the main
         * loop must see the indentation, so do not skip it. */
        if (e->pos == e->line_start) {
            return 0;
        }
        e_skip_inline_space(e);
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return 0;
    }

    /* following lines */
    for (;;) {
        if (e->pos >= e->len) {
            goto empty_value;
        }
        /* a raw NEL/LS/PS here is this line's break: consume it exactly
         * as the BLANK branch below consumes an LF-ended empty line —
         * yep_scan_line's line model does not treat a break byte at the
         * loop head as a line end, so without this the props-only line
         * re-entered e_node on the break byte (rows 1-2) and the marker
         * became the scalar's value (row 5's consume-first form) */
        if (yep_scan_mb_break_len(e->p, e->len, e->pos) > 0) {
            e_line_done(e, e->pos);
            continue;
        }
        yep_line_info li;
        if (e->pos == e->line_start) {
            /* seed the memo: the fast arms re-read this line's facts —
             * a direct scan made every following line double-scanned */
            li = e_line_info_here(e);
        } else {
            li = yep_scan_line(e->p, e->len, e->pos);
        }
        if (li.flags & YEP_LF_BLANK) {
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & YEP_LF_COMMENT) {
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END)) {
            /* a document boundary ends the pending node: the props-only
             * document ("---\n!\n---\n…") emits its empty scalar here,
             * never swallowing the next document as its value */
            goto empty_value;
        }
        if (li.indent == floor_col && e->depth > 0 &&
            (ctx == YEP_CTX_AFTER_COLON || ctx == YEP_CTX_VALUE_LINE || ctx == YEP_CTX_AFTER_Q) &&
            (li.first == '&' || li.first == '!')) {
            /* Only PROPS-ONLY lines are new nodes, not values
             * ("key: &x\n!!map\na: b"); "&anchor c: 3" is a keyed pair.
             * UNDER AN EXPLICIT '?' KEY a props-only line is worse: the
             * key would span lines waiting for a ':' that its own
             * props absorb — libyaml rejects ("could not find expected
             * ':' while scanning a simple key"), and absorbing the
             * props DROPPED the key's anchor so the emission wrote an
             * alias to an anchor that no longer existed ("? &a\n&F\n:
             * *a", the nightly's run 38207965687) */
            size_t t = li.offset + li.indent;
            int only_props = 1;
            char prev = ' '; /* '#' is a comment only after a blank —
                              * mid-token it is content ("&a ?#:" is a
                              * keyed pair whose plain carries a '#',
                              * the nightly's run 38204470906) */
            while (t < li.end) {
                char pc = e->p[t];
                if (pc == ' ' || pc == '\t') {
                    prev = pc;
                    t++;
                    continue;
                }
                int pc_comment = (pc == '#' && (prev == ' ' || prev == '\t'));
                /* an opening quote is node CONTENT, not a prop: the walk
                 * must stop there ('only_props' stays 0) — walking on put
                 * the line's ' #' inside the quoted span, read the quoted
                 * key of the nightly's run 39223056891 ("x:\n!t \"a #b\": y")
                 * as a comment and rejected the keyed pair */
                if (pc == '&' || pc == '!' || pc == '<' || pc == ',' || pc == '>' ||
                    (pc != '\n' && pc != '\r' && !pc_comment &&
                     !yep_ct_is((unsigned char)pc, YEP_CT_FLOW_IND) && pc != ':' && pc != ' ' &&
                     pc != '"' && pc != '\'')) {
                    prev = pc;
                    t++;
                    continue;
                }
                only_props = (pc == '\n' || pc == '\r' || pc_comment);
                break;
            }
            if (only_props) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos + li.indent);
            }
        }
        yep_ctx vctx = (ctx == YEP_CTX_AFTER_COLON) ? YEP_CTX_VALUE_LINE : ctx;
        /* At document root there is no parent block: a following line
         * at any indent is the value ("&a\n- x"). Explicit-key content
         * may align with the '?' column. Flow nodes and property runs
         * continue the value even at the parent column ("k:\n!!seq\n[a]"). */
        if (ctx == YEP_CTX_AFTER_Q && li.indent == floor_col && li.first == '-' &&
            li.offset + li.indent + 1 < li.end && e->p[li.offset + li.indent + 1] != ' ' &&
            e->p[li.offset + li.indent + 1] != '\t' && e->p[li.offset + li.indent + 1] != '\n' &&
            e->p[li.offset + li.indent + 1] != '\r') {
            /* "-:" and friends: a dash with a non-space follower is a
             * PLAIN key ("-" => null), never the pending key's content
             * ("?\n-:" lost the null pair, the nightly's run
             * 39045959147). A dash+space seq entry keeps the old
             * content path below (the catch-38154662070 shape). */
            goto empty_value;
        }
        if (ctx == YEP_CTX_AFTER_Q && li.indent == floor_col && li.first != ':' &&
            li.first != '-' && yep_plain_first_ok(li.first)) {
            /* a same-column PLAIN-START pair line is a SIBLING, not
             * the key's content: the buffered "? &a" key would strand
             * inside it and an alias to the anchor emitted a dangling
             * reference ("? &a\nt: &b\n*a"; the nightly's run
             * 38261610465; libyaml: "could not find expected ':'
             * while scanning a simple key"). Dashes keep the old path
             * (a block seq is the key's content — the catch-38154662070
             * pin's shape). Stop: the null value flushes and the main
             * loop owns the pair. */
            if (li.first == '?') {
                /* a '?'-led line at the key's own column is NEVER the
                 * key's content: an indicator ("? k") or a plain-key
                 * pair ("?:") alike, libyaml completes the pending key
                 * (null value) and the main loop re-dispatches the
                 * line. The old continuation swallowed it as a folded
                 * piece and the pending pair lost its null value
                 * ("?\n?:" parsed as one pair, libyaml two: the
                 * nightly's run 39041682907) */
                goto empty_value;
            }
            size_t sc = li.offset + li.indent;
            int sibling = 0;
            while (sc < li.end) {
                if (e->p[sc] == ':' &&
                    (sc + 1 >= (size_t)li.end || e->p[sc + 1] == ' ' || e->p[sc + 1] == '\t' ||
                     e->p[sc + 1] == '\n' || e->p[sc + 1] == '\r')) {
                    sibling = 1;
                    break;
                }
                sc++;
            }
            if (sibling) {
                goto empty_value;
            }
        }
        /* A flow collection at the parent column is normally the pending
         * value ("k:\n[a]"), but a flow followed by a same-line ':' is a
         * KEY of the parent map: the pending value is empty and the main
         * loop must own the line (crash-c0169e98: "a:\n{----}: 1" made
         * the flow a's value and then errored on its key colon). Probe
         * with the same pre-scan the block flow-key branch uses. */
        int flow_value = 0;
        if ((li.first == '[' || li.first == '{') && li.indent == floor_col) {
            flow_value = 1;
            size_t fsave = e->pos;
            uint32_t fline = e->line;
            size_t fls = e->line_start;
            e->pos = li.offset + li.indent;
            if (e_skip_flow(e) == 0) {
                e_skip_inline_space(e);
                if (e_colon_at(e, e->pos) && e->line == fline) {
                    flow_value = 0;
                }
            } else {
                yep_error_clear(&e->err); /* e_node re-raises the real error */
            }
            e->pos = fsave;
            e->line = fline;
            e->line_start = fls;
        }
        if (li.indent > floor_col || e->depth == 0 ||
            (ctx == YEP_CTX_AFTER_Q && li.indent == floor_col && li.first != ':') || flow_value ||
            /* libyaml never validates a block-scalar HEADER's column —
             * only the content must out-indent the parent (e_block_scalar
             * enforces that), so "|" / ">" at the key's own column is the
             * pending value (yeptris-ruby#258: the metanorma collection
             * fixture shape) */
            (vctx == YEP_CTX_VALUE_LINE && (li.first == '|' || li.first == '>') &&
             li.indent == floor_col)) {
            /* an empty-key ':' line cannot be a pending value's
             * content — libyaml errors ("did not find expected key";
             * the nightly's "- &a\n :" byte-instability crasher).
             * The same-line empty-key law lives in the shape arms. */
            if (li.first == ':') {
                size_t at2 = e->pos + li.indent + 1;
                if (at2 >= e->len || e->p[at2] == ' ' || e->p[at2] == '\t' || e->p[at2] == '\n' ||
                    e->p[at2] == '\r') {
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos + li.indent);
                }
            }
            e->pos += li.indent;
            if (++e->rec >= e->max_depth) {
                e->rec--;
                return e_fail(e, YEP_ERR_DEPTH, e->pos);
            }
            {
                int rc = e_node(e, vctx, floor_col);
                e->rec--;
                return rc;
            }
        }
        /* indentless sequence as a mapping value; under a sequence frame
         * a dash at the same column is a SIBLING, not a continuation */
        if (li.indent == floor_col && li.first == '-' &&
            ((ctx == YEP_CTX_AFTER_DASH && e->depth > 0 &&
              e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
              e->frames[e->depth - 1].col == floor_col) ||
             vctx == YEP_CTX_VALUE_LINE) &&
            (li.offset + li.indent + 1 >= e->len || e->p[li.offset + li.indent + 1] == ' ' ||
             e->p[li.offset + li.indent + 1] == '\t' || e->p[li.offset + li.indent + 1] == '\n' ||
             e->p[li.offset + li.indent + 1] == '\r')) {
            e->pos += li.indent;
            return e_node(e, ctx == YEP_CTX_AFTER_DASH ? ctx : vctx, floor_col);
        }
        goto empty_value;
    }

empty_value: {
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    ev.anchor = e->pend_anchor;
    ev.tag = e->pend_tag;
    ev.anchor_id = e->pend_anchor_id;
    e->pend_anchor.p = NULL;
    e->pend_anchor.len = 0;
    e->pend_anchor_id = 0;
    e->pend_tag.p = NULL;
    e->pend_tag.len = 0;
    { /* libyaml: the implicit null sits just past the key's colon
       * ("k:   " -> col 2, not the line end), zero-span */
        size_t at = e->pos;
        if (at > e->len) {
            at = e->len; /* the cursor may stand one past EOF after a
                          * flow close scan (the nightly's run
                          * 38147359781, '[&"[?:\n?(\nu\n?"]:...') —
                          * zero-copy means the mark reads in-bounds */
        }
        while (at > 0 && (e->p[at - 1] == ' ' || e->p[at - 1] == '\t' || e->p[at - 1] == '\n' ||
                          e->p[at - 1] == '\r')) {
            at--; /* the cursor may sit lines past the deferred key */
        }
        if (at == 0 || e->p[at - 1] != ':') {
            at = e->pos; /* not a plain "key:" defer: the cursor stands */
        }
        e_pos_mark(e, at, &ev.line, &ev.col);
        ev.end_line = ev.line;
        ev.end_col = ev.col;
    }
    return emit_now(e, &ev) == 0 ? 0 : -2;
}
}

/* ------------------------------------------------------------ lifecycle */

void yep_engine_set_resolver(yep_engine* e, const yep_resolver* r) {
    if (e != NULL) {
        e->resolver = r != NULL ? r : yep_resolver_core12();
    }
}

void yep_engine_set_compat_grammar(yep_engine* e, int on) {
    if (e != NULL) {
        e->compat_grammar = on != 0;
    }
}

void yep_engine_set_max_depth(yep_engine* e, int depth) {
    if (e != NULL && depth > 0) {
        e->max_depth = depth > YEP_MAX_DEPTH ? YEP_MAX_DEPTH : depth;
    }
}

yep_pool* yep_engine_detach_pool(yep_engine* e) {
    if (e == NULL || e->pool == NULL) {
        return NULL;
    }
    yep_pool* p = e->pool;
    e->pool = yep_pool_create(e->sys, 8192); /* fresh: engine stays usable */
    return p;
}

char* yep_engine_detach_norm_buf(yep_engine* e, size_t* len_out) {
    if (e == NULL) {
        if (len_out != NULL) {
            *len_out = 0;
        }
        return NULL;
    }
    char* b = e->norm_buf;
    if (len_out != NULL) {
        *len_out = e->norm_buf_len;
    }
    e->norm_buf = NULL;
    e->norm_buf_len = 0;
    return b;
}

void yep_engine_prepare(yep_engine* e, const yep_text_stats* st) {
    if (e == NULL || st == NULL) {
        return;
    }
    if (st->amp > 0) {
        yep_nametab_reserve(&e->anchors, (uint32_t)st->amp);
        /* every definition bumps the serial exactly once and '&' marks
         * every definition, so amp is the serial's exact ceiling: the
         * placed flags pre-fit here and the parse never pays the
         * doubling chain (alloc + copy + memset per growth). Best
         * effort — anchor_define's growth stays as the fallback for
         * engines driven without prepare. */
        anchor_placed_fit(e, (uint32_t)st->amp + 1);
    }
}

yep_engine* yep_engine_create(const yep_allocator* sys) {
    if (sys == NULL) {
        return NULL;
    }
    yep_pool* pool = yep_pool_create(sys, 8192);
    if (pool == NULL) {
        return NULL;
    }
    yep_engine* e = yep_alloc(sys, sizeof(yep_engine));
    if (e == NULL) {
        yep_pool_destroy(pool);
        return NULL;
    }
    memset(e, 0, sizeof(*e));
    e->sys = sys;
    e->pool = pool;
    e->max_depth = YEP_MAX_DEPTH;
    if (!yep_nametab_init(&e->anchors, sys)) {
        yep_free(e->sys, e);
        yep_pool_destroy(pool);
        return NULL;
    }
    yep_error_clear(&e->err);
    return e;
}

/* YAML 1.2 line breaks beyond \n/\r: NEL, LS, PS. When present, the
 * input is copied with each normalized to a single '\n' (libyaml reader
 * behavior); inputs without them keep the zero-copy path. The copied
 * length is not strlen()'s: the INPUT may carry NUL bytes of its own,
 * and a strlen() of the copy truncates the run at the first one (the
 * nightly's run 38524928004: "\0\n---\r<U+2028>" whole-parsed as an
 * empty stream while the chunked cut skipped the copy). The context
 * walk behind the copy lives below (issue #638); since #839 the
 * chunked route threads it across --- cuts so every document
 * normalizes to the same bytes the one-buffer walk produces. */

/* The NEL/LS/PS -> '\n' copy (the one-shot path rides this BEFORE the
 * engine so the normalized bytes own the document's `transcoded` slot;
 * the engine's per-feed copy stays for the streaming feeds). Returns
 * NULL when the input has no exotic break. */
/* The two possible LEAD bytes of a Unicode break (NEL = C2 85; LS/PS =
 * E2 80 A8/A9). The whole-document probe rides the SIMD stopset kernel
 * and only candidates get the byte check — the per-byte walk was 11%
 * of every parse (exclusive profile, block-heavy DOM, 2026-09-27).
 * Pinned against yep_stopset_init by Parse.NormBreakLeadsMatchRuntimeBuild. */
const yep_stopset yep_norm_break_leads = {
    .bitmap = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00},
    .groups = 1,
    .lo = {{0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00}},
    .hi = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
            0x02, 0x00}},
};

/* The context walk behind the break normalizer (issue #638): the fold
 * is quote-aware — NEL (C2 85) folds EVERYWHERE (libyaml's READ_LINE
 * folds it inside quoted scalars too), LS/PS (E2 80 A8/A9) fold
 * everywhere EXCEPT inside single/double-quoted regions, where libyaml
 * keeps them raw as content (the JSON suite's y_string_u+2028_line_sep
 * / y_string_u+2029_par_sep demand it). The arms mirror step_scan's
 * context tracking minus the cut/hold machinery — complete input, no
 * truncation points — with two engine-faithful corrections: the value
 * position survives spaces (`k: 'x'` opens the quote; a block plain
 * continuation `a 'b'` does not), and the block-scalar floor is the
 * header LINE's indent (step_scan's walk-back only serves col-0
 * headers). Verified against libyaml by the NormLsPs pins and the
 * fuzz-diff LS/PS comparator. */
static int step_bs_header(const char* p, size_t len, size_t i);
static size_t norm_next_fold_break(const char* p, size_t len, size_t i, size_t* br_len);

typedef struct yep_norm_ctx {
    int quote; /* 0 none, else the opening quote char */
    int in_comment;
    int in_verbatim;
    int in_flow;
    int tok_start;
    int word_led;
    int at_break;
    int bs_open;
    int bs_content;
    uint32_t bs_indent;       /* the header line's indent; (uint32_t)-1 = the
                               * doc-inline root rule (markers only) */
    uint8_t started;          /* at least one byte consumed: the walk's i == 0
                               * clauses are stream-start, not buffer-start —
                               * the chunked route resumes mid-stream at a ---
                               * cut, and the carried context must say so */
    unsigned char prev;       /* the last consumed byte (valid when started):
                               * the '#' and line-start tests peek p[i - 1],
                               * which at a resumed buffer is carried state */
    unsigned char bb;         /* the byte immediately before the last FOLDED
                               * break (0 = not armed): the r133 law — a
                               * tok_start armed by a fold whose before-break
                               * byte was plain content is a continuation
                               * line, and its "!<" must not open a verbatim
                               * tag (cleared wherever an indicator arms the
                               * slot instead) */
    unsigned char plain_open; /* a plain scalar VALUE word is in
                               * progress (set by every non-indicator content
                               * byte, preserved across breaks): the r147
                               * law — the bb gate's single-byte slice misses
                               * the run-tail shapes ("-  \n<NEL>!" arms
                               * bb='\n', in the indicator set), so the open
                               * keys on the value-in-progress fact: a
                               * fold-armed tok_start over an open plain is a
                               * continuation line whose "!<" is content,
                               * never props */
    int phantom_flow;         /* openers armed by the glued ':'-twin
                               * ("a:{b]" — the #836-accepted prev==':'
                               * clause) alone: libyaml keeps those bytes
                               * INTERIOR to the plain (no flow collection
                               * exists), so their depth must never arm the
                               * r155 real-flow clear on ']' / '}' — the
                               * walk's in_flow is monotone and the phantom
                               * opens inflate it, which defeated the
                               * "block context keeps the fact" law (the
                               * r169 nightly: the ']' cleared plain_open
                               * through the phantom and the next fold's
                               * "!<" opened a never-closed verbatim) */
} yep_norm_ctx;

typedef struct yep_norm_tok {
    size_t n;     /* bytes consumed */
    int fold_off; /* -1 verbatim, else the offset inside the token
                   * whose bytes become one '\n' */
    int fold_len;
} yep_norm_tok;

static void norm_break_arms(yep_norm_ctx* c) {
    c->tok_start = 1;
    c->word_led = 0;
    c->at_break = 1;
}

static yep_norm_tok norm_step(yep_norm_ctx* c, const char* p, size_t len, size_t i) {
    yep_norm_tok t = {1, -1, 0};
    unsigned char ch = (unsigned char)p[i];
    int nel = (ch == 0xC2 && i + 1 < len && (unsigned char)p[i + 1] == 0x85);
    int lps = (ch == 0xE2 && i + 2 < len && (unsigned char)p[i + 1] == 0x80 &&
               ((unsigned char)p[i + 2] & 0xFE) == 0xA8);

    if (c->in_comment) {
        /* the folds end a comment like any line break (step_scan's
         * law: the post-break '#' must read as a comment) */
        if (ch == '\n') {
            c->in_comment = 0;
            norm_break_arms(c);
        } else if (ch == '\r' && (i + 1 >= len || p[i + 1] != '\n')) {
            c->in_comment = 0;
            norm_break_arms(c);
        } else if (nel) {
            t.n = 2;
            t.fold_off = 0;
            t.fold_len = 2;
            c->in_comment = 0;
            norm_break_arms(c);
        } else if (lps) {
            t.n = 3;
            t.fold_off = 0;
            t.fold_len = 3;
            c->in_comment = 0;
            norm_break_arms(c);
        }
        return t;
    }
    if (c->quote != 0) {
        /* A col-0 --- / ... marker RESCUES the quote: the parser ends
         * the document at the marker (the post-cut reset says "the
         * tail began at a --- line in a clean state"), so bytes after
         * it are NOT quoted content. Whole-buffer runs must classify
         * them the same way or the routes diverge: round 25 of the
         * nightly (crash-27a77050) fed ">\n: '\n---\na<U+2029>" and
         * the whole route kept the PS raw inside the never-closed
         * quote while the chunked route (fresh probe after the cut)
         * folded it — a plain scalar "a " vs "a". */
        int rescue = (c->at_break && i + 2 < len && (ch == '-' || ch == '.') && p[i + 1] == ch &&
                      p[i + 2] == ch &&
                      (i + 3 >= len || p[i + 3] == ' ' || p[i + 3] == '\t' || p[i + 3] == '\n' ||
                       p[i + 3] == '\r'));
        if (rescue) {
            c->quote = 0;
            /* the marker is an unconditional document boundary (the
             * parser's own law) and the parser's post-cut reset wipes
             * the flow state; the walk must too, or a flow opened in
             * an EARLIER document re-lexes the next document's spaces
             * as token starts ("1,\"\n--- 2[\"\n--- T \"<LS>" --
             * whole kept the LS raw inside the phantom quote while
             * every chunked window folded it; the nightly's round 35,
             * crash-ef534bdf) */
            c->in_flow = 0;
        } else {
            /* NEL folds inside quotes too; LS/PS stay raw content — but
             * every break still arms the line-start state, or a col-0
             * marker after a quoted newline is invisible (at_break was
             * last armed OUTSIDE the quote) */
            if (nel) {
                t.n = 2;
                t.fold_off = 0;
                t.fold_len = 2;
                c->at_break = 1;
                return t;
            }
            if (lps) {
                t.n = 3;
                c->at_break = 1;
                return t;
            }
            if (ch == '\n' || (ch == '\r' && (i + 1 >= len || p[i + 1] != '\n'))) {
                c->at_break = 1;
                return t;
            }
            c->at_break = 0;
            if (c->quote == '\'') {
                if (ch == '\'') {
                    if (i + 1 < len && p[i + 1] == '\'') {
                        t.n = 2; /* '' escaped quote */
                        return t;
                    }
                    c->quote = 0;
                }
                return t;
            }
            if (ch == '\\') {
                /* the escape consumes the next byte — but a multibyte
                 * break under the escape still folds (an escaped break is
                 * a fold continuation, and the blind pass folded it) */
                if (i + 1 >= len) {
                    return t;
                }
                unsigned char nx = (unsigned char)p[i + 1];
                if (nx == 0xC2 && i + 2 < len && (unsigned char)p[i + 2] == 0x85) {
                    t.n = 3;
                    t.fold_off = 1;
                    t.fold_len = 2;
                    return t;
                }
                if (nx == 0xE2 && i + 3 < len && (unsigned char)p[i + 2] == 0x80 &&
                    ((unsigned char)p[i + 3] & 0xFE) == 0xA8) {
                    t.n = 4;
                    t.fold_off = 1;
                    t.fold_len = 3;
                    return t;
                }
                t.n = 2;
                return t;
            }
            if (ch == '"') {
                c->quote = 0;
            }
            return t;
        }
    }
    if (c->in_verbatim) {
        if (ch == '>') {
            c->in_verbatim = 0;
        }
        return t;
    }
    if (ch == '\n' || (ch == '\r' && (i + 1 >= len || p[i + 1] != '\n'))) {
        norm_break_arms(c);
        return t;
    }
    if (nel) {
        t.n = 2;
        t.fold_off = 0;
        t.fold_len = 2;
        c->bb = (i > 0) ? (unsigned char)p[i - 1] : '\n';
        norm_break_arms(c);
        return t;
    }
    if (lps) {
        t.n = 3;
        t.fold_off = 0;
        t.fold_len = 3;
        c->bb = (i > 0) ? (unsigned char)p[i - 1] : '\n';
        norm_break_arms(c);
        return t;
    }
    /* the i == 0 clauses below are STREAM start: `started`/`prev` carry
     * them across the chunked route's per-document resumes (issue #839)
     * — a fresh context at a --- cut would otherwise read the resumed
     * buffer's first byte as a line start the whole walk never saw */
    if (ch == '#' && (!c->started || c->at_break || c->prev == '\n' || c->prev == '\r' ||
                      c->prev == ' ' || c->prev == '\t')) {
        c->in_comment = 1;
        c->at_break = 0;
        return t;
    }
    int line_start = (!c->started || c->at_break || c->prev == '\n' || c->prev == '\r');

    /* block scalars: inside the fold no quote opens and every break
     * keeps its fold; the region ends at a col-0 marker, a %-line
     * after content, or a dedent to the header line's indent. It runs
     * BEFORE the token-space arm: a fold line's leading spaces are its
     * INDENT, and the space arm consumed them one byte at a time (the
     * at_break stays armed) so the indent walk below counted from the
     * FIRST NON-SPACE byte — a one-space-indented content line read
     * ind=0, the dedent rule (ind <= bs_indent) ended the fold
     * mid-content, and the byte lexed as a token: "!<" armed a phantom
     * verbatim whose breaks-ride-raw law left the whole tail
     * unnormalized, the raw NEL run classified as one content line,
     * and the framing walk spawned a phantom empty plain second
     * document libyaml never emits (the r194 nightly,
     * ">\n ! \n !<aaa: bbb\n...\n<NEL><NEL>"). */
    if (line_start && c->in_flow == 0 && c->bs_open) {
        size_t k = i;
        uint32_t ind = 0;
        while (k < len && p[k] == ' ') {
            ind++;
            k++;
        }
        if (k >= len) {
            t.n = len - i;
            c->at_break = 0;
            return t; /* a final line without its break */
        }
        if (p[k] == '\n' || p[k] == '\r' || p[k] == '#') {
            /* the line ends at ANY break spelling: a multibyte break
             * (NEL/LS/PS) FOLDS at the token tail, exactly like the
             * '#' comment arm ends its line (the pre-r194 path: the
             * token-space arm consumed the leading spaces one byte at
             * a time and the line's '#' hit the comment arm, whose
             * nel/lps case folds). Riding the break raw inside the
             * wholesale token leaked it to the engine's verbatim
             * block-scalar reader, and the r19-family indent
             * transform's verify (sub ">\n #ML<NEL>\n") stopped
             * flipping to parity — the r195 nightly ">\n#ML<NEL>". */
            size_t e = k;
            size_t bs = 0;
            size_t br = 0;
            while (e < len) {
                if (p[e] == '\n') {
                    bs = e;
                    br = 1;
                    e++;
                    break;
                }
                if (p[e] == '\r') {
                    bs = e;
                    br = (e + 1 < len && p[e + 1] == '\n') ? 2 : 1;
                    e += br;
                    break;
                }
                size_t mb = yep_scan_mb_break_len(p, len, e);
                if (mb > 0) {
                    bs = e;
                    br = mb;
                    e += mb;
                    break;
                }
                e++;
            }
            if (br == 0) {
                t.n = len - i;
                c->at_break = 0;
                return t; /* the tail rides the line to EOF */
            }
            t.n = e - i;
            if (p[bs] != '\n' && p[bs] != '\r') {
                t.fold_off = (int)(bs - i);
                t.fold_len = (int)br;
            }
            c->at_break = 1;
            return t; /* blank or comment-content line */
        }
        int marker = (ind == 0 && k + 2 < len &&
                      ((p[k] == '-' && p[k + 1] == '-' && p[k + 2] == '-') ||
                       (p[k] == '.' && p[k + 1] == '.' && p[k + 2] == '.')) &&
                      (k + 3 >= len || p[k + 3] == ' ' || p[k + 3] == '\t' || p[k + 3] == '\n' ||
                       p[k + 3] == '\r'));
        if (marker || (p[k] == '%' && ind == 0 && c->bs_content) ||
            (!marker && c->bs_indent != (uint32_t)-1 && ind <= (uint32_t)c->bs_indent)) {
            c->bs_open = 0; /* the fold ends: this line processes normally */
            if (marker) {
                c->in_flow = 0; /* markers are unconditional boundaries */
            }
        } else {
            size_t e = k;
            while (e < len && p[e] != '\n') {
                e++;
            }
            if (p[k] != '#' && p[k] != '\n' && p[k] != '\r') {
                c->bs_content = 1;
            }
            t.n = (e < len) ? (e - i + 1) : (len - i);
            c->at_break = (e < len);
            return t;
        }
    }

    if (ch == ' ' || ch == '\t') {
        /* a value position survives spaces in block context ("k: 'x'"
         * opens; the plain continuation "a 'b'" does not — its word
         * did not begin with an indicator) */
        c->tok_start = (c->in_flow > 0) ? 1 : (c->word_led || c->tok_start);
        return t;
    }

    /* the header: a '>' / '|' at a value position with a valid
     * follower opens the fold (the engine's own floor is the parent's
     * column, which the walk cannot know — the header LINE's indent
     * bounds it from above and the dedent law agrees wherever the
     * parse succeeds) */
    if (line_start && c->in_flow == 0 && !c->bs_open && c->tok_start && (ch == '>' || ch == '|') &&
        (i + 1 >= len || p[i + 1] == ' ' || p[i + 1] == '\t' || p[i + 1] == '\n' ||
         p[i + 1] == '\r' || (p[i + 1] >= '0' && p[i + 1] <= '9') || p[i + 1] == '+' ||
         p[i + 1] == '-')) {
        int hdr = step_bs_header(p, len, i);
        if (hdr > 0) {
            size_t ls = i;
            while (ls > 0 && p[ls - 1] != '\n' && p[ls - 1] != '\r') {
                ls--;
            }
            uint32_t ind = 0;
            size_t k = ls;
            while (k < len && p[k] == ' ') {
                ind++;
                k++;
            }
            c->bs_open = 1;
            c->bs_indent = ind;
            c->bs_content = 0;
            /* the header line ends at ANY fold break: the tail past
             * the indicators (usually the comment) is OUTSIDE the
             * block-content area, so the #638 fold law governs it — a
             * NEL/LS/PS there ends the header line and the next line
             * rides the bs branch from its own indent (the r171
             * nightly: the '\n'-only scan swallowed " #<"+NEL+" t"
             * whole, the NEL rode unnormalized to the engine, whose
             * comment skip then ate the content line — empty where
             * libyaml's IS_BREAKZ comment law reads "t") */
            size_t br_len = 0;
            size_t e = norm_next_fold_break(p, len, i, &br_len);
            if (br_len > 0) {
                t.n = (e + br_len) - i;
                t.fold_off = (int)(e - i); /* the break FOLDS: the copy
                                            * splices the '\n' (a
                                            * plain-consumed multibyte
                                            * break would ride raw) */
                t.fold_len = (int)br_len;
                c->at_break = 1;
            } else {
                t.n = len - i;
                c->at_break = 0;
            }
            return t;
        }
        /* not a header ("| : [" is a mapping key): fall through */
    }
    /* the doc-inline twin ("--- |"): the node hangs off the document,
     * so only a later marker ends its fold (col-0 lines are content) */
    if (!c->bs_open && ch == '-' && i + 2 < len && p[i + 1] == '-' && p[i + 2] == '-' &&
        (i + 3 >= len || p[i + 3] == ' ' || p[i + 3] == '\t' || p[i + 3] == '\n' ||
         p[i + 3] == '\r') &&
        (c->at_break || !c->started)) {
        c->in_flow = 0; /* the marker bounds the document even when the
                         * walk still believes a flow is open (the same
                         * round-35 law as the in-quote rescue) */
        size_t k = i + 3;
        while (k < len && (p[k] == ' ' || p[k] == '\t')) {
            k++;
        }
        if (k < len && (p[k] == '>' || p[k] == '|') &&
            (k + 1 >= len || p[k + 1] == ' ' || p[k + 1] == '\t' || p[k + 1] == '\n' ||
             p[k + 1] == '\r' || (p[k + 1] >= '0' && p[k + 1] <= '9') || p[k + 1] == '+' ||
             p[k + 1] == '-') &&
            step_bs_header(p, len, k) > 0) {
            c->bs_open = 1;
            c->bs_indent = (uint32_t)-1;
            c->bs_content = 0;
            c->plain_open = 0; /* the marker is a document boundary */
            /* the marker line's tail ends at any fold break (the same
             * law as the plain header above -- the comment tail is
             * outside the content area) */
            size_t br_len = 0;
            size_t e = norm_next_fold_break(p, len, i, &br_len);
            if (br_len > 0) {
                t.n = (e + br_len) - i;
                t.fold_off = (int)(e - i); /* the break folds (the header twin) */
                t.fold_len = (int)br_len;
                c->at_break = 1;
            } else {
                t.n = len - i;
                c->at_break = 0;
            }
            return t;
        }
        t.n = 3;           /* the marker rides; the line's tail processes normally */
        c->plain_open = 0; /* a document boundary: the next node's
                            * props position must not inherit the
                            * previous document's open plain */
        return t;
    }

    if (c->tok_start) {
        c->at_break = 0; /* any content byte ends the line-start state */
        if (ch == '!' && i + 1 < len && p[i + 1] == '<') {
            /* the r133 law: a tok_start armed by a FOLDED break whose
             * before-break byte was plain content is a continuation
             * line — its "!<" is plain content, not props; only a
             * new-node position (an indicator like ':' armed the slot)
             * opens a verbatim tag. Unconditioned, the continuation's
             * "!<" opened a phantom verbatim that swallowed the rest
             * of the input: later NELs rode unnormalized and the plain
             * fold emitted raw lead-stripped 0x85 into the value. The
             * r147 law extends the gate to the run-tail shapes the
             * single-byte bb probe misses ("-  \n<NEL>!" arms bb='\n',
             * in the indicator set; "key: <NEL><NEL>!" arms bb=0x85,
             * outside it — both while a plain VALUE is in progress,
             * which plain_open carries): a plain value word open
             * across the fold means the line is a continuation, and
             * libyaml's 1.1 read folds the same NELs to newlines
             * (READ_LINE spells NEL -> LF, the fold law joins). */
            int bb_plain = c->bb != 0 && !strchr("&*!-?:,[]{}#%'\"|> \t\n\r", c->bb);
            if (!bb_plain && !c->plain_open) {
                c->in_verbatim = 1;
                t.n = 2;
                return t;
            }
        }
        c->word_led = (ch == '&' || ch == '*' || ch == '!' || ch == '-' || ch == '?' || ch == ':' ||
                       ch == ',' || ch == '[' || ch == '{' || ch == ']' || ch == '}');
        if (ch == '\'' || ch == '"') {
            c->quote = ch; /* a quote at a value position IS a quoted token */
            c->tok_start = 0;
            return t;
        }
        c->tok_start = 0;
    }
    if ((ch == '[' || ch == '{') && (c->tok_start || c->prev == ':')) {
        /* a flow opener begins a token slot — at a token position, or
         * directly after the value ':' ("k:{a: 1}" opens; the blank
         * twin "k: {" rides tok_start already). Unconditioned, a
         * plain's interior '[' ("ya200%[") opened a PHANTOM flow whose
         * ',' armed the next token slot and a following "!<" opened a
         * phantom verbatim — breaks inside verbatim ride unnormalized,
         * the raw NELs defeated the line-facts kernel, and the plain
         * fold rendered one fold-SPACE per break instead of the n-1
         * newline tally (the r98 nightly — the #836 phantom's flow
         * half; the comma half landed in #836 itself). */
        int phantom = !c->tok_start; /* the glued ':'-twin alone (the
                                      * #836-accepted prev==':' state):
                                      * libyaml keeps the opener interior
                                      * to the plain, so its depth rides
                                      * phantom_flow, never the real count */
        c->tok_start = 1;
        if (phantom) {
            c->phantom_flow++;
            /* the phantom opener is INTERIOR to the open plain VALUE
             * word (libyaml's read keeps the bytes content) — the fact
             * must survive it, or the r147 gate dies two shapes later:
             * the slot the opener armed says "new node" while the plain
             * is still open, and a following "!<" opened a phantom
             * verbatim that swallowed the rest of the input (the r217
             * nightly: "a:{!<x>"+NEL — "k:{" armed by the ':' indicator
             * clears a live plain_open). The r155/r169 law's opener
             * twin: block context keeps the fact. */
        } else {
            c->plain_open = 0; /* the slot was armed by an indicator */
        }
        c->in_flow++;
    } else if (ch == ',') {
        /* flow separators glue the next token — THE COMMA IN REAL FLOW
         * ONLY. The unconditioned arm let a BLOCK plain's interior
         * comma arm tok_start, a following "!<" opened a phantom
         * verbatim tag that swallowed the rest of the input (breaks
         * inside ride unnormalized), and the raw NEL/LS/PS bytes then
         * defeated the line-facts kernel's \n/\r law — the plain
         * fold's blank-line tally lost them (the r96 nightly,
         * yeptris#836, "b,,,!<!<"+NEL^3). ':' stays unconditioned:
         * the BLOCK value position rides it ("k: 'x'" opens the
         * quote, the NormLsPs pins). The r217 law: "in flow" means
         * REAL flow depth (the r155/r169 gate) — the glued ':'-twin's
         * phantom opens inflate in_flow without any flow collection
         * existing in libyaml's read, so their commas are INTERIOR
         * plain content ("o&a-:{*,,,!<!<"+NEL^3: the phantom '{'
         * inflated in_flow, the interior ','s armed a props slot, and
         * the "!<" opened the phantom verbatim again). */
        if (c->in_flow > c->phantom_flow) {
            c->tok_start = 1;
            c->bb = 0; /* the slot was armed by an indicator, not a fold */
            c->plain_open = 0;
        }
    } else if (ch == ':') {
        /* the separator law (#833's own predicate): a ':' arms the
         * next token slot only before a blank/EOL/EOF — "k: 'x'"
         * opens the value quote (the NormLsPs pins) while an
         * interior "x:!<" of a block plain does NOT (the phantom
         * verbatim again, the #836 colon-led twin). The r217 law for
         * the DECLINED ':': a ':' before a non-blank byte is interior
         * to a plain, or the plain's own LEAD ("k:\t:{!<" — the
         * separating ':'+tab left plain_open dead, the value plain's
         * lead ':' declines, and the phantom '{' behind it armed a
         * props slot both of whose gates were dead — the "!<" opened
         * the phantom verbatim and the NEL run rode raw). The decline
         * IS the plain-context fact: arm the value word here. */
        if (i + 1 >= len || p[i + 1] == ' ' || p[i + 1] == '\t' || p[i + 1] == '\n' ||
            p[i + 1] == '\r') {
            c->tok_start = 1;
            c->bb = 0; /* the slot was armed by an indicator, not a fold */
            c->plain_open = 0;
        } else {
            c->plain_open = 1;
        }
    } else if (ch == ']' || ch == '}') {
        c->tok_start = 1;
        c->bb = 0; /* the slot was armed by an indicator, not a fold */
        if (c->in_flow > c->phantom_flow) {
            c->plain_open = 0;
        }
        /* block context keeps the fact: a plain cannot start with
         * ']' / '}' (c-indicator), so reaching this byte with the value
         * word open means it is INTERIOR content (the catch-85 law's
         * indicator twin) — the r155 law. The unconditional clear let
         * the next fold-armed "!<" open a phantom verbatim (the r147
         * gate's blind-spot twin: the ']' byte also blanks bb_plain —
         * it is in the bb indicator set — so both gate inputs said
         * "new node") and the NELs after it rode unnormalized raw into
         * the plain fold ("(999999;!<1]" +NEL+ "!<" +NEL+ "¥" +NEL^2).
         * The clear keys on REAL flow (in_flow above the phantom
         * depth): the glued ':'-twin's phantom opens inflate in_flow
         * without any flow collection existing in libyaml's read, and
         * the inflated count defeated the law on a plain's interior ']'
         * ("a:{b]" +NEL — the r169 nightly's fifth arming shape). */
    }
    /* the r147 law: a non-indicator content byte begins/continues a
     * plain scalar VALUE word — the fact survives breaks (breaks only
     * arm tok_start), so a later fold-armed "!<" reads it (indicator
     * bytes leave the fact to their own arm above) */
    if (!strchr("&*!-?:,[]{}#%'\"|>", ch)) {
        c->plain_open = 1;
    }
    return t;
}

char* yep_engine_normalize_breaks(const yep_allocator* sys, const char* p, size_t len,
                                  size_t* out_len, int* kept_raw);

static char* norm_breaks_run(const yep_allocator* sys, const char* p, size_t len, size_t* out_len,
                             int* kept_raw, yep_norm_ctx* ctx);

char* yep_engine_normalize_breaks(const yep_allocator* sys, const char* p, size_t len,
                                  size_t* out_len, int* kept_raw) {
    return norm_breaks_run(sys, p, len, out_len, kept_raw, NULL);
}

/* The context-threaded core (#839). ctx == NULL is the one-shot form:
 * fresh state, and the !exotic early exit stands (no walk, no state).
 * A non-NULL ctx rides in the step state: the chunked route parses
 * whole documents, so the walk never sees a mid-construct cut — the
 * divergence was the RESUME state. A --- cut lands exactly where the
 * whole walk holds some carried state (an open quote it has not been
 * rescued from, at_break, the raw prev byte); resuming fresh re-lexes
 * the marker line differently ("--- 'tail": whole's rescue leaves
 * tok_start 0 so the apostrophe is content, a fresh context's
 * tok_start 1 opens a quote). Carrying, never resetting: doc N's walk
 * sees the same bytes with the same state whole holds at the cut, so
 * doc N + 1's start state equals whole's there — the induction makes
 * the concatenated documents byte-identical to the one-buffer walk.
 * The probe ALWAYS runs in this form (even without exotic leads): the
 * carried state must advance over every byte, or the next resume
 * starts stale. Both outcomes keep zero-copy semantics: a document
 * whose exotic bytes all ride raw is used as-is, and those raw bytes
 * are exactly what the one-buffer walk's copy carries for them. */
static char* norm_breaks_run(const yep_allocator* sys, const char* p, size_t len, size_t* out_len,
                             int* kept_raw, yep_norm_ctx* ctx) {
    if (kept_raw != NULL) {
        *kept_raw = 0;
    }
    const unsigned char* q = (const unsigned char*)p;
    const yep_text_kernels* k = yep_text_active();
    size_t i = 0;
    int exotic = 0;
    while (i < len) {
        ptrdiff_t hit = k->stopset_find(&yep_norm_break_leads, (const char*)q + i, len - i);
        if (hit < 0) {
            break;
        }
        i += (size_t)hit;
        exotic = 1; /* any NEL/LS/PS lead byte needs the context walk */
        break;
    }
    yep_norm_ctx probe;
    if (ctx != NULL) {
        probe = *ctx;
    } else {
        if (!exotic) {
            return NULL;
        }
        memset(&probe, 0, sizeof(probe));
        probe.tok_start = 1;
    }
    int found = 0;
    for (size_t j = 0; j < len;) {
        yep_norm_tok t = norm_step(&probe, p, len, j);
        probe.started = 1;
        probe.prev = (unsigned char)p[j + t.n - 1];
        if (t.fold_off >= 0) {
            found = 1;
            break;
        }
        j += t.n;
    }
    if (!found) {
        if (ctx != NULL) {
            *ctx = probe; /* the carried state advanced even with no fold */
        } else {
            if (kept_raw != NULL) {
                *kept_raw = 1; /* every exotic byte rides raw */
            }
            return NULL; /* the exotic bytes all ride inside quoted regions:
                          * the zero-copy path keeps them raw (the fix) */
        }
        return NULL;
    }
    char* out = yep_alloc(sys, len + 1);
    if (out == NULL) {
        return NULL;
    }
    yep_norm_ctx cp;
    if (ctx != NULL) {
        cp = *ctx; /* restart from the same carried state: the probe's
                    * partial walk (it broke at the fold) is discarded */
    } else {
        memset(&cp, 0, sizeof(cp));
        cp.tok_start = 1;
    }
    size_t o = 0;
    for (size_t j = 0; j < len;) {
        yep_norm_tok t = norm_step(&cp, p, len, j);
        cp.started = 1;
        cp.prev = (unsigned char)p[j + t.n - 1];
        if (t.fold_off < 0) {
            if (kept_raw != NULL && *kept_raw == 0) {
                for (size_t m = 0; m + 1 < t.n; m++) {
                    if (yep_scan_mb_break_len(p + j + m, t.n - m, 0) > 0) {
                        *kept_raw = 1;
                        break;
                    }
                }
            }
            memcpy(out + o, p + j, t.n);
            o += t.n;
        } else {
            memcpy(out + o, p + j, (size_t)t.fold_off);
            o += (size_t)t.fold_off;
            /* A raw CR copied just before this rewrite must not pair
             * with the replacement '\n' into a phantom CRLF: the byte
             * readers count CR LF as ONE break (SKIP_LINE's pair law)
             * while the input's CR + NEL/LS/PS were TWO — the rewrite
             * silently halved the fold's break tally (";!\r<NEL>\r;\r"
             * read ";!\\n;" where the (n-1) newline law says
             * ";!\\n\\n;", the r188 nightly). Rewrite the WHOLE
             * trailing run of raw CRs to '\n' — one CR is one break,
             * and only the run's last byte could ever touch the
             * replacement, but a shallower rewrite would just move the
             * phantom pair one byte left ("a\r\r<NEL>" -> "a\r\n\n"
             * re-pairs on the first CR; the run makes it "a\n\n\n",
             * four breaks in, four breaks out). LFs never pair with
             * each other, so the run's count survives everywhere the
             * fold law counts breaks. */
            for (size_t t2 = o; t2 > 0 && out[t2 - 1] == '\r'; t2--) {
                out[t2 - 1] = '\n';
            }
            out[o++] = '\n';
            memcpy(out + o, p + j + t.fold_off + t.fold_len,
                   t.n - (size_t)t.fold_off - (size_t)t.fold_len);
            o += t.n - (size_t)t.fold_off - (size_t)t.fold_len;
        }
        j += t.n;
    }
    out[o] = '\0';
    if (out_len != NULL) {
        *out_len = o;
    }
    if (ctx != NULL) {
        *ctx = cp;
    }
    return out;
}

static const char* e_normalize_breaks(yep_engine* e, const char* p, size_t len, size_t* out_len,
                                      yep_norm_ctx* ctx) {
    char* out = norm_breaks_run(e->sys, p, len, out_len, NULL, ctx);
    if (out == NULL) {
        return p;
    }
    e->norm_buf = out;
    e->norm_buf_len = out ? *out_len : 0;
    return out;
}

typedef struct yep_stepstate {
    char* buf;
    size_t len, cap;
    size_t scanned;     /* bytes folded into quote/flow/comment state */
    int in_flow;        /* open [ { depth */
    int quote;          /* 0 none, else the opening quote char */
    int in_verbatim;    /* inside a "!<...>" verbatim tag: cuts would
                         * split the tag across the chunk boundary */
    int in_comment;     /* inside a # comment (until the line break) */
    int saw_directive;  /* a % line awaits its --- document */
    int started;        /* STREAM_START emitted */
    uint32_t line_base; /* whole-stream lines consumed by earlier runs */
    int bs_open;        /* a block scalar's content lines are pending: a
                         * '>' / '|' header was seen and its terminator
                         * has not */
    uint32_t bs_indent; /* the header line's indent: the content floor */
    int tok_start;      /* the next word may begin here: quotes are
                         * tokens only at a word start (the "A!'- %YAML"
                         * plain's apostrophe is content, not a quote) */
    int word_led;       /* the current word began with an indicator */
    int at_break;       /* the previous position was a line break
                         * (multibyte breaks leave a non-break byte as
                         * p[i-1], so the '#' prev-byte test misses them) */
    yep_norm_ctx nctx;  /* the break normalizer's carried context (#839):
                         * each document's walk resumes with exactly the
                         * state the whole-stream walk holds at the ---
                         * cut, so per-document normalization is
                         * byte-identical to the one-buffer walk */
    int gated;          /* the encoding front-end ran: BOM decisions
                         * belong to the stream's first bytes, never to
                         * a mid-stream chunk (a later U+FEFF is
                         * content). While 0 the head may still be a
                         * BOM prefix, and scanned is 0 by the same
                         * law -- every hold returns before step_scan. */
} yep_stepstate;

/* True when buf[0..len) could still be the PREFIX of a BOM (the rest
 * arrives with the next chunk): the sniff needs its full width, so a
 * 1-3-byte head holds the gate instead of reading a mark's bytes as
 * content. A buffer long enough for the sniff to decide never gets
 * here. */
static int step_bom_prefix(const char* buf, size_t len) {
    const unsigned char* p = (const unsigned char*)buf;
    switch (p[0]) {
    case 0xEF:
        return len == 1 || (len == 2 && p[1] == 0xBB);
    case 0xFF:
    case 0xFE:
        return len == 1;
    case 0x00:
        if (len == 1) {
            return 1;
        }
        if (p[1] != 0x00) {
            return 0;
        }
        return len == 2 || p[2] == 0xFE;
    default:
        return 0;
    }
}

static int engine_run_impl(yep_engine* e, const char* buf, size_t len, const yep_sink* sink,
                           int emit_start, int emit_end);

void yep_engine_front_done(yep_engine* e) {
    e->front_done = 1;
}

int yep_engine_run(yep_engine* e, const char* buf, size_t len, const yep_sink* sink) {
    /* the encoding front-end every raw byte-taking entry owns (push,
     * pull, schema, iterparse's first chunk, the libyaml differential):
     * BOM sniff + UTF-16/32 transcode. ANY head BOM is a mark -- a lone
     * one is an empty stream, UTF-8 included (libyaml's reader; the
     * nightly's round 122 parsed a lone EF BB BF as a U+FEFF scalar
     * where libyaml ended the stream). The front-end runs ONCE per
     * engine: parse.c/values.c pre-strip and mark it, and later
     * chunked runs never re-sniff (a mid-stream U+FEFF is content --
     * iterparse's chunk heads would otherwise be eaten). */
    const char* data = buf;
    size_t data_len = len;
    if (!e->front_done) {
        e->front_done = 1;
        yep_encoding enc = YEP_ENC_UNKNOWN;
        size_t bom = yep_bom_sniff((const unsigned char*)buf, len, &enc);
        data = buf + bom;
        data_len = len - bom;
        if (enc == YEP_ENC_UTF16LE || enc == YEP_ENC_UTF16BE || enc == YEP_ENC_UTF32LE ||
            enc == YEP_ENC_UTF32BE) {
            size_t tlen = 0;
            size_t terr = 0;
            int rc = yep_transcode_to_utf8(e->sys, enc, (const unsigned char*)data, data_len,
                                           &e->enc_buf, &tlen, &terr);
            if (rc != 0) {
                yep_error_set(&e->err, YEP_ERR_ENCODING, 1, 1, terr, "ill-formed input encoding");
                return -1;
            }
            size_t pverr = 0;
            if (!yep_printable_validate(e->enc_buf, tlen, &pverr)) {
                yep_error_set(&e->err, YEP_ERR_ENCODING, 1, 1, pverr,
                              "non-printable character at byte %zu", pverr);
                return -1;
            }
            data = (const char*)e->enc_buf;
            data_len = tlen;
        }
    }
    return engine_run_impl(e, data, data_len, sink, 1, 1);
}

void yep_engine_destroy(yep_engine* e) {
    if (e == NULL) {
        return;
    }
    if (e->step != NULL) {
        yep_stepstate* st = e->step;
        yep_free(e->sys, st->buf);
        yep_free(e->sys, st);
        e->step = NULL;
    }
    yep_nametab_free(&e->anchors);
    yep_free(e->sys, e->anchor_placed);
    yep_free(e->sys, e->norm_buf);
    yep_free(e->sys, e->enc_buf);
    yep_pool_destroy(e->pool);
    yep_free(e->sys, e);
}

/* ---- resumable stepping (TODO.impl/07) --------------------------------
 *
 * The pending buffer holds raw bytes; each step parses the complete-
 * document prefix and keeps the tail. A cut is safe only at the start
 * of a column-0 document marker (--- / ...) while no quoted scalar,
 * comment, or flow collection spans it — tracked incrementally as
 * chunks arrive. Anything ambiguous (lone-\r line breaks, an
 * unterminated marker at the buffer edge, quote pairs split by the
 * chunk boundary) merely delays the cut: buffering, never a wrong
 * split. Cuts land only at --- lines (never ...: that would orphan
 * an explicit DOCUMENT_END) and only when no % directive is pending:
 * directives travel with their document. Line numbers stay
 * stream-absolute: each run starts at the base derived from the
 * engine's own line counter, so stepped events are byte-identical to
 * whole-buffer ones. */

/* A quote opens a scalar only where a value can begin; anywhere else
 * it is literal plain text (the apostrophe in "don't"). This is exact
 * on valid YAML: a quote at a value position always opens. */
/* A line-start '|' / '>' is a block-scalar header only when the rest
 * of the line fits the grammar: optional chomping (+/-) and indent
 * (1-9) indicators in either order, then spaces/tabs, then an optional
 * space-led comment, then EOL. "| : [" is a plain mapping key whose
 * value opens a flow — treating it as a header skipped the '[' and let
 * a '---' cut land inside the still-open flow (#645: whole buffered
 * that flow to the depth cap, chunked cut and errored at 2:1).
 * Returns 1 valid, 0 definitely not a header, -1 truncated (the tail
 * may still complete a valid header). */

/* The next FOLD break at or after i under the #638 fold law outside
 * quotes ('\n', lone '\r' / CRLF, NEL, LS/PS): returns its start
 * offset and stores its consumed length, or returns 0 with *br_len 0
 * when the tail holds none. A truncated multibyte lead at the end
 * reads as "none": the one-shot walk runs on validated complete input
 * (a dangling lead never arrives), and the streaming scanner holds the
 * header line for the bytes that may still complete it. */
static size_t norm_next_fold_break(const char* p, size_t len, size_t i, size_t* br_len) {
    size_t e = i;
    *br_len = 0;
    while (e < len) {
        unsigned char ec = (unsigned char)p[e];
        if (ec == '\n') {
            *br_len = 1;
            return e;
        }
        if (ec == '\r') {
            *br_len = (e + 1 < len && p[e + 1] == '\n') ? 2 : 1;
            return e;
        }
        if (ec == 0xC2 && e + 1 < len && (unsigned char)p[e + 1] == 0x85) {
            *br_len = 2;
            return e;
        }
        if (ec == 0xE2 && e + 2 < len && (unsigned char)p[e + 1] == 0x80 &&
            ((unsigned char)p[e + 2] & 0xFE) == 0xA8) {
            *br_len = 3;
            return e;
        }
        e++;
    }
    return 0;
}

static int step_bs_header(const char* p, size_t len, size_t i) {
    size_t k = i + 1;
    int chomp = 0, indent = 0, space = 0;
    while (k < len) {
        char c = p[k];
        if (c == '+' || c == '-') {
            if (chomp || space)
                return 0;
            chomp = 1;
            k++;
            continue;
        }
        if (c >= '1' && c <= '9') {
            if (indent || space)
                return 0;
            indent = 1;
            k++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            space = 1;
            k++;
            continue;
        }
        if (c == '\n' || c == '\r')
            return 1;
        if (c == '#' && space)
            return 1; /* a comment needs separation */
        return 0;
    }
    return -1; /* the chunk ended mid-line */
}

/* Advances quote/flow/comment state over buf[scanned, len) and returns
 * the offset of the LAST safe cut (0 = none). When the final bytes'
 * meaning depends on bytes not yet fed (a quote that may be doubled,
 * a marker missing its terminator), scanned is held back so the next
 * scan re-reads them with context. */
static size_t step_scan(yep_stepstate* st) {
    const char* p = st->buf;
    size_t last_cut = 0;
    size_t hold = st->len;
    size_t i = st->scanned;
    for (; i < st->len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (st->in_comment) {
            /* the engine normalizes NEL/LS/PS to \n before the parse,
             * so they end a comment too -- and they are LINE BREAKS:
             * the line-start state must arm exactly like the main
             * path's break branches, or the post-break '#' reads as
             * content (at_break stays 0), a closer inside it closes
             * the scan's flow while the engine's stays open, and the
             * next --- cut lands inside that flow ("{\n#\u2029#]\n---
             * {..." whole 2 / chunked 3, the nightly's run
             * 39062394941) */
            if (c == '\n') {
                st->in_comment = 0;
                st->tok_start = 1;
                st->word_led = 0;
                st->at_break = 1;
            } else if (c == '\r' && (i + 1 >= st->len || (unsigned char)p[i + 1] != '\n')) {
                /* a lone CR is a line break too (CRLF rides the \n arm
                 * next iteration; the state is identical either way).
                 * Missing this arm kept the scan inside a comment whose
                 * line ended CR, the tab-led `{ {?` flow opener on the
                 * next line went untracked, and the cut landed at the
                 * following --- INSIDE the engine's open flow (the r97
                 * nightly, yeptris#839: whole errored at the flow's ---
                 * while chunked cut and errored at the cut edge) */
                st->in_comment = 0;
                st->tok_start = 1;
                st->word_led = 0;
                st->at_break = 1;
            } else if (c == 0xC2 && i + 1 < st->len && (unsigned char)p[i + 1] == 0x85) {
                i++;
                st->in_comment = 0;
                st->tok_start = 1;
                st->word_led = 0;
                st->at_break = 1;
            } else if (c == 0xE2 && i + 2 < st->len && (unsigned char)p[i + 1] == 0x80 &&
                       ((unsigned char)p[i + 2] & 0xFE) == 0xA8) {
                i += 2;
                st->in_comment = 0;
                st->tok_start = 1;
                st->word_led = 0;
                st->at_break = 1;
            }
            continue;
        }
        if (st->quote != 0) {
            if (st->quote == '\'') {
                if (c == '\'') {
                    if (i + 1 >= st->len) {
                        hold = i; /* '' or close? wait for the next byte */
                        break;
                    }
                    if (p[i + 1] == '\'') {
                        i++; /* escaped quote: still inside */
                        continue;
                    }
                    st->quote = 0;
                }
                continue;
            }
            if (c == '\\') {
                if (i + 1 >= st->len) {
                    hold = i; /* the escaped byte is unknown */
                    break;
                }
                i++;
                continue;
            }
            if (c == '"') {
                st->quote = 0;
            }
            continue;
        }
        if (st->in_verbatim) {
            if (c == '>') {
                st->in_verbatim = 0;
            }
            continue; /* verbatim tag bytes never cut (a truncated tag
                       * at the buffer end holds the tail whole) */
        }
        if (c == '#') {
            char prev = i > 0 ? p[i - 1] : '\n';
            if (prev == '\n' || prev == '\r' || prev == ' ' || prev == '\t' || st->at_break) {
                st->in_comment = 1;
                st->at_break = 0;
                continue;
            }
        }
        if (c == '\n' || (c == '\r' && (i + 1 >= st->len || p[i + 1] != '\n'))) {
            /* a lone \r is a line break too (the engine's break rule);
             * missing it left the line-start state desynced across the
             * cut and whole/chunked marked the flow's END differently
             * (the nightly's run 38549939326) */
            st->tok_start = 1;
            st->word_led = 0;
            st->at_break = 1;
            continue;
        }
        if (c == 0xC2) {
            /* NEL (C2 85): the engine's normalize maps it to \n before
             * the parse -- the scan must break on it too, or a comment
             * after the break goes unseen and the cut lands inside the
             * still-open flow (the nightly's round 120). C2 leads all
             * of U+0080-U+00BF: bounds FIRST (the old condition read
             * p[i+1] past the buffer on a split), hold only a prefix
             * that may still complete. */
            if (i + 1 >= st->len) {
                hold = i;
                break;
            }
            if ((unsigned char)p[i + 1] == 0x85) {
                i++;
                st->tok_start = 1;
                st->word_led = 0;
                st->at_break = 1;
                continue;
            }
        }
        if (c == 0xE2) {
            /* LS/PS (E2 80 A8 / A9): the same normalize law. E2 also
             * leads U+2000-U+2FFF symbols, so hold only a prefix that
             * may still complete into a break across the chunk
             * boundary (byte-by-byte feeding splits it every time). */
            if (i + 1 >= st->len) {
                hold = i;
                break;
            }
            if ((unsigned char)p[i + 1] != 0x80) {
                /* not the LS/PS family: plain content */
            } else if (i + 2 >= st->len) {
                hold = i;
                break;
            } else if (((unsigned char)p[i + 2] & 0xFE) == 0xA8) {
                i += 2;
                st->tok_start = 1;
                st->word_led = 0;
                st->at_break = 1;
                continue;
            }
        }
        if (c == ' ' || c == '\t') {
            /* in a flow a space always ends the token: the next byte
             * may begin the VALUE ("{f 'x'}" — the quote after the
             * plain key is the value's opener, and missing it let the
             * cut land inside the quoted span, splitting the scalar
             * across the chunk boundary: whole saw the far close and
             * ate the '---' marker, chunked errored on the unterminated
             * prefix — the nightly's run 38534965021) */
            st->tok_start = (st->in_flow > 0) ? 1 : st->word_led;
            /* the one-shot walk's law: the first post-break byte
             * clears the line-start fact — spaces included, or an
             * INDENTED "  ---" (a plain/seq position) read as a
             * col-0 marker through the line-start gate and the cut
             * landed inside the line (the r171 gate twin) */
            st->at_break = 0;
            continue;
        }
        if (st->tok_start) {
            st->at_break = 0; /* any content byte ends the line-start
                               * state ("x#": the '#' is content) */
            if (c == '!') {
                if (i + 1 >= st->len) {
                    hold = i; /* a possibly-truncated '!<': the next
                               * byte decides verbatim vs named */
                    break;
                }
                if (p[i + 1] == '<') {
                    st->in_verbatim = 1;
                    i++; /* the '<' rides */
                    continue;
                }
            }
            st->word_led = (c == '&' || c == '*' || c == '!' || c == '-' || c == '?' || c == ':' ||
                            c == ',' || c == '[' || c == '{' || c == ']' || c == '}');
            if (c == '?' && st->in_flow > 0) {
                /* a flow '?' opens the explicit-key MAP: the engine is
                 * inside one more collection than the bracket count
                 * shows, so marker cuts must stay out of it. The map
                 * closes only at a closer (or never, on garbage), so
                 * this over-counts on "[? a : b, c]" — the safe
                 * direction (a missed cut degrades toward whole-parse
                 * identity; the nightly's crash-c2894d7e, run
                 * 39091724384, cut at a --- the engine read as flow
                 * content and the lone '-' at the chunk edge errored
                 * differently than the whole parse). */
                st->in_flow++;
            }
            if (c == '\'' || c == '"') {
                st->quote = c; /* a quote at a word start IS a quoted token */
                st->tok_start = 0;
                continue;
            }
            st->tok_start = 0;
        }
        if (c == ',' || c == ':' || c == '[' || c == '{' || c == ']' || c == '}') {
            st->tok_start = 1; /* flow separators: the next byte may begin a
                                * token with no space ("{\"a\":...") */
        }
        if (c == '[' || c == '{') {
            /* EVERY opener counts -- the scan cannot tell a flow
             * opener from a block plain's content brace: "k: {", "&b["
             * and "[\n---" are real opens, "U{" is not, and no local
             * state separates them (the nightly's rounds 109-112 went
             * through unconditional counting, tok_start, and
             * tok_start+word_led). Over-counting only SUPPRESSES cuts
             * (the marker logic below stays out of flow), and a missed
             * cut degrades toward whole-parse semantics -- the
             * identity invariant's safe direction. The directive law
             * does not depend on this branch: the '%' case is hoisted
             * below. */
            st->in_flow++;
            continue;
        }
        if (c == ']' || c == '}') {
            if (st->in_flow > 0) {
                st->in_flow--;
            }
            continue;
        }
        if (c == '%') {
            /* hoisted out of the block branch: a directive line must
             * pair with its --- even when brace over-counting keeps
             * the scan "in flow" (a content brace never closed) --
             * the marker cut below lives inside the block branch and
             * would otherwise land between the directive and its ---
             * (the nightly's round 109) */
            st->saw_directive = 1; /* must stay with its --- line */
            continue;
        }
        if (st->in_flow == 0) {
            char prev = i > 0 ? p[i - 1] : '\n';
            if (prev != '\n' && prev != '\r' && !st->at_break) {
                /* the one-shot walk's line_start law honors at_break
                 * (-- the mb-break arms arm it); the prev-only check
                 * missed every line that opens after a NEL/LS/PS, so
                 * the ride-along never fired there (the r171 twin) */
                continue;
            }
            /* Block scalars: once a '>' / '|' header is seen, its
             * content is literal and the fold reader's extent depends
             * on bytes this run may not hold (markers, comments and
             * blank lines all ride along to EOF in the lenient
             * grammar) — so consume every line without updating the
             * quote/flow/comment state and WITHOUT cutting: a split
             * inside the fold changes the scalar's value (the
             * nightly's run 38291177166, ">\n---\n" and friends).
             * The fold ends only at the reader's own rule: a dedented
             * line that is not blank, not a comment, not a marker. */
            if (st->bs_open) {
                size_t k = i;
                uint32_t ind = 0;
                while (k < st->len && p[k] == ' ') {
                    ind++;
                    k++;
                }
                if (k >= st->len) {
                    hold = i; /* an incomplete line */
                    break;
                }
                if (p[k] == '\n' || p[k] == '#') {
                    size_t e = k;
                    while (e < st->len && p[e] != '\n') {
                        e++;
                    }
                    if (e >= st->len) {
                        hold = i;
                        break;
                    }
                    i = e; /* blank or comment content: the loop's i++ crosses the break */
                    continue;
                }
                int marker =
                    (p[k] == '-' && k + 2 < st->len && p[k + 1] == '-' && p[k + 2] == '-') ||
                    (p[k] == '.' && k + 2 < st->len && p[k + 1] == '.' && p[k + 2] == '.');
                if ((p[k] == '-' || p[k] == '.') && k + 2 >= st->len) {
                    hold = i; /* a marker possibly truncated by the chunk */
                    break;
                }
                if (!marker && st->bs_indent != (uint32_t)-1 && ind <= st->bs_indent) {
                    st->bs_open = 0; /* the fold ends here: normal processing resumes */
                } else {
                    size_t e = k;
                    while (e < st->len && p[e] != '\n') {
                        e++;
                    }
                    if (e >= st->len) {
                        hold = i;
                        break;
                    }
                    i = e;
                    continue;
                }
            }
            if ((c == '>' || c == '|') &&
                (i + 1 >= st->len || p[i + 1] == ' ' || p[i + 1] == '\t' || p[i + 1] == '\n' ||
                 p[i + 1] == '\r' || (p[i + 1] >= '0' && p[i + 1] <= '9') || p[i + 1] == '+' ||
                 p[i + 1] == '-')) {
                int hdr = step_bs_header(p, st->len, i);
                if (hdr < 0) {
                    hold = i; /* the header line may still complete */
                    break;
                }
                if (hdr > 0) {
                    uint32_t ind = 0;
                    size_t k = i;
                    while (k > 0 && p[k - 1] == ' ') {
                        k--;
                        ind++;
                    }
                    st->bs_open = 1;
                    st->bs_indent = ind;
                    /* the header line ends at ANY fold break (the
                     * one-shot walk's law, norm_next_fold_break): the
                     * tail past the indicators is outside the
                     * block-content area, so the #638 fold law
                     * governs it. The break arms' state rides here
                     * too — at_break opens the content line's
                     * ride-along through the line-start gate (the
                     * r171 nightly: the '\n'-only scan never
                     * completed a NEL-terminated header line and
                     * held the window at the '>' forever) */
                    size_t br_len = 0;
                    size_t e = norm_next_fold_break(p, st->len, i, &br_len);
                    if (br_len == 0) {
                        hold = i; /* an incomplete header line */
                        break;
                    }
                    st->tok_start = 1;
                    st->word_led = 0;
                    st->at_break = 1;
                    i = e + br_len - 1; /* the loop's i++ lands past the break */
                    continue;
                }
                /* Not a header ("| : [" is a mapping key): fall through
                 * so the line's flow/quote bytes are counted. */
            }
            if (c == '-') {
                if (i + 3 >= st->len) {
                    hold = i; /* --- possibly truncated by the chunk */
                    break;
                }
                if (p[i + 1] == '-' && p[i + 2] == '-') {
                    char after = p[i + 3];
                    if (after == '\n' || after == ' ' || after == '\t' || after == '\r') {
                        /* a doc-inline block scalar ("--- |"): the node
                         * hangs off the DOCUMENT, so its fold follows
                         * the root rule — only a later marker ends it
                         * (col-0 lines are content). The line-start
                         * header check below cannot see it (the '|'
                         * rides mid-line after the marker), and
                         * without the state the fold's content
                         * poisons the quote/flow tracking (the
                         * nightly's run 38352718743). */
                        if (!st->bs_open) {
                            size_t k2 = i + 3;
                            while (k2 < st->len && (p[k2] == ' ' || p[k2] == '\t')) {
                                k2++;
                            }
                            if (k2 < st->len && (p[k2] == '>' || p[k2] == '|')) {
                                st->bs_open = 1;
                                st->bs_indent = (uint32_t)-1; /* root rule: markers only */
                            }
                        }
                        /* the line before a marker must not be a
                         * directive-LOOKING line: the scan's quote
                         * state can close a quote where the parser
                         * reads content (or vice versa), so a %-led
                         * line may ride an open quote the scan
                         * believes in -- cutting then strands the
                         * directive from its --- and the window
                         * rejects it (the nightly's round 27,
                         * crash-95bbd95f: "i\n\"\n...\n%TAG ! \"\n--- ").
                         * The check is byte-level, independent of the
                         * quote tracking's verdict. */
                        int prev_pct = 0;
                        if (i > 0) {
                            size_t ls = i - 1; /* the break before the marker */
                            while (ls > 0 && p[ls - 1] != '\n' && p[ls - 1] != '\r') {
                                ls--;
                            }
                            prev_pct = p[ls] == '%';
                        }
                        if (st->saw_directive == 0 && i > 0 && !st->bs_open && !prev_pct) {
                            last_cut = i; /* interior document boundary */
                        }
                        st->saw_directive = 0; /* this --- owns the directives */
                    }
                }
            }
        }
    }
    st->scanned = hold < st->len ? hold : st->len;
    return last_cut;
}

int yep_engine_step(yep_engine* e, const char* chunk, size_t len, int final, const yep_sink* sink) {
    if (e == NULL || (chunk == NULL && len != 0)) {
        return -1;
    }
    if (e->step == NULL) {
        /* the step state rides the ENGINE's allocator (#904 item 3):
         * the raw calloc bypassed e->sys, so the fail-injection harness
         * (test_alloc_fail's counting allocator) could never reach the
         * streaming route's allocations — and the memory law
         * (allocator.h: nothing outside memory/ calls malloc directly)
         * does not stop at the engine's own struct */
        e->step = yep_alloc(e->sys, sizeof(yep_stepstate));
        if (e->step == NULL) {
            return -1;
        }
        memset(e->step, 0, sizeof(yep_stepstate));
    }
    yep_stepstate* st = e->step;
    st->tok_start = 1; /* the buffer starts at a line start */
    if (st->nctx.started == 0) {
        st->nctx.tok_start = 1; /* the stream's first walk: the same
                                 * fresh state the one-shot starts */
    }
    if (len > 0) {
        if (st->len + len > st->cap) {
            size_t cap = st->cap ? st->cap : 4096;
            while (cap < st->len + len) {
                cap *= 2;
            }
            /* the pending buffer grows through e->sys too: the
             * allocator has no realloc, so growth is alloc + copy +
             * free (the arena.c pattern) */
            char* nb = yep_alloc(e->sys, cap);
            if (nb == NULL) {
                return -1;
            }
            if (st->buf != NULL) {
                memcpy(nb, st->buf, st->len);
                yep_free(e->sys, st->buf);
            }
            st->buf = nb;
            st->cap = cap;
        }
        memcpy(st->buf + st->len, chunk, len);
        st->len += len;
    }
    /* the encoding front-end the one-shot owns (yep_engine_run): the
     * stepping route (and the recorder riding it) had NONE, so UTF-16
     * bytes fed through it parsed garbage silently. The gate runs
     * once, on the stream's first bytes: the UTF-8 BOM strips (a lone
     * one is an empty stream, not a U+FEFF scalar -- the one-shot's
     * round-122 law), a UTF-16/32 BOM rejects (chunk-wise transcoding
     * cannot carry state across feeds: a surrogate pair split by the
     * chunk boundary has nowhere to live), and an undecided prefix
     * holds until enough bytes decide it. */
    if (!st->gated && st->len > 0) {
        yep_encoding enc = YEP_ENC_UNKNOWN;
        size_t bom = yep_bom_sniff((const unsigned char*)st->buf, st->len, &enc);
        if (enc == YEP_ENC_UTF8) {
            memmove(st->buf, st->buf + bom, st->len - bom);
            st->len -= bom;
            st->gated = 1;
        } else if (enc == YEP_ENC_UTF16LE || enc == YEP_ENC_UTF16BE || enc == YEP_ENC_UTF32LE ||
                   enc == YEP_ENC_UTF32BE) {
            yep_error_set(&e->err, YEP_ERR_ENCODING, 1, 1, bom,
                          "the chunked route cannot decode UTF-16/32 input");
            return -1;
        } else if (st->len >= 4 || !step_bom_prefix(st->buf, st->len)) {
            st->gated = 1;
        } else if (!final) {
            return 0; /* the head may still complete a BOM: hold */
        }
    }
    if (!final) {
        size_t cut = step_scan(st);
        if (cut == 0 || cut >= st->len) {
            return 0; /* no complete document yet */
        }
        e->line_base = st->line_base;
        int rc = engine_run_impl(e, st->buf, cut, sink, !st->started, 0);
        if (rc != 0) {
            return rc;
        }
        st->started = 1;
        st->line_base = e->line - 1; /* the engine consumed to here */
        memmove(st->buf, st->buf + cut, st->len - cut);
        st->len -= cut;
        st->scanned = 0;
        /* the tail began at a --- line in a clean state */
        st->in_flow = 0;
        st->quote = 0;
        st->in_verbatim = 0;
        st->in_comment = 0;
        st->saw_directive = 0;
        return 0;
    }
    /* final: parse everything that remains and close the stream */
    e->line_base = st->line_base;
    int rc = engine_run_impl(e, st->buf, st->len, sink, !st->started, 1);
    yep_free(e->sys, st->buf);
    yep_free(e->sys, st);
    e->step = NULL;
    e->line_base = 0;
    return rc;
}

static int engine_run_impl(yep_engine* e, const char* buf, size_t len, const yep_sink* sink,
                           int emit_start, int emit_end) {
    if (e == NULL || (buf == NULL && len != 0)) {
        return -1;
    }
    yep_free(e->sys, e->norm_buf);
    e->norm_buf = NULL;
    e->norm_buf_len = 0;
    /* enc_buf is NOT reset here: yep_engine_run's front-end may have
     * transcoded THIS call's input into it microseconds ago -- buf
     * points into it, and freeing it before the break-normalize scan
     * was a use-after-free on every UTF-16/32 input (ASan: the 4-byte
     * artifact "ff fe fe fe", the nightly's combined-build round).
     * It is run-scoped scratch owned by the front-end; destroy frees
     * it, and the next transcode replaces it. */
    e->parsing_final = emit_end;
    size_t norm_len = len;
    yep_norm_ctx* nctx = e->step != NULL ? &((yep_stepstate*)e->step)->nctx : NULL;
    e->p = e_normalize_breaks(e, buf, len, &norm_len, nctx);
    e->len = norm_len;
    e->pos = 0;
    e->line = 1 + e->line_base;
    e->line_base = 0;
    e->line_start = 0;
    e->li_cache_line = 0;
    e->shape_cache_line = 0;
    e->facts_cache_line = 0;
    e->depth = 0;
    yep_nametab_clear(&e->anchors);
    if (e->anchor_placed != NULL) {
        memset(e->anchor_placed, 0, e->anchor_placed_cap);
    }
    e->alias_memo.len = 0;
    e->tagmap_n = 0;
    e->saw_yaml = 0;
    e->doc_begin = (size_t)-1;
    e->doc_content = 0;
    e->q_key_pending = 0;
    e->q_value_pending = 0;
    e->qmd_n = 0;
    e->sink = sink;
    yep_error_clear(&e->err);
    if (e->resolver == NULL) {
        e->resolver = yep_resolver_core12();
    }

    yep_event ev;
    if (emit_start) {
        e_event_init(&ev, YEP_EV_STREAM_START);
        ev.line = 1;
        ev.col = 1;
        ev.end_line = 1;
        ev.end_col = 1;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }

    int doc_open = 0;

    while (e->pos < e->len) {
        /* pos may sit MID-line here (e.g. at a comment left by the
         * "--- # c" inline path, whose e_line_done was a no-op): the
         * memo scans line_start and would rewind — scan from pos, the
         * pre-memo semantics */
        yep_line_info li =
            e->pos == e->line_start ? e_line_info_here(e) : yep_scan_line(e->p, e->len, e->pos);
        if ((li.flags & YEP_LF_TAB) && !(li.flags & YEP_LF_BLANK)) {
            size_t t = li.offset + li.indent;
            while (t < li.end && (e->p[t] == ' ' || e->p[t] == '\t')) {
                t++;
            }
            if (t >= li.end || (e->p[t] != '[' && e->p[t] != '{')) {
                e_fail(e, YEP_ERR_TAB_IN_INDENT, e->pos + li.indent);
                goto fail;
            }
            li.indent = (uint32_t)(t - li.offset);
        }
        if (li.flags & YEP_LF_DIRECTIVE) {
            if (doc_open) {
                e_fail(e, YEP_ERR_BAD_DIRECTIVE, e->pos);
                goto fail;
            }
            if (e->saw_yaml) {
                e_fail(e, YEP_ERR_BAD_DIRECTIVE, e->pos); /* repeated %YAML */
                goto fail;
            }
            {
                /* libyaml: the directive name is [0-9A-Za-z_-]+
                 * followed by a blank or the line end -- an empty or
                 * malformed name is an error ("could not find expected
                 * directive name"). The lenient skip let "%]" ride as a
                 * legal directive line whose DOCUMENT_START inclusion
                 * diverged across the step boundary (the nightly's
                 * round 117) */
                size_t n0 = li.offset + li.indent + 1;
                while (n0 < li.end && (yep_ct_is((unsigned char)e->p[n0], YEP_CT_ALPHA) ||
                                       yep_ct_is((unsigned char)e->p[n0], YEP_CT_DIGIT) ||
                                       e->p[n0] == '_' || e->p[n0] == '-')) {
                    n0++;
                }
                if (n0 == li.offset + li.indent + 1 ||
                    (n0 < li.end && e->p[n0] != ' ' && e->p[n0] != '\t')) {
                    e_fail(e, YEP_ERR_BAD_DIRECTIVE, li.offset + li.indent + 1);
                    goto fail;
                }
            }
            {
                /* %YAML takes exactly one version argument */
                size_t chk = li.offset + li.indent + 1;
                if (li.end - chk >= 4 && memcmp(e->p + chk, "YAML", 4) == 0) {
                    e->saw_yaml = 1;
                    size_t a0 = chk + 4;
                    while (a0 < li.end && e->p[a0] == ' ') {
                        a0++;
                    }
                    size_t a1 = a0;
                    int dots = 0;
                    while (a1 < li.end &&
                           ((e->p[a1] >= '0' && e->p[a1] <= '9') || e->p[a1] == '.')) {
                        dots += (e->p[a1] == '.');
                        a1++;
                    }
                    size_t rest = a1;
                    while (rest < li.end && e->p[rest] == ' ') {
                        rest++;
                    }
                    if (rest < li.end && e->p[rest] == '#' && rest > a1) {
                        rest = li.end; /* trailing comment (blank before #) */
                    }
                    if (a1 == a0 || dots != 1 || rest < li.end || e->p[a0] == '.' ||
                        e->p[a1 - 1] == '.') {
                        e_fail(e, YEP_ERR_BAD_DIRECTIVE, chk);
                        goto fail;
                    }
                }
            }
            /* %TAG <handle> <prefix> — registered for the next document */
            size_t d = li.offset + li.indent + 1;
            while (d < li.end && e->p[d] == ' ') {
                d++;
            }
            if (li.end - d >= 3 && memcmp(e->p + d, "TAG", 3) == 0 && e->tagmap_n < 8) {
                size_t h0 = d + 3;
                while (h0 < li.end && e->p[h0] == ' ') {
                    h0++;
                }
                size_t h1 = h0;
                while (h1 < li.end && e->p[h1] != ' ' && e->p[h1] != '\t') {
                    h1++;
                }
                size_t p0 = h1;
                while (p0 < li.end && (e->p[p0] == ' ' || e->p[p0] == '\t')) {
                    p0++;
                }
                size_t p1 = p0;
                while (p1 < li.end && e->p[p1] != ' ' && e->p[p1] != '\t') {
                    p1++;
                }
                {
                    /* a '#' inside the prefix token is NOT a comment:
                     * libyaml wants whitespace before a comment and
                     * rejects the glued form ("did not find expected
                     * whitespace or line break while scanning a %TAG
                     * directive"). Treating it as a comment registered
                     * a bogus prefix and split the whole/chunked
                     * verdicts (catch 154). */
                    const char* hash = (const char*)memchr(e->p + p0, '#', p1 - p0);
                    if (hash != NULL) {
                        e_fail(e, YEP_ERR_BAD_DIRECTIVE, (size_t)(hash - e->p));
                        return -1;
                    }
                }
                {
                    /* libyaml: only blanks and an optional comment may
                     * follow the prefix ("did not find expected comment
                     * or line break") — trailing content riding the
                     * prefix made parseable tags that never re-parsed
                     * (the nightly's "%TAG !m! !mymy 3 ..." trap) */
                    size_t r = p1;
                    while (r < li.end && (e->p[r] == ' ' || e->p[r] == '\t')) {
                        r++;
                    }
                    if (r < li.end && e->p[r] != '#') {
                        e_fail(e, YEP_ERR_BAD_DIRECTIVE, r);
                        return -1;
                    }
                }
                {
                    /* libyaml validates the handle: "!", "!!", or
                     * "!word!" (word = alnum/-), and a prefix is
                     * required ("did not find expected whitespace
                     * while scanning a %TAG directive"). The lenient
                     * accept registered "!'" and let the whole parse
                     * succeed where the chunked one rejected
                     * (catch 154). */
                    int handle_ok = 0;
                    size_t hl = h1 - h0;
                    if (hl == 1 && e->p[h0] == '!') {
                        handle_ok = 1;
                    } else if (hl == 2 && e->p[h0] == '!' && e->p[h0 + 1] == '!') {
                        handle_ok = 1;
                    } else if (hl >= 3 && e->p[h0] == '!' && e->p[h1 - 1] == '!') {
                        handle_ok = 1;
                        for (size_t k = h0 + 1; k + 1 < h1; k++) {
                            if (!(yep_ct_is((unsigned char)e->p[k], YEP_CT_ALPHA) ||
                                  yep_ct_is((unsigned char)e->p[k], YEP_CT_DIGIT) ||
                                  e->p[k] == '-')) {
                                handle_ok = 0;
                                break;
                            }
                        }
                    }
                    if (!handle_ok || p1 == p0) {
                        e_fail(e, YEP_ERR_BAD_DIRECTIVE, h0);
                        return -1;
                    }
                }
                if (h1 > h0 && p1 > p0 && e->p[h0] == '!') {
                    e->tagmap[e->tagmap_n].handle.p = e->p + h0;
                    e->tagmap[e->tagmap_n].handle.len = (uint32_t)(h1 - h0);
                    e->tagmap[e->tagmap_n].prefix.p = e->p + p0;
                    e->tagmap[e->tagmap_n].prefix.len = (uint32_t)(p1 - p0);
                    e->tagmap_n++;
                }
            }
            if (e->doc_begin == (size_t)-1) {
                e->doc_begin = li.offset; /* libyaml: DOCUMENT_START's
                                           * start_mark includes the
                                           * leading directive lines */
                e->doc_begin_line = e->line;
            }
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & (YEP_LF_BLANK | YEP_LF_COMMENT)) {
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & YEP_LF_DOC_START) {
            int rc = e_close_to(e, 0);
            if (rc != 0) {
                return rc;
            }
            if (doc_open) {
                if (!e->doc_content) {
                    e_event_init(&ev, YEP_EV_SCALAR);
                    ev.implicit = 1;
                    if (emit_now(e, &ev) != 0) {
                        return -2;
                    }
                }
                e->doc_content = 0;
                e_event_init(&ev, YEP_EV_DOCUMENT_END);
                e_pos_mark(e, li.offset + li.indent, &ev.line, &ev.col);
                ev.end_line = ev.line; /* zero-span at the new marker —
                                        * libyaml's implicit close mark */
                ev.end_col = ev.col;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                doc_open = 0;
                e->tagmap_n = 0; /* directives are per-document */
                yep_nametab_clear(&e->anchors);
                if (e->anchor_placed != NULL) {
                    memset(e->anchor_placed, 0, e->anchor_placed_cap);
                }
                /* so are anchors — and the repeat-alias memo: doc 1's
                 * "*ORIGIN" memoized the ordinal and doc 2's identical
                 * name answered from it ACROSS the boundary (the
                 * nightly's run 38266487549) */
                e->alias_memo.len = 0;
            }
            e_event_init(&ev, YEP_EV_DOCUMENT_START);
            ev.style = 1; /* explicit marker */
            if (e->doc_begin != (size_t)-1) {
                e_pos_mark(e, e->doc_begin, &ev.line, &ev.col);
            } else if (e->doc_begin_line != 0) {
                /* the directive line was consumed by an EARLIER step
                 * run: its offset is gone, the absolute line is not */
                ev.line = e->doc_begin_line;
                ev.col = 1;
            } else {
                e_pos_mark(e, li.offset + li.indent, &ev.line, &ev.col);
            }
            e_pos_mark(e, li.offset + li.indent + 3, &ev.end_line, &ev.end_col);
            e->doc_begin = (size_t)-1;
            e->doc_begin_line = 0;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            doc_open = 1;
            e->saw_yaml = 0;
            e->last_root_flow = 0; /* a new document may carry its own directives */
            /* the '?' / ':' pair state is per-document too: doc 4's flow
             * set q_key_pending and the boundary left it set, so doc 5's
             * bare ':' skipped its empty-key event — whole-buffer lost a
             * record the chunked feed (per-run reset) emitted (the
             * nightly's run 38287390219) */
            e->q_key_pending = 0;
            e->q_value_pending = 0;
            e->qmd_n = 0;
            e->pos = li.offset + li.indent + 3;
            e_skip_inline_space(e);
            if (!e_at_eol(e)) {
                e->doc_inline = 1;
                rc = e_node(e, YEP_CTX_FRESH, li.indent);
                e->doc_inline = 0;
                if (rc != 0) {
                    if (rc == -2) {
                        return -2;
                    }
                    goto fail;
                }
                e->doc_content = 1;
            }
            e_line_done(e, e->pos);
            continue;
        }
        if (li.flags & YEP_LF_DOC_END) {
            {
                /* the "..." marker's line ends at a break — a lone '\r'
                 * included: skipping it as whitespace read the NEXT
                 * line's "---" as trailing junk on the marker's own line
                 * ("...\r---\n" errored whole-buffer while the chunked
                 * feed's boundary split the lines correctly — the
                 * nightly's round 105) */
                size_t rest = li.offset + li.indent + 3;
                while (rest < e->len && (e->p[rest] == ' ' || e->p[rest] == '\t')) {
                    rest++;
                }
                if (rest < e->len && e->p[rest] != '\n' && e->p[rest] != '\r' &&
                    e->p[rest] != '#') {
                    e_fail(e, YEP_ERR_UNEXPECTED, rest);
                    goto fail;
                }
            }
            int rc = e_close_to(e, 0);
            if (rc != 0) {
                return rc;
            }
            if (doc_open) {
                if (!e->doc_content) {
                    e_event_init(&ev, YEP_EV_SCALAR);
                    ev.implicit = 1;
                    if (emit_now(e, &ev) != 0) {
                        return -2;
                    }
                }
                e_event_init(&ev, YEP_EV_DOCUMENT_END);
                ev.style = 1; /* explicit "..." marker */
                e_pos_mark(e, li.offset + li.indent, &ev.line, &ev.col);
                e_pos_mark(e, li.offset + li.indent + 3, &ev.end_line, &ev.end_col);
                e->tagmap_n = 0; /* directives are per-document */
                yep_nametab_clear(&e->anchors);
                if (e->anchor_placed != NULL) {
                    memset(e->anchor_placed, 0, e->anchor_placed_cap);
                }
                e->alias_memo.len = 0;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                doc_open = 0;
                e->doc_content = 0;
                e->saw_yaml = 0;
                e->doc_begin = (size_t)-1;
                e->doc_begin_line = 0;
            }
            e_line_done(e, li.end);
            continue;
        }

        if (!doc_open) {
            e_event_init(&ev, YEP_EV_DOCUMENT_START);
            if (e->doc_begin != (size_t)-1) {
                e_pos_mark(e, e->doc_begin, &ev.line, &ev.col);
            } else if (e->doc_begin_line != 0) {
                ev.line = e->doc_begin_line; /* a directive split across runs */
                ev.col = 1;
            } else {
                e_pos_mark(e, li.offset + li.indent, &ev.line, &ev.col);
            }
            ev.end_line = ev.line;
            ev.end_col = ev.col;
            e->doc_begin = (size_t)-1;
            e->doc_begin_line = 0;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            doc_open = 1;
        }

        /* unwind frames that cannot continue at this column. The
         * dash-blank test is computed once per line (TODO.restructure/75
         * — it was spelled out twice below); the top frame loads once
         * per iteration. */
        uint32_t c = li.indent;
        const int dash_blank =
            li.first == '-' &&
            (li.offset + li.indent + 1 >= e->len || e->p[li.offset + li.indent + 1] == ' ' ||
             e->p[li.offset + li.indent + 1] == '\t' || e->p[li.offset + li.indent + 1] == '\n' ||
             e->p[li.offset + li.indent + 1] == '\r');
        int map_closed_on_dash = 0; /* the root map closed because this line is a
                                     * seq entry — libyaml's parser-state name is
                                     * "did not find expected key" */
        while (e->depth > 0) {
            const yep_frame* fr = &e->frames[e->depth - 1];
            if (fr->col > c) {
                int rc = e_close_to(e, e->depth - 1);
                if (rc != 0) {
                    return rc;
                }
                continue;
            }
            if (fr->col == c) {
                int continues;
                if (fr->kind == YEP_FRAME_SEQ) {
                    continues = dash_blank;
                } else {
                    continues = yep_scan_is_key_start(li.first) || li.first == ':' ||
                                li.first == '?' || (li.first == '-' && !dash_blank);
                    if (!continues && dash_blank && e->depth == 1) {
                        map_closed_on_dash = 1;
                    }
                }
                if (!continues) {
                    int rc = e_close_to(e, e->depth - 1);
                    if (rc != 0) {
                        return rc;
                    }
                    continue;
                }
            }
            break;
        }

        if (e->depth == 0 && e->doc_content) {
            /* a completed root node followed by more content: a second
             * root in the same document ("word1  # c\nword2",
             * "---\n[1]\n[2]"). The oracle law is libyaml's parser
             * error — the stream wants a document marker and finds a
             * node ("did not find expected <document start>" at the
             * offending line's first byte; libyaml marks 2:1/3:1). A
             * seq entry where the just-closed root map wanted a key is
             * libyaml's "did not find expected key". Issue #900's
             * inline-map rows never reach this arm: they die at the
             * marker-line gate in e_open_map/e_open_seq. */
            return e_fail_msg(e, YEP_ERR_DOC_BOUNDARY, e->pos + li.indent,
                              map_closed_on_dash ? "did not find expected key"
                                                 : "did not find expected <document start>");
        }
        if (e->depth > 0 && e->frames[e->depth - 1].col < c) {
            /* deeper than any open container at main dispatch: nested
             * values are consumed inside e_parse_value, so this line is
             * an orphan continuation ("key: word1\n# c\n  word2") */
            e_fail(e, YEP_ERR_BAD_INDENT, e->pos + li.indent);
            goto fail;
        }
        e->pos += li.indent;
        unsigned char first = (unsigned char)e->p[e->pos];
        if (first == ':' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            /* explicit-key value line, or a bare ':' pair (empty key) */
            int rc;
            if (e->last_root_flow) {
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                goto fail; /* a root flow node cannot become a key */
            }
            if (e->depth == 0 || e->frames[e->depth - 1].kind != YEP_FRAME_MAP) {
                rc = e_open_map(e, c, e->line, c + 1, e->pend_anchor, e->pend_tag,
                                e->pend_anchor_id, 0);
                if (rc != 0) {
                    if (rc == -2) {
                        return -2;
                    }
                    goto fail;
                }
            }
            /* this ':' line completes the pending explicit pair only at
             * the pending pair's own map depth — deeper inside the key's
             * own content it is a plain bare-colon pair, and consuming
             * the pending there skipped this line's empty-key emit,
             * leaving an orphaned (anchorable) child the DOM could not
             * pair and the emit dropped (the nightly's round 102) */
            int completes_q =
                e->q_key_pending && e->qmd_n > 0 && e->depth == e->qmd_stack[e->qmd_n - 1];
            if (!completes_q) {
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.style = YEP_STYLE_PLAIN;
                kv.implicit = 1;
                kv.line = e->line;
                kv.col = (uint32_t)e_col(e, e->pos) + 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
            } else {
                e->q_key_pending = 0;
                if (e->qmd_n > 0) {
                    e->qmd_n--; /* the ':' line supplied the innermost value */
                }
                /* the flag governs the WHOLE stack: deeper pendings
                 * keep it up so their nulls still flush (the nightly's
                 * round 24, crash-bcc0f52c — the hard clear stranded
                 * the outer pair's null) */
                e->q_value_pending = e->qmd_n > 0;
            }
            e->pos++;
            e->cv_line = e->line; /* the value parses on THIS line's colon */
            rc = e_parse_value(e, YEP_CTX_AFTER_DASH, e->frames[e->depth - 1].col);
            if (rc != 0) {
                if (rc == -2) {
                    return -2;
                }
                goto fail;
            }
            e->doc_content = 1;
            e_line_done(e, e->pos);
            continue;
        }

        /* The cycle fast path (TODO.restructure/79 carve 1): a line
         * that CONTINUES the top frame (a map key at its column, a
         * seq dash) with no pending state is exactly e_classified's
         * contract — call it straight from the line loop and skip
         * e_node's prologue (pend save/clear, e_props, the gate) and
         * the dispatch tail. Non-classified shapes return 0 and take
         * the chain below unchanged. */
        if (e->depth > 0 && e->pend_anchor.p == NULL && e->pend_tag.p == NULL &&
            e->pend_anchor_id == 0 && !e->q_key_pending && !e->q_value_pending &&
            ((e->frames[e->depth - 1].kind == YEP_FRAME_MAP && e->frames[e->depth - 1].col == c) ||
             (e->frames[e->depth - 1].kind == YEP_FRAME_SEQ && e->frames[e->depth - 1].col == c &&
              dash_blank))) {
            int frc = e_classified(e, e->frames[e->depth - 1].col);
            if (frc != 0) {
                if (frc < 0) {
                    if (frc == -2) {
                        return -2;
                    }
                    goto fail;
                }
                e->doc_content = 1;
                e_line_done(e, e->pos);
                continue;
            }
        }

        {
            e->last_root_flow = 0;
            int rc = e_flush_q_value(e); /* a key without its ':' line */
            if (rc != 0) {
                return -2;
            }
            rc = e_node(e, YEP_CTX_FRESH, e->depth > 0 ? e->frames[e->depth - 1].col : 0);
            if (rc != 0) {
                if (rc == -2) {
                    return -2;
                }
                goto fail;
            }
            e->doc_content = 1;
        }
        e_line_done(e, e->pos);
    }

    if ((e->tagmap_n > 0 || e->saw_yaml) && !doc_open) {
        /* a directive was never followed by "---" */
        e_fail(e, YEP_ERR_BAD_DIRECTIVE, e->len);
        goto fail;
    }
    {
        int rc = e_close_to(e, 0);
        if (rc != 0) {
            return rc;
        }
    }
    if (doc_open) {
        if (!e->doc_content) {
            e_event_init(&ev, YEP_EV_SCALAR);
            ev.implicit = 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
        }
        e_event_init(&ev, YEP_EV_DOCUMENT_END);
        e_pos_mark(e, e->len, &ev.line, &ev.col);
        ev.end_line = ev.line;
        ev.end_col = ev.col;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
        e->tagmap_n = 0;
        yep_nametab_clear(&e->anchors);
        if (e->anchor_placed != NULL) {
            memset(e->anchor_placed, 0, e->anchor_placed_cap);
        }
        e->alias_memo.len = 0;
    }
    if (emit_end) {
        e_event_init(&ev, YEP_EV_STREAM_END);
        e_pos_mark(e, e->len, &ev.line, &ev.col);
        ev.end_line = ev.line;
        ev.end_col = ev.col;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }
    return 0;

fail:
    *yep_error_tls() = e->err;
    return -1;
}

size_t yep_engine_pos(const yep_engine* e) {
    return e ? e->pos : 0;
}

const yep_error* yep_engine_error(const yep_engine* e) {
    return e ? &e->err : NULL;
}
