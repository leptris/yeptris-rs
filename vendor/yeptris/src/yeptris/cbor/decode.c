/* decode.c — CBOR (RFC 8949) decode over the shared DOM (TODO.cbor/01).
 *
 * Bytes -> the same DOM JSON builds, one data item per call (trailing
 * bytes are "too much data"; CBOR Sequences are item 03). The walk is
 * iterative — the DOM's own builder stack hosts the nesting, so the
 * C stack never rides the input depth.
 *
 * Representation ledger (documented divergences from CBOR's model,
 * all pinned by the Appendix A + F suites):
 * - integers beyond int64 materialize as shortest-double text (the
 *   number-kernel shape-2 contract); bignums stay tag+bytes (2/3)
 * - byte strings and text strings are indistinguishable post-DOM
 *   (tag-str scalars, zero-copy spans); canonical re-encode (02)
 *   emits text strings
 * - undefined is the scalar "undefined"; unassigned simple values
 *   are "simple(N)" diagnostic text (both modes)
 * - tags ride the node's tag field as a decimal chain, outermost
 *   first ("2 55799"); never interpreted by the core
 * - non-string map keys: strict rejects; lenient materializes the
 *   diagnostic text for SCALAR keys — structure keys reject in both
 * - duplicate keys: last materialized wins, the JSON DOM path's rule
 * - indefinite-length strings concatenate their chunks in the DOM's
 *   string arena (the only string copies; definite strings borrow)
 */
#include "cbor.h"

#include <math.h>
#include <string.h>

#include <stdlib.h>

#include <yeptris/cbor.h>
#include <yeptris/resolve.h>

#include "../common/error.h"
#include "../doc.h"
#include "../emit/float/api.h"
#include "../encoding/encoding.h"
#include "../memory/allocator.h"
#include "../parse/events.h"
#include "doc.h"
#include "sink.h"

#define YEP_CBOR_INDEF UINT64_MAX
#define YEP_CBOR_MAX_ARG_BYTES 8

typedef struct {
    uint64_t rem;      /* items left; maps count 2*pairs; INDEF = until break */
    uint64_t consumed; /* items landed (the key/value parity) */
    uint8_t is_map;
} yep_cframe;

#define YEP_CBOR_TAGBUF                                                                            \
    512 /* a semantic-tag chain over ~23 links is                                                  \
         * pathological; the arena form had no cap,                                                \
         * the stack form does (documented) */

typedef struct {
    yep_dom* d; /* the DOM sink's ctx (NULL for pure sinks) */
    const unsigned char* p;
    size_t len;
    size_t i;
    int strict;
    YeptrisStatus status;
    /* the pending semantic-tag chain, text form ("N N N", outermost
     * first — the next item's writer consumes it) */
    char tagbuf[YEP_CBOR_TAGBUF];
    uint32_t taglen;
    /* indefinite-string accumulation (chunk framing is the grammar's;
     * the finished span goes to one str/bytes callback) */
    unsigned char* ibuf;
    size_t icap;
    uint32_t ilen;
    const yep_cbor_sink* s;
    yep_cframe frame[YEP_DOM_MAX_DEPTH];
    int depth;
} yep_cdec;

static void cbor_fail(yep_cdec* c, yep_err_code code, const char* msg) {
    yep_error_set(yep_error_tls(), code, 0, 0, c->i, "%s at byte %zu", msg, c->i);
}

#define CBOR_REJECT(what)                                                                          \
    do {                                                                                           \
        cbor_fail(c, YEP_ERR_UNEXPECTED, what);                                                    \
        c->status = YEPTRIS_ERROR_PARSE;                                                           \
        return 0;                                                                                  \
    } while (0)

/* The head's argument: additional info 0-23 inline, 24-27 a 1/2/4/8-byte
 * big-endian value; 28-30 are reserved (never well-formed). Strict mode
 * also rejects non-minimal encodings (RFC 8949 s4.2.1's argument rule).
 * ai 31 is the caller's (indefinite/break) — never reaches here. */
static int cbor_arg(yep_cdec* c, uint8_t ai, uint64_t* val) {
    static const uint8_t nbytes[4] = {1, 2, 4, 8};
    static const uint64_t minv[4] = {24, 0x100ull, 0x10000ull, 0x100000000ull};
    if (ai >= 28) {
        CBOR_REJECT("reserved additional information (28-30)");
    }
    uint64_t v = ai;
    if (ai >= 24) {
        size_t n = nbytes[ai - 24];
        if (c->len - c->i < n) {
            cbor_fail(c, YEP_ERR_UNEXPECTED, "truncated head");
            c->status = YEPTRIS_ERROR_PARSE;
            return 0;
        }
        v = 0;
        for (size_t k = 0; k < n; k++) {
            v = (v << 8) | c->p[c->i + k];
        }
        c->i += n;
        if (c->strict && v < minv[ai - 24]) {
            CBOR_REJECT("non-minimal length argument");
        }
    }
    *val = v;
    return 1;
}

