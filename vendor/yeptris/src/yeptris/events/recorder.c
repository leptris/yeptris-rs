/* recorder.c — chunked event recorder (TODO.impl/12).
 *
 * Feed chunks, drain bulk: fixed-size records plus one string arena,
 * reset at every feed entry. The engine's resumable stepping (07)
 * advances the parse as complete documents arrive — each feed's
 * records are the events for documents closed by that chunk, so a
 * stream watcher holds one document of memory, not the whole stream. */

#include <stdlib.h>
#include <string.h>

#include "../../include/yeptris/events.h"
#include "../parse/engine.h"
#include "../resolve/resolver.h"
#include "capture.h"

struct yeptris_recorder {
    yep_rec_store store;
    yep_engine* eng; /* created on first feed; steps per feed */
    int final_fed;
    int compat11;
    YeptrisStatus status;
    uint32_t err_line;
    uint32_t err_col;
};

YEPTRIS_API YeptrisRecorder yeptris_recorder_new(void) {
    return yeptris_recorder_new_ex(YEPTRIS_SCHEMA_12_CORE);
}

YEPTRIS_API YeptrisRecorder yeptris_recorder_new_ex(YeptrisSchema schema) {
    struct yeptris_recorder* r = calloc(1, sizeof(*r));
    if (r != NULL) {
        yep_rec_init(&r->store);
        r->compat11 = schema == YEPTRIS_SCHEMA_11_COMPAT;
    }
    return r;
}

/* The engine's error code decides the public status — the parse
 * route's law (parse.c maps YEP_ERR_* to the pinned codes). A clean
 * channel means the SINK failed: the capture ran out of memory, the
 * bytes never were a parse error. The MEMORY arm: the engine's own
 * allocation failures (e_fail(YEP_ERR_MEMORY)) used to read as a parse
 * error here — yep_engine_error always returns the slot, so the old
 * `err != NULL` discriminator could never fire (the audit's finding). */
static YeptrisStatus rec_status_from_err(const yep_error* err) {
    if (err == NULL || err->code == YEP_ERR_NONE || err->code == YEP_ERR_MEMORY) {
        return YEPTRIS_ERROR_MEMORY;
    }
    if (err->code == YEP_ERR_ENCODING) {
        return YEPTRIS_ERROR_ENCODING;
    }
    return YEPTRIS_ERROR_PARSE;
}

YEPTRIS_API YeptrisStatus yeptris_recorder_feed(YeptrisRecorder rec, const char* chunk, size_t len,
                                                int final) {
    if (rec == NULL || (chunk == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    if (rec->final_fed) {
        return YEPTRIS_ERROR_ARG; /* one stream per recorder */
    }
    if (rec->eng == NULL) {
        rec->eng = yep_engine_create(yep_system_allocator());
        if (rec->eng == NULL) {
            return YEPTRIS_ERROR_MEMORY;
        }
        yep_engine_set_resolver(rec->eng,
                                rec->compat11 ? yep_resolver_compat11() : yep_resolver_core12());
        /* the compat schema is the psych/libyaml surface: its grammar
         * must accept what libyaml accepts (the flow indent floor is
         * suite-strict, not libyaml — see yep_engine_set_compat_grammar;
         * the recorder is the py safe_load path, which rode the floor
         * and rejected multi-line flows #431 fixed everywhere else) */
        yep_engine_set_compat_grammar(rec->eng, rec->compat11);
    }
    if (final) {
        rec->final_fed = 1;
    }
    yep_rec_reset(&rec->store);
    yep_sink sink = {.on_event = yep_rec_on_event,
                     .ctx = &rec->store,
                     .on_flow_build = NULL,
                     .on_flow_commit = NULL,
                     .on_block_item = NULL};
    int rc = yep_engine_step(rec->eng, chunk, len, final, &sink);
    if (rc != 0) {
        const yep_error* err = yep_engine_error(rec->eng);
        if (err != NULL && err->code != YEP_ERR_NONE) {
            rec->err_line = err->line;
            rec->err_col = err->col;
        }
        rec->status = rec_status_from_err(err);
        rec->final_fed = 1; /* a failed feed kills the stream */
        return rec->status;
    }
    rec->status = YEPTRIS_OK;
    return rec->status;
}

YEPTRIS_API const YeptrisEventRecord* yeptris_recorder_records(YeptrisRecorder rec, size_t* count) {
    if (rec == NULL) {
        return NULL;
    }
    if (count != NULL) {
        *count = rec->store.n;
    }
    return rec->store.recs;
}

YEPTRIS_API const char* yeptris_recorder_arena(YeptrisRecorder rec, size_t* len) {
    if (rec == NULL) {
        return NULL;
    }
    if (len != NULL) {
        *len = rec->store.arena_len;
    }
    return rec->store.arena;
}

YEPTRIS_API YeptrisStatus yeptris_recorder_status(const YeptrisRecorder rec, uint32_t* line,
                                                  uint32_t* col) {
    if (rec == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    if (line != NULL) {
        *line = rec->err_line;
    }
    if (col != NULL) {
        *col = rec->err_col;
    }
    return rec->status;
}

YEPTRIS_API void yeptris_recorder_free(YeptrisRecorder rec) {
    if (rec == NULL) {
        return;
    }
    yep_engine_destroy(rec->eng);
    yep_rec_free(&rec->store);
    free(rec);
}
