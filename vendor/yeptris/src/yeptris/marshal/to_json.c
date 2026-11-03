/* to_json.c — the marshal READER: Ruby Marshal.dump bytes to JSON text
 * in one C call (yeptris-ruby#322, TODO.impl/#904 item 5: the reader's
 * MECE home — marshal-domain bytes live here, the JSON writer's
 * primitives stay in emit/writer.c).
 *
 * No per-scalar crossings, no object reconstruction. The format facts
 * mirror marshal.c's emission contract (empirically pinned against
 * Ruby 3.4): w_long small form
 * x==0 -> 0x00, |x| <= 122 -> sign*(|x|+5), size form tag +-k with
 * LE payload (2^(8k) - m for negatives); strings carry 'I' ivar
 * wrappers; arrays/hashes/strings/floats register for '@' links.
 *
 * '@' links MEMMOVE the registered object's emitted span — the first
 * walk's bytes are the cache; a lookup miss is a cycle (JSON cannot
 * express it, the binding falls back). No re-parse, no registration
 * on links: the object table stays exactly Ruby's stream order.
 *
 * The JSON-irrelevant constructs ('o' objects, 'S' structs, 'u'
 * user marshal, 'l' bignums beyond i64, non-finite floats) return
 * UNSUPPORTED — the binding falls back to its per-node walk.
 */

#include "emit/writer.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit/float/api.h"

#include <yeptris/marshal.h>

typedef struct {
    const char* p;
    size_t len;
    size_t at;
} mj_stream;

typedef struct {
    const char* p;
    uint32_t len;
} mj_sym;

typedef struct {
    size_t pos;     /* the stream position of the type byte */
    size_t out_off; /* where the emitted span starts in w's buffer */
    size_t out_len; /* the span's length */
} mj_obj;

typedef struct {
    yep_writer* w;
    mj_stream s;
    mj_sym* syms;
    size_t syms_n, syms_cap;
    mj_obj* objs;
    size_t objs_n, objs_cap;
    int failed; /* 0 ok, YEPTRIS_ERROR_UNSUPPORTED */
    int compact;
    int no_reg; /* the next mj_value entry skips registration — an
                 * 'I'-wrapped construct registers ONCE (at the 'I');
                 * its immediate inner must not add a second entry or
                 * every later '@' link shifts */
    int depth;  /* mj_value recursion guard — the reader walks hostile
                 * bytes, not Ruby's heap: nesting is unbounded in the
                 * input, the stack is not (the parser's own
                 * max_depth=1000 is the same law) */
} mj_ctx;

static int mj_fail(mj_ctx* c) {
    c->failed = YEPTRIS_ERROR_UNSUPPORTED;
    return -1;
}

static int mj_byte(mj_ctx* c, unsigned char* out) {
    if (c->s.at >= c->s.len)
        return mj_fail(c);
    *out = (unsigned char)c->s.p[c->s.at++];
    return 0;
}

/* the w_long inverse — mirrors m_w_long's encode exactly:
 *   0x00 -> 0; 1..4 -> positive size form k=b; 5..127 -> small +(
 *   b-5); 129..250 -> small -(251-b); 251..255 -> negative size form
 *   k=256-b (payload = LE(2^(8k) - m)); 128 unreachable. */
static int mj_long(mj_ctx* c, int64_t* out) {
    unsigned char b;
    if (mj_byte(c, &b) != 0)
        return -1;
    if (b == 0) {
        *out = 0;
        return 0;
    }
    if (b <= 4) { /* positive size form, k = b */
        int64_t v = 0;
        for (int i = 0; i < (int)b; i++) {
            unsigned char d;
            if (mj_byte(c, &d) != 0)
                return -1;
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
            if (mj_byte(c, &d) != 0)
                return -1;
            v |= (int64_t)d << (8 * i);
        }
        *out = v - ((int64_t)1 << (8 * bytes));
        return 0;
    }
}

static int mj_ivar_pairs(mj_ctx* c);
static int mj_value(mj_ctx* c);
static int mj_discard_value(mj_ctx* c);

static int mj_string_body(mj_ctx* c, int64_t len) {
    if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
        return mj_fail(c);
    emit_dq_json(c->w, c->s.p + c->s.at, (uint32_t)len);
    c->s.at += (size_t)len;
    return 0;
}

/* a symbol: ':' + long(len) + bytes — registers in the table; the
 * name is emitted as a JSON string when emit is set */
