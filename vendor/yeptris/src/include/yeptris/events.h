/* events.h — the four consumption models over one engine (TODO.impl/12).
 *
 * push:     one callback per event (C users; ~1 µs/event through FFI —
 *           bindings prefer the recorder's bulk drain).
 * pull:     StAX-style cursor; zero C->host dispatch; event strings are
 *           owned by the puller and valid UNTIL THE NEXT CALL.
 * recorder: fixed-size records + one string arena; bulk drain, O(chunks)
 *           host crossings. v1 buffers input until the final chunk; the
 *           streaming feed (mid-document resumption) lands with
 *           parse/feed.c and keeps this shape.
 * iterparse: document-at-a-time over multi-document streams; one
 *           document's events per call, memory bounded by the largest
 *           document. Events valid UNTIL THE NEXT CALL.
 *
 * All four are sinks over the single parse engine: identical event
 * streams, proven by the cross-model corpus check (test_events_cross).
 */
#ifndef YEPTRIS_EVENTS_H
#define YEPTRIS_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/error.h>   /* YeptrisStatus */
#include <yeptris/resolve.h> /* YeptrisSchema */
#include <yeptris/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event types (ABI-pinned order; mirrors the internal engine 1:1). */
typedef enum {
    YEPTRIS_EV_STREAM_START = 1,
    YEPTRIS_EV_STREAM_END,
    YEPTRIS_EV_DOCUMENT_START,
    YEPTRIS_EV_DOCUMENT_END,
    YEPTRIS_EV_SEQUENCE_START,
    YEPTRIS_EV_SEQUENCE_END,
    YEPTRIS_EV_MAPPING_START,
    YEPTRIS_EV_MAPPING_END,
    YEPTRIS_EV_SCALAR,
    YEPTRIS_EV_ALIAS,
} YeptrisEventType;

/* Flag bits in YeptrisEventRecord.flags. */
enum {
    YEPTRIS_EF_FLOW = 1u << 0,     /* collection in flow style */
    YEPTRIS_EF_EXPLICIT = 1u << 1, /* document with --- / ... marker */
    YEPTRIS_EF_IMPLICIT = 1u << 2, /* plain scalar, no tag */
};

/* One event, string fields pointing into the owning model's storage. */
typedef struct {
    YeptrisEventType type;
    int style;           /* YeptrisScalarStyle (dom.h) for scalars */
    int flow;            /* collection: flow style */
    int explicit_marker; /* document: --- / ... seen */
    int implicit;        /* scalar: plain and untagged */
    const char* value;   /* scalar content or alias name; NULL if empty */
    size_t value_len;
    const char* anchor; /* node property; NULL when absent */
    size_t anchor_len;
    const char* tag; /* resolved tag; NULL when absent */
    size_t tag_len;
    uint32_t line; /* 1-based node start */
    uint32_t col;
    uint32_t end_line; /* #179: libyaml end_mark semantics — 1-based line,
                        * col one past the node's end token */
    uint32_t end_col;
} YeptrisEvent;

/* ---- push ------------------------------------------------------------- */

/* Returns nonzero from the callback to abort the parse (surfaced as
 * YEPTRIS_ERROR_PARSE with the last-error channel set). Event strings
 * are valid only during the call. */
typedef int (*yeptris_event_fn)(void* ctx, const YeptrisEvent* ev);

YEPTRIS_API YeptrisStatus yeptris_push_parse(const char* buf, size_t len, yeptris_event_fn on_event,
                                             void* ctx);

/* ---- pull ------------------------------------------------------------- */

YEPTRIS_API YeptrisPullParser yeptris_pull_new(const char* buf, size_t len);

/* Next event (owned by the puller, valid until the next call), or
 * NULL at stream end or on error — distinguish with yeptris_pull_status. */
YEPTRIS_API const YeptrisEvent* yeptris_pull_next(YeptrisPullParser pull);

/* Bulk cursor (the FFI shape): up to max events per call, strings in
 * the puller's arena valid until the next pull call. Returns count. */