static uint32_t cbor_u64dec(uint64_t v, char* buf) {
    /* two digits per divide: the per-digit %10//10 loop was the decode
     * lane's hottest formatter (the #742 profile) — one /100 plus a
     * table load halves the divides; the reverse pass shrinks to the
     * pair loop */
    static const char PAIRS[] = "00010203040506070809"
                                "10111213141516171819"
                                "20212223242526272829"
                                "30313233343536373839"
                                "40414243444546474849"
                                "50515253545556575859"
                                "60616263646566676869"
                                "70717273747576777879"
                                "80818283848586878889"
                                "90919293949596979899";
    char tmp[20];
    uint32_t n = 0;
    while (v >= 100) {
        uint32_t r = (uint32_t)(v % 100u);
        v /= 100u;
        tmp[n++] = PAIRS[r * 2u + 1u];
        tmp[n++] = PAIRS[r * 2u];
    }
    if (v >= 10u) {
        tmp[n++] = PAIRS[v * 2u + 1u];
        tmp[n++] = PAIRS[v * 2u];
    } else {
        tmp[n++] = (char)('0' + (int)v);
    }
    for (uint32_t k = 0; k < n; k++) {
        buf[k] = tmp[n - 1u - k];
    }
    return n;
}

/* ldexp without libm: the exponent range here is [-24, 5], so scaling
 * by repeated doubling/halving is exact and bounded. */
static double cbor_pow2(double x, int e) {
    while (e > 0) {
        x *= 2.0;
        e--;
    }
    while (e < 0) {
        x *= 0.5;
        e++;
    }
    return x;
}

/* The RFC 8949 Appendix D half-precision decoder, verbatim shape. */
static double cbor_half(uint16_t h) {
    unsigned exp = (h >> 10) & 0x1fu;
    unsigned mant = h & 0x3ffu;
    double val;
    if (exp == 0) {
        val = cbor_pow2((double)mant, -24);
    } else if (exp != 31) {
        val = cbor_pow2((double)(mant + 1024), (int)exp - 25);
    } else {
        val = mant == 0 ? (double)INFINITY : (double)NAN;
    }
    return (h & 0x8000u) != 0 ? -val : val;
}

/* The pending semantic-tag chain, consumed by the next writer. */
static const char* cbor_take_tag(yep_cdec* c, uint32_t* len) {
    if (c->taglen == 0) {
        *len = 0;
        return NULL;
    }
    *len = c->taglen;
    c->taglen = 0;
    return c->tagbuf;
}

/* Rendered text (numbers-as-text, literals, diagnostics). */
static uint32_t cbor_text_node(yep_cdec* c, const char* text, uint32_t n, uint8_t tag_id,
                               int is_key) {
    uint32_t tag_len;
    const char* tag = cbor_take_tag(c, &tag_len);
    if (!c->s->text(c->s->ctx, text, n, tag_id, is_key, tag, tag_len)) {
        c->status = YEPTRIS_ERROR_MEMORY;
        return UINT32_MAX;
    }
    return 0; /* item identity is the sink's business */
}

/* Definite string (mt2 -> bytes_val, mt3 -> str_val), borrowing the input. */
static uint32_t cbor_span_node(yep_cdec* c, const unsigned char* p, uint32_t n, uint8_t mt,
                               int is_key) {
    uint32_t tag_len;
    const char* tag = cbor_take_tag(c, &tag_len);
    int ok = (mt == 2) ? c->s->bytes_val(c->s->ctx, p, n, 1, is_key, tag, tag_len)
                       : c->s->str_val(c->s->ctx, p, n, 1, is_key, tag, tag_len);
    if (!ok) {
        c->status = YEPTRIS_ERROR_MEMORY;
        return UINT32_MAX;
    }
    return 0;
}

static int cbor_open(yep_cdec* c, int is_map, uint64_t cap) {
    if (c->depth >= YEP_DOM_MAX_DEPTH) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, c->i,
                      "CBOR nesting exceeds the depth guard at byte %zu", c->i);
        c->status = YEPTRIS_ERROR_DEPTH;
        return 0;
    }
    uint32_t tag_len;
    const char* tag = cbor_take_tag(c, &tag_len);
    if (!c->s->open(c->s->ctx, is_map, cap, tag, tag_len)) {
        c->status = YEPTRIS_ERROR_MEMORY;
        return 0;
    }
    /* a sibling at this depth may have left state behind */
    c->frame[c->depth].consumed = 0;
    c->depth++;
    return 1;
}

static int cbor_note_tag(yep_cdec* c, uint64_t tag) {
    if (c->taglen + 1 + 20 > sizeof c->tagbuf) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, c->i,
                      "semantic-tag chain exceeds %u bytes at byte %zu", (unsigned)sizeof c->tagbuf,
                      c->i);
        c->status = YEPTRIS_ERROR_PARSE;
        return 0;
    }
    if (c->taglen > 0) {
        c->tagbuf[c->taglen++] = ' '; /* separator between chain links */
    }
    c->taglen += cbor_u64dec(tag, c->tagbuf + c->taglen);
    return 1;
}

