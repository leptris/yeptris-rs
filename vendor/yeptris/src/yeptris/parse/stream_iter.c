/* stream_iter.c — tolerant per-document stream iteration (#921).
 *
 * The load_stream contract parses the whole stream and returns nothing
 * when any document fails; consumers that must keep the valid
 * documents of a stream cannot use the engine for recovery. This
 * iterator frames documents ENGINE-SIDE (the abort-at-DOCUMENT_END
 * run iterparse uses — scanner-accurate: a quoted scalar spanning a
 * line that looks like a marker never splits) and reports failures
 * per document: on a parse-class error the iterator records that
 * document's verdict (the engine's line/col, which ARE the failed
 * document's own — each run starts at the frame) and resyncs to the
 * next line-start marker, so the host keeps the valid documents.
 *
 * The resync after a FAILURE is a raw marker scan: the failed
 * document's tail is ill-formed by definition, so the scanner-grade
 * framing above no longer has a well-defined subject; the raw scan is
 * the libyaml-familiar best effort and only ever runs on the recovery
 * path (valid documents never hit it).
 *
 * Cost note: each document parses twice (the boundary run + the slice
 * parse). The tolerance contract is the point; a single-pass upgrade
 * rides on the same sink seam when a workload demands it. */

#include <stdlib.h>
#include <string.h>

#include "../../include/yeptris/dom.h"
#include "../../include/yeptris/parse.h"
#include "../common/error.h"
#include "../common/simd_text.h"
#include "../encoding/encoding.h"
#include "../memory/allocator.h"
#include "../resolve/resolver.h"
#include "doc.h"
#include "engine.h"

/* The boundary finder: a sink that aborts the run at the first
 * DOCUMENT_END (iterparse's law — the marker line stays unconsumed
 * and doubles as the next document's start). Events are discarded;
 * the run's pos IS the document's end offset. */
typedef struct {
    int doc_ends;
} frame_sink;

static int frame_on_event(void* ctx, const yep_event* ev) {
    frame_sink* s = (frame_sink*)ctx;
    if (ev->type == YEP_EV_DOCUMENT_END) {
        s->doc_ends++;
        return 1; /* abort: the document is complete */
    }
    return 0;
}

struct yeptris_stream_iter {
    const char* buf;
    size_t len;
    size_t cursor;
    int done;
    int gated;                       /* the encoding front-end ran once (iterparse's
                                      * law: after it, cursor and engine pos live in
                                      * the same buffer space) */
    unsigned char* owned;            /* UTF-16/32 transcode output */
    char* norm;                      /* NEL/LS/PS-normalized copy */
    const YeptrisParseOptions* opts; /* borrowed */
    size_t index;                    /* 0-based stream document index (errors consume
                                      * one too — the host's yield order is the stream's) */
    YeptrisStatus last_status;       /* the LAST next()'s per-document verdict */
    uint32_t last_line;
    uint32_t last_col;
};

yeptris_stream_iter* yeptris_stream_iter_new(const char* buf, size_t len,
                                             const YeptrisParseOptions* opts) {
    if (buf == NULL && len != 0) {
        return NULL;
    }
    struct yeptris_stream_iter* it = calloc(1, sizeof(*it));
    if (it == NULL) {
        return NULL;
    }
    it->buf = buf;
    it->len = len;
    it->opts = opts;
    return it;
}

/* The resync: the next line-start marker (--- / ...) followed by
 * space, break, or EOF — the c-forbidden lookahead. Raw bytes: the
 * recovery path's best effort (see the file comment). The scan starts
 * AFTER the failed frame's first line: the frame opened at `from`'s
 * line, and the engine's failure pos can sit at EOF (the run scans to
 * the end before reporting), so anchoring on `from` itself is the only
 * stable reference. */
