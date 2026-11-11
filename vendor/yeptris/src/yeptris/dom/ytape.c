/* ytape.c — the packed YAML record tape (#378, TODO.max-perf/12).
 *
 * Recorder: a yep_sink over the SEVEN committed-op callbacks plus the
 * general event path. The engine stays in its fused arms (no event
 * construction on classified shapes — the event-recorder prototype's
 * measured floor); the recorder's writes are 8-byte words instead of
 * the eager builders' 48-byte node inits, arena copies, resolver calls
 * and placement.
 *
 * Replay: decodes words into the SAME builder functions
 * (dom_on_block_pair/open/item, dom_on_scalar, dom_on_flow_build,
 * yep_dom_on_event), so typing, anchoring and placement semantics are
 * the eager path's by construction. The differential gate
 * (test/unit/test_ytape.cpp) pins tree identity across the corpora.
 */

#include <stdio.h>
#include <string.h>

#include "dom/dom.h"
#include "memory/allocator.h"
#include "memory/pool.h"
#include "parse/scalars.h"
#include "resolve/resolver.h"
#include "scan/json.h"
#include "scan/scan.h"
#include "tape_in.h"
#include "yeptris/tape.h"
#include "ytape.h"

/* ---- word packing ---- */

#define YTP_KIND_OF(w) ((uint8_t)((w) >> 56))

static int yt_put(yep_ytape* t, uint64_t w);

static uint64_t yt_span_in(uint32_t off, uint32_t len, uint8_t kind) {
    return ((uint64_t)kind << 56) | ((uint64_t)(len & 0xFFFFFFu) << 32) | (uint64_t)off;
}

static uint64_t yt_props(uint8_t kind, uint32_t anchor_id, uint32_t flags) {
    return ((uint64_t)kind << 56) | ((uint64_t)(anchor_id & 0xFFFFFFu) << 32) |
           (uint64_t)(flags & 0xFFFFFFFFu);
}

/* ---- the recorder ---- */

int ytap_init(yep_ytape* t, const char* input, size_t len, int max_depth) {
    memset(t, 0, sizeof(*t));
    t->input = input;
    t->input_len = len;
    t->max_depth = max_depth;
    size_t cap = len / 2 + 64;
    if (cap > 0xFFFFFFFFu) {
        cap = 0xFFFFFFFFu;
    }
    t->w = (uint64_t*)yep_alloc(yep_system_allocator(), cap * sizeof(uint64_t));
    if (t->w == NULL) {
        return -1;
    }
    t->cap = (uint32_t)cap;
    return 0;
}

void ytap_free(yep_ytape* t) {
    if (t->flow_scratch != NULL) {
        yeptris_tape_free((yeptris_json_tape*)t->flow_scratch);
        yep_free(yep_system_allocator(), t->flow_scratch);
        t->flow_scratch = NULL;
    }
    if (t->w != NULL) {
        yep_free(yep_system_allocator(), t->w);
        t->w = NULL;
    }
}

/* Growth bounded by the input (the bounded-parse law): a 4N-word
 * ceiling, never a runaway realloc chain. The allocator has no
 * realloc — one copy per growth, at most a few per document. */
static int yt_put(yep_ytape* t, uint64_t w) {
    if (t->count >= t->cap) {
        if (t->oom) {
            return -1;
        }
        size_t ncap = (size_t)t->cap * 2;
        size_t ceiling = t->input_len * 4 + 256;
        if (ncap > ceiling) {
            ncap = ceiling;
        }
        if (ncap <= (size_t)t->cap) {
            t->oom = 1;
            return -1;
        }
        uint64_t* nw = (uint64_t*)yep_alloc(yep_system_allocator(), ncap * sizeof(uint64_t));
        if (nw == NULL) {
            t->oom = 1;
            return -1;
        }
        memcpy(nw, t->w, (size_t)t->count * sizeof(uint64_t));
        yep_free(yep_system_allocator(), t->w);
        t->w = nw;
        t->cap = (uint32_t)ncap;
    }
    t->w[t->count++] = w;
    return 0;
}

/* The pool-form span: two words, 32-bit length, raw pointer (stable:
 * pool blocks never move until the document frees them). */
static int yt_span_pool(yep_ytape* t, const void* p, size_t len) {
    if (yt_put(t, ((uint64_t)YTP_SPAN_POOL << 56) | (uint64_t)(uint32_t)len) != 0) {
        return -1;
    }
    return yt_put(t, (uint64_t)(uintptr_t)p);
}

/* One span word when the view is input-relative and fits 24 bits; the
 * generic pair otherwise. */
static int yt_span(yep_ytape* t, const yep_view* v) {
    if (v->p >= t->input && v->p <= t->input + t->input_len && v->len < 0x1000000u) {
        return yt_put(t, yt_span_in((uint32_t)(v->p - t->input), (uint32_t)v->len, YTP_SPAN_IN));
    }
    return yt_span_pool(t, v->p, v->len);
}

/* Span-mode bits shared by the props words: 0 absent, 1 input, 2 pool
 * (mode is re-derived by pointer range — the recorder never trusts
 * borrowedness for tag/anchor views). */
#define YTM_ABSENT 0
#define YTM_IN 1
#define YTM_POOL 2

static int yt_mode(const yep_ytape* t, const yep_view* v) {
    if (v->p == NULL || v->len == 0) {
        return YTM_ABSENT;
    }
    if (v->p >= t->input && v->p <= t->input + t->input_len) {
        return YTM_IN;
    }
    return YTM_POOL;
}

static int yt_span_by_mode(yep_ytape* t, const yep_view* v) {
    if (v->p == NULL || v->len == 0) {
        return 0;
    }
    return yt_span(t, v);
}

/* The general event path. STREAM/DOCUMENT frames compact to one word
 * (the DOM reads nothing else off them); SEQ/MAP/SCALAR/ALIAS pack
 * props + value + optional tag/anchor spans. */
static int yt_on_event(void* ctx, const yep_event* ev) {
    yep_ytape* t = (yep_ytape*)ctx;
    switch (ev->type) {
    case YEP_EV_STREAM_START:
    case YEP_EV_STREAM_END:
    case YEP_EV_DOCUMENT_START:
    case YEP_EV_DOCUMENT_END:
        if (ev->type == YEP_EV_DOCUMENT_START) {
            t->docs++;
        }
        return yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | (uint64_t)ev->type);
    case YEP_EV_SEQ_START:
    case YEP_EV_MAP_START:
        if (t->depth >= YEP_DOM_MAX_DEPTH) {
            return -1; /* the DOM answers -1 here; the engine's error
                        * path stays identical */
        }
        t->depth++;
        break;
    case YEP_EV_SEQ_END:
    case YEP_EV_MAP_END:
        t->depth--;
        break;
    default:
        break;
    }
    uint32_t anchor_hi = 0;
    uint32_t aid = ev->anchor_id;
    if (aid > 0xFFFFFFu) {
        anchor_hi = 1;
        aid = 0xFFFFFFu;
    }
    uint32_t tag_mode = (uint32_t)yt_mode(t, &ev->tag);
    uint32_t anchor_mode = (uint32_t)yt_mode(t, &ev->anchor);
    uint32_t flags = (uint32_t)ev->type | ((uint32_t)ev->style << 4) |
                     ((uint32_t)ev->implicit << 7) | ((uint32_t)ev->flow << 8) |
                     ((uint32_t)ev->multiline << 9) | ((uint32_t)(ev->borrowed != 0) << 10) |
                     (tag_mode << 11) | (anchor_mode << 13) | (anchor_hi << 15) |
                     ((uint32_t)ev->tag_id << 16);
    if (yt_put(t, yt_props(YTP_EVENT, aid, flags)) != 0) {
        return -1;
    }
    if (yt_span(t, &ev->value) != 0) {
        return -1;
    }
    if (tag_mode != YTM_ABSENT && yt_span_by_mode(t, &ev->tag) != 0) {
        return -1;
    }
    if (anchor_mode != YTM_ABSENT && yt_span_by_mode(t, &ev->anchor) != 0) {
        return -1;
    }
    if (anchor_hi && yt_put(t, (uint64_t)ev->anchor_id) != 0) {
        return -1;
    }
    return 0;
}

static int yt_on_scalar(void* ctx, const yep_view* value, const yep_view* tag,
                        const yep_view* anchor, uint32_t anchor_id, uint8_t tag_id, uint8_t style,
                        uint8_t implicit, uint8_t flow, int borrowed) {
    yep_ytape* t = (yep_ytape*)ctx;
    uint32_t anchor_hi = 0;
    uint32_t aid = anchor_id;
    if (aid > 0xFFFFFFu) {
        anchor_hi = 1;
        aid = 0xFFFFFFu;
    }
    uint32_t tag_mode = (uint32_t)yt_mode(t, tag);
    uint32_t anchor_mode = (uint32_t)yt_mode(t, anchor);
    uint32_t flags = (uint32_t)tag_id | ((uint32_t)style << 8) | ((uint32_t)implicit << 11) |
                     ((uint32_t)flow << 12) | ((uint32_t)(borrowed != 0) << 13) | (tag_mode << 14) |
                     (anchor_mode << 16) | (anchor_hi << 18);
    if (yt_put(t, yt_props(YTP_SCALAR, aid, flags)) != 0) {
        return -1;
    }
    if (yt_span(t, value) != 0) {
        return -1;
    }
    if (tag_mode != YTM_ABSENT && yt_span_by_mode(t, tag) != 0) {
        return -1;
    }
    if (anchor_mode != YTM_ABSENT && yt_span_by_mode(t, anchor) != 0) {
        return -1;
    }
    if (anchor_hi && yt_put(t, (uint64_t)anchor_id) != 0) {
        return -1;
    }
    return 0;
}

static int yt_on_block_pair(void* ctx, const yep_view* key, const yep_block_value* v, uint32_t line,
                            uint32_t key_col, uint32_t val_col) {
    (void)line;
    (void)key_col;
    (void)val_col; /* yep_dnode carries no positions (64-2c) */
    yep_ytape* t = (yep_ytape*)ctx;
    uint32_t anchor_hi = 0;
    uint32_t aid = v->anchor_id;
    if (aid > 0xFFFFFFu) {
        anchor_hi = 1;
        aid = 0xFFFFFFu;
    }
    uint32_t flags = ((uint32_t)v->cls & 7u) | ((uint32_t)(v->borrowed != 0) << 3) |
                     ((uint32_t)yt_mode(t, &v->anchor) << 4) | (anchor_hi << 6);
    if (yt_put(t, yt_props(YTP_PAIR, aid, flags)) != 0) {
        return -1;
    }
    if (yt_span(t, key) != 0) {
        return -1;
    }
    /* ALIAS names record as spans either way (the eager builder
     * stamps borrowed=1 for them); plain values borrow by the flag. */
    if (v->borrowed || v->cls == YEP_LVAL_ALIAS) {
        if (yt_span(t, &v->value) != 0) {
            return -1;
        }
    } else if (yt_span_pool(t, v->value.p, v->value.len) != 0) {
        return -1;
    }
    if (((flags >> 4) & 3u) != YTM_ABSENT && yt_span_by_mode(t, &v->anchor) != 0) {
        return -1;
    }
    if (anchor_hi && yt_put(t, (uint64_t)v->anchor_id) != 0) {
        return -1;
    }
    return 1; /* handled: the engine skips the two events */
}

