/* parse.h — the parse entry points (TODO.impl/07/11 public surface). */

#ifndef YEPTRIS_PARSE_H
#define YEPTRIS_PARSE_H

#include <stddef.h>

#include <yeptris/api.h>
#include <yeptris/error.h>
#include <yeptris/resolve.h>
#include <yeptris/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parses a YAML document set. buf must remain valid for the lifetime of
 * the returned document (scalar values are borrowed zero-copy views).
 * On failure returns NULL; *status (may be NULL) receives the code and
 * yeptris_last_error() carries the message with line/column.
 *
 * Memory: the caller owns the document; free it with
 * yeptris_document_free — one call releases everything. */
YEPTRIS_API YeptrisDocument yeptris_parse(const char* buf, size_t len, YeptrisStatus* status);

/* Per-parse options (TODO.impl/10): schema selects the implicit-typing
 * resolver (default YEPTRIS_SCHEMA_12_CORE), max_depth caps nesting
 * (0: the library default), strict/tab_policy/recover are reserved
 * pins of the option shape (the strict grammar is the default today;
 * lenient modes land with the recover work). Zero-initialized = the
 * yeptris_parse defaults. */
typedef struct YeptrisParseOptions {
    YeptrisSchema schema;
    int max_depth;
    int strict;
    int tab_policy;
    int recover;
} YeptrisParseOptions;

YEPTRIS_API YeptrisDocument yeptris_parse_ex(const char* buf, size_t len,
                                             const YeptrisParseOptions* opts,
                                             YeptrisStatus* status);

/* ---- tolerant per-document stream iteration (#921) ----------------------
 *
 * load_stream parses the whole stream and aborts on the first bad
 * document; this iterator frames documents engine-side (the abort-at-
 * DOCUMENT_END run — scanner-accurate: a quoted scalar spanning a
 * marker-looking line never splits) and reports failures PER DOCUMENT:
 * a parse-class error surfaces as the step's status with NULL *out, the
 * error channel carrying the failed document's own line/col (each run
 * starts at the frame), and the iterator resynced to the next line-
 * start marker — the host keeps calling and keeps the valid documents.
 *
 *   yeptris_stream_iter* it = yeptris_stream_iter_new(buf, len, &opts);
 *   YeptrisDocument doc;
 *   size_t index;
 *   YeptrisStatus st;
 *   while ((st = yeptris_stream_iter_next(it, &doc, &index)) == YEPTRIS_OK && doc != NULL) {
 *       // doc is document `index` of the stream; free it with
 *       // yeptris_document_free
 *   }
 *   // st == OK && doc == NULL: the stream is over; st != OK: document
 *   // `index` failed (per-document line/col via
 *   // yeptris_stream_iter_status), keep iterating.
 *
 * Memory: the caller owns each *out (yeptris_document_free). The
 * iterator borrows `buf` until free. Each document parses twice (the
 * boundary run + the slice parse) — the tolerance contract is the
 * point; a single-pass upgrade rides the same seam when needed.
 */
typedef struct yeptris_stream_iter yeptris_stream_iter;

/* The iterator borrows `buf` until free; `opts` is borrowed (may be
 * NULL: the 12_CORE defaults). NULL on arg/alloc failure. */
YEPTRIS_API yeptris_stream_iter* yeptris_stream_iter_new(const char* buf, size_t len,
                                                         const YeptrisParseOptions* opts);

/* The next document: OK + *out = the document (the caller frees it);
 * a parse/DEPTH-class DOCUMENT failure: that status + *out = NULL +
 * the error channel set for THAT document — the iterator has already
 * resynced, keep calling; OK + *out = NULL: the stream is over;
 * MEMORY/ENCODING: stream-fatal, the iterator is done. *index (may be
 * NULL) is the 0-based stream document index this step consumed
 * (failures consume an index too — the yield order is the stream's). */
YEPTRIS_API YeptrisStatus yeptris_stream_iter_next(yeptris_stream_iter* it, YeptrisDocument* out,
                                                   size_t* index);

/* The LAST step's per-document verdict and its 1-based line/col
 * (0/0 when the last step was a success or the stream end). */
YEPTRIS_API YeptrisStatus yeptris_stream_iter_status(const yeptris_stream_iter* it, uint32_t* line,
                                                     uint32_t* col);

/* The last successful step's document SLICE inside the iterator's
 * buffer space (post-gate: BOM/transcode/normalization may have moved
 * the buffer — the offsets are relative to the CURRENT buf the
 * iterator holds, which is the space `yeptris_stream_iter_next`'s
 * documents live in). Hosts that materialize through their own value
 * route (the Python/Rust/TS drains) re-parse exactly these bytes and
 * inherit the engine's framing without a DOM walk: one typing SSOT —
 * the drain — and scanner-accurate frames. ARG on NULL; ARG when the
 * last step was not a successful document. */
YEPTRIS_API YeptrisStatus yeptris_stream_iter_slice(const yeptris_stream_iter* it, size_t* off,
                                                    size_t* len);

/* The iterator's ACTIVE buffer (the caller's bytes, or the post-gate
 * owned copy after a BOM strip / UTF-16-32 transcode / NEL-LS-PS
 * normalization) and its length. `yeptris_stream_iter_slice`'s offsets
 * index THIS buffer — hosts copy their frame bytes out of here
 * (ctypes.string_at / Rust slice / TS Buffer) and re-parse them through
 * their own drain. Borrowed: valid until yeptris_stream_iter_free. */
YEPTRIS_API YeptrisStatus yeptris_stream_iter_buffer(const yeptris_stream_iter* it,
                                                     const char** buf, size_t* len);

YEPTRIS_API void yeptris_stream_iter_free(yeptris_stream_iter* it);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_PARSE_H */