static size_t next_marker(const char* p, size_t len, size_t from) {
    size_t i = from;
    while (i < len && p[i] != '\n' && p[i] != '\r') {
        i++;
    }
    if (i < len) {
        i++;
    }
    int line_start = 1;
    while (i < len) {
        char ch = p[i];
        if (ch == '\n' || ch == '\r') {
            line_start = 1;
            i++;
            continue;
        }
        if (line_start && ch == '-' && i + 2 < len && p[i + 1] == '-' && p[i + 2] == '-' &&
            (i + 3 == len || p[i + 3] == ' ' || p[i + 3] == '\n' || p[i + 3] == '\r' ||
             p[i + 3] == '\t')) {
            return i;
        }
        if (line_start && ch == '.' && i + 2 < len && p[i + 1] == '.' && p[i + 2] == '.' &&
            (i + 3 == len || p[i + 3] == ' ' || p[i + 3] == '\n' || p[i + 3] == '\r' ||
             p[i + 3] == '\t')) {
            return i;
        }
        line_start = 0;
        i++;
    }
    return len; /* no marker: the tail is unrecoverable */
}

static void it_fail(struct yeptris_stream_iter* it, YeptrisStatus st, uint32_t line, uint32_t col) {
    it->last_status = st;
    it->last_line = line;
    it->last_col = col;
}

static int engine_opts(yep_engine* eng, const YeptrisParseOptions* opts) {
    /* the typing SSOT: one switch (parse.c owns it) */
    YeptrisSchema schema = opts != NULL ? opts->schema : YEPTRIS_SCHEMA_12_CORE;
    yep_engine_set_resolver(eng, resolver_for_schema(schema));
    if (opts != NULL && opts->schema == YEPTRIS_SCHEMA_11_COMPAT) {
        /* the compat grammar is the psych/libyaml surface (parse_impl's
         * law) */
        yep_engine_set_compat_grammar(eng, 1);
    }
    if (opts != NULL && opts->max_depth > 0) {
        yep_engine_set_max_depth(eng, opts->max_depth);
    }
    return 0;
}