static int mj_symbol(mj_ctx* c, int emit) {
    int64_t len;
    if (mj_long(c, &len) != 0)
        return -1;
    if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
        return mj_fail(c);
    const char* name = c->s.p + c->s.at;
    uint32_t nlen = (uint32_t)len;
    c->s.at += (size_t)len;
    if (c->syms_n == c->syms_cap) {
        c->syms_cap = c->syms_cap ? c->syms_cap * 2 : 16;
        c->syms = (mj_sym*)realloc(c->syms, c->syms_cap * sizeof(*c->syms));
        if (!c->syms)
            return mj_fail(c);
    }
    c->syms[c->syms_n].p = name;
    c->syms[c->syms_n].len = nlen;
    c->syms_n++;
    if (emit)
        emit_dq_json(c->w, name, nlen);
    return 0;
}

/* the count + (symbol, value) pairs that follow an 'I' wrapper's
 * wrapped value. Parsed and DISCARDED — encoding/freeze ivars have
 * no JSON spelling; the symbol still enters the ';' table. */
static int mj_ivar_pairs(mj_ctx* c) {
    int64_t count;
    if (mj_long(c, &count) != 0)
        return -1;
    for (int64_t i = 0; i < count; i++) {
        unsigned char nt;
        if (mj_byte(c, &nt) != 0)
            return -1;
        if (nt == ':') {
            if (mj_symbol(c, 0) != 0)
                return -1; /* the name: tabled, not emitted */
        } else if (nt == ';') {
            int64_t sidx;
            if (mj_long(c, &sidx) != 0)
                return -1;
            if (sidx < 0 || (size_t)sidx >= c->syms_n)
                return mj_fail(c);
        } else {
            return mj_fail(c); /* ivar names are symbols only */
        }
        if (mj_discard_value(c) != 0)
            return -1; /* the ivar value */
    }
    return 0;
}

/* stream-position-only walk: consumes one value, emits nothing */

#define MJ_DEPTH_MAX 1000

/* the discard twin rides the same budget (its 'I'/'['/'{' recursion is
 * just as hostile-byte reachable) */
static int mj_discard_value_inner(mj_ctx* c);

static int mj_discard_value(mj_ctx* c) {
    if (c->depth >= MJ_DEPTH_MAX)
        return mj_fail(c);
    c->depth++;
    int rc = mj_discard_value_inner(c);
    c->depth--;
    return rc;
}

static int mj_discard_value_inner(mj_ctx* c) {
    unsigned char t;
    int rc = mj_byte(c, &t);
    if (rc != 0)
        return -1;
    switch (t) {
    case '0':
    case 'T':
    case 'F':
        return 0;
    case 'i':
    case ';':
    case '@': {
        int64_t v;
        return mj_long(c, &v);
    }
    case 'l': {
        unsigned char sign;
        int64_t nlimbs;
        if (mj_byte(c, &sign) != 0)
            return -1;
        if (sign != 0x2b && sign != 0x2d)
            return mj_fail(c);
        if (mj_long(c, &nlimbs) != 0)
            return -1;
        if (nlimbs < 0 || (uint64_t)(nlimbs * 2) > (uint64_t)(c->s.len - c->s.at))
            return mj_fail(c);
        c->s.at += (size_t)(nlimbs * 2);
        return 0;
    }
    case 'f':
    case '"': {
        int64_t len;
        if (mj_long(c, &len) != 0)
            return -1;
        if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
            return mj_fail(c);
        c->s.at += (size_t)len;
        return 0;
    }
    case 'I':
        if (mj_discard_value(c) != 0)
            return -1;
        return mj_ivar_pairs(c);
    case ':':
        return mj_symbol(c, 0);
    case '[': {
        int64_t count;
        if (mj_long(c, &count) != 0)
            return -1;
        for (int64_t i = 0; i < count; i++) {
            if (mj_discard_value(c) != 0)
                return -1;
        }
        return 0;
    }
    case '{': {
        int64_t count;
        if (mj_long(c, &count) != 0)
            return -1;
        for (int64_t i = 0; i < count; i++) {
            if (mj_discard_value(c) != 0)
                return -1; /* key */
            if (mj_discard_value(c) != 0)
                return -1; /* value */
        }
        return 0;
    }
    default:
        return mj_fail(c);
    }
}