/* One complete indefinite-length string (chunks concatenate in the
 * arena; text chunks validate per chunk — RFC 8949 s3.2.3's code-point
 * boundary rule makes per-chunk validity imply whole-string validity). */
static int cbor_indef_string(yep_cdec* c, int is_text) {
    uint32_t total = 0;
    for (;;) {
        if (c->i >= c->len) {
            CBOR_REJECT("unterminated indefinite-length string");
        }
        uint8_t ib = c->p[c->i];
        c->i++;
        if (ib == 0xFF) {
            break;
        }
        if ((ib >> 5) != (is_text ? 3 : 2) || (ib & 0x1F) == 31) {
            CBOR_REJECT("indefinite-length string chunk must be definite, same major type");
        }
        uint64_t n = 0;
        if (!cbor_arg(c, ib & 0x1F, &n)) {
            return 0;
        }
        if (n > c->len - c->i) {
            CBOR_REJECT("truncated string chunk");
        }
        if (is_text && !yep_utf8_validate(c->p + c->i, (size_t)n, NULL)) {
            yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, c->i,
                          "ill-formed UTF-8 in text string at byte %zu", c->i);
            c->status = YEPTRIS_ERROR_ENCODING;
            return 0;
        }
        if (n > 0) { /* zero-length chunks are legal, just empty */
            if (total + n > c->icap) {
                size_t ncap = c->icap ? c->icap * 2 : 256;
                while (ncap < total + n) {
                    ncap *= 2;
                }
                unsigned char* nib = realloc(c->ibuf, ncap);
                if (nib == NULL) {
                    c->status = YEPTRIS_ERROR_MEMORY;
                    return 0;
                }
                c->ibuf = nib;
                c->icap = ncap;
            }
            memcpy(c->ibuf + total, c->p + c->i, (size_t)n);
            total += (uint32_t)n;
        }
        c->i += (size_t)n;
    }
    /* one value over the accumulated span */
    uint32_t tag_len;
    const char* tag = cbor_take_tag(c, &tag_len);
    int ok =
        (is_text ? c->s->str_val : c->s->bytes_val)(c->s->ctx, c->ibuf, total, 0, 0, tag, tag_len);
    if (!ok) {
        c->status = YEPTRIS_ERROR_MEMORY;
        return 0;
    }
    return 1;
}

/* Known integer: value = negative ? -mag : mag, mag <= 2^63. */
static int cbor_int_node(yep_cdec* c, int negative, uint64_t mag, int is_key) {
    uint32_t tag_len;
    const char* tag = cbor_take_tag(c, &tag_len);
    if (!c->s->int_val(c->s->ctx, negative, mag, is_key, tag, tag_len)) {
        c->status = YEPTRIS_ERROR_MEMORY;
        return 0;
    }
    return 1;
}

static int cbor_float_node(yep_cdec* c, double dv, int is_key) {
    uint32_t tag_len;
    const char* tag = cbor_take_tag(c, &tag_len);
    if (!c->s->float_val(c->s->ctx, dv, is_key, tag, tag_len)) {
        c->status = YEPTRIS_ERROR_MEMORY;
        return 0;
    }
    return 1;
}

/* The diagnostic renderer for lenient-mode non-string SCALAR map keys
 * (the plan's policy: what the JSON path can round-trip). */
static int cbor_key_diag(yep_cdec* c, uint8_t mt, uint64_t val, double dv) {
    char buf[48];
    uint32_t n;
    switch (mt) {
    case 0:
        n = cbor_u64dec(val, buf);
        break;
    case 1:
        buf[0] = '-';
        n = 1 + cbor_u64dec(val, buf + 1);
        break;
    case 2: { /* h'hex' */
        buf[0] = 'h';
        buf[1] = '\'';
        static const char hexd[] = "0123456789abcdef";
        n = 2;
        for (uint64_t k = 0; k < val && n + 2 < sizeof(buf); k++) {
            uint8_t b = c->p[c->i + k];
            buf[n++] = hexd[b >> 4];
            buf[n++] = hexd[b & 15];
        }
        buf[n++] = '\'';
        break;
    }
    case 7:
        if (val == 20) {
            memcpy(buf, "false", 5);
            n = 5;
        } else if (val == 21) {
            memcpy(buf, "true", 4);
            n = 4;
        } else if (val == 22) {
            memcpy(buf, "null", 4);
            n = 4;
        } else if (val == 23) {
            memcpy(buf, "undefined", 9);
            n = 9;
        } else {
            memcpy(buf, "simple(", 7);
            n = 7 + cbor_u64dec(val, buf + 7);
            buf[n++] = ')';
        }
        break;
    default:
        return cbor_float_node(c, dv, 1); /* floats: the shortest text */
    }
    return cbor_text_node(c, buf, n, 0, 1) != UINT32_MAX;
}

/* Decodes ONE data item (scalar: creates+places the node; container:
 * opens+pushes a frame; tag: notes the chain and reports "again").
 * Returns 1 item consumed/started, 0 failure, 2 = tag (no slot). */
