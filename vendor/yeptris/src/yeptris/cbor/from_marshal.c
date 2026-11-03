/* from_marshal.c — Ruby Marshal.dump bytes to ONE CBOR data item
 * (yeptris-ruby#322's CBOR half: the binding's dump face).
 *
 * The twin of emit/writer.c's marshal READER (yeptris_json_from_marshal):
 * the Marshal stream is walked once, in C, and the CBOR item is
 * appended straight to the output — no per-node Ruby crossings, no
 * Document, no YAMLTree. The format facts mirror marshal.c's emission
 * contract (pinned against Ruby 3.4): w_long small form x==0 -> 0x00,
 * |x| <= 122 -> sign*(|x|+5), size form tag +-k with LE payload; 'I'
 * ivar wrappers; arrays/hashes/strings/floats/bignums register for
 * '@' links (0-based stream order, wrappers transparent).
 *
 * '@' links DUPLICATE the registered construct's emitted bytes when the
 * construct is a host SCALAR (strings, floats, bignums, symbols): the
 * per-node route never anchors host scalars, so it emits the value
 * twice. Arrays and hashes are anchored there (the "&1" / "*1" pair)
 * and the CBOR encoder refuses anchors — a link to one is UNSUPPORTED,
 * and so is a link to a construct still being emitted (a cycle).
 *
 * Parity ledger (empirically pinned against the per-node route; the
 * route's own machinery is binding/lib/yeptris/psych/visitors.rb +
 * Builder#build_string + cbor/encode.c):
 * - Symbols spell ":name" (the YAMLTree's `":#{sym}"` scalar), NOT the
 *   bare name the JSON face emits; symbols with non-UTF-8 bytes ride a
 *   byte string (the YAMLTree scalar's encoder fallback)
 * - a String VALUE is emitted only when the per-node route leaves it a
 *   text scalar: ASCII-8BIT strings must be ASCII-only (non-ASCII ones
 *   take the !binary tag the CBOR encoder refuses); 'I'-wrapped UTF-8
 *   strings must be valid UTF-8 (the Psych plain-safety regexp raises
 *   otherwise); US-ASCII strings must be ASCII-only; strings in any
 *   other named encoding are UNSUPPORTED
 * - a String KEY loses its encoding in the per-node route (map_add
 *   stores the raw bytes), so the key law is byte-based: valid UTF-8
 *   takes the compat-11 resolver's verdict — INT/FLOAT/BOOL/NULL keys
 *   (numeric-looking text, y/yes/on/true, ~/null, the empty key) are
 *   UNSUPPORTED, everything else is text; invalid UTF-8 rides a byte
 *   string
 * - the reserved diagnostic words ("undefined", "simple(N)") reproduce
 *   encode.c's simple ledger on text keys and values
 * - map keys also keep the per-node route's duplicate check: two keys
 *   whose stored TEXT matches fail there. Among supported keys equal
 *   emitted bytes === equal text, so the check is a pairwise scan of
 *   the emitted key spans
 * - bignums up to four 16-bit words ride the integer law inside int64;
 *   past either end they re-render as the preferred float of their
 *   magnitude (the fallback number-kernel's ledger, sign included).
 *   Wider bignums and non-finite float text are UNSUPPORTED
 * - booleans in key position are UNSUPPORTED: the per-node canonical
 *   sort keys bools by their TEXT ("true") but emits f4/f5
 *
 * Canonical mode sorts each map's pairs by the bytewise order of
 * their ENCODED keys (encode.c's s4.2.1 insertion sort, stable); a
 * per-map scratch buffer holds the stream-order pairs, then the
 * sorted spans copy into the item. Registration order stays the
 * STREAM's — only the bytes are reordered. */

#include "encode_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yeptris/cbor.h>
#include <yeptris/marshal.h>
#include <yeptris/resolve.h>

#include "../encoding/encoding.h"
#include "../resolve/resolver.h"

#define CM_MAX_DEPTH 512

/* a string VALUE's Ruby encoding, as the per-node route sees it */
#define CM_ENC_BINARY 0  /* no :E ivar — ASCII-8BIT */
#define CM_ENC_UTF8 1    /* :E true */
#define CM_ENC_USASCII 2 /* :E false */
#define CM_ENC_NAMED 3   /* :E names another encoding */

typedef struct {
    const char* p;
    size_t len;
    size_t at;
} cm_stream;