YeptrisStatus yeptris_stream_iter_next(struct yeptris_stream_iter* it, YeptrisDocument* out,
                                       size_t* index) {
    if (it == NULL || out == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    if (index != NULL) {
        *index = it->index;
    }
    for (;;) {
        /* the encoding front-end, ONCE (iterparse's gate law: BOM
         * strip, UTF-16/32 transcode, NEL/LS/PS normalization — cursor
         * and engine pos must live in one buffer space or the resume
         * arithmetic corrupts slices) */
        if (!it->gated) {
            it->gated = 1;
            const yep_allocator* sys = yep_system_allocator();
            yep_encoding enc = YEP_ENC_UNKNOWN;
            size_t bom = yep_bom_sniff((const unsigned char*)it->buf, it->len, &enc);
            if (enc == YEP_ENC_UTF16LE || enc == YEP_ENC_UTF16BE || enc == YEP_ENC_UTF32LE ||
                enc == YEP_ENC_UTF32BE) {
                size_t tlen = 0;
                size_t terr = 0;
                unsigned char* owned = NULL;
                int rc = yep_transcode_to_utf8(sys, enc, (const unsigned char*)it->buf + bom,
                                               it->len - bom, &owned, &tlen, &terr);
                if (rc != 0) {
                    it->done = 1;
                    it_fail(it, YEPTRIS_ERROR_ENCODING, 1, 1);
                    return YEPTRIS_ERROR_ENCODING;
                }
                it->owned = owned;
                it->buf = (const char*)owned;
                it->len = tlen;
                it->cursor = 0;
            } else {
                it->cursor = bom;
            }
            size_t nlen = 0;
            int kept_raw = 0;
            char* norm = yep_engine_normalize_breaks(sys, it->buf + it->cursor,
                                                     it->len - it->cursor, &nlen, &kept_raw);
            if (norm != NULL) {
                it->norm = norm;
                it->buf = norm;
                it->len = nlen;
                it->cursor = 0;
            }
        }
        if (it->done) {
            it_fail(it, YEPTRIS_OK, 0, 0);
            return YEPTRIS_OK; /* *out stays NULL: the stream is over */
        }

        /* one boundary run: abort at DOCUMENT_END, learn the extent */
        frame_sink fs = {0};
        yep_engine* eng = yep_engine_create(yep_system_allocator());
        if (eng == NULL) {
            it->done = 1;
            it_fail(it, YEPTRIS_ERROR_MEMORY, 0, 0);
            return YEPTRIS_ERROR_MEMORY;
        }
        engine_opts(eng, it->opts);
        yep_sink sink = {.on_event = frame_on_event,
                         .ctx = &fs,
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

        if (rc == -1 && (err_code == YEP_ERR_MEMORY || err_code == YEP_ERR_ENCODING)) {
            /* the stream-fatal classes: the arena state is unknown
             * (memory) or the front-end's space is broken (encoding —
             * it gated once, so this cannot be a per-document miss) */
            it->done = 1;
            it_fail(it, err_code == YEP_ERR_MEMORY ? YEPTRIS_ERROR_MEMORY : YEPTRIS_ERROR_ENCODING,
                    err_line, err_col);
            return it->last_status;
        }
        if (rc == -1 || (rc != 0 && fs.doc_ends == 0)) {
            /* THIS document failed (parse-class or the run died before
             * any document completed): report the per-document verdict
             * — the run started at the frame, so the engine's line/col
             * ARE the failed document's own — then resync to the next
             * marker and keep going */
            it_fail(it, err_code == YEP_ERR_DEPTH ? YEPTRIS_ERROR_DEPTH : YEPTRIS_ERROR_PARSE,
                    err_line, err_col);
            size_t at = next_marker(it->buf, it->len, it->cursor);
            if (at >= it->len) {
                it->done = 1; /* the tail is unrecoverable */
            } else {
                it->cursor = at;
            }
            it->index++;
            if (index != NULL) {
                *index = it->index - 1;
            }
            return it->last_status; /* *out stays NULL: the failed slot */
        }
        if (fs.doc_ends == 0) {
            /* natural end or a blank tail: the stream is over */
            it->done = 1;
            it_fail(it, YEPTRIS_OK, 0, 0);
            return YEPTRIS_OK;
        }

        /* the frame is [cursor, cursor+pos): parse it as ONE document
         * (the slice's own marker or bare content rides parse_ex's
         * contract; the default tape route is what load uses) */
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse_ex(it->buf + it->cursor, pos, it->opts, &st);
        if (doc == NULL || st != YEPTRIS_OK) {
            /* the boundary run accepted what the slice parse refused:
             * report as a failed document and resync the same way */
            it_fail(it, st == YEPTRIS_OK ? YEPTRIS_ERROR_PARSE : st, 0, 0);
            yeptris_document_free(doc);
            size_t at = next_marker(it->buf, it->len, it->cursor);
            if (at >= it->len) {
                it->done = 1;
            } else {
                it->cursor = at;
            }
            it->index++;
            if (index != NULL) {
                *index = it->index - 1;
            }
            return it->last_status;
        }
        it->cursor += pos;
        it->index++;
        if (index != NULL) {
            *index = it->index - 1;
        }
        it_fail(it, YEPTRIS_OK, 0, 0);
        *out = doc;
        return YEPTRIS_OK;
    }
}

YeptrisStatus yeptris_stream_iter_status(const struct yeptris_stream_iter* it, uint32_t* line,
                                         uint32_t* col) {
    if (it == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    if (line != NULL) {
        *line = it->last_line;
    }
    if (col != NULL) {
        *col = it->last_col;
    }
    return it->last_status;
}

void yeptris_stream_iter_free(struct yeptris_stream_iter* it) {
    if (it == NULL) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, it->owned);
    yep_free(sys, it->norm);
    free(it);
}
