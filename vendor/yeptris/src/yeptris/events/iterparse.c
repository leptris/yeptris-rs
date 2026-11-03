/* iterparse.c — document-at-a-time iteration (TODO.impl/12).
 *
 * Multi-document streams: each next() runs the engine from the current
 * cursor and captures ONE document's events (records and arena reset
 * per document — memory bounded by the largest document, not the
 * stream). The sink aborts at DOCUMENT_END; the engine's position
 * accessor gives the resume offset. */

#include <stdlib.h>
#include <string.h>

#include "../../include/yeptris/events.h"
#include "../common/simd_text.h"
#include "../encoding/encoding.h"
#include "../memory/allocator.h"
#include "../parse/engine.h"
#include "capture.h"

typedef struct {
    yep_rec_store* store;
    int doc_ends; /* DOCUMENT_END events seen this run */
    int oom;      /* the capture failed: yep_rec_on_event returned
                   * nonzero — the engine's rc (-2) is shared with the
                   * normal document-abort, so without this bit an OOM
                   * was indistinguishable from doc-complete and the
                   * final event vanished silently (the audit's
                   * finding) */
} yep_iter_sink;

static int iter_on_event(void* ctx, const yep_event* ev) {
    yep_iter_sink* s = (yep_iter_sink*)ctx;
    /* stream framing is synthesized by the iterator (the aborted run
     * never reaches STREAM_END); documents are the slice unit */
    if (ev->type == YEP_EV_STREAM_START || ev->type == YEP_EV_STREAM_END) {
        return 0;
    }
    if (ev->type == YEP_EV_DOCUMENT_END) {
        s->doc_ends++;
    }
    int rc = yep_rec_on_event(s->store, ev);
    if (rc != 0) {
        s->oom = 1;
        return rc;
    }
    if (s->doc_ends >= 1) {
        return 1; /* abort: the document is complete */
    }
    return 0;
}

struct yeptris_iterparse {
    const char* buf;
    size_t len;
    size_t cursor;
    int done;
    int gated;            /* the encoding front-end ran once: after it, buf
                           * indexes the space every engine run's pos lives
                           * in (the resume arithmetic requires it) */
    unsigned char* owned; /* UTF-16/32 transcode output (buf moved) */
    char* norm;           /* NEL/LS/PS-normalized copy (buf moved) */
    int emitted_stream_start;
    int pending_stream_end; /* deliver a final lone STREAM_END slice */
    YeptrisStatus status;
    uint32_t err_line;
    uint32_t err_col;
    yep_rec_store store;
    size_t next;
    YeptrisEvent* mat;
    size_t mat_cap;
    YeptrisEvent final_end;
    YeptrisEvent final_pair[2];
};

YEPTRIS_API YeptrisIterparse yeptris_iterparse_new(const char* buf, size_t len) {
    if (buf == NULL && len != 0) {
        return NULL;
    }
    struct yeptris_iterparse* it = calloc(1, sizeof(*it));
    if (it == NULL) {
        return NULL;
    }
    it->buf = buf;
    it->len = len;
    yep_rec_init(&it->store);
    return it;
}