/* MJ_DEBUG is read ONCE per process (the audit's finding: the memo
 * path ran getenv per hit — an environment table walk inside the hot
 * link copy). The flag is process-fixed for the reader's lifetime:
 * a benign first-use race only double-reads the same name and both
 * threads store the identical verdict through the atomic (the
 * cpu.c one-time-detect shape). */
static int mj_debug_env(void) {
    static atomic_int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_acquire);
    if (v < 0) {
        v = getenv("MJ_DEBUG") != NULL;
        atomic_store_explicit(&cached, v, memory_order_release);
    }
    return v;
}

/* the memo'd span of a registered object, copied to the cursor. The
 * span lives earlier in the same buffer (append-only), so the copy
 * is memmove-safe. */
static int mj_emit_memo(mj_ctx* c, const mj_obj* o) {
    if (mj_debug_env()) {
        char prev[13];
        uint32_t k = o->out_len < 12 ? (uint32_t)o->out_len : 12;
        memcpy(prev, c->w->p + o->out_off, k);
        prev[k] = '\0';
        fprintf(stderr, "memo off=%zu len=%zu: %s\n", o->out_off, o->out_len, prev);
    }
    if (o->out_len == 0)
        return mj_fail(c); /* not memoized yet: a cycle */
    wr_reserve(c->w, o->out_len);
    if (c->w->oom)
        return -1;
    memmove(c->w->p + c->w->len, c->w->p + o->out_off, o->out_len);
    wr_col_update(c->w, c->w->p + c->w->len, (uint32_t)o->out_len);
    c->w->len += o->out_len;
    /* the raw advance bypasses wr_put: hand back the reservation it
     * would have consumed, or the next write rides past cap */
    c->w->reserve -= o->out_len;
    c->w->last = c->w->p[c->w->len - 1];
    return 0;
}

static int mj_value_inner(mj_ctx* c);

/* the wrapper owns the recursion budget: hostile bytes can nest arrays
 * without bound, the stack cannot (the parser's max_depth=1000 law) */
static int mj_value(mj_ctx* c) {
    if (c->depth >= MJ_DEPTH_MAX)
        return mj_fail(c);
    c->depth++;
    int rc = mj_value_inner(c);
    c->depth--;
    return rc;
}