typedef struct {
    const char* p;
    uint32_t len;
} cm_sym;

typedef struct {
    uint8_t* p;
    size_t len;
    size_t cap;
} cm_buf;

typedef struct {
    cm_buf* buf; /* where the construct's bytes were emitted (NULL: consumed) */
    size_t off;
    size_t len;         /* 0 until the construct completes: a link is a cycle */
    unsigned char kind; /* the wrapped value's lead byte ('I' looks through) */
    /* a String keeps its payload here: a link lands in a position whose
     * role law (value or map key) may re-spell it, so the span copy is
     * not enough. senc is -1 for every non-string construct. */
    const char* sp;
    uint32_t slen;
    int senc;
} cm_obj;

typedef struct {
    cm_stream s;
    cm_sym* syms;
    size_t syms_n, syms_cap;
    cm_obj* objs;
    size_t objs_n, objs_cap;
    cm_buf** keep; /* every buffer, owed until the walk ends */
    size_t keep_n, keep_cap;
    uint32_t opts;
    int failed; /* 0 ok, else the YeptrisStatus to return */
    int no_reg; /* an 'I'-wrapped construct registers ONCE (at the 'I') */
    int in_key; /* the next walk emits a map key (the key law applies) */
    int depth;
} cm_ctx;

static int cm_fail(cm_ctx* c, YeptrisStatus st) {
    if (c->failed == 0) {
        c->failed = (int)st;
    }
    return -1;
}

/* ---- stream ---------------------------------------------------------- */

static int cm_byte(cm_ctx* c, unsigned char* out) {
    if (c->s.at >= c->s.len) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    *out = (unsigned char)c->s.p[c->s.at++];
    return 0;
}

/* the w_long inverse (marshal.c's m_w_long) */
static int cm_long(cm_ctx* c, int64_t* out) {
    unsigned char b;
    if (cm_byte(c, &b) != 0) {
        return -1;
    }
    if (b == 0) {
        *out = 0;
        return 0;
    }
    if (b <= 4) { /* positive size form, k = b */
        int64_t v = 0;
        for (int i = 0; i < (int)b; i++) {
            unsigned char d;
            if (cm_byte(c, &d) != 0) {
                return -1;
            }
            v |= (int64_t)d << (8 * i);
        }
        *out = v;
        return 0;
    }
    if (b <= 127) {
        *out = (int64_t)b - 5;
        return 0;
    }
    if (b >= 129 && b <= 250) {
        *out = (int64_t)b - 251;
        return 0;
    }
    { /* 251..255: negative size form */
        int bytes = 256 - (int)b;
        int64_t v = 0;
        for (int i = 0; i < bytes; i++) {
            unsigned char d;
            if (cm_byte(c, &d) != 0) {
                return -1;
            }
            v |= (int64_t)d << (8 * i);
        }
        *out = v - ((int64_t)1 << (8 * bytes));
        return 0;
    }
}

/* length-prefixed bytes with a bounds check */
static int cm_take(cm_ctx* c, int64_t len, const char** p) {
    if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at)) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    *p = c->s.p + c->s.at;
    c->s.at += (size_t)len;
    return 0;
}

/* ---- output buffers -------------------------------------------------- */

static int cm_reserve(cm_ctx* c, cm_buf* b, size_t extra) {
    if (b->cap - b->len >= extra) {
        return 0;
    }
    size_t cap = b->cap ? b->cap : 64;
    while (cap - b->len < extra) {
        if (cap > (size_t)-1 / 2) {
            return cm_fail(c, YEPTRIS_ERROR_MEMORY);
        }
        cap *= 2;
    }
    uint8_t* np = (uint8_t*)realloc(b->p, cap);
    if (np == NULL) {
        return cm_fail(c, YEPTRIS_ERROR_MEMORY);
    }
    b->p = np;
    b->cap = cap;
    return 0;
}

static cm_buf* cm_buf_new(cm_ctx* c) {
    cm_buf* b = (cm_buf*)calloc(1, sizeof(*b));
    if (b == NULL) {
        cm_fail(c, YEPTRIS_ERROR_MEMORY);
        return NULL;
    }
    if (c->keep_n == c->keep_cap) {
        size_t cap = c->keep_cap ? c->keep_cap * 2 : 8;
        cm_buf** nk = (cm_buf**)realloc(c->keep, cap * sizeof(*nk));
        if (nk == NULL) {
            free(b);
            cm_fail(c, YEPTRIS_ERROR_MEMORY);
            return NULL;
        }
        c->keep = nk;
        c->keep_cap = cap;
    }
    c->keep[c->keep_n++] = b;
    return b;
}