static int yt_on_block_open(void* ctx, const yep_view* key, uint32_t line, uint32_t key_col) {
    (void)line;
    (void)key_col;
    yep_ytape* t = (yep_ytape*)ctx;
    if (t->depth >= YEP_DOM_MAX_DEPTH) {
        return 0; /* the engine's event path owns the depth error */
    }
    if (key->len < 0x1000000u && key->p >= t->input && key->p <= t->input + t->input_len) {
        if (yt_put(t, yt_span_in((uint32_t)(key->p - t->input), (uint32_t)key->len, YTP_OPEN)) !=
            0) {
            return -1;
        }
    } else {
        /* a pool or oversized block key: the scalar form's open-key
         * bit carries it (defensive — the engine classifies keys from
         * the line; replay routes back to on_block_open) */
        if (yt_put(t, yt_props(YTP_SCALAR, 0, 1u << 21)) != 0) {
            return -1;
        }
        if (yt_span(t, key) != 0) {
            return -1;
        }
    }
    t->depth++;
    return 1;
}

static int yt_on_block_item(void* ctx, const yep_block_value* v) {
    yep_ytape* t = (yep_ytape*)ctx;
    if (v->borrowed && v->value.len < 0x1000000u && v->value.p >= t->input &&
        v->value.p <= t->input + t->input_len) {
        if (yt_put(t, yt_span_in((uint32_t)(v->value.p - t->input), (uint32_t)v->value.len,
                                 YTP_ITEM)) != 0) {
            return -1;
        }
        return 1;
    }
    /* pool content or oversized: the scalar form with the item bit */
    if (yt_put(t, yt_props(YTP_SCALAR, 0, 1u << 20)) != 0) {
        return -1;
    }
    if (yt_span_pool(t, v->value.p, v->value.len) != 0) {
        return -1;
    }
    return 1;
}

/* the >1024 simple-key law, read off the scratch records: a STR at a
 * map's key slot over the limit answers the general walk's long_key
 * verdict (the fused walk carries no such flag). Members alternate
 * key/value in a map frame; a container in the value slot completes
 * at its CLOSE. len == 0xFFFFFF (a CONT-extended token, >16 MiB) is
 * over the limit by definition. */
static uint32_t yt_flow_scan(const yeptris_json_tape* s, int* long_key) {
    /* the record-walk verdicts the fused walk does not carry: the
     * >1024 simple-key law (a STR at a map's key slot over the limit;
     * the law counts the RAW token — a quoted key's record holds the
     * INNER span, so the two quotes add back on; 0xFFFFFF = a
     * CONT-extended >16 MiB token, over by definition) and the span's
     * maximum nesting (the fused walk's depth cap is its own, not the
     * engine's max_depth — the caller falls back when exceeded) */
    struct {
        uint8_t map;
        uint8_t key_next;
    } st[257];
    int d = 0;
    uint32_t maxd = 0;
    st[0].map = 0;
    st[0].key_next = 0;
    for (uint32_t i = 1; i < s->count; i++) { /* recs[0] is the DOC */
        uint64_t r = s->recs[i];
        uint8_t kind = (uint8_t)(r & 0xFFu);
        if (kind == YEP_T_MAP_OPEN || kind == YEP_T_SEQ_OPEN) {
            if (d >= 256) {
                *long_key = 0;
                return 0xFFFFu; /* deeper than the scratch stack: the
                                 * general kernel's depth path decides */
            }
            /* the container consumes the parent frame's pending slot
             * (key or value); after it closes the OTHER slot is next —
             * a container value left key_next pinned at 1, and every
             * key after a container value skipped the law (the
             * nightly's round-108 shape: {"": [...], "giant": x}) */
            if (d > 0 && st[d].map) {
                st[d].key_next = (uint8_t)!st[d].key_next;
            }
            d++;
            if ((uint32_t)d > maxd) {
                maxd = (uint32_t)d;
            }
            st[d].map = (uint8_t)(kind == YEP_T_MAP_OPEN);
            st[d].key_next = 0;
            continue;
        }
        if (kind == YEP_T_CLOSE) {
            if (d > 0) {
                d--;
            }
            continue;
        }
        if (st[d].map && st[d].key_next == 0) {
            uint32_t mlen = (uint32_t)(r >> 8) & 0xFFFFFFu;
            if (kind == YEP_T_STR && mlen + 2u > 1024u &&
                yep_simple_key_chars((const char*)s->_src + (size_t)((uint32_t)(r >> 32)), mlen) +
                        2 >
                    1024) {
                /* the budget counts characters (the round-108 shrink:
                 * a 2-byte code point at byte delta 1025 is legal) */
                *long_key = 1;
            }
            st[d].key_next = 1;
        } else if (st[d].map) {
            st[d].key_next = 0;
        }
    }
    return maxd;
}

static int yt_flow_classify_record(yep_ytape* t, const char* p, size_t open, size_t len,
                                   const yep_view* anchor, const yep_view* tag, uint32_t anchor_id,
                                   int max_depth, size_t* close);

static int yt_on_flow_build(void* ctx, const char* p, size_t open, size_t len, uint32_t line,
                            size_t line_start, yep_view anchor, yep_view tag, uint32_t anchor_id,
                            int max_depth, size_t* close) {
    (void)line;
    (void)line_start; /* the flow builder reads neither */
    return yt_flow_classify_record((yep_ytape*)ctx, p, open, len, &anchor, &tag, anchor_id,
                                   max_depth, close);
}

/* the shared flow-span classification + ONE-record emission (the
 * sink's entry and the flow-rooted fast path both land here). Returns
 * the sink's verdict codes: 0 = not JSON-class (the general kernel),
 * 1 = recorded (with *close set), 2 = long-key (pass 2 owns the
 * error). */
static int yt_flow_classify_record(yep_ytape* t, const char* p, size_t open, size_t len,
                                   const yep_view* anchor, const yep_view* tag, uint32_t anchor_id,
                                   int max_depth, size_t* close) {
    if (t->depth >= YEP_DOM_MAX_DEPTH) {
        return 0; /* the event path owns the error shape */
    }
    /* classification = the DOM's own walk with the builds stripped:
     * the same reject/long-key verdicts, then ONE record for the
     * whole staged span (replay runs the full build through
     * dom_on_flow_build). The verdict comes from the FUSED lenient
     * walk into a reused scratch tape — 3.5x the naive token loop on
     * the spike corpora (item 86); any refusal keeps the naive loop,
     * whose accept/reject contract is the same by construction. */
    if (t->flow_scratch == NULL) {
        t->flow_scratch = yep_alloc(yep_system_allocator(), sizeof(yeptris_json_tape));
        if (t->flow_scratch != NULL) {
            memset(t->flow_scratch, 0, sizeof(yeptris_json_tape));
        }
    }
    t->max_depth = max_depth; /* replay re-walks at the engine's limit */
    /* the naive token loop with a BYTE BUDGET: small spans (the common
     * per-line flow map) finish it — the loop carries the exact close
     * and long_key verdicts for free; a budget exhaustion hands the
     * span to the fused lenient walk, which amortizes its fixed entry
     * cost on large spans (2.1x the naive loop at 6.6 MB, item 86) */
    {
        yep_json_walk w;
        yep_json_tok tok;
        yep_json_walk_init(&w, p, len, open, max_depth);
        w.strict = 1; /* mirrors the engine pass-1 / dom builder gates:
                       * reject anything non-JSON so the span falls to
                       * the general kernel (whole/chunked agreement) */
        int exhausted = 0;
        for (;;) {
            if (w.i - open > 256) {
                exhausted = 1;
                break;
            }
            yep_jw_status st = yep_json_walk_next(&w, &tok);
            if (st == YEP_JW_REJECT) {
                return 0; /* not JSON-class: the general kernel */
            }
            if (st == YEP_JW_DONE) {
                if (w.long_key) {
                    return 2; /* pass 2 owns the simple-key error */
                }
                *close = w.close; /* the walk's own position: exact */
                goto record;
            }
        }
        if (exhausted) {
            yeptris_json_tape* s = t->flow_scratch;
            if (s == NULL) {
                s = (yeptris_json_tape*)yep_alloc(yep_system_allocator(),
                                                  sizeof(yeptris_json_tape));
                if (s != NULL) {
                    memset(s, 0, sizeof(yeptris_json_tape));
                    t->flow_scratch = s;
                }
            }
            if (s != NULL) {
                /* the carve reuses the block through the _srclen
                 * watermark — no per-span free (flow-json walks 200k
                 * spans per parse) */
                /* strict_nums=1: the general walk validates number runs
                 * even at strict=0 (M5DY's 2001-07-02 dates — an
                 * unvalidated accept here diverges from the engine's
                 * own reject) */
                if (s->_block != NULL && s->_srclen < len + 2) {
                    /* the block cannot hold this span: release it so the
                     * walk's carve sizes a fresh one (carve's own growth
                     * semantics belong to the strict route's live
                     * pointers) */
                    yeptris_tape_free(s);
                    memset(s, 0, sizeof(yeptris_json_tape));
                }
                if (yep_tape_walk_lenient_fused(p, len, open, s, 1, 0, 1) == YEPTRIS_OK) {
                    if (s->_srclen < len + 2) {
                        s->_srclen = len + 2; /* the reuse watermark */
                    }
                    int lk = 0;
                    uint32_t smax = yt_flow_scan(s, &lk);
                    if (lk) {
                        return 2; /* pass 2 owns the simple-key error */
                    }
                    if (smax > (uint32_t)max_depth) {
                        return 0; /* over the engine's depth limit: the
                                   * general walk owns the exact DEPTH
                                   * error shape */
                    }
                    goto record_fused;
                }
                return 0; /* the fused walk refused: the general kernel */
            }
            return 0; /* scratch alloc refused: the general kernel */
        }
    }
    return -1; /* unreachable */
record_fused: {
    size_t c = open;
    int depth = 0;
    for (;;) {
        unsigned char ch = (unsigned char)p[c];
        if (ch == '"') { /* the walk validated the string; skip runs */
            c++;
            while (p[c] != '"') {
                if (p[c] == '\\') {
                    c++;
                }
                c++;
            }
            c++;
            continue;
        }
        if (ch == '{' || ch == '[') {
            depth++;
        } else if (ch == '}' || ch == ']') {
            depth--;
            if (depth == 0) {
                break;
            }
        }
        c++;
    }
    *close = c;
}
record:
    t->flow_mark = t->count;
    uint32_t anchor_hi = 0;
    uint32_t aid = anchor_id;
    if (aid > 0xFFFFFFu) {
        anchor_hi = 1;
        aid = 0xFFFFFFu;
    }
    uint32_t tag_mode = (uint32_t)yt_mode(t, tag);
    uint32_t anchor_mode = (uint32_t)yt_mode(t, anchor);
    uint32_t flags = tag_mode | (anchor_mode << 2) | (anchor_hi << 4);
    if (yt_put(t, yt_props(YTP_FLOW, aid, flags)) != 0) {
        return -1;
    }
    if (yt_put(t, (uint64_t)open | ((uint64_t)len << 32)) != 0) {
        return -1;
    }
    if (tag_mode != YTM_ABSENT && yt_span_by_mode(t, tag) != 0) {
        return -1;
    }
    if (anchor_mode != YTM_ABSENT && yt_span_by_mode(t, anchor) != 0) {
        return -1;
    }
    if (anchor_hi && yt_put(t, (uint64_t)anchor_id) != 0) {
        return -1;
    }
    return 1;
}

static int yt_on_flow_commit(void* ctx) {
    (void)ctx; /* the records are already written */
    return 1;
}

static void yt_on_flow_rollback(void* ctx) {
    yep_ytape* t = (yep_ytape*)ctx;
    t->count = t->flow_mark; /* the DOM's stage reset, in words */
}

void ytap_sink(yep_ytape* t, yep_sink* sink) {
    memset(sink, 0, sizeof(*sink));
    sink->on_event = yt_on_event;
    sink->ctx = t;
    sink->on_flow_build = yt_on_flow_build;
    sink->on_flow_commit = yt_on_flow_commit;
    sink->on_flow_rollback = yt_on_flow_rollback;
    sink->on_block_pair = yt_on_block_pair;
    sink->on_scalar = yt_on_scalar;
    sink->on_block_open = yt_on_block_open;
    sink->on_block_item = yt_on_block_item;
    sink->compose = 1; /* the tape replays into a DOM: the composer's
                        * undefined-alias rejection stays at parse time */
}