static int mj_value_inner(mj_ctx* c) {
    unsigned char t;
    size_t tpos = c->s.at;
    int reg_idx = -1;
    size_t span_start = 0;
    int register_this = !c->no_reg;
    c->no_reg = 0;
    if (mj_byte(c, &t) != 0)
        return -1;
    /* arrays, hashes, strings, floats, bignums and anything
     * 'I'-wrapped register — ONCE per construct, at the outer byte
     * (the wrapper's no_reg suppresses the inner) (0-based stream
     * order). Bignums hold a slot but Ruby never emits a link to one
     * (the second occurrence re-emits in full); the entry only keeps
     * the slot count aligned for later links. Integers, symbols and
     * the other immediates never register. */
    if (register_this && (t == '[' || t == '{' || t == '"' || t == 'f' || t == 'I' || t == 'l')) {
        if (c->objs_n == c->objs_cap) {
            c->objs_cap = c->objs_cap ? c->objs_cap * 2 : 16;
            c->objs = (mj_obj*)realloc(c->objs, c->objs_cap * sizeof(*c->objs));
            if (!c->objs)
                return mj_fail(c);
        }
        c->objs[c->objs_n].pos = tpos;
        c->objs[c->objs_n].out_off = c->w->len;
        c->objs[c->objs_n].out_len = 0;
        reg_idx = (int)c->objs_n;
        c->objs_n++;
        span_start = c->w->len;
    }
    switch (t) {
    case '0':
        wr_put(c->w, "null", 4);
        break;
    case 'T':
        wr_put(c->w, "true", 4);
        break;
    case 'F':
        wr_put(c->w, "false", 5);
        break;
    case 'i': {
        int64_t v;
        if (mj_long(c, &v) != 0)
            return -1;
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
        wr_put(c->w, buf, (uint32_t)n);
        break;
    }
    case 'l': {
        /* the bignum: sign byte '+'/'-' + w_long(byte count) + LE
         * bytes; the i64-representable range emits, wider falls back */
        unsigned char sign;
        if (mj_byte(c, &sign) != 0)
            return -1;
        if (sign != 0x2b && sign != 0x2d)
            return mj_fail(c);
        int64_t nlimbs;
        if (mj_long(c, &nlimbs) != 0)
            return -1;
        if (nlimbs < 1 || nlimbs > 4)
            return mj_fail(c); /* beyond 64 bits: fall back */
        int64_t v = 0;
        for (int64_t i = 0; i < nlimbs; i++) {
            unsigned char lo, hi;
            if (mj_byte(c, &lo) != 0 || mj_byte(c, &hi) != 0)
                return -1;
            /* shift in the unsigned domain: the signed form put
             * 0xFFFF into the sign bit at limb 3 (UB, UBSan-flagged) */
            v |= (int64_t)((uint64_t)(lo | ((uint64_t)hi << 8)) << (16 * i));
        }
        if (sign == 0x2d)
            v = -v;
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
        wr_put(c->w, buf, (uint32_t)n);
        break;
    }
    case 'f': {
        int64_t len;
        if (mj_long(c, &len) != 0)
            return -1;
        if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
            return mj_fail(c);
        char tmp[64];
        uint32_t tl = (uint32_t)len < sizeof tmp - 1 ? (uint32_t)len : (uint32_t)sizeof tmp - 1;
        memcpy(tmp, c->s.p + c->s.at, tl);
        tmp[tl] = '\0';
        c->s.at += (size_t)len;
        double d = strtod(tmp, NULL);
        if (!isfinite(d))
            return mj_fail(c); /* JSON has no inf/nan spelling — and a
                                * strtod OVERFLOW ("1e999") produces
                                * one from text that never spells it */
        char buf[40];
        int wl = yep_d2s_json(d, buf);
        wr_put(c->w, buf, (uint32_t)wl);
        break;
    }
    case '"': {
        int64_t len;
        if (mj_long(c, &len) != 0)
            return -1;
        int rc = mj_string_body(c, len);
        if (rc != 0)
            return -1;
        break;
    }
    case 'I': {
        /* the wrapped value IS the payload: re-dispatch into it, then
         * the ivar pairs ride. The inner dispatch must not register —
         * the construct registered at the 'I' (the double entry
         * shifted every later '@' link) */
        c->no_reg = 1;
        if (mj_value(c) != 0) {
            c->no_reg = 0;
            return -1;
        }
        c->no_reg = 0;
        if (mj_ivar_pairs(c) != 0)
            return -1;
        break;
    }
    case ':': {
        /* a bare symbol VALUE: emitted as its name string */
        if (mj_symbol(c, 1) != 0)
            return -1;
        break;
    }
    case ';': {
        int64_t idx;
        if (mj_long(c, &idx) != 0)
            return -1;
        if (idx < 0 || (size_t)idx >= c->syms_n)
            return mj_fail(c);
        emit_dq_json(c->w, c->syms[idx].p, c->syms[idx].len);
        break;
    }
    case '@': {
        /* an object link: the registered span memmoves to the cursor */
        int64_t idx;
        if (mj_long(c, &idx) != 0)
            return -1;
        if (idx < 0 || (size_t)idx >= c->objs_n)
            return mj_fail(c);
        if (mj_emit_memo(c, &c->objs[idx]) != 0)
            return -1;
        break;
    }
    case '[':
    case '{': {
        int64_t count;
        if (mj_long(c, &count) != 0)
            return -1;
        if (count < 0)
            return mj_fail(c);
        wr_byte(c->w, t == '[' ? '[' : '{');
        const char* sep = c->compact ? "," : ", ";
        uint32_t sep_len = c->compact ? 1u : 2u;
        for (int64_t i = 0; i < count; i++) {
            if (i > 0)
                wr_put(c->w, sep, sep_len);
            if (t == '[') {
                if (mj_value(c) != 0)
                    return -1;
            } else {
                /* the key: a string (with its 'I' wrapper) or a
                 * symbol — JSON keys are strings */
                unsigned char kt;
                size_t ktpos = c->s.at;
                if (mj_byte(c, &kt) != 0)
                    return -1;
                if (kt == 'I') {
                    if (mj_value(c) != 0)
                        return -1;
                    if (mj_ivar_pairs(c) != 0)
                        return -1;
                } else if (kt == '"') {
                    /* the key registers like any string object: its
                     * span is memo'd for later '@' key links */
                    int kreg = (int)c->objs_n;
                    size_t kspan = c->w->len;
                    if (kreg >= 0) {
                        if (c->objs_n == c->objs_cap) {
                            c->objs_cap = c->objs_cap ? c->objs_cap * 2 : 16;
                            c->objs = (mj_obj*)realloc(c->objs, c->objs_cap * sizeof(*c->objs));
                            if (!c->objs)
                                return mj_fail(c);
                        }
                        c->objs[c->objs_n].pos = ktpos;
                        c->objs[c->objs_n].out_off = c->w->len;
                        c->objs[c->objs_n].out_len = 0;
                        c->objs_n++;
                    }
                    int64_t len;
                    if (mj_long(c, &len) != 0)
                        return -1;
                    int rc = mj_string_body(c, len);
                    if (rc != 0)
                        return -1;
                    if (kreg >= 0) {
                        c->objs[kreg].out_off = kspan;
                        c->objs[kreg].out_len = c->w->len - kspan;
                    }
                } else if (kt == ':') {
                    if (mj_symbol(c, 1) != 0)
                        return -1;
                } else if (kt == ';') {
                    int64_t idx;
                    if (mj_long(c, &idx) != 0)
                        return -1;
                    if (idx < 0 || (size_t)idx >= c->syms_n)
                        return mj_fail(c);
                    emit_dq_json(c->w, c->syms[idx].p, c->syms[idx].len);
                } else if (kt == '@') {
                    int64_t idx;
                    if (mj_long(c, &idx) != 0)
                        return -1;
                    if (idx < 0 || (size_t)idx >= c->objs_n)
                        return mj_fail(c);
                    if (mj_emit_memo(c, &c->objs[idx]) != 0)
                        return -1;
                } else {
                    return mj_fail(c);
                }
                wr_put(c->w, c->compact ? ":" : ": ", c->compact ? 1u : 2u);
                if (mj_value(c) != 0)
                    return -1;
            }
        }
        wr_byte(c->w, t == '[' ? ']' : '}');
        break;
    }
    default:
        return mj_fail(c);
    }
    if (reg_idx >= 0) {
        c->objs[reg_idx].out_off = span_start;
        c->objs[reg_idx].out_len = c->w->len - span_start;
    }
    return 0;
}