YEPTRIS_API size_t yeptris_pull_next_batch(YeptrisPullParser pull, YeptrisEvent* out, size_t max);

YEPTRIS_API YeptrisStatus yeptris_pull_status(const YeptrisPullParser pull, uint32_t* line,
                                              uint32_t* col);
YEPTRIS_API void yeptris_pull_free(YeptrisPullParser pull);

/* ---- recorder --------------------------------------------------------- */

/* Fixed-size record; strings slice the arena by offset+length. */
typedef struct {
    uint8_t type; /* YeptrisEventType */
    uint8_t style;
    uint8_t flags;  /* YEPTRIS_EF_* */
    uint8_t tag_id; /* resolved implicit tag (YeptrisTagId, resolve.h) —
                     * the typing SSOT, so hosts never re-scan (the
                     * struct is 44B, ABI-pinned; the bindings mirror it) */
    uint32_t line;
    uint32_t col;
    uint32_t end_line; /* #179: libyaml end_mark semantics (see the
                        * internal event) — 1-based, col past the end */
    uint32_t end_col;
    uint32_t value_off;
    uint32_t value_len;
    uint32_t anchor_off;
    uint32_t anchor_len;
    uint32_t tag_off;
    uint32_t tag_len;
} YeptrisEventRecord; /* 44B — the bindings mirror this layout in the
                         same lockstep window (the 0.6.9 law) */

YEPTRIS_API YeptrisRecorder yeptris_recorder_new(void);

/* Same, with the implicit-typing schema for the parse (bindings pass
 * compat_11 to reproduce Psych's typing). */
YEPTRIS_API YeptrisRecorder yeptris_recorder_new_ex(YeptrisSchema schema);

/* Accumulates input; the parse advances whenever a complete document
 * has arrived (its closing --- / ... marker line), and final != 0
 * flushes the trailing document. Each feed's records — fixed-size
 * array plus the arena — hold the documents closed by that chunk and
 * are valid until the next feed: drain per chunk. Returns
 * YEPTRIS_OK, YEPTRIS_ERROR_PARSE (terminal; drain what this feed
 * recorded) or YEPTRIS_ERROR_ARG. */
YEPTRIS_API YeptrisStatus yeptris_recorder_feed(YeptrisRecorder rec, const char* chunk, size_t len,
                                                int final);

/* Bulk accessors: the record array and the string arena. Valid until
 * the next feed; reset at every feed entry (drain per chunk). */
YEPTRIS_API const YeptrisEventRecord* yeptris_recorder_records(YeptrisRecorder rec, size_t* count);
YEPTRIS_API const char* yeptris_recorder_arena(YeptrisRecorder rec, size_t* len);

/* Status of the last feed with its 1-based error position (0 = the
 * failure carries no position, e.g. a capture allocation failure).
 * Mirror of yeptris_pull_status / yeptris_iterparse_status. */
YEPTRIS_API YeptrisStatus yeptris_recorder_status(const YeptrisRecorder rec, uint32_t* line,
                                                  uint32_t* col);

YEPTRIS_API void yeptris_recorder_free(YeptrisRecorder rec);

/* ---- iterparse -------------------------------------------------------- */

YEPTRIS_API YeptrisIterparse yeptris_iterparse_new(const char* buf, size_t len);

/* The next document's events (owned by the iterator, valid until the
 * next call): *count events, or NULL when the stream is exhausted.
 * STREAM_START / STREAM_END frame the stream exactly once each; the
 * DOCUMENT_* pair frames each yielded slice. */
YEPTRIS_API const YeptrisEvent* yeptris_iterparse_next(YeptrisIterparse it, size_t* count);

YEPTRIS_API YeptrisStatus yeptris_iterparse_status(const YeptrisIterparse it, uint32_t* line,
                                                   uint32_t* col);
YEPTRIS_API void yeptris_iterparse_free(YeptrisIterparse it);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_EVENTS_H */
