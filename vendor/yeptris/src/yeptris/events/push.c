/* push.c — callback push over the engine sink (TODO.impl/12).
 *
 * The C-user convenience shape: one call per event, strings valid only
 * during the callback. Bindings prefer the recorder's bulk drain. */

#include <string.h>

#include "../../include/yeptris/events.h"
#include "../common/simd_text.h"
#include "../parse/engine.h"
#include "yeptris/error.h"
#include "yeptris/parse.h"

typedef struct {
    yeptris_event_fn fn;
    void* ctx;
    uint32_t abort_line; /* the mark of the event the callback aborted
                          * on: the abort-text law sets the channel AT
                          * the abort point, not at 0/0 */
    uint32_t abort_col;
} yep_push_ctx;

static int push_on_event(void* ctx, const yep_event* ev) {
    yep_push_ctx* p = (yep_push_ctx*)ctx;
    YeptrisEvent e;
    memset(&e, 0, sizeof(e));
    switch (ev->type) {
    case YEP_EV_STREAM_START:
        e.type = YEPTRIS_EV_STREAM_START;
        break;
    case YEP_EV_STREAM_END:
        e.type = YEPTRIS_EV_STREAM_END;
        break;
    case YEP_EV_DOCUMENT_START:
        e.type = YEPTRIS_EV_DOCUMENT_START;
        e.explicit_marker = (ev->style == 1);
        break;
    case YEP_EV_DOCUMENT_END:
        e.type = YEPTRIS_EV_DOCUMENT_END;
        e.explicit_marker = (ev->style == 1);
        break;
    case YEP_EV_SEQ_START:
        e.type = YEPTRIS_EV_SEQUENCE_START;
        break;
    case YEP_EV_SEQ_END:
        e.type = YEPTRIS_EV_SEQUENCE_END;
        break;
    case YEP_EV_MAP_START:
        e.type = YEPTRIS_EV_MAPPING_START;
        break;
    case YEP_EV_MAP_END:
        e.type = YEPTRIS_EV_MAPPING_END;
        break;
    case YEP_EV_SCALAR:
        e.type = YEPTRIS_EV_SCALAR;
        e.style = ev->style;
        e.implicit = ev->implicit;
        break;
    case YEP_EV_ALIAS:
        e.type = YEPTRIS_EV_ALIAS;
        break;
    default:
        return 0;
    }
    e.flow = ev->flow;
    e.line = ev->line;
    e.col = ev->col;
    /* the cross-model identity law (#179, test_event_marks): push,
     * pull and the recorder emit identical mark data — dropping the
     * end marks here made the push route's events half-marked where
     * pull/recorder carried all four fields */
    e.end_line = ev->end_line;
    e.end_col = ev->end_col;
    e.value = ev->value.len > 0 ? (const char*)ev->value.p : NULL;
    e.value_len = ev->value.len;
    e.anchor = ev->anchor.len > 0 ? (const char*)ev->anchor.p : NULL;
    e.anchor_len = ev->anchor.len;
    e.tag = ev->tag.len > 0 ? (const char*)ev->tag.p : NULL;
    e.tag_len = ev->tag.len;
    int rc = p->fn(p->ctx, &e);
    if (rc != 0) {
        /* the abort point travels with the abort: the TLS slot's mark
         * is where the walk stopped, not 0/0 (events.h's contract) */
        p->abort_line = ev->line;
        p->abort_col = ev->col;
    }
    return rc;
}

YEPTRIS_API YeptrisStatus yeptris_push_parse(const char* buf, size_t len, yeptris_event_fn on_event,
                                             void* ctx) {
    if (buf == NULL && len != 0) {
        return YEPTRIS_ERROR_ARG;
    }
    if (on_event == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    /* the push run owns the channel for its duration: clearing at
     * entry makes the abort-text law verifiable (the message a caller
     * reads after a push is THIS run's verdict, never a stale one
     * from an earlier operation on the thread — error.h promises ""
     * after a success) */
    yep_error_clear(yep_error_tls());
    yep_engine* eng = yep_engine_create(yep_system_allocator());
    if (eng == NULL) {
        return YEPTRIS_ERROR_MEMORY;
    }
    yep_push_ctx p = {on_event, ctx, 0, 0};
    yep_sink sink = {.on_event = push_on_event,
                     .ctx = &p,
                     .on_flow_build = NULL,
                     .on_flow_commit = NULL,
                     .on_flow_rollback = NULL,
                     .on_block_pair = NULL,
                     .on_block_open = NULL,
                     .on_block_item = NULL};
    yep_text_stats pst;
    yep_text_active()->scan_stats(buf, len, &pst);
    yep_engine_prepare(eng, &pst);
    int rc = yep_engine_run(eng, buf, len, &sink);
    /* the error slot lives inside the engine and destroy munmaps it
     * (glibc serves the ~260KB engine with mmap): copy the verdict out
     * BEFORE the free — reading err->code after destroy segfaulted the
     * round-5 CI */
    yep_err_code err_code = YEP_ERR_NONE;
    {
        const yep_error* err = yep_engine_error(eng);
        if (err != NULL) {
            err_code = err->code;
        }
    }
    yep_engine_destroy(eng);
    if (rc == 0) {
        return YEPTRIS_OK;
    }
    /* the engine's own memory failures report MEMORY (the parse route's
     * law); the callback abort keeps its documented contract — a parse
     * stop with the last-error channel SET (#904 item 4, the
     * abort-text law: the old code returned PARSE over a CLEAN slot,
     * so yeptris_last_error served whatever stale message this thread's
     * previous operation had left there). YEP_ERR_ABORTED carries the
     * mark of the event the callback aborted on. */
    if (err_code == YEP_ERR_MEMORY) {
        return YEPTRIS_ERROR_MEMORY;
    }
    if (err_code == YEP_ERR_NONE) {
        yep_error_set(yep_error_tls(), YEP_ERR_ABORTED, p.abort_line, p.abort_col, 0,
                      "parse aborted by the event callback at %u:%u", p.abort_line, p.abort_col);
    }
    return YEPTRIS_ERROR_PARSE;
}