static int cbor_item(yep_cdec* c) {
    if (c->i >= c->len) {
        CBOR_REJECT("truncated head");
    }
    uint8_t ib = c->p[c->i++];
    uint8_t mt = ib >> 5;
    uint8_t ai = ib & 0x1F;

    /* the map-key position decides representation before the item is
     * built: even items consumed = key, odd = value (the frame's own
     * parity — the DOM's placement machine tracks its own copy) */
    int key_slot = 0;
    if (c->depth > 0 && c->frame[c->depth - 1].is_map && c->frame[c->depth - 1].consumed % 2 == 0) {
        key_slot = 1;
    }

    if (ai == 31) {
        switch (mt) {
        case 2:
        case 3: {
            if (key_slot) {
                CBOR_REJECT("string key required");
            }
            return cbor_indef_string(c, mt == 3) ? 1 : 0;
        }
        case 4:
            if (key_slot) {
                CBOR_REJECT("structure as map key");
            }
            if (!cbor_open(c, 0, YEP_CBOR_INDEF)) {
                return 0;
            }
            c->frame[c->depth - 1].rem = YEP_CBOR_INDEF;
            c->frame[c->depth - 1].is_map = 0;
            return 1;
        case 5:
            if (key_slot) {
                CBOR_REJECT("structure as map key");
            }
            if (!cbor_open(c, 1, YEP_CBOR_INDEF)) {
                return 0;
            }
            c->frame[c->depth - 1].rem = YEP_CBOR_INDEF;
            c->frame[c->depth - 1].is_map = 1;
            return 1;
        default:
            CBOR_REJECT("additional information 31 with major type 0, 1, 6, or as an item");
        }
    }

    uint64_t arg = 0;
    if (mt == 7) {
        /* major type 7: ai is the payload shape, not an argument width */
        arg = ai;
    } else if (!cbor_arg(c, ai, &arg)) {
        return 0;
    }

    switch (mt) {
    case 0: {
        if (key_slot && c->strict) {
            CBOR_REJECT("string key required");
        }
        if (key_slot) {
            return cbor_key_diag(c, 0, arg, 0) ? 1 : 0;
        }
        if (arg <= (uint64_t)INT64_MAX) {
            return cbor_int_node(c, 0, arg, key_slot) ? 1 : 0;
        }
        return cbor_float_node(c, (double)arg, key_slot) ? 1 : 0;
    }
    case 1: {
        if (key_slot && c->strict) {
            CBOR_REJECT("string key required");
        }
        if (key_slot) {
            return cbor_key_diag(c, 1, arg, 0) ? 1 : 0;
        }
        if (arg <= (uint64_t)INT64_MAX + 1) { /* -1-2^63 == INT64_MIN */
            uint64_t mag = arg + 1;
            return cbor_int_node(c, 1, mag, key_slot) ? 1 : 0;
        }
        return cbor_float_node(c, -((double)arg + 1.0), key_slot) ? 1 : 0;
    }
    case 2:
    case 3: {
        if (arg > c->len - c->i) {
            CBOR_REJECT("truncated string");
        }
        if (mt == 3 && !yep_utf8_validate(c->p + c->i, (size_t)arg, NULL)) {
            yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, c->i,
                          "ill-formed UTF-8 in text string at byte %zu", c->i);
            c->status = YEPTRIS_ERROR_ENCODING;
            return 0;
        }
        if (key_slot && mt == 2 && c->strict) {
            CBOR_REJECT("string key required"); /* a byte string is not text */
        }
        if (key_slot && mt == 2) {
            int ok = cbor_key_diag(c, 2, arg, 0);
            c->i += (size_t)arg;
            return ok ? 1 : 0;
        }
        uint32_t id = cbor_span_node(c, c->p + c->i, (uint32_t)arg, (uint8_t)mt, key_slot);
        c->i += (size_t)arg;
        return id != UINT32_MAX ? 1 : 0;
    }
    case 4: {
        if (key_slot) {
            CBOR_REJECT("structure as map key");
        }
        if (arg > c->len - c->i) { /* every item consumes >= 1 byte */
            CBOR_REJECT("array overruns the input");
        }
        if (!cbor_open(c, 0, arg)) {
            return 0;
        }
        c->frame[c->depth - 1].rem = arg;
        c->frame[c->depth - 1].is_map = 0;
        return 1;
    }
    case 5: {
        if (key_slot) {
            CBOR_REJECT("structure as map key");
        }
        if (arg > (c->len - c->i) / 2) { /* pairs need 2 items each */
            CBOR_REJECT("map overruns the input");
        }
        if (!cbor_open(c, 1, arg * 2)) {
            return 0;
        }
        c->frame[c->depth - 1].rem = arg * 2;
        c->frame[c->depth - 1].is_map = 1;
        return 1;
    }
    case 6:
        if (!cbor_note_tag(c, arg)) {
            return 0;
        }
        return 2; /* the chain needs its content item */
    default:      /* 7 */
        break;
    }

    /* major type 7: simple values and floats */
    double dv = 0;
    switch (ai) {
    case 20:
        if (key_slot) {
            return cbor_key_diag(c, 7, 20, 0) ? 1 : 0;
        }
        return cbor_text_node(c, "false", 5, YEPTRIS_TAG_BOOL, key_slot) != UINT32_MAX ? 1 : 0;
    case 21:
        if (key_slot) {
            return cbor_key_diag(c, 7, 21, 0) ? 1 : 0;
        }
        return cbor_text_node(c, "true", 4, YEPTRIS_TAG_BOOL, key_slot) != UINT32_MAX ? 1 : 0;
    case 22:
        if (key_slot) {
            return cbor_key_diag(c, 7, 22, 0) ? 1 : 0;
        }
        return cbor_text_node(c, "null", 4, YEPTRIS_TAG_NULL, key_slot) != UINT32_MAX ? 1 : 0;
    case 23:
        if (key_slot) {
            return cbor_key_diag(c, 7, 23, 0) ? 1 : 0;
        }
        return cbor_text_node(c, "undefined", 9, 0, key_slot) != UINT32_MAX ? 1 : 0;
    case 24: {
        if (c->len - c->i < 1) {
            CBOR_REJECT("truncated simple value");
        }
        uint64_t v = c->p[c->i++];
        if (v < 32) {
            CBOR_REJECT("two-byte simple value below 32");
        }
        if (key_slot) {
            return cbor_key_diag(c, 7, v, 0) ? 1 : 0;
        }
        char buf[16];
        memcpy(buf, "simple(", 7);
        uint32_t n = 7 + cbor_u64dec(v, buf + 7);
        buf[n++] = ')';
        return cbor_text_node(c, buf, n, 0, key_slot) != UINT32_MAX ? 1 : 0;
    }
    case 25: {
        if (c->len - c->i < 2) {
            CBOR_REJECT("truncated half float");
        }
        uint16_t h = (uint16_t)((c->p[c->i] << 8) | c->p[c->i + 1]);
        c->i += 2;
        dv = cbor_half(h);
        break;
    }
    case 26: {
        if (c->len - c->i < 4) {
            CBOR_REJECT("truncated single float");
        }
        uint32_t w = ((uint32_t)c->p[c->i] << 24) | ((uint32_t)c->p[c->i + 1] << 16) |
                     ((uint32_t)c->p[c->i + 2] << 8) | (uint32_t)c->p[c->i + 3];
        float f;
        c->i += 4;
        memcpy(&f, &w, sizeof(f)); /* the single's bit pattern, widened */
        dv = (double)f;
        break;
    }
    case 27: {
        if (c->len - c->i < 8) {
            CBOR_REJECT("truncated double float");
        }
        uint64_t w = 0;
        for (int k = 0; k < 8; k++) {
            w = (w << 8) | c->p[c->i + (size_t)k];
        }
        c->i += 8;
        memcpy(&dv, &w, sizeof(dv));
        break;
    }
    default:
        if (ai < 20) { /* simple value 0..19 */
            if (key_slot) {
                return cbor_key_diag(c, 7, ai, 0) ? 1 : 0;
            }
            char buf[16];
            memcpy(buf, "simple(", 7);
            uint32_t n = 7 + cbor_u64dec(ai, buf + 7);
            buf[n++] = ')';
            return cbor_text_node(c, buf, n, 0, key_slot) != UINT32_MAX ? 1 : 0;
        }
        CBOR_REJECT("reserved additional information");
    }

    if (key_slot && c->strict) {
        CBOR_REJECT("string key required");
    }
    if (key_slot) {
        return cbor_key_diag(c, 8, 0, dv) ? 1 : 0;
    }
    return cbor_float_node(c, dv, key_slot) ? 1 : 0;
}