/* ---- replay ---- */

static int yt_read_span(const yep_ytape* t, const uint64_t* w, uint32_t* i, uint32_t count,
                        yep_view* v) {
    if (*i >= count) {
        return -1;
    }
    uint64_t word = w[(*i)++];
    uint8_t k = YTP_KIND_OF(word);
    if (k == YTP_SPAN_IN) {
        v->p = t->input + (uint32_t)word;
        v->len = (uint32_t)(word >> 32) & 0xFFFFFFu;
        return 0;
    }
    if (k == YTP_SPAN_POOL) {
        if (*i >= count) {
            return -1;
        }
        v->p = (const char*)(uintptr_t)w[(*i)++];
        v->len = (uint32_t)word;
        return 0;
    }
    return -1; /* a props word in a span slot: corruption */
}

int dom_from_ytape(yep_dom* d, const yep_ytape* t) {
    if (d == NULL || t == NULL || t->w == NULL) {
        return -1;
    }
    d->input_base = t->input;
    d->input_len = t->input_len;
    yep_dom_prepare_len(d, t->input_len);
    const uint64_t* w = t->w;
    uint32_t count = t->count;
    uint32_t i = 0;
    while (i < count) {
        uint8_t kind = YTP_KIND_OF(w[i]);
        switch (kind) {
        case YTP_EV_COMPACT: {
            yep_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = (yep_event_type)(uint8_t)w[i];
            i++;
            if (yep_dom_on_event(d, &ev) != 0) {
                return -1;
            }
            break;
        }
        case YTP_EVENT: {
            uint32_t flags = (uint32_t)(w[i] & 0xFFFFFFFFu);
            uint32_t aid = (uint32_t)(w[i] >> 32) & 0xFFFFFFu;
            i++;
            yep_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = (yep_event_type)(flags & 0xFu);
            ev.style = (uint8_t)((flags >> 4) & 7u);
            ev.implicit = (uint8_t)((flags >> 7) & 1u);
            ev.flow = (uint8_t)((flags >> 8) & 1u);
            ev.multiline = (uint8_t)((flags >> 9) & 1u);
            ev.borrowed = (int)((flags >> 10) & 1u);
            uint32_t tag_mode = (flags >> 11) & 3u;
            uint32_t anchor_mode = (flags >> 13) & 3u;
            uint32_t anchor_hi = (flags >> 15) & 1u;
            ev.tag_id = (uint8_t)((flags >> 16) & 0xFFu);
            if (yt_read_span(t, w, &i, count, &ev.value) != 0) {
                return -1;
            }
            if (tag_mode != YTM_ABSENT) {
                if (yt_read_span(t, w, &i, count, &ev.tag) != 0) {
                    return -1;
                }
            }
            if (anchor_mode != YTM_ABSENT) {
                if (yt_read_span(t, w, &i, count, &ev.anchor) != 0) {
                    return -1;
                }
            }
            if (anchor_hi) {
                if (i >= count) {
                    return -1;
                }
                aid = (uint32_t)w[i++];
            }
            ev.anchor_id = aid;
            if (yep_dom_on_event(d, &ev) != 0) {
                return -1;
            }
            break;
        }
        case YTP_SCALAR: {
            uint32_t flags = (uint32_t)(w[i] & 0xFFFFFFFFu);
            uint32_t aid = (uint32_t)(w[i] >> 32) & 0xFFFFFFu;
            i++;
            int is_item = (int)((flags >> 20) & 1u);
            int is_open_key = (int)((flags >> 21) & 1u);
            if (is_item || is_open_key) {
                yep_view v;
                if (yt_read_span(t, w, &i, count, &v) != 0) {
                    return -1;
                }
                yep_block_value bv;
                memset(&bv, 0, sizeof(bv));
                bv.cls = YEP_LVAL_PLAIN;
                bv.borrowed = (v.p >= t->input && v.p <= t->input + t->input_len) ? 1 : 0;
                bv.value = v;
                int rc = is_item ? dom_on_block_item(d, &bv) : dom_on_block_open(d, &v, 0, 0);
                if (rc < 0) {
                    return -1;
                }
                break;
            }
            uint8_t tag_id = (uint8_t)(flags & 0xFFu);
            uint8_t style = (uint8_t)((flags >> 8) & 7u);
            uint8_t implicit = (uint8_t)((flags >> 11) & 1u);
            uint8_t flow = (uint8_t)((flags >> 12) & 1u);
            int borrowed = (int)((flags >> 13) & 1u);
            uint32_t tag_mode = (flags >> 14) & 3u;
            uint32_t anchor_mode = (flags >> 16) & 3u;
            uint32_t anchor_hi = (flags >> 18) & 1u;
            yep_view value, tag = {0}, anchor = {0};
            if (yt_read_span(t, w, &i, count, &value) != 0) {
                return -1;
            }
            if (tag_mode != YTM_ABSENT && yt_read_span(t, w, &i, count, &tag) != 0) {
                return -1;
            }
            if (anchor_mode != YTM_ABSENT && yt_read_span(t, w, &i, count, &anchor) != 0) {
                return -1;
            }
            if (anchor_hi) {
                if (i >= count) {
                    return -1;
                }
                aid = (uint32_t)w[i++];
            }
            if (dom_on_scalar(d, &value, tag_mode ? &tag : NULL, anchor_mode ? &anchor : NULL, aid,
                              tag_id, style, implicit, flow, borrowed) < 0) {
                return -1;
            }
            break;
        }
        case YTP_ITEM: {
            yep_view v;
            v.p = t->input + (uint32_t)w[i];
            v.len = (uint32_t)(w[i] >> 32) & 0xFFFFFFu;
            i++;
            yep_block_value bv;
            memset(&bv, 0, sizeof(bv));
            bv.cls = YEP_LVAL_PLAIN;
            bv.borrowed = 1;
            bv.value = v;
            if (dom_on_block_item(d, &bv) < 0) {
                return -1;
            }
            break;
        }
        case YTP_PAIR: {
            uint32_t flags = (uint32_t)(w[i] & 0xFFFFFFFFu);
            uint32_t aid = (uint32_t)(w[i] >> 32) & 0xFFFFFFu;
            i++;
            yep_block_value bv;
            memset(&bv, 0, sizeof(bv));
            bv.cls = (uint8_t)(flags & 7u);
            bv.borrowed = (int)((flags >> 3) & 1u);
            uint32_t anchor_mode = (flags >> 4) & 3u;
            uint32_t anchor_hi = (flags >> 6) & 1u;
            yep_view key;
            if (yt_read_span(t, w, &i, count, &key) != 0) {
                return -1;
            }
            if (yt_read_span(t, w, &i, count, &bv.value) != 0) {
                return -1;
            }
            if (bv.cls == YEP_LVAL_ALIAS) {
                bv.borrowed = 1; /* the eager builder stamps alias names */
            }
            if (anchor_mode != YTM_ABSENT && yt_read_span(t, w, &i, count, &bv.anchor) != 0) {
                return -1;
            }
            if (anchor_hi) {
                if (i >= count) {
                    return -1;
                }
                aid = (uint32_t)w[i++];
            }
            bv.anchor_id = aid;
            if (dom_on_block_pair(d, &key, &bv, 0, 0, 0) < 0) {
                return -1;
            }
            break;
        }
        case YTP_OPEN: {
            yep_view key;
            key.p = t->input + (uint32_t)w[i];
            key.len = (uint32_t)(w[i] >> 32) & 0xFFFFFFu;
            i++;
            if (dom_on_block_open(d, &key, 0, 0) < 0) {
                return -1;
            }
            break;
        }
        case YTP_FLOW: {
            uint32_t flags = (uint32_t)(w[i] & 0xFFFFFFFFu);
            uint32_t aid = (uint32_t)(w[i] >> 32) & 0xFFFFFFu;
            i++;
            uint32_t tag_mode = flags & 3u;
            uint32_t anchor_mode = (flags >> 2) & 3u;
            uint32_t anchor_hi = (flags >> 4) & 1u;
            if (i >= count) {
                return -1;
            }
            size_t open = (size_t)(uint32_t)w[i];
            size_t len = (size_t)(uint32_t)(w[i] >> 32);
            i++;
            yep_view tag = {0}, anchor = {0};
            if (tag_mode != YTM_ABSENT && yt_read_span(t, w, &i, count, &tag) != 0) {
                return -1;
            }
            if (anchor_mode != YTM_ABSENT && yt_read_span(t, w, &i, count, &anchor) != 0) {
                return -1;
            }
            if (anchor_hi) {
                if (i >= count) {
                    return -1;
                }
                aid = (uint32_t)w[i++];
            }
            size_t close = 0;
            if (dom_on_flow_build(d, t->input, open, len, 0, 0, anchor, tag, aid, t->max_depth,
                                  &close) != 1) {
                return -1;
            }
            if (dom_on_flow_commit(d) < 0) {
                return -1;
            }
            break;
        }
        default:
            return -1; /* a span word where an op belongs: corruption */
        }
    }
    return 0;
}

/* ---- the fused block runner (TODO.restructure/79, session 3) -------
 *
 * ONE SWAR sweep per line + inline classification + an indent stack,
 * emitting the same record stream the engine's sink would — without
 * the engine. Byte-exact against the engine over the block-family
 * corpora (wide 600k / block 1.0M / deep 17k words, differential
 * pinned by the FusedTape test). Any line the loop does not own bails
 * to 1 and the caller re-runs the engine route (restart fallback).
 *
 * Bails (the engine route owns them): quoted/tagged/flow/folded/block
 * scalar values, compact map items, anchored CONTAINER keys (the
 * first pair inside one rides a two-SCALAR form with content-typed
 * tag ids — capture before owning), doc markers/directives, empty
 * "key:" at EOF or mid-doc, undefined aliases, root sequences. */

#define YT_FUSED_MAX_FRAMES 256
#define YT_FUSED_ANCHORS 65536

typedef struct {
    uint32_t indent; /* the frame's key column */
    uint8_t kind;    /* 0 map, 1 seq */
    uint8_t pending; /* a "key:" awaiting its child */
    uint8_t started; /* the content container's start event went out */
    uint8_t
        own_map; /* the frame carries its own map (OPEN/root/seq); the key-SCALAR form does not */
    uint8_t phantom;      /* a content map opened by this frame's pairs is still live */
    uint32_t content_col; /* the column this frame's pairs live at, +1 (0 = none) */
    uint32_t key_off, key_len;
    uint32_t aid;           /* anchor ordinal on the pending container */
    uint32_t a_off, a_len;  /* the pending anchor's name span */
    uint8_t anchored_first; /* the first pair/item rides the two-SCALAR form */
} yt_frame;

typedef struct {
    yep_ytape* t;
    const char* p;
    size_t len;
    uint32_t anchor_seq;
    const yep_resolver* resolver; /* the first pair inside an anchored container
                                   * bakes the engine's tag classification */
    yep_pool* pool;               /* block-scalar content (transferred to the doc) */
    /* the repeat-alias memo (engine anchor_id_of): the last alias
     * resolution answers by name bytes before any probe, and anchor
     * definitions never invalidate it — re-aliasing after a
     * redefinition still reports the memoized ordinal */
    uint32_t memo_off, memo_len, memo_ord;
    /* anchor name -> ordinal (the alias records carry the target's
     * id; a miss bails: the engine owns undefined aliases). The table
     * is heap-allocated on the first definition — small parses never
     * pay for the 64k slots anchor-heavy documents need. */
    struct {
        uint32_t off, len, aid, tag;
    }* anchors;
    uint8_t* anchors_opened; /* per-aid: the anchored container's start event went out */
} yt_fused;

