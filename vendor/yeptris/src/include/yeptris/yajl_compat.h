/* yajl_compat.h — the yajl generator migration layer (TODO.impl/21).
 *
 * REAL yajl_gen symbol names over the yeptris core: users link
 * -lyeptris-yajl instead of -lyajl and change nothing else. The
 * generator validates call order (yajl's own state machine) and
 * builds a Yeptris document; get_buf serializes JSON (beautify
 * selects the pretty writer, default compact).
 *
 * v2 scope: the generator (yajl_gen_*) and the SAX parser
 * (yajl_alloc/yajl_parse/...). The parser accumulates yajl_parse
 * feeds and delivers all callbacks at yajl_complete_parse (the core
 * parses whole documents; timing is batched, order and content are
 * yajl's). Strict RFC 8259 always; yajl_allow_comments masks
 * JavaScript comments (block star-slash, line double-slash) outside
 * strings before the strict parse — offsets preserved, yajl's exact
 * semantics. Unsupported options (trailing garbage / multiple
 * values / partial values) are rejected at yajl_config time: zero,
 * per yajl's own error contract.
 */
#ifndef YEPTRIS_YAJL_COMPAT_H
#define YEPTRIS_YAJL_COMPAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    yajl_gen_status_ok = 0,
    yajl_gen_keys_must_be_strings, /* 1 */
    yajl_max_depth_exceeded,       /* unused: depth-guarded builder */
    yajl_gen_in_error_state = 3,
    yajl_gen_generation_complete, /* 4: gen after get_buf without clear */
    yajl_gen_invalid_string = 5   /* NULL bytes with len 0 handled; reserved */
} yajl_gen_status;

typedef enum {
    yajl_gen_beautify = 1,      /* int: pretty output */
    yajl_gen_indent_string = 2, /* const char*: reserved (fixed 2-space) */
    yajl_gen_validate_utf8 = 3  /* int: always on (the front-end validates) */
} yajl_gen_option;

typedef struct yajl_gen_t* yajl_gen;
typedef struct yajl_alloc_funcs_t yajl_alloc_funcs;

/* NULL alloc (the yajl default) uses the system allocator. */
yajl_gen yajl_gen_alloc(const yajl_alloc_funcs* afs);
void yajl_gen_free(yajl_gen g);
void yajl_gen_reset(yajl_gen g, const char* sep_ignored);

/* var-args: (yajl_gen_beautify, int). Returns 1 on success. */
int yajl_gen_config(yajl_gen g, yajl_gen_option opt, ...);

yajl_gen_status yajl_gen_map_open(yajl_gen g);
yajl_gen_status yajl_gen_map_close(yajl_gen g);
yajl_gen_status yajl_gen_array_open(yajl_gen g);
yajl_gen_status yajl_gen_array_close(yajl_gen g);

/* In a map, the first call per entry is the KEY: it must be a
 * string (yajl accepts raw bytes; keys serialize JSON-quoted). */
yajl_gen_status yajl_gen_string(yajl_gen g, const unsigned char* str, size_t len);
yajl_gen_status yajl_gen_integer(yajl_gen g, long long n);
yajl_gen_status yajl_gen_double(yajl_gen g, double d);
yajl_gen_status yajl_gen_number(yajl_gen g, const char* s, size_t len);
yajl_gen_status yajl_gen_null(yajl_gen g);
yajl_gen_status yajl_gen_bool(yajl_gen g, int b);

/* The serialized document (owned by the generator; valid until the
 * next generating call or reset). Empty until the ROOT closes. */
const unsigned char* yajl_gen_get_buf(yajl_gen g, size_t* len);
void yajl_gen_clear(yajl_gen g);

/* ---- SAX parser ------------------------------------------------------- */

typedef enum { yajl_status_ok = 0, yajl_status_client_canceled, yajl_status_error } yajl_status;

const char* yajl_status_to_string(yajl_status code);

/* Zero return from a callback cancels the parse
 * (yajl_status_client_canceled). When yajl_number is set it carries
 * EVERY number in string form; yajl_integer/yajl_double are then
 * ignored (yajl's own precedence). */
typedef struct {
    int (*yajl_null)(void* ctx);
    int (*yajl_boolean)(void* ctx, int boolVal);
    int (*yajl_integer)(void* ctx, long long integerVal);
    int (*yajl_double)(void* ctx, double doubleVal);
    int (*yajl_number)(void* ctx, const char* numberVal, size_t numberLen);
    int (*yajl_string)(void* ctx, const unsigned char* stringVal, size_t stringLen);
    int (*yajl_start_map)(void* ctx);
    int (*yajl_map_key)(void* ctx, const unsigned char* key, size_t stringLen);
    int (*yajl_end_map)(void* ctx);
    int (*yajl_start_array)(void* ctx);
    int (*yajl_end_array)(void* ctx);
} yajl_callbacks;

typedef enum {
    yajl_allow_comments = 0x01,        /* JS comments masked, offsets kept */
    yajl_dont_validate_strings = 0x02, /* accepted no-op: always on */
    yajl_allow_trailing_garbage = 0x04,
    yajl_allow_multiple_values = 0x08,
    yajl_allow_partial_values = 0x10
} yajl_option;

typedef struct yajl_handle_t* yajl_handle;

/* callbacks may be NULL (pure validation). afs accepted, ignored. */
yajl_handle yajl_alloc(const yajl_callbacks* callbacks, yajl_alloc_funcs* afs, void* ctx);
void yajl_free(yajl_handle handle);

/* var-args int option: 1 on, 0 off. Returns 0 on unknown/unsupported
 * options (trailing garbage / multiple values / partial values). */
int yajl_config(yajl_handle h, yajl_option opt, ...);

/* Accumulates feeds; callbacks fire at complete_parse. After an
 * error or cancel the handle stays in that state. */
yajl_status yajl_parse(yajl_handle hand, const unsigned char* jsonText, size_t jsonTextLength);
yajl_status yajl_complete_parse(yajl_handle hand);

/* Malloc'd message (free with yajl_free_error); NULL when no error.
 * verbose adds the offending line and a caret. */
unsigned char* yajl_get_error(yajl_handle hand, int verbose, const unsigned char* jsonText,
                              size_t jsonTextLength);
void yajl_free_error(yajl_handle hand, unsigned char* str);

/* Bytes consumed: the whole input once complete; on error, the
 * offset of the violation. */
size_t yajl_get_bytes_consumed(yajl_handle hand);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_YAJL_COMPAT_H */