static int cm_put(cm_ctx* c, cm_buf* b, const void* p, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (cm_reserve(c, b, n) != 0) {
        return -1;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 0;
}

static int cm_byte_out(cm_ctx* c, cm_buf* b, uint8_t v) {
    if (cm_reserve(c, b, 1) != 0) {
        return -1;
    }
    b->p[b->len++] = v;
    return 0;
}

static int cm_head(cm_ctx* c, cm_buf* b, uint8_t mt, uint64_t arg) {
    if (cm_reserve(c, b, 9) != 0) {
        return -1;
    }
    size_t pos = 0;
    yep_cbor_put_head(b->p + b->len, &pos, mt, arg);
    b->len += pos;
    return 0;
}

static int cm_ascii_only(const char* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if ((unsigned char)p[i] >= 0x80) {
            return 0;
        }
    }
    return 1;
}

static int cm_text(cm_ctx* c, cm_buf* b, const char* p, uint32_t n) {
    int sv = yep_cbor_simple_of(p, n);
    if (sv >= 0) { /* the reserved diagnostic words (encode.c's ledger) */
        if (sv < 24) {
            return cm_byte_out(c, b, (uint8_t)(0xE0 | (uint8_t)sv));
        }
        return cm_byte_out(c, b, 0xF8) != 0 ? -1 : cm_byte_out(c, b, (uint8_t)sv);
    }
    if (n > 0 && !yep_utf8_validate((const unsigned char*)p, n, NULL)) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    if (cm_head(c, b, 3, n) != 0) {
        return -1;
    }
    return cm_put(c, b, p, n);
}

/* a symbol spells ":name" (the YAMLTree scalar the per-node route
 * emits); invalid UTF-8 rides a byte string like encode.c's fallback */
static int cm_symbol_text(cm_ctx* c, cm_buf* b, const char* p, uint32_t n) {
    uint8_t mt = n == 0 || yep_utf8_validate((const unsigned char*)p, n, NULL) ? 3 : 2;
    if (cm_head(c, b, mt, (uint64_t)n + 1) != 0) {
        return -1;
    }
    if (cm_byte_out(c, b, ':') != 0) {
        return -1;
    }
    return cm_put(c, b, p, n);
}

/* A map KEY carries no encoding in the per-node route: map_add stores
 * the raw bytes as a PLANE scalar and the document's compat-11
 * resolver retypes it. INT/FLOAT/BOOL/NULL verdicts are UNSUPPORTED
 * (the fallback's number/bool/null key spelling is the only behavior;
 * an empty key is the NULL verdict). Text keys keep the simple
 * ledger; invalid UTF-8 becomes a byte string (encode.c's
 * cbor_encode_key). */
static int cm_key_bytes(cm_ctx* c, cm_buf* b, const char* p, uint32_t n) {
    if (n > 0 && !yep_utf8_validate((const unsigned char*)p, n, NULL)) {
        if (cm_head(c, b, 2, n) != 0) {
            return -1;
        }
        return cm_put(c, b, p, n);
    }
    yep_tag_id tag = yep_resolver_compat11()->resolve(NULL, p, n);
    if (tag == YEPTRIS_TAG_INT || tag == YEPTRIS_TAG_FLOAT || tag == YEPTRIS_TAG_BOOL ||
        tag == YEPTRIS_TAG_NULL) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    return cm_text(c, b, p, n);
}

/* A string VALUE keeps the per-node route's Builder#build_string law:
 * non-ASCII ASCII-8BIT strings take the !binary tag the encoder
 * refuses; UTF-8 must validate (the plain-safety regexp raises);
 * US-ASCII must be 7-bit; named encodings are UNSUPPORTED. */