/* Frame model (engine-verified): st[] frames are the engine's *silent*
 * containers — one OPEN record per frame (or one key SCALAR for the
 * sibling form, which carries no map of its own). Pairs inside a frame
 * open a *phantom* content map at the pair column (one start event per
 * column); a later pair at the frame's own indent closes the phantom
 * and rides the frame silently (the engine's sibling-continue). Maps
 * pop strictly shallower; seqs also leave at their column when a
 * non-dash arrives. The root closes only when its start event fired. */

/* chunked name hash: 8 bytes per round, one mix per round (the
 * byte-wise FNV chain cost ~6 cycles/byte on this corpus's 80k
 * lookups+puts per parse). The tail masks to the real bytes so only
 * equal names hash equal. */
static uint32_t yt_anchor_hash(const char* p, uint32_t off, uint32_t len) {
    uint64_t h = 2166136261u;
    uint32_t i = 0;
    for (; i < len;) {
        uint32_t room = len - i;
        uint64_t w;
        if (room >= 8) {
            memcpy(&w, p + off + i, 8);
            i += 8;
        } else {
            uint64_t tail = 0;
            memcpy(&tail, p + off + i, room);
            w = tail;
            i = len;
        }
        h = (h ^ w) * 11400714819323198485ull;
        h ^= h >> 29;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static int yt_anchor_table(yt_fused* F) {
    if (F->anchors == NULL) {
        F->anchors =
            (void*)yep_alloc(yep_system_allocator(), YT_FUSED_ANCHORS * sizeof(*F->anchors));
        if (F->anchors == NULL) {
            return 0;
        }
        memset(F->anchors, 0, YT_FUSED_ANCHORS * sizeof(*F->anchors));
        F->anchors_opened =
            (void*)yep_alloc(yep_system_allocator(), YT_FUSED_ANCHORS * sizeof(uint8_t));
        if (F->anchors_opened == NULL) {
            return 0;
        }
        memset(F->anchors_opened, 0, YT_FUSED_ANCHORS * sizeof(uint8_t));
    }
    return 1;
}

static void yt_anchor_open(yt_fused* F, uint32_t aid) {
    if (F->anchors_opened != NULL && aid != 0 && aid <= YT_FUSED_ANCHORS) {
        F->anchors_opened[aid - 1] = 1;
    }
}

static void yt_anchor_put(yt_fused* F, uint32_t off, uint32_t len, uint32_t aid) {
    uint32_t m = YT_FUSED_ANCHORS - 1;
    uint32_t tag = yt_anchor_hash(F->p, off, len);
    uint32_t i = tag & m;
    for (uint32_t n = 0; n < 64; n++) {
        if (F->anchors[i].len == 0) {
            F->anchors[i].off = off;
            F->anchors[i].len = len;
            F->anchors[i].aid = aid;
            F->anchors[i].tag = tag;
            return;
        }
        if (F->anchors[i].tag == tag && F->anchors[i].len == len &&
            memcmp(F->p + F->anchors[i].off, F->p + off, len) == 0) {
            F->anchors[i].aid = aid; /* redefinition: latest wins */
            return;
        }
        i = (i + 1) & m;
    }
    /* chain too long: leave unfindable — aliases to it will bail */
}

static int yt_anchor_get(yt_fused* F, uint32_t off, uint32_t len, uint32_t* aid) {
    if (F->anchors == NULL) {
        return 0; /* no definitions yet: the engine owns the error */
    }
    if (F->memo_len != 0 && F->memo_len == len &&
        memcmp(F->p + F->memo_off, F->p + off, len) == 0) {
        *aid = F->memo_ord;
        return 1;
    }
    uint32_t m = YT_FUSED_ANCHORS - 1;
    uint32_t tag = yt_anchor_hash(F->p, off, len);
    uint32_t i = tag & m;
    for (uint32_t n = 0; n < 64; n++) {
        if (F->anchors[i].len == 0) {
            return 0;
        }
        if (F->anchors[i].tag == tag && F->anchors[i].len == len &&
            memcmp(F->p + F->anchors[i].off, F->p + off, len) == 0) {
            *aid = F->anchors[i].aid;
            F->memo_off = off;
            F->memo_len = len;
            F->memo_ord = *aid;
            return 1;
        }
        i = (i + 1) & m;
    }
    return 0;
}

static void yt_f_empty_span(yt_fused* F) {
    /* the dead value-span pair the container events always carry */
    (void)yt_put(F->t, ((uint64_t)YTP_SPAN_POOL << 56) | 0u);
    (void)yt_put(F->t, 0);
}

static void yt_f_start(yt_fused* F, yt_frame* fr) {
    if (fr->started) {
        return;
    }
    fr->started = 1;
    (void)yt_put(F->t, yt_props(YTP_EVENT, 0, fr->kind == 1 ? 5u : 7u));
    yt_f_empty_span(F);
    F->t->depth++;
    if (fr->kind == 0) {
        fr->phantom = 1;
    }
}

static void yt_f_close(yt_fused* F, uint8_t kind) {
    (void)yt_put(F->t, yt_props(YTP_EVENT, 0, kind == 1 ? 6u : 8u));
    yt_f_empty_span(F);
    F->t->depth--;
}

/* the OPEN-vs-SCALAR key form: the engine opens via on_block_open
 * unless its top frame is already a map continuing at this key's
 * column (a sibling key) — the contentless root always opens */
static uint8_t yt_f_open_form(const yt_frame* parent, int parent_is_root, uint32_t key_col) {
    if (parent_is_root && parent->content_col == 0) {
        return 1;
    }
    if (parent->content_col != 0) {
        return (uint8_t)(key_col + 1 != parent->content_col && key_col != parent->indent);
    }
    return (uint8_t)(key_col != parent->indent);
}

/* resolve a pending "key:": the OPEN record materializes the key's
 * map (it closes later); the SCALAR form is the bare key — the value
 * container that follows IS the pair's, no map of its own */
static void yt_f_open(yt_fused* F, yt_frame* fr, uint8_t open_form) {
    if (open_form && fr->aid == 0 && fr->key_len < 0x1000000u && fr->key_off <= F->t->input_len) {
        (void)yt_put(F->t, yt_span_in(fr->key_off, fr->key_len, YTP_OPEN));
        fr->own_map = 1;
    } else {
        (void)yt_put(F->t, yt_props(YTP_SCALAR, 0, 0x2900u));
        (void)yt_put(F->t, yt_span_in(fr->key_off, fr->key_len, YTP_SPAN_IN));
        fr->own_map = 0;
    }
    fr->started = 0;
    fr->phantom = 0;
    F->t->depth++;
}

/* The flat-map specialist (item 86): plain `key: value` lines at one
 * fixed content column, one map frame, nothing else. A separate FUNCTION
 * so its code stays inside the I-cache — the equivalent inline loop
 * measured 26% SLOWER by bloating yt_f_run past the cache edge. Returns
 * the position it stopped at; the caller reprocesses that line through
 * the full arms, so every bail here is a strict subset of what yt_f_run
 * would decide — parity holds by construction. */
static size_t yt_flat_run(yep_ytape* t, const char* p, size_t pos, size_t len, uint32_t run_cc) {
    const uint64_t k_nl = 0x0A0A0A0A0A0A0A0Aull, k_cr = 0x0D0D0D0D0D0D0D0Dull,
                   k_sp = 0x2020202020202020ull, k_co = 0x3A3A3A3A3A3A3A3Aull,
                   k_ha = 0x2323232323232323ull;
    for (;;) {
        if (pos >= len) {
            return pos;
        }
        /* the sweep: the main loop's line facts, unchanged */
        size_t whole = len - pos;
        size_t end = whole, ind = whole;
        int have_ind = 0;
        size_t i = 0;
        size_t stop = whole;
        int stop_set = 0;
        while (i < whole) {
            size_t avail = whole - i < 8 ? whole - i : 8;
            uint64_t x;
            if (avail == 8) {
                memcpy(&x, p + pos + i, 8);
            } else {
                char buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                memcpy(buf, p + pos + i, avail);
                memcpy(&x, buf, 8);
            }
            uint64_t valid = avail == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (avail * 8)) - 1);
            uint64_t br = (yep_swar_eq8(x, k_nl) | yep_swar_eq8(x, k_cr)) & valid;
            uint64_t room = (br ? ((br & (~br + 1)) - 1) : valid) & YEP_SWAR_FLAGS & valid;
            if (!have_ind) {
                uint64_t nons = ~(yep_swar_eq8(x, k_sp)) & room;
                if (nons) {
                    ind = i + (size_t)(yep_ctz64(nons) >> 3);
                    have_ind = 1;
                }
            }
            if (have_ind && !stop_set) {
                uint64_t stm = (yep_swar_eq8(x, k_co) | yep_swar_eq8(x, k_ha)) & room;
                if (stm) {
                    size_t cand = i + (size_t)(yep_ctz64(stm) >> 3);
                    if (cand >= ind) {
                        stop = cand;
                        stop_set = 1;
                    }
                }
            }
            if (br) {
                end = i + (size_t)(yep_ctz64(br) >> 3);
                break;
            }
            i += 8;
        }
        if (!have_ind) {
            ind = end;
        }
        if (!stop_set) {
            stop = end;
        }
        uint32_t le = (uint32_t)(pos + end);
        uint32_t li = (uint32_t)(pos + ind);
        uint32_t ls = (uint32_t)(pos + stop);
        uint32_t col = (uint32_t)(li - pos);
        /* blank and comment lines ride silently — the main loop skips
         * them with no state change */
        if (ind >= end || (p[li] == '#' && (li == 0 || p[li - 1] == ' ' || p[li - 1] == '\t' ||
                                            p[li - 1] == '\n'))) {
            pos = le < len ? (size_t)le + 1 : len;
            continue;
        }
        if (col + 1 != run_cc) {
            return pos; /* off-column: the pop/nest logic owns it */
        }
        /* document markers are the engine's alone — the "..."+content
         * error law lives there (the nightly's marshal crasher) */
        if (le - li >= 3 && (memcmp(p + li, "...", 3) == 0 || memcmp(p + li, "---", 3) == 0)) {
            size_t mk2 = li + 3;
            if (mk2 >= le || p[mk2] == ' ' || p[mk2] == '\t') {
                return pos;
            }
        }
        unsigned char c = (unsigned char)p[li];
        if (c == '\t' || c == '-' || c == '&' || c == '!' || c == '*' || c == '\'' || c == '"' ||
            c == '[' || c == '{' || c == '|' || c == '>' || c == '%' || c == '?' ||
            !yep_plain_first_ok(c)) {
            return pos; /* indicators, tab-led, and the plain-first law
                           (a ']' key rode the flat-run — the nightly's
                           marshal crasher "<: .ba\n]ge: 21"): full arms */
        }
        if (!stop_set || p[ls] != ':' || (ls + 1 < le && p[ls + 1] != ' ' && p[ls + 1] != '\t')) {
            return pos; /* not a plain key: value line */
        }
        size_t t0 = li;
        uint32_t kend = ls;
        while (kend > t0 && (p[kend - 1] == ' ' || p[kend - 1] == '\t')) {
            kend--;
        }
        if (kend == t0 || (uint32_t)(kend - t0) > 1024u) {
            return pos; /* empty key / the simple-key length law */
        }
        size_t vt = ls + 1;
        while (vt < le && (p[vt] == ' ' || p[vt] == '\t')) {
            vt++;
        }
        if (vt >= le || p[vt] == '#') {
            return pos; /* "key:" pending: the main loop's frame arm */
        }
        unsigned char vc = (unsigned char)p[vt];
        if (vc == '"' || vc == '\'') {
            /* the quoted plain-value arm, same law as the main loop:
             * same-line close, no backslash, no '' doubling, a comment
             * needs separating whitespace */
            size_t ce = vt + 1;
            int esc = 0;
            while (ce < le) {
                if (p[ce] == '\\') {
                    esc = 1;
                    break;
                }
                if (p[ce] == vc) {
                    break;
                }
                ce++;
            }
            if (ce >= le || esc) {
                return pos; /* unterminated or escaped: the engine */
            }
            if (vc == '\'' && ce + 1 < le && p[ce + 1] == '\'') {
                return pos; /* the '' escape: the engine */
            }
            size_t te = ce + 1;
            while (te < le && (p[te] == ' ' || p[te] == '\t')) {
                te++;
            }
            if (te < le && (p[te] != '#' || te == ce + 1)) {
                return pos; /* trailing content after the close */
            }
            (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u));
            (void)yt_put(t, yt_span_in((uint32_t)t0, kend - (uint32_t)t0, YTP_SPAN_IN));
            (void)yt_put(t, yt_props(YTP_SCALAR, 0, vc == '"' ? 0x2300u : 0x2200u));
            (void)yt_put(t, yt_span_in((uint32_t)(vt + 1), (uint32_t)(ce - vt - 1), YTP_SPAN_IN));
            pos = le < len ? (size_t)le + 1 : len;
            continue;
        }
        if (vc == '*' || vc == '|' || vc == '>' || vc == '[' || vc == '{' || vc == '!' ||
            vc == '%' || vc == '&' ||
            ((vc == '-' || vc == '?') &&
             (vt + 1 >= le || p[vt + 1] == ' ' || p[vt + 1] == '\t' || p[vt + 1] == '#'))) {
            return pos; /* block/flow/anchor values: the full arms */
        }
        if (!yep_plain_first_ok(vc)) {
            return pos; /* ']' '}' ',' '@' '`': no plain value can start
                         * there (the nightly's "k1: --a &p{\n0: ]" marshal
                         * crasher) — the engine errors cleanly */
        }
        /* a terminating ':' inside the value is the mapping-values error */
        {
            const char* h = (const char*)memchr(p + vt, ':', le - vt);
            while (h != NULL) {
                if (h + 1 >= p + le || h[1] == ' ' || h[1] == '\t') {
                    return pos; /* the engine's error route */
                }
                h = (const char*)memchr(h + 1, ':', le - (size_t)(h + 1 - p));
            }
        }
        uint32_t vend = le;
        {
            const char* hh = (const char*)memchr(p + vt, '#', le - vt);
            while (hh != NULL) {
                if (hh > p + vt && (hh[-1] == ' ' || hh[-1] == '\t')) {
                    vend = (uint32_t)(hh - p);
                    break;
                }
                hh = (const char*)memchr(hh + 1, '#', le - (size_t)(hh + 1 - p));
            }
        }
        while (vend > vt && (p[vend - 1] == ' ' || p[vend - 1] == '\t')) {
            vend--;
        }
        (void)yt_put(t, yt_props(YTP_PAIR, 0, 2u | (1u << 3)));
        (void)yt_put(t, yt_span_in((uint32_t)t0, kend - (uint32_t)t0, YTP_SPAN_IN));
        (void)yt_put(t, yt_span_in((uint32_t)vt, vend - (uint32_t)vt, YTP_SPAN_IN));
        pos = le < len ? (size_t)le + 1 : len;
    }
}