/* The DOM sink: the yeptris_cbor_decode public path. Each impl is the
 * pre-sink writer, byte for byte (the differential suite pins it). */

static int dom_sink_text(void* dp, const char* s, uint32_t n, uint8_t tag_id, int is_key,
                         const char* tag, uint32_t tag_len) {
    yep_dom* d = dp;
    (void)is_key;
    yep_view tag_v;
    const yep_view* tagp = NULL;
    if (tag != NULL) {
        char* dst = yep_dom_str_tail(d, tag_len);
        if (dst == NULL) {
            return 0;
        }
        memcpy(dst, tag, tag_len);
        yep_sview piece = yep_dom_str_commit(d, tag_len);
        tag_v.p = d->str + (piece.off & YEP_SV_OFF);
        tag_v.len = tag_len;
        tagp = &tag_v;
    }
    uint32_t id = dom_open_node(d, YEP_DOM_SCALAR, tagp, NULL, 0, YEP_STYLE_PLAIN, 1, 0);
    if (id == UINT32_MAX) {
        return 0;
    }
    yep_dnode* node = &d->nodes[id];
    char* dst = yep_dom_str_tail(d, n);
    if (dst == NULL) {
        return 0;
    }
    memcpy(dst, s, n);
    node->value = yep_dom_str_commit(d, n);
    node->tag_id = tag_id;
    return dom_place(d, id) == 0;
}