static int cm_value_bytes(cm_ctx* c, cm_buf* b, const char* p, uint32_t n, int enc) {
    int ascii = cm_ascii_only(p, n);
    switch (enc) {
    case CM_ENC_UTF8:
        if (!ascii && !yep_utf8_validate((const unsigned char*)p, n, NULL)) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        break;
    case CM_ENC_BINARY:
    case CM_ENC_USASCII:
        if (!ascii) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        break;
    default:
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    return cm_text(c, b, p, n);
}

/* ---- tables ---------------------------------------------------------- */

static int cm_symbol(cm_ctx* c, int emit, cm_buf* out) {
    int64_t len;
    if (cm_long(c, &len) != 0) {
        return -1;
    }
    const char* name;
    if (cm_take(c, len, &name) != 0) {
        return -1;
    }
    if (c->syms_n == c->syms_cap) {
        size_t cap = c->syms_cap ? c->syms_cap * 2 : 16;
        cm_sym* ns = (cm_sym*)realloc(c->syms, cap * sizeof(*ns));
        if (ns == NULL) {
            return cm_fail(c, YEPTRIS_ERROR_MEMORY);
        }
        c->syms = ns;
        c->syms_cap = cap;
    }
    c->syms[c->syms_n].p = name;
    c->syms[c->syms_n].len = (uint32_t)len;
    c->syms_n++;
    if (emit) {
        return cm_symbol_text(c, out, name, (uint32_t)len);
    }
    return 0;
}

/* the object memo entry for a registering construct. Returns the index
 * or -1 (failed). */
static int cm_obj_register(cm_ctx* c, cm_buf* out, unsigned char kind) {
    if (c->objs_n == c->objs_cap) {
        size_t cap = c->objs_cap ? c->objs_cap * 2 : 16;
        cm_obj* no = (cm_obj*)realloc(c->objs, cap * sizeof(*no));
        if (no == NULL) {
            cm_fail(c, YEPTRIS_ERROR_MEMORY);
            return -1;
        }
        c->objs = no;
        c->objs_cap = cap;
    }
    size_t idx = c->objs_n++;
    c->objs[idx].buf = out;
    c->objs[idx].off = out != NULL ? out->len : 0;
    c->objs[idx].len = 0;
    c->objs[idx].kind = kind;
    c->objs[idx].sp = NULL;
    c->objs[idx].slen = 0;
    c->objs[idx].senc = -1;
    return (int)idx;
}

/* '@' link. A linked STRING is re-spelled by the role law of the
 * position it lands in (a value keeps Builder#build_string's encoding
 * rules; a map key resolves like every other key text) — the per-node
 * route duplicates the scalar into that position, so a copied span
 * would carry the wrong role's spelling. Every other registering
 * construct copies its span: floats encode identically in both roles,
 * and containers are declined (the per-node route anchors them). */
static int cm_emit_memo(cm_ctx* c, const cm_obj* o, cm_buf* out) {
    if (out == NULL) {
        return 0; /* consumed, not emitted */
    }
    if (o->senc >= 0) {
        return c->in_key ? cm_key_bytes(c, out, o->sp, o->slen)
                         : cm_value_bytes(c, out, o->sp, o->slen, o->senc);
    }
    if (o->buf == NULL) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED); /* consumed, not emitted */
    }
    if (o->len == 0) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED); /* a cycle */
    }
    if (o->kind == '[' || o->kind == '{') {
        /* the per-node route anchors containers; the encoder refuses
         * the alias — the fallback raises */
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    if (cm_reserve(c, out, o->len) != 0) {
        return -1;
    }
    /* the span may live in out itself: reserve first, then memmove */
    memmove(out->p + out->len, o->buf->p + o->off, o->len);
    out->len += o->len;
    return 0;
}

static int cm_value(cm_ctx* c, cm_buf* out);

/* The count + (symbol, value) pairs that follow an 'I' wrapper. The
 * values are consumed (out == NULL) but still REGISTER — Marshal's
 * link slots are stream-order, context-free. When enc is non-NULL the
 * encoding entries are captured: ":E" carries true (UTF-8) / false
 * (US-ASCII), and a named encoding rides its own ":encoding" ivar. */
