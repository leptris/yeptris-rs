/* yajl_compat.c — the yajl generator over the yeptris core (TODO.impl/21).
 *
 * A state machine over the DOM builder: yajl's call-order rules
 * (maps alternate string-key/value; a closed root accepts no more)
 * enforced here, values build a document, get_buf serializes JSON.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/yeptris/dom.h"
#include "../../include/yeptris/error.h"
#include "../../include/yeptris/json.h"
#include "../../include/yeptris/yajl_compat.h"

#include "../doc.h"
#include "../emit/writer.h"

#define YAJL_MAX_STACK 512

typedef struct yajl_gen_t {
    yeptris_document* doc;
    YeptrisNode stack[YAJL_MAX_STACK];
    int depth;
    int key_pending[YAJL_MAX_STACK]; /* per open map: string key next */
    int root_closed;
    YeptrisNode pending_key; /* map: key placed, awaiting its value */
    unsigned char* buf;      /* get_buf cache: freed on next gen call */
    size_t buf_len;
    int beautify;
    char numbuf[48];
} yajl_gen_t;

yajl_gen yajl_gen_alloc(const yajl_alloc_funcs* afs) {
    (void)afs; /* the system allocator (yajl's NULL default) */
    yajl_gen g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    g->doc = yeptris_document_new();
    if (g->doc == NULL) {
        free(g);
        return NULL;
    }
    return g;
}

void yajl_gen_free(yajl_gen g) {
    if (g == NULL) {
        return;
    }
    free(g->buf);
    yeptris_document_free(g->doc);
    free(g);
}

void yajl_gen_reset(yajl_gen g, const char* sep) {
    if (g == NULL) {
        return;
    }
    (void)sep; /* yajl emits the separator between streams; the buf
                * clears on the next generation anyway */
    free(g->buf);
    g->buf = NULL;
    g->buf_len = 0;
    yeptris_document_free(g->doc);
    g->doc = yeptris_document_new();
    g->depth = 0;
    g->root_closed = 0;
    g->pending_key = NULL;
    memset(g->key_pending, 0, sizeof(g->key_pending));
}

int yajl_gen_config(yajl_gen g, yajl_gen_option opt, ...) {
    if (g == NULL) {
        return 0;
    }
    va_list ap;
    va_start(ap, opt);
    int rc = 0;
    switch (opt) {
    case yajl_gen_beautify:
        g->beautify = va_arg(ap, int) != 0;
        rc = 1;
        break;
    case yajl_gen_indent_string:
    case yajl_gen_validate_utf8:
        rc = 1; /* accepted: fixed 2-space indent; UTF-8 always on */
        break;
    }
    va_end(ap);
    return rc;
}

/* Places a completed value under the innermost open container (or
 * closes the root). yajl rule: inside a map, the value slot must be
 * preceded by its string key (key_pending). */