static int dom_sink_span(void* dp, const unsigned char* p, uint32_t n, int borrowed, int is_key,
                         const char* tag, uint32_t tag_len) {
    yep_dom* d = dp;
    (void)is_key;
    (void)borrowed; /* 0 rides the scratch buffer: still copied to the arena */
    yep_view tag_v;
    const yep_view* tagp = NULL;
    if (tag != NULL) {
        char* dst = yep_dom_str_tail(d, tag_len);
        if (dst == NULL) {
            return 0;
        }
        memcpy(dst, tag, tag_len);
        yep_sview piece = yep_dom_str_commit(d, tag_len);
        tag_v.p = d->str + (piece.off & YEP_SV_OFF);
        tag_v.len = tag_len;
        tagp = &tag_v;
    }
    uint32_t id = dom_open_node(d, YEP_DOM_SCALAR, tagp, NULL, 0, YEP_STYLE_DOUBLE_QUOTED, 0, 0);
    if (id == UINT32_MAX) {
        return 0;
    }
    yep_dnode* node = &d->nodes[id];
    if (borrowed) {
        node->value.off = (uint32_t)((const char*)p - d->input_base);
        node->value.len = n;
    } else if (n > 0) { /* the empty indefinite string needs no arena */
        char* dst = yep_dom_str_tail(d, n);
        if (dst == NULL) {
            return 0;
        }
        memcpy(dst, p, n);
        node->value = yep_dom_str_commit(d, n);
    }
    return dom_place(d, id) == 0;
}

static int dom_sink_str(void* dp, const unsigned char* p, uint32_t n, int borrowed, int is_key,
                        const char* tag, uint32_t tag_len) {
    return dom_sink_span(dp, p, n, borrowed, is_key, tag, tag_len);
}

static int dom_sink_bytes(void* dp, const unsigned char* p, uint32_t n, int borrowed, int is_key,
                          const char* tag, uint32_t tag_len) {
    return dom_sink_span(dp, p, n, borrowed, is_key, tag, tag_len);
}

static int dom_sink_open(void* dp, int is_map, uint64_t cap, const char* tag, uint32_t tag_len) {
    yep_dom* d = dp;
    (void)cap;
    yep_view tag_v;
    const yep_view* tagp = NULL;
    if (tag != NULL) {
        char* dst = yep_dom_str_tail(d, tag_len);
        if (dst == NULL) {
            return 0;
        }
        memcpy(dst, tag, tag_len);
        yep_sview piece = yep_dom_str_commit(d, tag_len);
        tag_v.p = d->str + (piece.off & YEP_SV_OFF);
        tag_v.len = tag_len;
        tagp = &tag_v;
    }
    uint32_t id =
        dom_open_node(d, is_map ? YEP_DOM_MAPPING : YEP_DOM_SEQUENCE, tagp, NULL, 0, 0, 0, 1);
    if (id == UINT32_MAX) {
        return 0;
    }
    if (dom_place(d, id) != 0) {
        return 0;
    }
    d->map_pending_key[d->depth] = 0;
    d->stack[d->depth++] = id;
    return 1;
}

static int dom_sink_close(void* dp, int is_map) {
    yep_dom* d = dp;
    (void)is_map;
    d->depth--;
    return 1;
}

static int dom_sink_int(void* dp, int negative, uint64_t mag, int is_key, const char* tag,
                        uint32_t tag_len) {
    /* the int path formats straight into the arena (the #742
     * profile's 26% lane): no stack buffer, no second copy through
     * the text sink — tail, format in place, commit */
    (void)is_key;
    yep_dom* d = dp;
    yep_view tag_v;
    const yep_view* tagp = NULL;
    if (tag != NULL) {
        char* tdst = yep_dom_str_tail(d, tag_len);
        if (tdst == NULL) {
            return 0;
        }
        memcpy(tdst, tag, tag_len);
        yep_sview piece = yep_dom_str_commit(d, tag_len);
        tag_v.p = d->str + (piece.off & YEP_SV_OFF);
        tag_v.len = tag_len;
        tagp = &tag_v;
    }
    uint32_t id = dom_open_node(d, YEP_DOM_SCALAR, tagp, NULL, 0, YEP_STYLE_PLAIN, 1, 0);
    if (id == UINT32_MAX) {
        return 0;
    }
    yep_dnode* node = &d->nodes[id];
    char* dst = yep_dom_str_tail(d, 21); /* '-' plus twenty digits */
    if (dst == NULL) {
        return 0;
    }
    uint32_t n = 0;
    if (negative) {
        dst[n++] = '-';
    }
    n += cbor_u64dec(mag, dst + n);
    node->value = yep_dom_str_commit(d, n);
    node->tag_id = YEPTRIS_TAG_INT;
    return dom_place(d, id) == 0;
}