static int yt_f_run(yt_fused* F) {
    const char* p = F->p;
    size_t len = F->len;
    yep_ytape* t = F->t;
    yt_frame st[YT_FUSED_MAX_FRAMES];
    int depth = 0;
    memset(&st[0], 0, sizeof(st[0]));
    st[0].started = 1; /* the root's start event defers to its first pair */
    int root_open = 0;
    int root_evented = 0;
    uint32_t root_pair_col = 0; /* the root map's first pair line's column
                                 * (0 = none seen): the second-root dedent
                                 * law reads this, NOT content_col — the
                                 * pending arm never set content_col (" k:
                                 * &a", the nightly's run 39064966578,
                                 * issue #630) and arming content_col here
                                 * instead broke the " k: \n j: w" siblings
                                 * (the next nightly's crash-753ce4c1: the
                                 * map dissolved into four root scalars) */
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 1u);
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 3u);
    size_t pos = 0;
    while (pos < len) {
        /* the sweep: one NUL-padded SWAR walk per line */
        const uint64_t k_nl = 0x0A0A0A0A0A0A0A0Aull, k_cr = 0x0D0D0D0D0D0D0D0Dull,
                       k_sp = 0x2020202020202020ull, k_co = 0x3A3A3A3A3A3A3A3Aull,
                       k_ha = 0x2323232323232323ull;
        size_t whole = len - pos;
        size_t end = whole, indent = whole, stop = whole;
        int have_indent = 0, stop_set = 0;
        size_t i = 0;
        while (i < whole) {
            size_t avail = whole - i < 8 ? whole - i : 8;
            uint64_t x;
            if (avail == 8) {
                memcpy(&x, p + pos + i, 8);
            } else {
                char buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                memcpy(buf, p + pos + i, avail);
                memcpy(&x, buf, 8);
            }
            uint64_t valid = avail == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (avail * 8)) - 1);
            uint64_t br = (yep_swar_eq8(x, k_nl) | yep_swar_eq8(x, k_cr)) & valid;
            uint64_t room = (br ? ((br & (~br + 1)) - 1) : valid) & YEP_SWAR_FLAGS & valid;
            if (!have_indent) {
                uint64_t nons = ~(yep_swar_eq8(x, k_sp)) & room;
                if (nons) {
                    indent = i + (size_t)(yep_ctz64(nons) >> 3);
                    have_indent = 1;
                }
            }
            if (have_indent && !stop_set) {
                uint64_t stm = (yep_swar_eq8(x, k_co) | yep_swar_eq8(x, k_ha)) & room;
                if (stm) {
                    size_t cand = i + (size_t)(yep_ctz64(stm) >> 3);
                    if (cand >= indent) {
                        stop = cand;
                        stop_set = 1;
                    }
                }
            }
            if (br) {
                end = i + (size_t)(yep_ctz64(br) >> 3);
                break;
            }
            i += 8;
        }
        if (!have_indent) {
            indent = end;
        }
        if (!stop_set) {
            stop = end;
        }
        uint32_t line_end = (uint32_t)(pos + end);
        uint32_t line_indent = (uint32_t)(pos + indent);
        uint32_t line_stop = (uint32_t)(pos + stop);
        uint8_t blank = (uint8_t)(indent >= end);
        unsigned char c = blank ? 0 : (unsigned char)p[line_indent];
        uint8_t comment =
            (uint8_t)(c == '#' && (line_indent == 0 || p[line_indent - 1] == ' ' ||
                                   p[line_indent - 1] == '\t' || p[line_indent - 1] == '\n'));
        uint8_t dash = 0;
        if (c == '-' && line_indent + 1 < line_end) {
            unsigned char n1 = (unsigned char)p[line_indent + 1];
            if (n1 == ' ' || n1 == '\t') {
                dash = 1;
            }
        } else if (c == '-' && line_indent + 1 >= line_end) {
            dash = 1; /* a line-end dash is an indicator, not the scalar "-" */
        }
        size_t next = line_end < len ? (size_t)line_end + 1 : len;
        if (!blank && !comment) {
            size_t t0 = line_indent;
            uint32_t col = (uint32_t)(line_indent - pos);
            if (c == '\t') {
                return 1; /* tab-led content: the engine's error */
            }
            /* document markers are the engine's alone: only it carries
             * the "..."+content error law (the nightly's marshal
             * crasher, "a: b\n... vi an :  }" — the runner swallowed
             * the post-'...' content as a pair) */
            if (col == 0 && line_end - line_indent >= 3 &&
                (memcmp(p + line_indent, "...", 3) == 0 ||
                 memcmp(p + line_indent, "---", 3) == 0)) {
                size_t mk = line_indent + 3;
                if (mk >= line_end || p[mk] == ' ' || p[mk] == '\t') {
                    return 1;
                }
            }
            if (dash && depth > 0 && st[depth].pending && col >= st[depth].indent) {
                /* the pending key resolves into a sequence: its own
                 * frame (the key's map survives below it) */
                if (depth >= YT_FUSED_MAX_FRAMES - 1) {
                    return 1; /* the frame stack is full: the engine route */
                }
                int anchored = st[depth].aid != 0;
                if (anchored) {
                    if (col == st[depth].indent) {
                        return 1; /* indentless anchored seq: the engine owns it
                                   * (the frame-close pairing assumes the seq's
                                   * own column, deeper than the key's) */
                    }
                    /* anchored sequence: S2900 key + the anchored SEQ start */
                    if (depth == 1) {
                        if (!root_open) {
                            (void)yt_put(t, yt_props(YTP_EVENT, 0, 7u));
                            yt_f_empty_span(F);
                            t->depth++;
                            root_open = 1;
                            root_evented = 1;
                            st[0].started = 1;
                        }
                    } else if (!st[depth - 1].started && col != st[depth - 1].indent) {
                        yt_f_start(F, &st[depth - 1]);
                    }
                    uint32_t xaid = st[depth].aid;
                    uint32_t xoff = st[depth].a_off;
                    uint32_t xlen = st[depth].a_len;
                    (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u));
                    (void)yt_put(t, yt_span_in(st[depth].key_off, st[depth].key_len, YTP_SPAN_IN));
                    (void)yt_put(t, yt_props(YTP_EVENT, xaid, 5u | (1u << 13)));
                    yt_f_empty_span(F);
                    (void)yt_put(t, yt_span_in(xoff, xlen, YTP_SPAN_IN));
                    yt_anchor_open(F, xaid);
                    st[depth].pending = 0; /* the seq frame shadows this slot */
                } else {
                    uint8_t of = yt_f_open_form(&st[depth - 1], depth == 1, st[depth].indent);
                    yt_f_open(F, &st[depth], of);
                    if (depth == 1 && st[0].content_col == 0) {
                        /* the root bookkeeping the anchored branch carries:
                         * without it a dedent past the root map's column
                         * pops to a content_col-0 st[0] and the pair arm
                         * read "root pairs at column 0" — the nightly's
                         * run 38128905644, " american:\n  - Boston...\n
                         * national:" split into PHANTOM DOCUMENTS
                         * ("---\nb\n---\nv") while the engine rejected */
                        st[0].content_col = st[depth].indent + 1;
                    }
                    if (st[depth].content_col == 0) {
                        /* the opened map's pairs live at the KEY's
                         * column (the anchored branch tracks this;
                         * without it the off-column guard never fires
                         * and a key line BETWEEN the resolved seq's
                         * column and the parent's fell through to a
                         * SILENTLY TRUNCATED document — the nightly's
                         * run 38118641722, "ue:\n  - one\n woc:\n
                         *  - one\n wt o-  : thrt" dropped lines 3-5
                         * while the engine rejected at 3:2) */
                        st[depth].content_col = st[depth].indent + 1;
                    }
                    root_open = 1;
                    st[depth].pending = 0;
                }
                depth++;
                memset(&st[depth], 0, sizeof(st[depth]));
                st[depth].kind = 1;
                st[depth].indent = col;
                st[depth].own_map = 1;
                st[depth].anchored_first = (uint8_t)anchored;
                if (anchored) {
                    st[depth].started = 1; /* the E5 went out above */
                } else {
                    yt_f_start(F, &st[depth]);
                }
            } else {
                while (depth > 0 && (st[depth].kind == 1 ? (col < st[depth].indent ||
                                                            (col <= st[depth].indent && !dash))
                                                         : col < st[depth].indent)) {
                    if (st[depth].kind == 1 && col == st[depth].indent && !dash &&
                        !(depth > 1 && st[depth - 1].kind == 0 &&
                          st[depth - 1].indent == st[depth].indent)) {
                        return 1; /* a non-dash line at an INDENTED seq's
                                   * column: the seq owns its column, the
                                   * pair cannot join it and cannot close
                                   * it (libyaml: "did not find expected
                                   * '-' indicator"; the nightly's run
                                   * 38117287882, "amer:\n  - j\n  n:\n
                                   *  - Bksl,} -- }" — the runner popped
                                   * the seq and SILENTLY DROPPED the
                                   * lines while the engine rejected).
                                   * Only the INDENTLESS seq — parent map
                                   * keys at the same column — closes on
                                   * a key line there ("k:\n- j\nn: v") */
                    }
                    if (st[depth].pending) {
                        return 1; /* empty "key:" mid-doc: the engine owns it */
                    }
                    if (st[depth].phantom) {
                        yt_f_close(F, st[depth].kind);
                        st[depth].phantom = 0;
                    }
                    if (st[depth].own_map) {
                        yt_f_close(F, st[depth].kind);
                    }
                    depth--;
                }
            }
            if (dash) {
                if (st[depth].kind == 1 && col == st[depth].indent) {
                    yt_f_start(F, &st[depth]);
                    size_t vt = t0 + 1;
                    while (vt < line_end && (p[vt] == ' ' || p[vt] == '\t')) {
                        vt++;
                    }
                    uint32_t vend = line_end;
                    while (vend > vt && (p[vend - 1] == ' ' || p[vend - 1] == '\t')) {
                        vend--;
                    }
                    if (vend == vt) {
                        return 1; /* empty item */
                    }
                    unsigned char vc = (unsigned char)p[vt];
                    if (vc == '"' || vc == '\'' || vc == '*' || vc == '&' || vc == '[' ||
                        vc == '{' || vc == '!' || vc == '|' || vc == '>' || vc == '%' ||
                        vc == '?') {
                        return 1;
                    }
                    if (!yep_plain_first_ok(vc)) {
                        return 1; /* ']'/','/'}'/'@'/'`' first: no plain
                                     item can start there (the nightly's
                                     marshal crasher, "x:\n- ]\"#") — the
                                     engine errors cleanly */
                    }
                    if (vc == '-' && (vt + 1 >= line_end || p[vt + 1] == ' ' || p[vt + 1] == '\t' ||
                                      p[vt + 1] == '#')) {
                        return 1; /* nested dash (incl. line-end — the
                                     nightly's "n:\n- -\t" crasher: a
                                     trailing bare '-' is an indicator,
                                     never the plain scalar "-"): the
                                     engine's nested seq */
                    }
                    /* compact map item "- k: v"? */
                    for (size_t ic = vt; ic < vend; ic++) {
                        if (p[ic] == ':') {
                            size_t nn = ic + 1;
                            if (nn >= vend || p[nn] == ' ' || p[nn] == '\t') {
                                return 1; /* compact items: the engine */
                            }
                        }
                    }
                    if (st[depth].anchored_first && F->resolver != NULL) {
                        yep_tag_id tid =
                            F->resolver->resolve(F->resolver->ctx, p + vt, (uint32_t)(vend - vt));
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u | (uint32_t)tid));
                        (void)yt_put(t, yt_span_in((uint32_t)vt, vend - (uint32_t)vt, YTP_SPAN_IN));
                        st[depth].anchored_first = 0;
                    } else {
                        (void)yt_put(t, yt_span_in((uint32_t)vt, vend - (uint32_t)vt, YTP_ITEM));
                    }
                    if (!st[depth].content_col) {
                        st[depth].content_col = col + 1;
                    }
                } else {
                    return 1;
                }
            } else if (stop_set && p[line_stop] == ':' &&
                       (line_stop + 1 >= line_end || p[line_stop + 1] == ' ' ||
                        p[line_stop + 1] == '\t') &&
                       !(c == '&' || c == '!' || c == '*' || c == '\'' || c == '"' || c == '[' ||
                         c == '{' || c == '|' || c == '>' || c == '%' || c == '?' || c == '-') &&
                       yep_plain_first_ok((unsigned char)c)) {
                if (depth == 0) {
                    if (root_pair_col != 0 && col < root_pair_col) {
                        /* a dedent below the root map's own pair column:
                         * a SECOND root without "---" (issue #630, " k:
                         * &a\n  - r\na: b" — the pending-value root arm
                         * never arms content_col, so the off-column
                         * guard below cannot see this; the engine and
                         * libyaml reject with "did not find expected
                         * <document start>") */
                        return 1;
                    }
                    if (root_pair_col == 0) {
                        root_pair_col = col; /* the first root pair owns its column */
                    }
                }
                if (st[depth].kind == 0 &&
                    ((st[depth].content_col != 0 && col + 1 != st[depth].content_col &&
                      (col != st[depth].indent || (depth == 0 && st[0].content_col > 1))) ||
                     (st[depth].content_col == 0 && depth > 0 && !st[depth].pending &&
                      col != st[depth].indent))) {
                    /* an OFF-COLUMN pair. No pending key awaits it, so a
                     * more-indented line cannot open a child and a
                     * dedented one is a second root (the catch-39 law;
                     * libyaml: "mapping values are not allowed in this
                     * context"). Two states: a frame with its own content
                     * column rejects every other column; a frame whose
                     * content map already closed (the phantom close above)
                     * only accepts pairs at its own key column. The pop
                     * loop pops on col < indent alone — without these
                     * checks "a:\n  b: 1\nc: 2\n  d: 3" rode the stale
                     * frame as a phantom sibling (the nightly's run
                     * 38179552341) */
                    return 1;
                }
                uint32_t kend = line_stop;
                while (kend > t0 && (p[kend - 1] == ' ' || p[kend - 1] == '\t')) {
                    kend--;
                }
                if (kend == t0) {
                    return 1; /* empty plain key (": v"): the engine's two-scalar form */
                }
                if ((uint32_t)(kend - t0) > 1024u) {
                    return 1; /* the simple-key length law: the engine errors */
                }
                size_t vt = line_stop + 1;
                while (vt < line_end && (p[vt] == ' ' || p[vt] == '\t')) {
                    vt++;
                }
                uint32_t aoff = 0, alen = 0, aid = 0;
                if (vt < line_end && p[vt] == '&') {
                    size_t ne = vt + 1;
                    while (ne < line_end && ne < len) {
                        unsigned char pc = (unsigned char)p[ne];
                        if (pc <= ' ' || pc == ',' || pc == '[' || pc == ']' || pc == '{' ||
                            pc == '}' || pc == '#') {
                            break;
                        }
                        ne++;
                    }
                    aoff = (uint32_t)(vt + 1);
                    alen = (uint32_t)(ne - vt - 1);
                    if (alen == 0) {
                        return 1; /* an anchor needs a name (libyaml):
                                     the engine owns the error */
                    }
                    if (ne < line_end && p[ne] == '#') {
                        return 1; /* a glued '#' is junk on the name
                                     (libyaml: "did not find expected
                                     alphabetic or numeric character") —
                                     the engine owns it */
                    }
                    F->anchor_seq++;
                    aid = F->anchor_seq;
                    if (!yt_anchor_table(F)) {
                        return 1;
                    }
                    yt_anchor_put(F, aoff, alen, aid);
                    vt = ne;
                    while (vt < line_end && (p[vt] == ' ' || p[vt] == '\t')) {
                        vt++;
                    }
                }
                if (vt >= line_end || p[vt] == '#') {
                    /* "key:" with a following-lines container: push pending
                     * (aid rides it: an anchored container resolves through
                     * the S2900 + anchored-start form, not on_block_open) */
                    if (st[depth].kind == 0 && st[depth].content_col != 0 &&
                        col + 1 != st[depth].content_col &&
                        (col != st[depth].indent || (depth == 0 && st[0].content_col > 1))) {
                        return 1; /* off-column: a continuation line — the multiline arm;
                                   * at the ROOT the indent exemption dies once pairs
                                   * live above column 0: a col-0 line under a col-1
                                   * root is a DEDENT — a second root without "---",
                                   * the engine's error (libyaml: "did not find
                                   * expected <document start>") */
                    }
                    if (st[depth].kind != 0) {
                        return 1; /* a pending "key:" over a SEQ frame: the
                                   * key cannot join the sequence (libyaml
                                   * "mapping values are not allowed in
                                   * this context"; the nightly's run
                                   * 38127584324, "k:\n- 42\n bar:\n
                                   *  - 45 e." — the pending pushed UNDER
                                   * the seq and materialized as an ITEM
                                   * while the engine rejected) */
                    }
                    if (st[depth].kind == 0 && st[depth].content_col != 0 &&
                        col + 1 != st[depth].content_col && st[depth].phantom) {
                        yt_f_close(F, st[depth].kind);
                        st[depth].phantom = 0;
                        st[depth].content_col =
                            0; /* the content map is gone:
                                * its column must not greet later pairs (the
                                * stale-frame law, run 38179552341) — after the
                                * chained-pending arm resumes here, a stale
                                * column rode yt_f_open_form's sibling test and
                                * misfiled the next pending's container as a
                                * key-SCALAR ("b:\n c: d\nq:\n b:\n  c: d\n"
                                * lost q's map; the nightly's run 42488986434) */
                    }
                    if (depth > 0 && st[depth].pending) {
                        /* a chained pending ("a:\na:", the deep-nesting
                         * "k:\n  k:\n    k:" ladder): resolve the outer
                         * one before the sibling's frame takes the slot.
                         * A same-column sibling key (col == the pending's
                         * own column) gives the pending a NULL value —
                         * the null pair MUST land now, or the OPEN'd
                         * container swallows the sibling key as its
                         * first pair's value (the nightly's run
                         * 38110714729 tree corruption: {a: a, ?{b: c}:
                         * h} instead of {a: null, a: {b: c}, h: v}).
                         * When the OPEN form fired, the null pairs
                         * INSIDE the opened container (parent → {k:
                         * null}); under the key-SCALAR form it completes
                         * the key's pair in the parent's map. A deeper
                         * line (col > indent) opens the pending's
                         * container and the chain nests. Anchored
                         * pendings keep the engine: their anchor rides
                         * the event path (yt_f_open drops the name) */
                        if (st[depth].aid != 0) {
                            return 1;
                        }
                        uint8_t of = yt_f_open_form(&st[depth - 1], depth == 1, st[depth].indent);
                        if (aid != 0 && of == 0) {
                            /* the outer pending resolved as a SIBLING
                             * key-SCALAR: its value container is "the pair's",
                             * materialized by the next OPEN-class record — but
                             * the anchored pending resolves through the event
                             * path, which emits its KEY SCALAR *before* the
                             * anchored MAP_START. That key pairs with the
                             * stranded outer key (the pair's value slot) and
                             * the anchored map strands as a sibling key of the
                             * parent: "2: e\n4:\n  5: &5\n    5: e\n7: s"
                             * built {2:e, 4:"5", {5:e}&5:"7", s:null} instead
                             * of {2:e, 4:{5:{5:e}&5}, 7:s} (the nightly's
                             * crash-d8e3b3f392c37a62257576ce20d5a19b351f66ad,
                             * round 213 — the first shape where a completed
                             * pair arms root_pair_col BEFORE the chain). The
                             * engine resolves the composition. */
                            return 1;
                        }
                        yt_f_open(F, &st[depth], of);
                        root_open = 1;
                        if (col == st[depth].indent) {
                            (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x0904u));
                            yt_f_empty_span(F);
                        }
                        st[depth].pending = 0;
                    }
                    /* a spent key-SCALAR frame (resolved, phantom closed,
                     * no map of its own) gives its slot to the sibling —
                     * chained same-column pendings would otherwise grow
                     * the stack by one per record (the frame-full bail) */
                    if (!(depth > 0 && !st[depth].pending && !st[depth].phantom &&
                          !st[depth].own_map)) {
                        if (depth >= YT_FUSED_MAX_FRAMES - 1) {
                            return 1;
                        }
                        depth++;
                    }
                    memset(&st[depth], 0, sizeof(st[depth]));
                    st[depth].indent = col;
                    st[depth].pending = 1;
                    st[depth].key_off = (uint32_t)t0;
                    st[depth].key_len = kend - (uint32_t)t0;
                    st[depth].aid = aid;
                    st[depth].a_off = aoff;
                    st[depth].a_len = alen;
                    st[depth].anchored_first = (uint8_t)(aid != 0);
                } else {
                    if (st[depth].kind == 0 && st[depth].content_col != 0 &&
                        col + 1 != st[depth].content_col &&
                        (col != st[depth].indent || (depth == 0 && st[0].content_col > 1))) {
                        return 1; /* off-column: a continuation line — the multiline arm;
                                   * the root's indent exemption dies once pairs
                                   * live above column 0 (dedent = second root) */
                    }
                    if (depth > 0 && st[depth].pending && st[depth].aid != 0) {
                        if (depth > 1) {
                            /* an anchored pending BELOW the root frame: the
                             * deferral's records only flush at depth 1 — a
                             * deeper one drops its pairs at EOF ("4:\n4: r\n
                             * 5:\n &5\n 5: e" lost the anchored map; the
                             * nightly's run 38202382251) — the engine
                             * resolves it */
                            return 1;
                        }
                        if (col <= st[depth].indent) {
                            return 1; /* same-column "content": the anchored value is
                                       * null and this line is a sibling — the engine */
                        }
                        /* an anchored container resolves through the event
                         * path: the parent's content map opens for the key
                         * first, then the key SCALAR + the anchored start */
                        if (depth == 1) {
                            if (!root_open) {
                                (void)yt_put(t, yt_props(YTP_EVENT, 0, 7u));
                                yt_f_empty_span(F);
                                t->depth++;
                                root_open = 1;
                                root_evented = 1;
                                st[0].started = 1;
                            }
                            /* the root now has content: later sibling pendings
                             * take the key-SCALAR form, not OPEN */
                            if (st[0].content_col == 0) {
                                st[0].content_col = st[depth].indent + 1;
                            }
                        } else if (!st[depth - 1].started && col != st[depth - 1].indent) {
                            yt_f_start(F, &st[depth - 1]);
                        }
                        uint32_t xaid = st[depth].aid;
                        uint32_t xoff = st[depth].a_off;
                        uint32_t xlen = st[depth].a_len;
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u));
                        (void)yt_put(t,
                                     yt_span_in(st[depth].key_off, st[depth].key_len, YTP_SPAN_IN));
                        (void)yt_put(t, yt_props(YTP_EVENT, xaid, 7u | (1u << 13)));
                        yt_f_empty_span(F);
                        (void)yt_put(t, yt_span_in(xoff, xlen, YTP_SPAN_IN));
                        yt_anchor_open(F, xaid);
                        memset(&st[depth], 0, sizeof(st[depth]));
                        st[depth].indent = col; /* the engine's frame pops on the CONTENT
                                                 * column, not the anchored key's */
                        st[depth].own_map = 1;
                        st[depth].started = 1;
                        st[depth].anchored_first = 1;
                        t->depth++;
                    } else if (depth > 0 && st[depth].pending) {
                        uint8_t of = yt_f_open_form(&st[depth - 1], depth == 1, st[depth].indent);
                        yt_f_open(F, &st[depth], of);
                        if (depth == 1 && st[0].content_col == 0) {
                            /* the pending->MAP resolution needs the same
                             * root bookkeeping the seq arm gained (run
                             * 38134498534, "  e:\n   f: g\nh: i" — the
                             * dedent popped to a content_col-0 root and
                             * the doc accepted what the engine rejects) */
                            st[0].content_col = st[depth].indent + 1;
                        }
                        root_open = 1;
                        st[depth].pending = 0;
                        if (col == st[depth].indent) {
                            /* a same-column sibling pair resolves the pending
                             * key to null (the engine's empty-value scalar) */
                            (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x0904u));
                            yt_f_empty_span(F);
                        }
                    } else if (!root_open && depth == 0) {
                        (void)yt_put(t, yt_props(YTP_EVENT, 0, 7u));
                        yt_f_empty_span(F);
                        t->depth++;
                        root_open = 1;
                        root_evented = 1;
                        st[0].started = 1;
                    }
                    if (col == st[depth].indent) {
                        /* the engine's sibling-continue: the live phantom
                         * content map closes, the pair rides the frame's
                         * own map without a start event */
                        if (st[depth].phantom) {
                            yt_f_close(F, st[depth].kind);
                            st[depth].phantom = 0;
                            st[depth].content_col = 0; /* the content map is
                                                        * gone: its column must not
                                                        * greet later pairs (the
                                                        * stale frame below) */
                        }
                    } else {
                        yt_f_start(F, &st[depth]);
                    }
                    if (st[depth].kind != 0) {
                        return 1;
                    }
                    unsigned char vc = (unsigned char)p[vt];
                    uint32_t flags;
                    if ((vc == '*' && aid != 0) || (vc == '*' && st[depth].anchored_first)) {
                        return 1; /* an anchored/first alias: the engine owns it */
                    }
                    if (vc == '*') {
                        size_t ne = vt + 1;
                        while (ne < line_end) {
                            unsigned char pc = (unsigned char)p[ne];
                            if (pc <= ' ' || pc == ',' || pc == '[' || pc == ']' || pc == '{' ||
                                pc == '}' || pc == '#') {
                                break;
                            }
                            ne++;
                        }
                        if (ne < line_end && p[ne] == '#') {
                            return 1; /* a glued '#' is junk on the
                                       * name (the engine errors:
                                       * "did not find expected
                                       * alphabetic or numeric
                                       * character") */
                        }
                        {
                            size_t ae = ne;
                            while (ae < line_end && (p[ae] == ' ' || p[ae] == '\t')) {
                                ae++;
                            }
                            if (ae < line_end && p[ae] != '#') {
                                return 1; /* an alias node is COMPLETE —
                                           * on the line only a blank
                                           * then a comment may follow
                                           * ("<<: *x &y": the engine
                                           * errors, libyaml "did not
                                           * find expected key"; this
                                           * branch silently dropped the
                                           * bytes) */
                            }
                        }
                        uint32_t taid = 0;
                        if (!yt_anchor_get(F, (uint32_t)(vt + 1), (uint32_t)(ne - vt - 1), &taid)) {
                            return 1; /* undefined alias: the engine errors */
                        }
                        if (taid == 0 || taid > YT_FUSED_ANCHORS || F->anchors_opened == NULL ||
                            !F->anchors_opened[taid - 1]) {
                            return 1; /* alias to an anchor whose container never opened
                                       * (null-valued or still pending): the engine */
                        }
                        (void)yt_put(t, yt_props(YTP_PAIR, taid, 3u /*ALIAS*/));
                        (void)yt_put(t, yt_span_in((uint32_t)t0, kend - (uint32_t)t0, YTP_SPAN_IN));
                        (void)yt_put(t, yt_span_in((uint32_t)(vt + 1), (uint32_t)(ne - vt - 1),
                                                   YTP_SPAN_IN));
                        if (!st[depth].content_col) {
                            st[depth].content_col = col + 1;
                        }
                        goto line_done;
                    }
                    if ((vc == '"' || vc == '\'') && aid == 0) {
                        /* same-line quoted value: the two-SCALAR form (the
                         * key plain + the styled value, quotes stripped);
                         * escapes and multiline quotes stay with the engine */
                        size_t ce = vt + 1;
                        int esc = 0;
                        while (ce < line_end) {
                            if (p[ce] == '\\') {
                                esc = 1;
                                break;
                            }
                            if (p[ce] == vc) {
                                break;
                            }
                            ce++;
                        }
                        if (ce >= line_end || esc) {
                            return 1; /* unterminated or escaped: the engine */
                        }
                        if (vc == '\'' && ce + 1 < line_end && p[ce + 1] == '\'') {
                            return 1; /* the '' escape: the engine */
                        }
                        size_t te = ce + 1;
                        while (te < line_end && (p[te] == ' ' || p[te] == '\t')) {
                            te++;
                        }
                        /* a comment needs separating whitespace — "#"
                         * glued to the quote is content (the engine errors) */
                        if (te < line_end && (p[te] != '#' || te == ce + 1)) {
                            return 1; /* trailing content after the close */
                        }
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u));
                        (void)yt_put(t, yt_span_in((uint32_t)t0, kend - (uint32_t)t0, YTP_SPAN_IN));
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, vc == '"' ? 0x2300u : 0x2200u));
                        (void)yt_put(t, yt_span_in((uint32_t)(vt + 1), (uint32_t)(ce - vt - 1),
                                                   YTP_SPAN_IN));
                        st[depth].anchored_first = 0;
                        if (!st[depth].content_col) {
                            st[depth].content_col = col + 1;
                        }
                        goto line_done;
                    }
                    if (vc == '|' && aid == 0 && !st[depth].anchored_first &&
                        (vt + 1 >= line_end || p[vt + 1] == ' ' || p[vt + 1] == '\t')) {
                        /* the header tail: only blanks and an optional
                         * comment may follow the indicator (libyaml:
                         * "did not find expected comment or line break";
                         * the nightly's run 38119775008, "ext: | c* " —
                         * this arm took the literal and SILENTLY DROPPED
                         * the trailing header bytes while the engine
                         * rejected) */
                        size_t ht = vt + 1;
                        while (ht < line_end && (p[ht] == ' ' || p[ht] == '\t')) {
                            ht++;
                        }
                        if (ht < line_end && p[ht] != '#') {
                            return 1;
                        }
                        /* bare literal block, clip chomp: measure the block,
                         * copy it into the finish pool, emit the pool form */
                        size_t scan = next;
                        uint32_t bindent = 0;
                        uint32_t lead_blank_max = 0;
                        int have_bi = 0, have_content = 0;
                        size_t clen = 0, tb = 0;
                        size_t end_scan = scan;
                        /* per-line copy facts: blocks whose lines fit
                         * the array copy straight from it — the second,
                         * boundary-rederiving walk disappears. Larger
                         * blocks (nested in block-heavy) keep the walk
                         * below. */
                        struct {
                            uint32_t off;
                            uint32_t n;
                            uint8_t blank;
                        } ln[16];
                        size_t nl = 0;
                        int spilled = 0;
                        while (scan < len) {
                            size_t ls = scan;
                            size_t le = ls;
                            while (le < len && p[le] != '\n' && p[le] != '\r') {
                                le++;
                            }
                            size_t ie = ls;
                            while (ie < le && p[ie] == ' ') {
                                ie++;
                            }
                            uint32_t cpy_off = 0, cpy_n = 0;
                            uint8_t cpy_blank = 0;
                            if (ie == le) {
                                /* a blank line: spaces past the block indent are
                                 * CONTENT; only stripped-empty lines clip away */
                                size_t ind = ie - ls;
                                if (!have_content && ind > lead_blank_max) {
                                    lead_blank_max = (uint32_t)ind;
                                }
                                size_t res = have_bi && ind > bindent ? ind - bindent : 0;
                                clen += res + 1;
                                if (res == 0) {
                                    tb += 1;
                                } else {
                                    tb = 0;
                                }
                                cpy_n = (uint32_t)res;
                                cpy_blank = 1;
                            } else {
                                uint32_t ind = (uint32_t)(ie - ls);
                                if (!have_bi) {
                                    if (ind <= col) {
                                        break; /* dedented past the block */
                                    }
                                    if (ind < lead_blank_max) {
                                        return 1; /* a LEADING blank line
                                                   * with MORE indent
                                                   * floors the block
                                                   * (libyaml's
                                                   * auto-detect takes
                                                   * the blanks' max;
                                                   * content below the
                                                   * floor breaks the
                                                   * map — Psych: "did
                                                   * not find expected
                                                   * key"; the nightly's
                                                   * run 38163867815,
                                                   * "a: |\n    \n  b"):
                                                   * the engine's error */
                                    }
                                    bindent = ind;
                                    have_bi = 1;
                                } else if (ind < bindent) {
                                    break; /* the block ends */
                                }
                                if (memchr(p + ls, '\t', le - ls) != NULL) {
                                    return 1; /* tabs: the engine's error */
                                }
                                clen += (le - (ls + bindent)) + 1;
                                have_content = 1;
                                tb = 0;
                                cpy_off = (uint32_t)(ls + bindent);
                                cpy_n = (uint32_t)(le - (ls + bindent));
                            }
                            if (nl < 16) {
                                ln[nl].off = cpy_off;
                                ln[nl].n = cpy_n;
                                ln[nl].blank = cpy_blank;
                                nl++;
                            } else {
                                spilled = 1;
                            }
                            end_scan =
                                (le < len && p[le] == '\r' && le + 1 < len && p[le + 1] == '\n')
                                    ? le + 2
                                    : le + 1; /* lone CR is a 1-byte break */
                            scan = end_scan;
                        }
                        if (!have_content) {
                            return 1; /* empty/blank-only block: the engine */
                        }
                        size_t flen = clen - tb; /* clip: the trailing stripped-empty lines */
                        if (F->pool == NULL) {
                            F->pool = yep_pool_create(yep_system_allocator(), 4096);
                            if (F->pool == NULL) {
                                return 1;
                            }
                        }
                        /* the BLOCK is clen-sized: both copy paths write
                         * every measured line (the clip lives in the
                         * SPAN, not the copy) — allocating flen wrote
                         * tb bytes past the block whenever a literal
                         * ended with stripped-empty lines (the nightly's
                         * run 38137087116 ASan heap-buffer-overflow,
                         * WRITE at ytape.c's re-walk memcpy) */
                        char* dst = (char*)yep_pool_alloc(F->pool, clen, 16);
                        if (dst == NULL) {
                            return 1;
                        }
                        {
                            char* q = dst;
                            if (!spilled) {
                                /* the array path: sizing and copy derive
                                 * from the SAME facts — exact by
                                 * construction, no re-walk */
                                for (size_t x = 0; x < nl; x++) {
                                    if (ln[x].blank) {
                                        if (ln[x].n > 0) {
                                            memset(q, ' ', ln[x].n);
                                            q += ln[x].n;
                                        }
                                    } else {
                                        memcpy(q, p + ln[x].off, ln[x].n);
                                        q += ln[x].n;
                                    }
                                    *q++ = '\n';
                                }
                            } else {
                                size_t s2 = next;
                                while (s2 < scan) {
                                    size_t ls = s2;
                                    size_t le = ls;
                                    while (le < len && p[le] != '\n' && p[le] != '\r') {
                                        le++;
                                    }
                                    size_t ie = ls;
                                    while (ie < le && p[ie] == ' ') {
                                        ie++;
                                    }
                                    if (ie == le) {
                                        size_t ind = ie - ls;
                                        size_t res = have_bi && ind > bindent ? ind - bindent : 0;
                                        if (res > 0) {
                                            memset(q, ' ', res);
                                            q += res;
                                        }
                                        *q++ = '\n';
                                    } else {
                                        memcpy(q, p + ls + bindent, le - (ls + bindent));
                                        q += le - (ls + bindent);
                                        *q++ = '\n';
                                    }
                                    s2 = (le < len && p[le] == '\r' && le + 1 < len &&
                                          p[le + 1] == '\n')
                                             ? le + 2
                                             : le + 1; /* lone CR is a 1-byte break */
                                }
                            }
                            if ((size_t)(q - dst) != clen) {
                                return 1; /* drift: bail, not corrupt */
                            }
                        }
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u));
                        (void)yt_put(t, yt_span_in((uint32_t)t0, kend - (uint32_t)t0, YTP_SPAN_IN));
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x0400u));
                        (void)yt_put(t, ((uint64_t)YTP_SPAN_POOL << 56) | (uint64_t)flen);
                        (void)yt_put(t, (uint64_t)(uintptr_t)dst);
                        if (!st[depth].content_col) {
                            st[depth].content_col = col + 1;
                        }
                        next = scan;
                        goto line_done;
                    }
                    if (vc == '"' || vc == '\'' || vc == '|' || vc == '>' || vc == '[' ||
                        vc == '{' || vc == '!' || vc == '%' || vc == '&' || vc == '*') {
                        return 1; /* quoted/block/flow/tag/anchor/alias values:
                                     the engine */
                    }
                    if ((vc == '-' || vc == '?') && (vt + 1 >= line_end || p[vt + 1] == ' ' ||
                                                     p[vt + 1] == '\t' || p[vt + 1] == '#')) {
                        return 1; /* an indicator-as-value: the engine errors */
                    }
                    if (!yep_plain_first_ok(vc)) {
                        return 1; /* '}' ']' ',' '@' '`' '%': no node
                                   * content can start there (the nightly's
                                   * fuzz_roundtrip crasher, "u: }") — the engine
                                   * errors cleanly */
                    }
                    /* a terminating ':' inside the value is the
                     * mapping-values error — the engine owns it */
                    for (const char* h = (const char*)memchr(p + vt, ':', line_end - vt);
                         h != NULL && h < p + line_end;
                         h = (const char*)memchr(h + 1, ':', line_end - (size_t)(h + 1 - p))) {
                        if (h + 1 >= p + line_end || h[1] == ' ' || h[1] == '\t') {
                            return 1;
                        }
                    }
                    /* trailing comment bounds the plain value */
                    uint32_t vend = line_end;
                    {
                        const char* h = (const char*)memchr(p + vt, '#', line_end - vt);
                        while (h != NULL) {
                            if (h > p + vt && (h[-1] == ' ' || h[-1] == '\t')) {
                                vend = (uint32_t)(h - p);
                                break;
                            }
                            h = (const char*)memchr(h + 1, '#', line_end - (size_t)(h + 1 - p));
                        }
                    }
                    while (vend > vt && (p[vend - 1] == ' ' || p[vend - 1] == '\t')) {
                        vend--;
                    }
                    if (st[depth].anchored_first) {
                        /* the first pair inside an anchored container: the
                         * two-SCALAR form with the engine's tag bake */
                        if (aid != 0 || F->resolver == NULL) {
                            return 1;
                        }
                        yep_tag_id tid =
                            F->resolver->resolve(F->resolver->ctx, p + vt, (uint32_t)(vend - vt));
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u));
                        (void)yt_put(t, yt_span_in((uint32_t)t0, kend - (uint32_t)t0, YTP_SPAN_IN));
                        (void)yt_put(t, yt_props(YTP_SCALAR, 0, 0x2900u | (uint32_t)tid));
                        (void)yt_put(t, yt_span_in((uint32_t)vt, vend - (uint32_t)vt, YTP_SPAN_IN));
                        st[depth].anchored_first = 0;
                        if (!st[depth].content_col) {
                            st[depth].content_col = col + 1;
                        }
                        goto line_done;
                    }
                    if (aid != 0) {
                        flags = 4u | (1u << 3) | (1u << 4);
                    } else {
                        flags = 2u | (1u << 3);
                    }
                    (void)yt_put(t, yt_props(YTP_PAIR, aid, flags));
                    (void)yt_put(t, yt_span_in((uint32_t)t0, kend - (uint32_t)t0, YTP_SPAN_IN));
                    (void)yt_put(t, yt_span_in((uint32_t)vt, vend - (uint32_t)vt, YTP_SPAN_IN));
                    if (aid != 0) {
                        (void)yt_put(t, yt_span_in(aoff, alen, YTP_SPAN_IN));
                    }
                    if (!st[depth].content_col) {
                        st[depth].content_col = col + 1;
                    }
                    if (aid == 0 && !st[depth].phantom && st[depth].content_col == col + 1) {
                        /* the specialist takes the run at ANY frame's
                         * content column: nested flat runs (anchored
                         * containers, leaf maps) qualify; an off-column
                         * line bails to the pop/nest arms as usual.
                         * anchored_first is already cleared whenever a
                         * plain emission reaches here (the anchored arm
                         * leaves early), so no extra gate is owed. */
                        size_t sp = yt_flat_run(t, p, next, len, st[depth].content_col);
                        if (sp != next) {
                            pos = sp;
                            goto line_done_keep;
                        }
                    }
                }
            } else {
                return 1; /* unclassified line: the engine route */
            }
        }
    line_done:
        pos = next;
    line_done_keep: /* the specialist's stop line: reprocess it in place */
        ;
    }
    if (!root_open) {
        return 1; /* empty document: the engine owns it */
    }
    while (depth > 0) {
        if (st[depth].pending) {
            return 1; /* empty "key:" at EOF: the engine */
        }
        if (st[depth].phantom) {
            yt_f_close(F, st[depth].kind);
            st[depth].phantom = 0;
        }
        if (st[depth].own_map) {
            yt_f_close(F, st[depth].kind);
        }
        depth--;
    }
    if (root_evented) {
        yt_f_close(F, 0); /* only an event-started root closes */
    }
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 4u);
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 2u);
    t->docs = 1;
    return 0;
}