YEPTRIS_API const YeptrisEvent* yeptris_iterparse_next(YeptrisIterparse it, size_t* count) {
    if (it == NULL) {
        return NULL;
    }
    for (;;) {
        /* the encoding front-end, ONCE (the yep_engine_run law): the
         * resume arithmetic below adds the engine's pos to it->cursor,
         * and pos lives in the buffer space the RUN parsed — never the
         * raw bytes. The front-end's consumed prefix broke that twice:
         * the BOM it strips (pos cannot see it, so a BOM'd stream
         * re-parsed the previous document's tail), and the UTF-16/32
         * transcode (pos counts UTF-8 bytes, cursor counts raw ones —
         * slices corrupted). The NEL/LS/PS break normalizer inside the
         * run shifts pos the same way, so the gate owns both: encode
         * to UTF-8 and normalize here, move buf, and the spaces agree. */
        if (!it->gated) {
            it->gated = 1;
            const yep_allocator* sys = yep_system_allocator();
            yep_encoding enc = YEP_ENC_UNKNOWN;
            size_t bom = yep_bom_sniff((const unsigned char*)it->buf, it->len, &enc);
            if (enc == YEP_ENC_UTF16LE || enc == YEP_ENC_UTF16BE || enc == YEP_ENC_UTF32LE ||
                enc == YEP_ENC_UTF32BE) {
                size_t tlen = 0;
                size_t terr = 0;
                int rc = yep_transcode_to_utf8(sys, enc, (const unsigned char*)it->buf + bom,
                                               it->len - bom, &it->owned, &tlen, &terr);
                if (rc != 0) {
                    it->done = 1;
                    it->status = YEPTRIS_ERROR_ENCODING;
                    it->err_line = 1;
                    it->err_col = 1;
                    return NULL;
                }
                /* the copy IS the post-gate buffer: it opens at the
                 * first content byte (the mark never entered it), so
                 * cursor restarts at zero — keeping `bom` here skipped
                 * the copy's first unit and corrupted the first slice */
                it->buf = (const char*)it->owned;
                it->len = tlen;
                it->cursor = 0;
            } else {
                it->cursor = bom; /* the mark is the front-end's prefix */
            }
            size_t nlen = 0;
            int kept_raw = 0;
            char* norm = yep_engine_normalize_breaks(sys, it->buf + it->cursor,
                                                     it->len - it->cursor, &nlen, &kept_raw);
            if (norm != NULL) {
                it->norm = norm;
                it->buf = norm;
                it->len = nlen;
                it->cursor = 0; /* the copy starts at its own zero */
            }
        }
        if (it->done && it->pending_stream_end && it->next >= it->store.n) {
            it->pending_stream_end = 0;
            if (!it->emitted_stream_start) {
                /* a stream with zero documents still frames +STR */
                it->emitted_stream_start = 1;
                it->final_pair[0].type = YEPTRIS_EV_STREAM_START;
                it->final_pair[1].type = YEPTRIS_EV_STREAM_END;
                if (count != NULL) {
                    *count = 2;
                }
                return it->final_pair;
            }
            it->final_end.type = YEPTRIS_EV_STREAM_END;
            if (count != NULL) {
                *count = 1;
            }
            return &it->final_end;
        }
        if (it->next < it->store.n) {
            /* materialize the whole captured slice (one document) */
            size_t n = it->store.n;
            if (n + 1 > it->mat_cap) {
                YeptrisEvent* nm = realloc(it->mat, (n + 1) * sizeof(*nm));
                if (nm == NULL) {
                    it->done = 1;
                    it->status = YEPTRIS_ERROR_MEMORY;
                    return NULL;
                }
                it->mat = nm;
                it->mat_cap = n + 1;
            }
            for (size_t i = 0; i < n; i++) {
                yep_rec_materialize(&it->store, i, &it->mat[i]);
            }
            size_t base = 0;
            if (!it->emitted_stream_start) {
                it->emitted_stream_start = 1;
                /* prepend the synthesized STREAM_START */
                memmove(&it->mat[1], it->mat, n * sizeof(*it->mat));
                memset(&it->mat[0], 0, sizeof(it->mat[0]));
                it->mat[0].type = YEPTRIS_EV_STREAM_START;
                base = 1;
            }
            it->next = it->store.n; /* consumed */
            if (count != NULL) {
                *count = n + base;
            }
            return it->mat;
        }
        if (it->done) {
            return NULL;
        }
        /* capture the next document: abort the run at its DOCUMENT_END;
         * the marker line stays unconsumed and doubles as the next
         * document's start on the resumed run */
        yep_rec_reset(&it->store);
        it->next = 0;
        yep_iter_sink s = {&it->store, 0, 0};
        yep_engine* eng = yep_engine_create(yep_system_allocator());
        if (eng == NULL) {
            it->done = 1;
            it->status = YEPTRIS_ERROR_MEMORY;
            return NULL;
        }
        yep_sink sink = {.on_event = iter_on_event,
                         .ctx = &s,
                         .on_flow_build = NULL,
                         .on_flow_commit = NULL,
                         .on_flow_rollback = NULL,
                         .on_block_pair = NULL,
                         .on_block_open = NULL,
                         .on_block_item = NULL};
        yep_text_stats pst;
        yep_text_active()->scan_stats(it->buf + it->cursor, it->len - it->cursor, &pst);
        yep_engine_prepare(eng, &pst);
        int rc = yep_engine_run(eng, it->buf + it->cursor, it->len - it->cursor, &sink);
        size_t pos = yep_engine_pos(eng);
        /* the error slot lives inside the engine and destroy munmaps it
         * (glibc serves the ~260KB engine with mmap): copy the verdict
         * out BEFORE the free */
        yep_err_code err_code = YEP_ERR_NONE;
        uint32_t err_line = 0, err_col = 0;
        {
            const yep_error* err = yep_engine_error(eng);
            if (err != NULL) {
                err_code = err->code;
                err_line = err->line;
                err_col = err->col;
            }
        }
        yep_engine_destroy(eng);
        /* A failed run with a SET channel is a parse-class error and the
         * CODE picks the public status; a failed run with a CLEAN
         * channel is the capture sink dying — memory, never a parse
         * error, and never a silent stream end (the old code fell into
         * the doc-complete arm on rc == -2 and the final event
         * vanished). The abort (rc == -2 without s.oom) stays the
         * normal per-document completion. */
        int failed = rc == -1 || s.oom;
        if (failed) {
            it->done = 1;
            if (err_code != YEP_ERR_NONE) {
                it->err_line = err_line;
                it->err_col = err_col;
                it->status = err_code == YEP_ERR_MEMORY     ? YEPTRIS_ERROR_MEMORY
                             : err_code == YEP_ERR_ENCODING ? YEPTRIS_ERROR_ENCODING
                                                            : YEPTRIS_ERROR_PARSE;
            } else {
                it->status = YEPTRIS_ERROR_MEMORY;
            }
            if (it->store.n == 0) {
                if (!it->emitted_stream_start) {
                    /* the engine emitted +STR before failing: match it */
                    it->emitted_stream_start = 1;
                    it->final_end.type = YEPTRIS_EV_STREAM_START;
                    if (count != NULL) {
                        *count = 1;
                    }
                    return &it->final_end;
                }
                return NULL;
            }
            continue; /* surface the partial slice, then stop */
        }
        it->cursor += pos;
        if (rc == 0 || s.doc_ends == 0) {
            it->done = 1; /* natural end or blank tail: stream is over */
            it->pending_stream_end = 1;
        }
    }
}

YEPTRIS_API YeptrisStatus yeptris_iterparse_status(const YeptrisIterparse it, uint32_t* line,
                                                   uint32_t* col) {
    if (it == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    if (line != NULL) {
        *line = it->err_line;
    }
    if (col != NULL) {
        *col = it->err_col;
    }
    return it->status;
}

YEPTRIS_API void yeptris_iterparse_free(YeptrisIterparse it) {
    if (it == NULL) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, it->owned);
    yep_free(sys, it->norm);
    yep_rec_free(&it->store);
    free(it->mat);
    free(it);
}