static yajl_gen_status place(yajl_gen g, YeptrisNode value) {
    if (g->root_closed) {
        return yajl_gen_generation_complete;
    }
    if (g->depth > 0 && yeptris_node_kind(g->stack[g->depth - 1]) == YEPTRIS_NODE_MAPPING &&
        g->key_pending[g->depth - 1] && yeptris_node_style(value) != YEPTRIS_STYLE_DOUBLE_QUOTED) {
        return yajl_gen_keys_must_be_strings; /* yajl's map-key rule */
    }
    if (g->depth == 0) {
        if (g->root_closed) {
            return yajl_gen_generation_complete;
        }
        if (yeptris_document_set_root(g->doc, value) != YEPTRIS_OK) {
            return yajl_gen_in_error_state;
        }
        /* a root SCALAR completes generation; an open container
         * continues (it closes later at depth 0) */
        g->root_closed = 1;
        return yajl_gen_status_ok;
    }
    YeptrisNode top = g->stack[g->depth - 1];
    if (yeptris_node_kind(top) == YEPTRIS_NODE_MAPPING) {
        if (g->key_pending[g->depth - 1]) {
            /* this placement IS the key: remember it for the value
             * call (keys are strings per yajl's rule) */
            g->pending_key = value;
            g->key_pending[g->depth - 1] = 0;
        } else {
            if (g->pending_key == NULL) {
                return yajl_gen_in_error_state;
            }
            if (yeptris_node_map_add_node(top, g->pending_key, value) != YEPTRIS_OK) {
                return yajl_gen_in_error_state;
            }
            g->pending_key = NULL;
            g->key_pending[g->depth - 1] = 1;
        }
        return yajl_gen_status_ok;
    }
    if (yeptris_node_seq_add(top, value) != YEPTRIS_OK) {
        return yajl_gen_in_error_state;
    }
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_map_open(yajl_gen g) {
    if (g == NULL) {
        return yajl_gen_in_error_state;
    }
    if (g->root_closed) {
        return yajl_gen_generation_complete;
    }
    if (g->depth >= YAJL_MAX_STACK) {
        return yajl_max_depth_exceeded;
    }
    YeptrisNode m = yeptris_node_new_mapping(g->doc);
    if (m == NULL) {
        return yajl_gen_in_error_state;
    }
    yajl_gen_status st = place(g, m);
    if (st != yajl_gen_status_ok) {
        return st;
    }
    g->root_closed = 0; /* an open root container keeps generating */
    g->stack[g->depth] = m;
    g->key_pending[g->depth] = 1;
    g->depth++;
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_map_close(yajl_gen g) {
    if (g == NULL || g->depth == 0 ||
        yeptris_node_kind(g->stack[g->depth - 1]) != YEPTRIS_NODE_MAPPING) {
        return yajl_gen_in_error_state;
    }
    if (!g->key_pending[g->depth - 1] && g->pending_key != NULL) {
        return yajl_gen_keys_must_be_strings; /* an incomplete pair */
    }
    g->depth--;
    if (g->depth == 0) {
        g->root_closed = 1; /* the root container finished */
    }
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_array_open(yajl_gen g) {
    if (g == NULL) {
        return yajl_gen_in_error_state;
    }
    if (g->root_closed) {
        return yajl_gen_generation_complete;
    }
    if (g->depth >= YAJL_MAX_STACK) {
        return yajl_max_depth_exceeded;
    }
    YeptrisNode sq = yeptris_node_new_sequence(g->doc);
    if (sq == NULL) {
        return yajl_gen_in_error_state;
    }
    yajl_gen_status st = place(g, sq);
    if (st != yajl_gen_status_ok) {
        return st;
    }
    g->root_closed = 0;
    g->stack[g->depth] = sq;
    g->key_pending[g->depth] = 0;
    g->depth++;
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_array_close(yajl_gen g) {
    if (g == NULL || g->depth == 0 ||
        yeptris_node_kind(g->stack[g->depth - 1]) != YEPTRIS_NODE_SEQUENCE) {
        return yajl_gen_in_error_state;
    }
    g->depth--;
    if (g->depth == 0) {
        g->root_closed = 1;
    }
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_string(yajl_gen g, const unsigned char* str, size_t len) {
    if (g == NULL) {
        return yajl_gen_in_error_state;
    }
    if (g->root_closed) {
        return yajl_gen_generation_complete;
    }
    if (g->depth > 0 && yeptris_node_kind(g->stack[g->depth - 1]) == YEPTRIS_NODE_MAPPING &&
        !g->key_pending[g->depth - 1]) {
        /* value slot: a JSON string */
        YeptrisNode n =
            yeptris_node_new_scalar(g->doc, (const char*)str, len, YEPTRIS_STYLE_DOUBLE_QUOTED);
        if (n == NULL) {
            return yajl_gen_in_error_state;
        }
        return place(g, n);
    }
    /* key slot (or root/seq): keys build as plain scalar nodes whose
     * JSON serialization quotes them; yajl requires map keys to be
     * strings, and gen_string is the only legal key call */
    YeptrisNode n =
        yeptris_node_new_scalar(g->doc, (const char*)str, len, YEPTRIS_STYLE_DOUBLE_QUOTED);
    if (n == NULL) {
        return yajl_gen_in_error_state;
    }
    return place(g, n);
}

yajl_gen_status yajl_gen_integer(yajl_gen g, long long n) {
    if (g == NULL) {
        return yajl_gen_in_error_state;
    }
    snprintf(g->numbuf, sizeof(g->numbuf), "%lld", n);
    YeptrisNode v =
        yeptris_node_new_scalar(g->doc, g->numbuf, strlen(g->numbuf), YEPTRIS_STYLE_PLAIN);
    if (v == NULL) {
        return yajl_gen_in_error_state;
    }
    return place(g, v);
}

yajl_gen_status yajl_gen_double(yajl_gen g, double d) {
    if (g == NULL) {
        return yajl_gen_in_error_state;
    }
    snprintf(g->numbuf, sizeof(g->numbuf), "%.17g", d);
    YeptrisNode v =
        yeptris_node_new_scalar(g->doc, g->numbuf, strlen(g->numbuf), YEPTRIS_STYLE_PLAIN);
    if (v == NULL) {
        return yajl_gen_in_error_state;
    }
    return place(g, v);
}

yajl_gen_status yajl_gen_number(yajl_gen g, const char* s, size_t len) {
    if (g == NULL || s == NULL) {
        return yajl_gen_in_error_state;
    }
    YeptrisNode v = yeptris_node_new_scalar(g->doc, s, len, YEPTRIS_STYLE_PLAIN);
    if (v == NULL) {
        return yajl_gen_in_error_state;
    }
    return place(g, v);
}

yajl_gen_status yajl_gen_null(yajl_gen g) {
    if (g == NULL) {
        return yajl_gen_in_error_state;
    }
    YeptrisNode v = yeptris_node_new_scalar(g->doc, "null", 4, YEPTRIS_STYLE_PLAIN);
    if (v == NULL) {
        return yajl_gen_in_error_state;
    }
    return place(g, v);
}

yajl_gen_status yajl_gen_bool(yajl_gen g, int b) {
    if (g == NULL) {
        return yajl_gen_in_error_state;
    }
    YeptrisNode v =
        yeptris_node_new_scalar(g->doc, b ? "true" : "false", b ? 4 : 5, YEPTRIS_STYLE_PLAIN);
    if (v == NULL) {
        return yajl_gen_in_error_state;
    }
    return place(g, v);
}

const unsigned char* yajl_gen_get_buf(yajl_gen g, size_t* len) {
    if (g == NULL || !g->root_closed) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    if (g->buf == NULL) {
        size_t n = 0;
        char* out = g->beautify ? yep_serialize_json_pretty((const yeptris_document*)g->doc, &n)
                                : yep_serialize_json_compact((const yeptris_document*)g->doc, &n);
        if (out == NULL) {
            if (len != NULL) {
                *len = 0;
            }
            return NULL;
        }
        if (n > 0 && out[n - 1] == '\n') {
            out[n - 1] = '\0'; /* yajl output carries no newline */
            n--;
        }
        g->buf = (unsigned char*)out;
        g->buf_len = n;
    }
    if (len != NULL) {
        *len = g->buf_len;
    }
    return g->buf;
}

void yajl_gen_clear(yajl_gen g) {
    if (g == NULL) {
        return;
    }
    yajl_gen_reset(g, NULL);
}

/* ---- SAX parser (TODO.impl/21 v4) --------------------------------------
 *
 * yajl_parse feeds accumulate; yajl_complete_parse runs the parse
 * (strict RFC 8259 by default, the YAML front end with
 * yajl_allow_comments) and walks the DOM to the callbacks. Events
 * arrive at completion — batched in time, identical in order and
 * content to yajl's incremental delivery. */

typedef struct yajl_handle_t {
    yajl_callbacks cbs;
    void* ctx;
    char* buf;
    size_t len, cap;
    int allow_comments;
    int state;        /* 0 feeding, 1 complete-ok, 2 error, 3 canceled */
    int num_overflow; /* a number unrepresentable as double (yajl errors) */
    char err[192];
    size_t err_off; /* byte offset of the violation */
    int have_err_off;
} yajl_handle_t;

const char* yajl_status_to_string(yajl_status code) {
    switch (code) {
    case yajl_status_ok:
        return "ok, no error";
    case yajl_status_client_canceled:
        return "client canceled parse";
    case yajl_status_error:
        return "unknown error?";
    }
    return "unknown error?";
}

yajl_handle yajl_alloc(const yajl_callbacks* callbacks, yajl_alloc_funcs* afs, void* ctx) {
    (void)afs; /* the system allocator (yajl's NULL default) */
    yajl_handle_t* h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return NULL;
    }
    if (callbacks != NULL) {
        h->cbs = *callbacks;
    }
    h->ctx = ctx;
    return h;
}

void yajl_free(yajl_handle handle) {
    if (handle != NULL) {
        free(handle->buf);
        free(handle);
    }
}

int yajl_config(yajl_handle h, yajl_option opt, ...) {
    if (h == NULL) {
        return 0;
    }
    va_list ap;
    va_start(ap, opt);
    int on = va_arg(ap, int);
    va_end(ap);
    switch (opt) {
    case yajl_allow_comments:
        h->allow_comments = on != 0;
        return 1;
    case yajl_dont_validate_strings:
        return 1; /* no-op: UTF-8 validation is always on */
    case yajl_allow_trailing_garbage:
    case yajl_allow_multiple_values:
    case yajl_allow_partial_values:
    default:
        return 0; /* unsupported: honest error, not silent misparse */
    }
}

yajl_status yajl_parse(yajl_handle hand, const unsigned char* jsonText, size_t jsonTextLength) {
    if (hand == NULL || (jsonText == NULL && jsonTextLength != 0)) {
        return yajl_status_error;
    }
    if (hand->state != 0) {
        /* yajl parks a completed handle; an error/cancel sticks */
        return hand->state == 1 ? yajl_status_ok : yajl_status_error;
    }
    if (jsonTextLength > 0) {
        if (hand->len + jsonTextLength > hand->cap) {
            size_t cap = hand->cap ? hand->cap : 4096;
            while (cap < hand->len + jsonTextLength) {
                cap *= 2;
            }
            char* nb = realloc(hand->buf, cap);
            if (nb == NULL) {
                return yajl_status_error;
            }
            hand->buf = nb;
            hand->cap = cap;
        }
        memcpy(hand->buf + hand->len, jsonText, jsonTextLength);
        hand->len += jsonTextLength;
    }
    return yajl_status_ok;
}

/* Replaces JavaScript comments (yajl's allow_comments: block
 * star-slash and line double-slash) outside strings with spaces —
 * every byte offset and line number is preserved, so the strict
 * parse sees the exact text yajl would. String boundaries (with
 * escapes) are tracked so a double slash inside \"http://x\" stays
 * content. */
static void mask_js_comments(char* p, size_t len) {
    enum { NORM, STR } st = NORM;
    size_t i = 0;
    while (i < len) {
        char c = p[i];
        if (st == STR) {
            if (c == '\\') {
                i += 2; /* the escaped byte is content */
                continue;
            }
            if (c == '"') {
                st = NORM;
            }
            i++;
            continue;
        }
        if (c == '"') {
            st = STR;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && p[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < len && !(p[j] == '*' && p[j + 1] == '/')) {
                j++;
            }
            size_t end = (j + 1 < len && p[j] == '*' && p[j + 1] == '/') ? j + 2 : len;
            for (size_t k = i; k < end; k++) {
                if (p[k] != '\n') { /* line numbers stay true */
                    p[k] = ' ';
                }
            }
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < len && p[i + 1] == '/') {
            while (i < len && p[i] != '\n') {
                p[i++] = ' ';
            }
            continue;
        }
        i++;
    }
}

/* line:col (1-based) back to a byte offset in the accumulated input */
static size_t err_offset_of(const yajl_handle_t* h, uint32_t line, uint32_t col) {
    size_t off = 0;
    uint32_t ln = 1;
    while (off < h->len && ln < line) {
        if (h->buf[off] == '\n') {
            ln++;
        }
        off++;
    }
    return off + (col > 0 ? col - 1 : 0);
}

static void fail_parse(yajl_handle_t* h, const char* what, uint32_t line, uint32_t col) {
    h->state = 2;
    h->err_off = line != 0 ? err_offset_of(h, line, col) : h->len;
    h->have_err_off = 1;
    snprintf(h->err, sizeof(h->err), "parse error: %s", what);
}

/* Walks one node; 0 = a callback canceled */
static int walk_node(yajl_handle_t* h, YeptrisNode n) {
    switch (yeptris_node_kind(n)) {
    case YEPTRIS_NODE_SEQUENCE:
        if (h->cbs.yajl_start_array != NULL && !h->cbs.yajl_start_array(h->ctx)) {
            return 0;
        }
        for (size_t i = 0; i < yeptris_node_seq_count(n); i++) {
            if (!walk_node(h, yeptris_node_seq_at(n, i))) {
                return 0;
            }
        }
        if (h->cbs.yajl_end_array != NULL && !h->cbs.yajl_end_array(h->ctx)) {
            return 0;
        }
        return 1;
    case YEPTRIS_NODE_MAPPING:
        if (h->cbs.yajl_start_map != NULL && !h->cbs.yajl_start_map(h->ctx)) {
            return 0;
        }
        size_t pairs = yeptris_node_map_count(n);
        for (size_t i = 0; i < pairs; i++) {
            YeptrisNode k = NULL;
            YeptrisNode v = NULL;
            if (yeptris_node_map_at(n, i, &k, &v) != YEPTRIS_OK) {
                return 1; /* unreachable on a parsed document */
            }
            size_t klen = 0;
            const char* key = yeptris_node_value(k, &klen);
            if (h->cbs.yajl_map_key != NULL &&
                !h->cbs.yajl_map_key(h->ctx, (const unsigned char*)(key ? key : ""), klen)) {
                return 0;
            }
            if (v != NULL && !walk_node(h, v)) {
                return 0;
            }
        }
        if (h->cbs.yajl_end_map != NULL && !h->cbs.yajl_end_map(h->ctx)) {
            return 0;
        }
        return 1;
    default:
        break;
    }
    /* scalar: the resolver's typing decides the callback (a quoted
     * "12" is a string — same discrimination jsonc_compat relies on) */
    size_t vlen = 0;
    const char* v = yeptris_node_value(n, &vlen);
    if (h->cbs.yajl_number != NULL) {
        int64_t i = 0;
        double d = 0;
        if (yeptris_node_int(n, &i) == YEPTRIS_OK || yeptris_node_float(n, &d) == YEPTRIS_OK) {
            return h->cbs.yajl_number(h->ctx, v ? v : "", vlen);
        }
    } else {
        int64_t i = 0;
        double d = 0;
        int b = 0;
        if (yeptris_node_int(n, &i) == YEPTRIS_OK) {
            return h->cbs.yajl_integer == NULL || h->cbs.yajl_integer(h->ctx, (long long)i);
        }
        if (yeptris_node_float(n, &d) == YEPTRIS_OK) {
            if (!(d > -HUGE_VAL && d < HUGE_VAL)) {
                /* yajl: unrepresentable numbers are parse errors when
                 * the number callback is absent */
                h->num_overflow = 1;
                return 0;
            }
            return h->cbs.yajl_double == NULL || h->cbs.yajl_double(h->ctx, d);
        }
        if (yeptris_node_bool(n, &b) == YEPTRIS_OK) {
            return h->cbs.yajl_boolean == NULL || h->cbs.yajl_boolean(h->ctx, b);
        }
    }
    size_t wlen = 0;
    const char* w = yeptris_node_value(n, &wlen);
    if (wlen == 4 && w != NULL && memcmp(w, "null", 4) == 0) {
        return h->cbs.yajl_null == NULL || h->cbs.yajl_null(h->ctx);
    }
    return h->cbs.yajl_string == NULL ||
           h->cbs.yajl_string(h->ctx, (const unsigned char*)(v ? v : ""), vlen);
}

yajl_status yajl_complete_parse(yajl_handle hand) {
    if (hand == NULL) {
        return yajl_status_error;
    }
    if (hand->state == 3) {
        return yajl_status_client_canceled;
    }
    if (hand->state == 2) {
        return yajl_status_error;
    }
    if (hand->state == 1) {
        return yajl_status_ok; /* yajl parks completed handles */
    }
    if (hand->len == 0) {
        fail_parse(hand, "premature EOF", 1, 1);
        return yajl_status_error;
    }
    YeptrisStatus st = YEPTRIS_OK;
    const char* parse_buf = hand->buf;
    char* masked = NULL;
    if (hand->allow_comments && hand->buf != NULL) {
        masked = malloc(hand->len);
        if (masked == NULL) {
            return yajl_status_error;
        }
        memcpy(masked, hand->buf, hand->len);
        mask_js_comments(masked, hand->len);
        parse_buf = masked;
    }
    YeptrisDocument doc = yeptris_parse_json(parse_buf, hand->len, &st);
    if (doc == NULL) {
        uint32_t line = 0;
        uint32_t col = 0;
        const char* msg = yeptris_last_error(&line, &col);
        fail_parse(hand, msg != NULL ? msg : "invalid JSON", line, col);
        return yajl_status_error;
    }
    if (yeptris_document_count(doc) != 1) {
        yeptris_document_free(doc);
        fail_parse(hand, "trailing garbage", 1, 1);
        return yajl_status_error;
    }
    int ok = walk_node(hand, yeptris_document_root(doc, 0));
    int overflow = hand->num_overflow;
    yeptris_document_free(doc);
    free(masked);
    if (!ok && overflow) {
        fail_parse(hand, "number unrepresentable as double or integer", 1, 1);
        return yajl_status_error;
    }
    if (!ok) {
        hand->state = 3;
        snprintf(hand->err, sizeof(hand->err), "client canceled parse");
        return yajl_status_client_canceled;
    }
    hand->state = 1;
    hand->err_off = hand->len;
    hand->have_err_off = 1;
    return yajl_status_ok;
}

unsigned char* yajl_get_error(yajl_handle hand, int verbose, const unsigned char* jsonText,
                              size_t jsonTextLength) {
    (void)jsonText;
    (void)jsonTextLength;
    if (hand == NULL || hand->state < 2) {
        return NULL;
    }
    if (!verbose) {
        size_t need = strlen(hand->err) + 1;
        unsigned char* out = malloc(need);
        if (out != NULL) {
            memcpy(out, hand->err, need);
        }
        return out;
    }
    /* verbose: the offending line plus a caret at the column */
    size_t line_start = 0;
    size_t line_end = hand->len;
    if (hand->have_err_off) {
        line_start = hand->err_off;
        while (line_start > 0 && hand->buf[line_start - 1] != '\n') {
            line_start--;
        }
        line_end = hand->err_off;
        while (line_end < hand->len && hand->buf[line_end] != '\n') {
            line_end++;
        }
    }
    size_t lineno = 1;
    for (size_t i = 0; i < line_start; i++) {
        if (hand->buf[i] == '\n') {
            lineno++;
        }
    }
    size_t caret = hand->err_off > line_start ? hand->err_off - line_start : 0;
    char* out = malloc(strlen(hand->err) + (line_end - line_start) + caret + 64);
    if (out == NULL) {
        return NULL;
    }
    int n = snprintf(out, strlen(hand->err) + 64, "%s at line %zu:\n", hand->err, lineno);
    memcpy(out + n, hand->buf + line_start, line_end - line_start);
    n += (int)(line_end - line_start);
    out[n++] = '\n';
    memset(out + n, ' ', caret);
    n += (int)caret;
    out[n++] = '^';
    out[n] = '\0';
    return (unsigned char*)out;
}

void yajl_free_error(yajl_handle hand, unsigned char* str) {
    (void)hand;
    free(str);
}

size_t yajl_get_bytes_consumed(yajl_handle hand) {
    if (hand == NULL) {
        return 0;
    }
    if (hand->have_err_off) {
        return hand->err_off;
    }
    return hand->len;
}