static int cm_ivar_pairs(cm_ctx* c, int* enc) {
    int64_t count;
    if (cm_long(c, &count) != 0) {
        return -1;
    }
    if (count < 0) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    for (int64_t i = 0; i < count; i++) {
        unsigned char nt;
        if (cm_byte(c, &nt) != 0) {
            return -1;
        }
        int is_e = 0;
        int is_enc = 0;
        if (nt == ':') {
            if (cm_symbol(c, 0, NULL) != 0) {
                return -1;
            }
            const cm_sym* s = &c->syms[c->syms_n - 1];
            is_e = s->len == 1 && s->p[0] == 'E';
            is_enc = s->len == 8 && memcmp(s->p, "encoding", 8) == 0;
        } else if (nt == ';') {
            int64_t sidx;
            if (cm_long(c, &sidx) != 0) {
                return -1;
            }
            if (sidx < 0 || (size_t)sidx >= c->syms_n) {
                return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
            }
            is_e = c->syms[sidx].len == 1 && c->syms[sidx].p[0] == 'E';
            is_enc = c->syms[sidx].len == 8 && memcmp(c->syms[sidx].p, "encoding", 8) == 0;
        } else {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        if (enc != NULL && (is_e || is_enc)) {
            if (c->s.at >= c->s.len) {
                return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
            }
            unsigned char vt = (unsigned char)c->s.p[c->s.at];
            *enc = is_enc      ? CM_ENC_NAMED
                   : vt == 'T' ? CM_ENC_UTF8
                   : vt == 'F' ? CM_ENC_USASCII
                               : CM_ENC_NAMED;
        }
        if (cm_value(c, NULL) != 0) { /* the ivar value: walked, not emitted */
            return -1;
        }
    }
    return 0;
}

/* 'I' + '"': the encoding ivar follows the payload, and the value law
 * needs it — parse both here instead of the transparent wrapper path.
 * reg_idx is the wrapper's own memo slot; the payload recorded there
 * lets a later '@' re-spell the string by its landing role. */
static int cm_wrapped_string(cm_ctx* c, cm_buf* out, int reg_idx) {
    unsigned char inner;
    if (cm_byte(c, &inner) != 0) {
        return -1;
    }
    if (inner != '"') {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    int64_t len;
    if (cm_long(c, &len) != 0) {
        return -1;
    }
    const char* p;
    if (cm_take(c, len, &p) != 0) {
        return -1;
    }
    int enc = CM_ENC_BINARY; /* no :E ivar: the original was ASCII-8BIT */
    if (cm_ivar_pairs(c, &enc) != 0) {
        return -1;
    }
    if (reg_idx >= 0) {
        c->objs[reg_idx].sp = p;
        c->objs[reg_idx].slen = (uint32_t)len;
        c->objs[reg_idx].senc = enc;
    }
    if (out == NULL) {
        return 0;
    }
    if (c->in_key) {
        return cm_key_bytes(c, out, p, (uint32_t)len);
    }
    return cm_value_bytes(c, out, p, (uint32_t)len, enc);
}

/* the per-node route's map-key spellings: strings (plain or 'I'
 * wrapped), symbols and their links, integers, floats, bignums and
 * object links. nil/array/hash/object/boolean keys stay with the
 * fallback — its to_s/raise behavior (and the bool sort-key split) is
 * the only behavior. */
static int cm_key_lead_ok(unsigned char lead) {
    return lead == '"' || lead == 'I' || lead == ':' || lead == ';' || lead == '@' || lead == 'i' ||
           lead == 'l' || lead == 'f';
}

/* one map key (the key law applies to the construct's lead) */
static int cm_map_key(cm_ctx* c, cm_buf* b) {
    int was = c->in_key;
    c->in_key = 1;
    int rc = cm_value(c, b);
    c->in_key = was;
    return rc;
}

/* Walks one value. out == NULL: consume only (ivar values). A
 * registering construct ('[', '{', '"', 'f', 'l', an 'I' wrapper)
 * closes its memo span once its bytes are out — the JSON reader's
 * single-close-out law. A bignum takes a slot even though Ruby never
 * emits a link to one: the slot count is what later '@' indices
 * address. */
static int cm_value(cm_ctx* c, cm_buf* out) {
    unsigned char t;
    int rc = 0;
    int reg_idx = -1;
    int register_this = !c->no_reg;
    c->no_reg = 0;
    if (c->depth >= CM_MAX_DEPTH) {
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    if (cm_byte(c, &t) != 0) {
        return -1;
    }
    if (register_this && (t == '[' || t == '{' || t == '"' || t == 'f' || t == 'l' || t == 'I')) {
        unsigned char kind = t;
        if (t == 'I') { /* the wrapper is transparent for links */
            kind = c->s.at < c->s.len ? (unsigned char)c->s.p[c->s.at] : 0;
        }
        reg_idx = cm_obj_register(c, out, kind);
        if (reg_idx < 0) {
            return -1;
        }
    }
    switch (t) {
    case '0':
        rc = out != NULL ? cm_byte_out(c, out, 0xF6) : 0;
        break;
    case 'T':
        rc = out != NULL ? cm_byte_out(c, out, 0xF5) : 0;
        break;
    case 'F':
        rc = out != NULL ? cm_byte_out(c, out, 0xF4) : 0;
        break;
    case 'i': {
        int64_t v;
        if (cm_long(c, &v) != 0) {
            return -1;
        }
        if (out != NULL) {
            rc =
                v >= 0 ? cm_head(c, out, 0, (uint64_t)v) : cm_head(c, out, 1, (uint64_t)(-(v + 1)));
        }
        break;
    }
    case 'l': {
        unsigned char sign;
        if (cm_byte(c, &sign) != 0) {
            return -1;
        }
        if (sign != 0x2b && sign != 0x2d) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        int64_t nlimbs;
        if (cm_long(c, &nlimbs) != 0) {
            return -1;
        }
        if (nlimbs < 1 || (uint64_t)(nlimbs * 2) > (uint64_t)(c->s.len - c->s.at)) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        if (nlimbs > 4) { /* beyond four 16-bit words: the fallback's floats */
            c->s.at += (size_t)(nlimbs * 2);
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        uint64_t mag = 0;
        for (int64_t i = 0; i < nlimbs; i++) {
            unsigned char lo, hi;
            if (cm_byte(c, &lo) != 0 || cm_byte(c, &hi) != 0) {
                return -1;
            }
            mag |= (uint64_t)(lo | ((uint64_t)hi << 8)) << (16 * i);
        }
        if (out == NULL) {
            break;
        }
        if (sign == 0x2d) {
            if (mag <= ((uint64_t)1 << 63)) {
                rc = mag == 0 ? cm_head(c, out, 0, 0) : cm_head(c, out, 1, mag - 1);
            } else {
                /* past int64 the per-node route re-renders the scalar as
                 * a preferred float, sign included */
                double d = -(double)mag;
                uint8_t fw = yep_cbor_float_width(d);
                if (cm_reserve(c, out, 9) != 0) {
                    return -1;
                }
                size_t pos = 0;
                yep_cbor_put_float(out->p + out->len, &pos, d, fw);
                out->len += pos;
            }
        } else if (mag <= (uint64_t)INT64_MAX) {
            rc = cm_head(c, out, 0, mag);
        } else {
            /* the per-node route re-renders out-of-int64 integers as
             * preferred floats (the number kernel's ledger) */
            double d = (double)mag;
            uint8_t fw = yep_cbor_float_width(d);
            if (cm_reserve(c, out, 9) != 0) {
                return -1;
            }
            size_t pos = 0;
            yep_cbor_put_float(out->p + out->len, &pos, d, fw);
            out->len += pos;
        }
        break;
    }
    case 'f': {
        int64_t len;
        if (cm_long(c, &len) != 0) {
            return -1;
        }
        const char* p;
        if (cm_take(c, len, &p) != 0) {
            return -1;
        }
        if (out == NULL) {
            break;
        }
        char tmp[64];
        if (len >= (int64_t)sizeof(tmp)) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        memcpy(tmp, p, (size_t)len);
        tmp[len] = '\0';
        if (strcmp(tmp, "inf") == 0 || strcmp(tmp, "-inf") == 0 || strcmp(tmp, "nan") == 0) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED); /* the host loop raises */
        }
        double d = strtod(tmp, NULL);
        if (!isfinite(d)) {
            /* the literal spellings gate above; a strtod OVERFLOW
             * ("1e999") manufactures the same non-finite without
             * spelling it — the same strict reject (Ruby streams only
             * ever carry finites or the literal inf/nan spellings) */
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        uint8_t fw = yep_cbor_float_width(d);
        if (cm_reserve(c, out, 9) != 0) {
            return -1;
        }
        size_t pos = 0;
        yep_cbor_put_float(out->p + out->len, &pos, d, fw);
        out->len += pos;
        break;
    }
    case '"': {
        int64_t len;
        if (cm_long(c, &len) != 0) {
            return -1;
        }
        const char* p;
        if (cm_take(c, len, &p) != 0) {
            return -1;
        }
        if (reg_idx >= 0) {
            c->objs[reg_idx].sp = p;
            c->objs[reg_idx].slen = (uint32_t)len;
            c->objs[reg_idx].senc = CM_ENC_BINARY;
        }
        if (out != NULL) {
            rc = c->in_key ? cm_key_bytes(c, out, p, (uint32_t)len)
                           : cm_value_bytes(c, out, p, (uint32_t)len, CM_ENC_BINARY);
        }
        break;
    }
    case 'I': {
        if (c->s.at < c->s.len && c->s.p[c->s.at] == '"') {
            rc = cm_wrapped_string(c, out, reg_idx);
        } else if (c->in_key) {
            /* an I-wrapped array/hash key is the container's TEXT in
             * the per-node route ("[1]") — not expressible here */
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        } else {
            c->no_reg = 1;
            rc = cm_value(c, out);
            c->no_reg = 0;
            if (rc == 0) {
                rc = cm_ivar_pairs(c, NULL);
            }
        }
        break;
    }
    case ':':
        rc = out != NULL ? cm_symbol(c, 1, out) : cm_symbol(c, 0, NULL);
        break;
    case ';': {
        int64_t idx;
        if (cm_long(c, &idx) != 0) {
            return -1;
        }
        if (idx < 0 || (size_t)idx >= c->syms_n) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        if (out != NULL) {
            rc = cm_symbol_text(c, out, c->syms[idx].p, c->syms[idx].len);
        }
        break;
    }
    case '@': {
        int64_t idx;
        if (cm_long(c, &idx) != 0) {
            return -1;
        }
        if (idx < 0 || (size_t)idx >= c->objs_n) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        rc = cm_emit_memo(c, &c->objs[idx], out);
        break;
    }
    case '[': {
        int64_t count;
        if (cm_long(c, &count) != 0) {
            return -1;
        }
        if (count < 0) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        if (out != NULL && cm_head(c, out, 4, (uint64_t)count) != 0) {
            return -1;
        }
        c->depth++;
        for (int64_t i = 0; i < count; i++) {
            if (cm_value(c, out) != 0) {
                c->depth--;
                return -1;
            }
        }
        c->depth--;
        break;
    }
    case '{': {
        int64_t count;
        if (cm_long(c, &count) != 0) {
            return -1;
        }
        if (count < 0) {
            return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
        }
        if (count > 1 && (size_t)count > (size_t)-1 / (2 * sizeof(size_t))) {
            return cm_fail(c, YEPTRIS_ERROR_MEMORY);
        }
        if (out == NULL) { /* an ivar value: consume the pairs only */
            c->depth++;
            for (int64_t i = 0; i < count; i++) {
                if (cm_map_key(c, NULL) != 0 || cm_value(c, NULL) != 0) {
                    c->depth--;
                    return -1;
                }
            }
            c->depth--;
            break;
        }
        int canonical = (c->opts & YEPTRIS_CBOR_CANONICAL) != 0;
        cm_buf* scratch = NULL;
        size_t* key_off = NULL;
        size_t* key_len = NULL;
        size_t* pair_off = NULL;
        size_t* pair_len = NULL;
        uint32_t* order = NULL;
        int mrc = -1;
        if (count > 1) {
            /* the key spans serve the duplicate-key check (both modes)
             * and the canonical sort */
            key_off = (size_t*)calloc((size_t)count, sizeof(size_t));
            key_len = (size_t*)calloc((size_t)count, sizeof(size_t));
            if (key_off == NULL || key_len == NULL) {
                cm_fail(c, YEPTRIS_ERROR_MEMORY);
                goto map_out;
            }
        }
        if (canonical && count > 0) {
            scratch = cm_buf_new(c);
            pair_off = (size_t*)calloc((size_t)count, sizeof(size_t));
            pair_len = (size_t*)calloc((size_t)count, sizeof(size_t));
            order = (uint32_t*)calloc((size_t)count, sizeof(uint32_t));
            if (scratch == NULL || pair_off == NULL || pair_len == NULL || order == NULL) {
                cm_fail(c, YEPTRIS_ERROR_MEMORY);
                goto map_out;
            }
        }
        {
            cm_buf* dst = (canonical && count > 0) ? scratch : out;
            if (!canonical && cm_head(c, out, 5, (uint64_t)count) != 0) {
                goto map_out;
            }
            c->depth++;
            for (int64_t i = 0; i < count; i++) {
                if (c->s.at >= c->s.len) {
                    cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
                    goto map_out;
                }
                if (!cm_key_lead_ok((unsigned char)c->s.p[c->s.at])) {
                    cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
                    goto map_out;
                }
                if (canonical) {
                    pair_off[i] = dst->len;
                }
                if (count > 1) {
                    key_off[i] = dst->len;
                }
                if (cm_map_key(c, dst) != 0) {
                    goto map_out;
                }
                if (count > 1) {
                    key_len[i] = dst->len - key_off[i];
                }
                if (cm_value(c, dst) != 0) {
                    goto map_out;
                }
                if (canonical) {
                    pair_len[i] = dst->len - pair_off[i];
                    order[i] = (uint32_t)i;
                }
            }
            c->depth--;
            if (count > 1) {
                /* the per-node route's map_add duplicate check compares
                 * the stored key TEXT; among supported keys that is
                 * exactly emitted-byte equality */
                for (int64_t i = 1; i < count; i++) {
                    for (int64_t j = 0; j < i; j++) {
                        if (key_len[i] == key_len[j] &&
                            (key_len[i] == 0 ||
                             memcmp(dst->p + key_off[i], dst->p + key_off[j], key_len[i]) == 0)) {
                            cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
                            goto map_out;
                        }
                    }
                }
            }
            if (canonical) {
                for (int64_t p = 1; p < count; p++) { /* insertion sort: stable */
                    uint32_t cur = order[p];
                    int64_t j = p;
                    while (j > 0) {
                        uint32_t other = order[j - 1];
                        const uint8_t* a = dst->p + key_off[cur];
                        const uint8_t* b = dst->p + key_off[other];
                        size_t la = key_len[cur];
                        size_t lb = key_len[other];
                        size_t min = la < lb ? la : lb;
                        int cmp = memcmp(a, b, min);
                        if (cmp < 0 || (cmp == 0 && la < lb)) {
                            order[j] = order[j - 1];
                            j--;
                        } else {
                            break;
                        }
                    }
                    order[j] = cur;
                }
                if (cm_head(c, out, 5, (uint64_t)count) != 0) {
                    goto map_out;
                }
                for (int64_t p = 0; p < count; p++) {
                    uint32_t cur = order[p];
                    if (cm_put(c, out, dst->p + pair_off[cur], pair_len[cur]) != 0) {
                        goto map_out;
                    }
                }
            }
            mrc = 0;
        }
    map_out:
        free(pair_off);
        free(pair_len);
        free(order);
        free(key_off);
        free(key_len);
        rc = mrc;
        break;
    }
    default:
        return cm_fail(c, YEPTRIS_ERROR_UNSUPPORTED);
    }
    if (reg_idx >= 0 && out != NULL) {
        c->objs[reg_idx].len = out->len - c->objs[reg_idx].off;
    }
    return rc;
}

YEPTRIS_API YeptrisStatus yeptris_cbor_from_marshal(const char* m, size_t m_len, uint32_t opts,
                                                    char** out, size_t* out_len) {
    if (out == NULL || out_len == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;
    *out_len = 0;
    if (m == NULL || m_len < 2 || (unsigned char)m[0] != 0x04 || (unsigned char)m[1] != 0x08) {
        return YEPTRIS_ERROR_UNSUPPORTED;
    }
    cm_ctx c;
    memset(&c, 0, sizeof(c));
    c.s.p = m;
    c.s.len = m_len;
    c.s.at = 2;
    c.opts = opts;
    cm_buf* root = cm_buf_new(&c);
    YeptrisStatus st;
    if (root == NULL) {
        st = YEPTRIS_ERROR_MEMORY;
    } else if (cm_value(&c, root) != 0) {
        st = c.failed ? (YeptrisStatus)c.failed : YEPTRIS_ERROR_UNSUPPORTED;
    } else if (c.s.at != m_len) {
        st = YEPTRIS_ERROR_UNSUPPORTED; /* trailing bytes: not one stream */
    } else {
        *out = (char*)root->p;
        *out_len = root->len;
        root->p = NULL;
        root->len = 0;
        st = YEPTRIS_OK;
    }
    for (size_t i = 0; i < c.keep_n; i++) {
        free(c.keep[i]->p);
        free(c.keep[i]);
    }
    free(c.keep);
    free(c.syms);
    free(c.objs);
    return st;
}
