/* ytape.h — the packed YAML record tape (#378, TODO.max-perf/12).
 *
 * The eager DOM builders' callback stream packed into 8-byte words:
 * the common block ops carry spans only (the resolver call and the
 * 48-byte node build defer to replay), the general event path packs
 * into a props word plus span words. Replay calls the SAME builder
 * functions with reconstructed views — resolution and placement
 * semantics are the eager path's by construction (the differential
 * gate pins tree identity across the corpora).
 *
 * Spans are input-relative when they fall inside [input, input+len]
 * and fit 24 bits; anything else (pool content, oversized spans) is a
 * generic (ptr, len) pair — the engine's finish pool owns that
 * memory, its blocks are stable until the document frees it, and the
 * lazy route keeps the pool on the document until replay copies (the
 * builders copy exactly what must outlive the engine — dom_str_in).
 *
 * The recorder mirrors the eager builders' return-code discipline
 * (depth caps answer 0 so the engine's event path owns the error;
 * OOM answers <0) so error precedence survives the detour. */
#ifndef YEP_YTAPE_H
#define YEP_YTAPE_H

#include <stddef.h>

#include "parse/events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yep_pool yep_pool;
struct yep_resolver;

typedef struct yep_ytape {
    uint64_t* w;
    uint32_t count;
    uint32_t cap;
    const char* input; /* the POST-BOM data pointer (span base) */
    size_t input_len;
    int depth;       /* the DOM's depth mirror (cap parity) */
    int max_depth;   /* the engine's flow-walk limit, replayed */
    int flow_strict; /* mirrors dom->flow_strict (0 everywhere today) */
    int docs;        /* DOCUMENT_START count (the empty-stream check) */
    int oom;
    uint32_t flow_mark;    /* record mark for the staged flow build */
    struct yep_pool* pool; /* fused block-scalar content (owned by the doc
                            * after a successful run; NULL on the engine route) */
    void* flow_scratch;    /* the flow classifier's reused json-tape scratch */
} yep_ytape;

/* Record kinds — byte 7 of a word. */
enum {
    YTP_SPAN_IN = 0,      /* [off:32][len:24][0x00] */
    YTP_EV_COMPACT = 1,   /* stream/document frames: [type:8][...][kind] */
    YTP_EVENT = 2,        /* props + value + [tag] + [anchor] */
    YTP_SCALAR = 3,       /* props + value + [tag] + [anchor]; is_item bit
                           * routes the replay to the dash-item builder */
    YTP_ITEM = 4,         /* borrowed dash item: [off:32][len:24][kind] */
    YTP_PAIR = 5,         /* props + key + value + [anchor] + [anchor hi] */
    YTP_OPEN = 6,         /* `key:`'s fresh map: [off:32][len:24][kind] */
    YTP_FLOW = 7,         /* props + [open:32|len:32] + [tag] + [anchor] */
    YTP_SPAN_POOL = 0xFF, /* [len:32][kind] + [ptr:64] */
};

/* Recorder lifecycle. Returns 0, or -1 when the word carve fails
 * (caller answers YEPTRIS_ERROR_MEMORY — bounded-parse law). */
int ytap_init(yep_ytape* t, const char* input, size_t len, int max_depth);

/* The fused block runner (TODO.restructure/79): ONE sweep per line
 * emitting the same records the engine's sink would. Returns 0 when
 * the document was fully owned; 1 on any bail (the caller resets the
 * tape and runs the engine route). */
int ytap_fused_run(yep_ytape* t, const struct yep_resolver* resolver);

/* The flow-rooted fast path: a document that is exactly one flow
 * collection plus whitespace takes the classification directly —
 * the block runner and the engine never run. Returns 0 when owned;
 * 1 on any fallback. */
int ytap_flow_rooted(yep_ytape* t);

/* Rewind a bailed tape for the engine re-run. */
void ytap_reset(yep_ytape* t);
void ytap_sink(yep_ytape* t, yep_sink* sink);
void ytap_free(yep_ytape* t);

/* Replay: builds the node tree through the eager builders. Returns 0,
 * or -1 (a builder refused — allocation/depth; the lazy route treats
 * it as the JSON tape route does). Sets d->input_base/len. */
int dom_from_ytape(yep_dom* d, const yep_ytape* t);

#ifdef __cplusplus
}
#endif

#endif /* YEP_YTAPE_H */
