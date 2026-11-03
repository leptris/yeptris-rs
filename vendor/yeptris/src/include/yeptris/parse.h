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

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_PARSE_H */