static int dom_sink_float(void* dp, double dv, int is_key, const char* tag, uint32_t tag_len) {
    char buf[48];
    (void)is_key;
    uint32_t n;
    if (dv != dv) {
        memcpy(buf, "NaN", 3);
        n = 3;
    } else if (dv > 1.7976931348623157e308) {
        memcpy(buf, "Infinity", 8);
        n = 8;
    } else if (dv < -1.7976931348623157e308) {
        memcpy(buf, "-Infinity", 9);
        n = 9;
    } else {
        n = (uint32_t)yep_d2s_shortest(dv, buf);
    }
    return dom_sink_text(dp, buf, n, YEPTRIS_TAG_FLOAT, is_key, tag, tag_len);
}

static const yep_cbor_sink yep_cbor_dom_sink_impl = {
    NULL,         dom_sink_text,  dom_sink_int,  dom_sink_float,
    dom_sink_str, dom_sink_bytes, dom_sink_open, dom_sink_close,
};

int yep_cbor_decode_gen(const unsigned char* p, size_t len, int strict, size_t* consumed,
                        const yep_cbor_sink* sink) {
    yep_cdec c;
    memset(&c, 0, sizeof(c));
    c.s = sink;
    c.p = p;
    c.len = len;
    c.strict = strict;
    c.status = YEPTRIS_OK;
    int done_top = 0; /* the ONE top-level item is done (tags never count) */
    int rc;
    for (;;) {
        if (c.depth == 0) {
            if (done_top) { /* the root closed; one data item per call */
                if (c.i != len && consumed == NULL) {
                    cbor_fail(&c, YEP_ERR_UNEXPECTED, "trailing bytes after the data item");
                    rc = YEPTRIS_ERROR_PARSE;
                    goto done;
                }
                if (consumed != NULL) {
                    *consumed = c.i;
                }
                rc = YEPTRIS_OK;
                goto done;
            }
            int st = cbor_item(&c);
            if (st == 0) {
                rc = (int)c.status;
                goto done;
            }
            if (st == 2) {
                continue; /* tag chain: the content item follows */
            }
            done_top = 1; /* scalar root finishes at the tail check above */
            continue;
        }
        yep_cframe* fr = &c.frame[c.depth - 1];
        if (fr->rem == 0) { /* definite container complete */
            if (!c.s->close(c.s->ctx, fr->is_map)) {
                rc = YEPTRIS_ERROR_MEMORY;
                goto done;
            }
            c.depth--;
            continue;
        }
        if (fr->rem == YEP_CBOR_INDEF) {
            if (c.i < c.len && c.p[c.i] == 0xFF) {
                if (fr->is_map && fr->consumed % 2 == 1) {
                    cbor_fail(&c, YEP_ERR_UNEXPECTED,
                              "break in a map value position (odd item count)");
                    rc = YEPTRIS_ERROR_PARSE;
                    goto done;
                }
                c.i++;
                if (!c.s->close(c.s->ctx, fr->is_map)) {
                    rc = YEPTRIS_ERROR_MEMORY;
                    goto done;
                }
                c.depth--;
                continue;
            }
        }
        size_t parent = (size_t)(c.depth - 1);
        int st = cbor_item(&c);
        if (st == 0) {
            rc = (int)c.status;
            goto done;
        }
        if (st == 2) {
            continue; /* tags never consume a container slot */
        }
        c.frame[parent].consumed++;
        if (c.frame[parent].rem != YEP_CBOR_INDEF) {
            c.frame[parent].rem--;
            /* one item (scalar, or a container now open) consumed a parent slot */
        }
    }
done:
    free(c.ibuf);
    return rc;
}

int yep_cbor_decode_dom_sink(void* dp, const unsigned char* p, size_t len, int strict,
                             size_t* consumed) {
    yep_cbor_sink s = yep_cbor_dom_sink_impl;
    s.ctx = dp;
    return yep_cbor_decode_gen(p, len, strict, consumed, &s);
}

int yep_cbor_decode_dom(yep_dom* d, const unsigned char* p, size_t len, int strict,
                        size_t* consumed) {
    return yep_cbor_decode_dom_sink(d, p, len, strict, consumed);
}

static YeptrisDocument cbor_wrap(yep_dom* dom, const void* buf, const yep_allocator* sys) {
    yeptris_document* d = yep_alloc(sys, sizeof(yeptris_document));
    if (d == NULL) {
        yep_dom_destroy(dom);
        return NULL;
    }
    yep_mutex_init(&d->lazy_mu);
    d->dom = dom;
    d->sys = sys;
    d->schema = YEPTRIS_SCHEMA_12_CORE;
    d->transcoded = NULL;
    d->transcoded_len = 0;
    d->input = (const char*)buf;
    d->finish_pool = NULL;
    d->lazy_tape = NULL; /* field-by-field ctor: no garbage for free */
    d->lazy_kind = 0;
    return (YeptrisDocument)d;
}