YEPTRIS_API YeptrisStatus yeptris_json_from_marshal(const char* m, size_t m_len, int compact,
                                                    char** out, size_t* out_len) {
    if (out == NULL || out_len == NULL)
        return YEPTRIS_ERROR_ARG;
    *out = NULL;
    *out_len = 0;
    if (m == NULL || m_len < 2 || (unsigned char)m[0] != 0x04 || (unsigned char)m[1] != 0x08)
        return YEPTRIS_ERROR_UNSUPPORTED;
    yep_emitter em;
    memset(&em, 0, sizeof(em));
    yep_writer* w = &em.w;
    w->sc_dec = NULL;
    w->sc_i = 0;
    w->json = 1;
    w->json_compact = compact ? 1 : 0;
    w->json_pretty = 0;
    w->canonical = 0;
    w->explicit_doc_start = 0;
    w->best_width = 0;
    w->sv_input = NULL;
    w->sv_arena = NULL;
    w->sink = NULL;
    w->force_flow = 0;
    w->in_flow = 0;
    w->flow_item = 0;
    w->unserializable = 0;
    w->indent = 2; /* #903 fields: the JSON lane never folds or
                    * block-indents, but the writer stays fully
                    * initialized */
    w->block_fold = 0;

    mj_ctx c;
    memset(&c, 0, sizeof(c));
    c.w = w;
    c.s.p = m;
    c.s.len = m_len;
    c.s.at = 2; /* the 4.8 header */
    c.compact = compact ? 1 : 0;

    /* one wet pass into a growable buffer (the #352 slice-2 shape).
     * Raw malloc is the CONTRACT here, not a bypass (#904 item 3): the
     * route takes bytes, not a document — there is no document
     * allocator to route through, and *out is "malloc'd, free with
     * yeptris_marshal_free" (marshal.h), so the growth in wr_grow must
     * stay realloc-on-system too or the caller's free() breaks. */
    size_t est = m_len * 2 + 64;
    char* buf = (char*)malloc(est);
    if (!buf)
        return YEPTRIS_ERROR_MEMORY;
    w->p = buf;
    w->cap = est;
    w->grow = 1;

    mj_value(&c);

    int failed = c.failed;
    size_t wrote = w->len;
    buf = w->p;
    free(c.syms);
    free(c.objs);
    if (failed || w->oom) {
        free(buf);
        return failed ? (YeptrisStatus)failed : YEPTRIS_ERROR_MEMORY;
    }
    buf[wrote] = '\0';
    *out = buf;
    *out_len = wrote;
    return YEPTRIS_OK;
}
