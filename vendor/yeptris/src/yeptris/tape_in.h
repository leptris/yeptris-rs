/* tape_in.h — the tape module's internal seams (not public ABI).
 *
 * parse.c's #342 slice-2 lazy route drives the fused lenient walk
 * directly: the gate and opener checks it already ran must not be
 * repeated (the lenient public entry re-gates). The walk's contract
 * is the public entry's: gate-clean buffer, p[open] is '[' or '{',
 * tail must be whitespace-only. */
#ifndef YEP_TAPE_IN_H
#define YEP_TAPE_IN_H

#include <stddef.h>

#include <yeptris/parse.h>
#include <yeptris/tape.h>

/* tape.c (was tape_walk_lnt_fused) */
int yep_tape_long_simple_key(const yeptris_json_tape* t);

YeptrisStatus yep_tape_walk_lenient_fused(const char* p, size_t len, size_t open,
                                          yeptris_json_tape* t, int strict_nums, int clean_only,
                                          int reuse_block);

/* parse.c (#378 slice 1): the YAML parse carrying the packed record
 * tape — the tree materializes on first access (yep_doc_dom). */
#ifdef __cplusplus
extern "C" {
#endif
YeptrisDocument yeptris_parse_ytape_ex(const char* buf, size_t len, const YeptrisParseOptions* opts,
                                       YeptrisStatus* status);

/* the eager form (the A/B reference lane): the engine builds nodes at
 * parse, no tape */
YeptrisDocument yeptris_parse_eager_ex(const char* buf, size_t len, const YeptrisParseOptions* opts,
                                       YeptrisStatus* status);
#ifdef __cplusplus
}
#endif

#endif