YEPTRIS_API size_t yeptris_cbor_decode_sequence(const void* buf, size_t len, uint32_t opts,
                                                yeptris_cbor_item_cb cb, void* ctx,
                                                YeptrisStatus* status) {
    if ((buf == NULL && len != 0) || cb == NULL || (opts & ~YEPTRIS_CBOR_STRICT) != 0) {
        if (status != NULL) {
            *status = YEPTRIS_ERROR_ARG;
        }
        return 0;
    }
    int strict = (opts & YEPTRIS_CBOR_STRICT) != 0;
    const yep_allocator* sys = yep_system_allocator();
    const unsigned char* p = (const unsigned char*)buf;
    size_t i = 0;
    size_t count = 0;
    for (;;) {
        if (i >= len) {
            if (status != NULL) {
                *status = YEPTRIS_OK; /* RFC 8742: an empty sequence is valid */
            }
            return count;
        }
        yep_dom* dom = yep_dom_create(sys);
        if (dom == NULL) {
            if (status != NULL) {
                *status = YEPTRIS_ERROR_MEMORY;
            }
            return count;
        }
        dom->input_base = (const char*)(p + i); /* item-relative borrows */
        dom->input_len = len - i;
        size_t used = 0;
        YeptrisStatus st = (YeptrisStatus)yep_cbor_decode_dom(dom, p + i, len - i, strict, &used);
        if (st != YEPTRIS_OK) {
            yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, i,
                          "CBOR sequence item %zu at byte %zu is not well-formed", count, i);
            yep_dom_destroy(dom);
            if (status != NULL) {
                *status = st == YEPTRIS_ERROR_MEMORY ? st : YEPTRIS_ERROR_PARSE;
            }
            return count;
        }
        YeptrisDocument doc = cbor_wrap(dom, p + i, sys);
        if (doc == NULL) {
            if (status != NULL) {
                *status = YEPTRIS_ERROR_MEMORY;
            }
            return count;
        }
        if (cb(ctx, doc, count) != 0) { /* the callback owns each item;
                                           nonzero aborts the iteration */
            if (status != NULL) {
                *status = YEPTRIS_OK;
            }
            return count;
        }
        i += used;
        count++;
    }
}

YEPTRIS_API YeptrisDocument yeptris_cbor_decode(const void* buf, size_t len, uint32_t opts,
                                                YeptrisStatus* status) {
    if ((buf == NULL && len != 0) || (opts & ~YEPTRIS_CBOR_STRICT) != 0) {
        if (status != NULL) {
            *status = YEPTRIS_ERROR_ARG;
        }
        return NULL;
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_dom* dom = yep_dom_create(sys);
    if (dom == NULL) {
        if (status != NULL) {
            *status = YEPTRIS_ERROR_MEMORY;
        }
        return NULL;
    }
    dom->input_base = (const char*)buf;
    dom->input_len = len;
    YeptrisStatus st = (YeptrisStatus)yep_cbor_decode_dom(dom, (const unsigned char*)buf, len,
                                                          (opts & YEPTRIS_CBOR_STRICT) != 0, NULL);
    if (st != YEPTRIS_OK) {
        yep_dom_destroy(dom);
        if (status != NULL) {
            *status = st;
        }
        return NULL;
    }
    yeptris_document* doc = yep_alloc(sys, sizeof(yeptris_document));
    if (doc == NULL) {
        yep_dom_destroy(dom);
        if (status != NULL) {
            *status = YEPTRIS_ERROR_MEMORY;
        }
        return NULL;
    }
    yep_mutex_init(&doc->lazy_mu);
    doc->dom = dom;
    doc->sys = sys;
    doc->schema = YEPTRIS_SCHEMA_12_CORE; /* CBOR's model is the core schema */
    doc->transcoded = NULL;
    doc->transcoded_len = 0;
    doc->input = (const char*)buf;
    doc->finish_pool = NULL;
    doc->lazy_tape = NULL; /* field-by-field ctor: no garbage for free */
    if (status != NULL) {
        *status = YEPTRIS_OK;
    }
    return (YeptrisDocument)doc;
}

YEPTRIS_API YeptrisStatus yeptris_cbor_decode_sax(const void* buf, size_t len, uint32_t opts,
                                                  const yeptris_cbor_sax* sax, size_t* consumed,
                                                  YeptrisStatus* status) {
    if ((buf == NULL && len != 0) || sax == NULL || (opts & ~YEPTRIS_CBOR_STRICT) != 0 ||
        sax->text == NULL || sax->int_val == NULL || sax->float_val == NULL ||
        sax->str_val == NULL || sax->bytes_val == NULL || sax->open == NULL || sax->close == NULL) {
        if (status != NULL) {
            *status = YEPTRIS_ERROR_ARG;
        }
        return YEPTRIS_ERROR_ARG;
    }
    /* yeptris_cbor_sax IS yep_cbor_sink (sink.h aliases the public
     * face): the decoder drives the caller's vtable directly. */
    YeptrisStatus st = (YeptrisStatus)yep_cbor_decode_gen((const unsigned char*)buf, len,
                                                          (opts & YEPTRIS_CBOR_STRICT) != 0,
                                                          consumed, (const yep_cbor_sink*)sax);
    if (status != NULL) {
        *status = st;
    }
    return st;
}