/* The flow-rooted fast path (item 86): a document whose entire content
 * is ONE flow collection plus whitespace skips both the block runner
 * and the engine — the budgeted classification runs once and the
 * engine's exact five-word scaffolding lands directly. Any marker,
 * comment, anchor-bearing prefix, or second content falls back. */
int ytap_flow_rooted(yep_ytape* t) {
    const char* p = t->input;
    size_t len = t->input_len;
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) {
        i++;
    }
    if (i >= len || (p[i] != '[' && p[i] != '{')) {
        return 1; /* not flow-rooted: the block runner's turn */
    }
    size_t mark = t->count;
    size_t open = i;
    yep_view none = {NULL, 0};
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 1u);
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 3u);
    size_t close = 0;
    int v = yt_flow_classify_record(t, p, open, len - open, &none, &none, 0,
                                    1000 /* YEP_MAX_DEPTH: the engine's
                                          * default limit (engine.c) */
                                    ,
                                    &close);
    if (v != 1) {
        t->count = (uint32_t)mark; /* unwind: the engine route owns it */
        t->depth = 0;
        return 1;
    }
    for (size_t e = close + 1; e < len; e++) {
        if (p[e] != ' ' && p[e] != '\t' && p[e] != '\n' && p[e] != '\r') {
            t->count = (uint32_t)mark; /* trailing content: the engine */
            t->depth = 0;
            return 1;
        }
    }
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 4u);
    (void)yt_put(t, ((uint64_t)YTP_EV_COMPACT << 56) | 2u);
    t->docs = 1;
    return 0;
}

int ytap_fused_run(yep_ytape* t, const yep_resolver* resolver) {
    yt_fused* F = (yt_fused*)yep_alloc(yep_system_allocator(), sizeof(*F));
    if (F == NULL) {
        return 1; /* allocation refused: the engine route */
    }
    memset(F, 0, sizeof(*F));
    F->t = t;
    F->p = t->input;
    F->len = t->input_len;
    F->resolver = resolver;
    size_t mark = t->count;
    int rc = yt_f_run(F);
    yep_pool* rpool = F->pool; /* capture before the runner frees */
    yep_free(yep_system_allocator(), F->anchors);
    yep_free(yep_system_allocator(), F->anchors_opened);
    yep_free(yep_system_allocator(), F);
    if (rc != 0) {
        t->count = (uint32_t)mark; /* unwind: the caller resets and re-runs */
        t->depth = 0;
        if (rpool != NULL) {
            yep_pool_destroy(rpool);
        }
        return 1;
    }
    if (rpool != NULL) {
        t->pool = rpool; /* transferred to the document with the tape */
    }
    return 0;
}

void ytap_reset(yep_ytape* t) {
    t->count = 0;
    t->depth = 0;
    t->docs = 0;
    t->oom = 0;
    t->flow_mark = 0;
}
