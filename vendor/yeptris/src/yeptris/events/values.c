/* values.c — the typed value stream (TODO.impl/15 phase F).
 *
 * One engine run through the shared capture sink, then one C pass
 * converting every scalar by its resolver tag through the number
 * kernels (parse/numbers.c — the same converters the node typed
 * accessors ride). The host walks a flat typed array: no per-scalar
 * parsing, no pending-key bookkeeping (is_key), anchor names arrive
 * as uniform YEP_V_ANCHOR entries decorating the value they bind.
 *
 * TODO.restructure/21: the record machinery is shared (values_priv.h)
 * with the Marshal emitter, a DOM linearizer produces the same records
 * from a built tree, and the input entry sniffs strict JSON to build
 * through the JSON scanner — any grammar surprise defers to the engine
 * so records stay byte-identical across routes. */

#include <stdlib.h>
#include <string.h>

#include "../../include/yeptris/values.h"
#include "../common/simd_text.h"
#include "../dom/dom.h"
#include "../encoding/encoding.h"
#include "../memory/allocator.h"
#include "../parse/engine.h"
#include "../parse/numbers.h"
#include "../resolve/resolver.h"
#include "../scan/json.h"
#include "capture.h"
#include "values_priv.h"

uint32_t yep_val_arena_put(yep_value_ctx* c, const char* p, uint32_t len) {
    if (c->arena_len + len + 1 > c->arena_cap) {
        size_t cap = c->arena_cap ? c->arena_cap : 256;
        while (cap < c->arena_len + len + 1) {
            cap *= 2;
        }
        char* na = realloc(c->arena, cap);
        if (na == NULL) {
            c->oom = 1;
            return 0;
        }
        c->arena = na;
        c->arena_cap = cap;
    }
    if (len != 0) {
        memcpy(c->arena + c->arena_len, p, len);
    }
    uint32_t off = (uint32_t)c->arena_len;
    c->arena_len += len;
    c->arena[c->arena_len++] = '\0'; /* names read as C strings */
    return off;
}

void yep_val_put(yep_value_ctx* c, const YeptrisValue* v) {
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 256;
        YeptrisValue* nv = realloc(c->vals, cap * sizeof(*nv));
        if (nv == NULL) {
            c->oom = 1;
            return;
        }
        c->vals = nv;
        c->cap = cap;
    }
    c->vals[c->n++] = *v;
}

/* The map slot rule: in a mapping, entries alternate key/value — the
 * entry landing in an empty key slot is the key (is_key set), the
 * next completes the pair. Applies to scalars, aliases, and
 * collection opens (complex keys) alike. */
void yep_val_slot(yep_value_ctx* c, YeptrisValue* v) {
    if (c->depth > 0 && c->depth <= YEP_V_MAX_DEPTH) {
        int di = c->depth - 1;
        if (c->is_seq[di]) {
            /* a sequence entry never takes a key slot — the toggle
             * flagged every ODD-position entry is_key=1, and the
             * marshal strict-dup scan then false-flagged arrays with a
             * repeated value at two odd positions ("[\"a\",\"b\",\"a\"]"
             * = "duplicate key in object"; yeptris-ruby#333, the
             * yeptris#680 class resurfacing through the value
             * stream's own parity) */
            return;
        }
        if (c->key_pend[di] == 0) {
            v->is_key = 1;
            c->key_pend[di] = 1;
        } else {
            c->key_pend[di] = 0;
        }
    }
}

static void anchor_entry(yep_value_ctx* c, const char* name, uint32_t len) {
    YeptrisValue a = {YEP_V_ANCHOR, 0, 0, 0, 0, len, 0};
    a.off = yep_val_arena_put(c, name, len);
    yep_val_put(c, &a);
}

/* The scalar conversion SSOT: tag verdict + raw bytes → typed record.
 * The engine transform and the DOM linearizer both land here, so the
 * routes cannot drift. */
static void val_scalar(yep_value_ctx* c, uint8_t tag_id, int implicit_plain, const char* text,
                       uint32_t len) {
    YeptrisValue v = {0, 0, 0, 0, 0, 0, 0};
    int64_t vi = 0;
    double vd = 0.0;
    v.tag_id = tag_id;
    /* EVERY scalar carries its raw bytes: hosts with schema quirks
     * (Psych's single-char y/n, PyYAML's dot-required floats)
     * re-decide from tag_id + text without re-running a conversion
     * grammar */
    v.off = yep_val_arena_put(c, text, len);
    v.len = len;
    if (tag_id != YEPTRIS_TAG_BOOL) {
        /* b doubles as the implicit-plain flag for the other kinds
         * (host symbol scans and friends key on it) */
        v.b = implicit_plain ? 1 : 0;
    }
    switch (tag_id) {
    case YEPTRIS_TAG_NULL:
        v.kind = YEP_V_NULL;
        break;
    case YEPTRIS_TAG_BOOL:
        v.kind = YEP_V_BOOL;
        v.b = (uint8_t)yep_num_bool_ci(text, len);
        break;
    case YEPTRIS_TAG_INT:
        v.kind = YEP_V_INT;
        if (yep_num_i64(text, len, &vi) != 0) {
            /* the resolver tagged it but the kernel rejects:
             * degrade to STR — host policy decides */
            v.kind = YEP_V_STR;
            v.p = 0;
        } else {
            v.p = (uint64_t)vi;
        }
        break;
    case YEPTRIS_TAG_FLOAT:
        v.kind = YEP_V_FLOAT;
        if (yep_num_f64(text, len, &vd) != 0) {
            v.kind = YEP_V_STR;
        } else {
            memcpy(&v.p, &vd, sizeof(v.p));
        }
        break;
    case YEPTRIS_TAG_TIMESTAMP:
        v.kind = YEP_V_TIMESTAMP;
        break;
    default:
        v.kind = YEP_V_STR;
        break;
    }
    yep_val_slot(c, &v);
    yep_val_put(c, &v);
}

static int transform(yep_value_ctx* c, const yep_rec_store* store) {
    const YeptrisEventRecord* rs = store->recs;
    const char* ra = store->arena ? store->arena : "";
    for (size_t i = 0; i < store->n; i++) {
        const YeptrisEventRecord* r = &rs[i];
        if (r->anchor_len != 0 && r->type != YEPTRIS_EV_ALIAS) {
            anchor_entry(c, ra + r->anchor_off, r->anchor_len);
        }
        YeptrisValue v = {0, 0, 0, 0, 0, 0, 0};
        switch (r->type) {
        case YEPTRIS_EV_STREAM_START:
        case YEPTRIS_EV_STREAM_END:
            continue;
        case YEPTRIS_EV_DOCUMENT_START:
            v.kind = YEP_V_DOC;
            yep_val_put(c, &v);
            continue;
        case YEPTRIS_EV_SEQUENCE_START:
            v.kind = YEP_V_SEQ_OPEN;
            v.tag_id = r->tag_id;
            yep_val_slot(c, &v);
            yep_val_put(c, &v);
            if (c->depth < YEP_V_MAX_DEPTH) {
                c->key_pend[c->depth] = 0;
                c->is_seq[c->depth] = 1;
            }
            c->depth++;
            continue;
        case YEPTRIS_EV_MAPPING_START:
            v.kind = YEP_V_MAP_OPEN;
            v.tag_id = r->tag_id;
            yep_val_slot(c, &v);
            yep_val_put(c, &v);
            if (c->depth < YEP_V_MAX_DEPTH) {
                c->key_pend[c->depth] = 0;
                c->is_seq[c->depth] = 0;
            }
            c->depth++;
            continue;
        case YEPTRIS_EV_SEQUENCE_END:
        case YEPTRIS_EV_MAPPING_END:
            v.kind = YEP_V_CLOSE;
            yep_val_put(c, &v);
            c->depth--;
            continue;
        case YEPTRIS_EV_ALIAS:
            v.kind = YEP_V_ALIAS;
            v.off = yep_val_arena_put(c, ra + r->value_off, r->value_len);
            v.len = r->value_len;
            yep_val_slot(c, &v);
            yep_val_put(c, &v);
            continue;
        case YEPTRIS_EV_SCALAR: {
            const char* text = ra + r->value_off;
            uint32_t len = r->value_len;
            int plain = (r->flags & YEPTRIS_EF_IMPLICIT) != 0;
            val_scalar(c, r->tag_id, plain, text, len);
            continue;
        }
        default:
            continue;
        }
    }
    return c->oom ? -1 : 0;
}

static yep_value_ctx* ctx_create(void) {
    return calloc(1, sizeof(yep_value_ctx));
}

void yep_value_ctx_free(yep_value_ctx* c) {
    if (c == NULL) {
        return;
    }
    free(c->vals);
    free(c->arena);
    free(c);
}

static int ctx_finalize(yep_value_ctx* c, yep_value_ctx** out) {
    if (c->oom) {
        yep_value_ctx_free(c);
        return -1;
    }
    if (c->arena == NULL) {
        c->arena = malloc(1); /* non-NULL so hosts can free blindly */
        if (c->arena == NULL) {
            yep_value_ctx_free(c);
            return -1;
        }
    }
    *out = c;
    return 0;
}

/* ---- the DOM linearizer (21): preorder walk, same records ---- */

static int lin_node(yep_value_ctx* c, const yep_dom* d, uint32_t id);

static int lin_children(yep_value_ctx* c, const yep_dom* d, const yep_dnode* n) {
    for (uint32_t cid = n->first_child; cid != UINT32_MAX; cid = d->nodes[cid].next_sibling) {
        if (lin_node(c, d, cid) != 0) {
            return -1;
        }
    }
    return 0;
}

static int lin_node(yep_value_ctx* c, const yep_dom* d, uint32_t id) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return -1;
    }
    if (n->anchor.len != 0 && n->kind != YEP_DOM_ALIAS) {
        yep_view a = yep_dom_view(d, n->anchor);
        anchor_entry(c, a.p, (uint32_t)a.len);
    }
    YeptrisValue v = {0, 0, 0, 0, 0, 0, 0};
    switch (n->kind) {
    case YEP_DOM_SCALAR: {
        yep_view s = yep_dom_view(d, n->value);
        val_scalar(c, n->tag_id, n->implicit, s.p, (uint32_t)s.len);
        return 0;
    }
    case YEP_DOM_ALIAS: {
        yep_view s = yep_dom_view(d, n->value);
        v.kind = YEP_V_ALIAS;
        v.off = yep_val_arena_put(c, s.p, (uint32_t)s.len);
        v.len = (uint32_t)s.len;
        yep_val_slot(c, &v);
        yep_val_put(c, &v);
        return 0;
    }
    default:
        break;
    }
    v.kind = n->kind == YEP_DOM_SEQUENCE ? YEP_V_SEQ_OPEN : YEP_V_MAP_OPEN;
    v.tag_id = n->tag_id;
    yep_val_slot(c, &v);
    yep_val_put(c, &v);
    if (c->depth < YEP_V_MAX_DEPTH) {
        c->key_pend[c->depth] = 0;
        c->is_seq[c->depth] = (n->kind == YEP_DOM_SEQUENCE);
    }
    c->depth++;
    if (lin_children(c, d, n) != 0) {
        return -1;
    }
    v.kind = YEP_V_CLOSE;
    v.tag_id = 0;
    v.is_key = 0;
    yep_val_put(c, &v);
    c->depth--;
    return 0;
}

int yep_values_from_dom(const yep_dom* d, uint32_t root_id, int with_doc, yep_value_ctx** out) {
    *out = NULL;
    if (d == NULL) {
        return -1;
    }
    yep_value_ctx* c = ctx_create();
    if (c == NULL) {
        return -1;
    }
    if (with_doc) {
        YeptrisValue dv = {YEP_V_DOC, 0, 0, 0, 0, 0, 0};
        yep_val_put(c, &dv);
    }
    if (root_id != UINT32_MAX && lin_node(c, d, root_id) != 0) {
        yep_value_ctx_free(c);
        return -1;
    }
    return ctx_finalize(c, out);
}

/* ---- the input entry: strict-JSON sniff, else the engine ---- */

static int looks_strict_json(const char* p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = p[i];
        if (ch == '{' || ch == '[') {
            return 1;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            continue;
        }
        return 0;
    }
    return 0;
}

/* JSON route: validate + direct build + linearize. Returns 0 taken,
 * 1 not applicable (grammar surprise — engine decides), -1 fatal. */
static int drain_json_route(const char* yaml, size_t len, yep_value_ctx** out) {
    size_t verr = 0;
    if (!yep_json_document(yaml, len, &verr)) {
        return 1; /* not strict JSON: YAML flow, tags, or junk — engine */
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_dom* dom = yep_dom_create(sys);
    if (dom == NULL) {
        return -1;
    }
    dom->input_base = yaml; /* strict JSON is UTF-8 by definition */
    dom->input_len = len;
    yep_dom_prepare_len(dom, len);
    int rc = yep_dom_build_json(dom, yaml, len);
    if (rc != 0) {
        /* builder/validator disagreement: belt and braces, engine wins */
        yep_dom_destroy(dom);
        return rc == -1 ? -1 : 1;
    }
    yep_value_ctx* c = ctx_create();
    if (c == NULL) {
        yep_dom_destroy(dom);
        return -1;
    }
    int bad = 0;
    for (uint32_t i = 0; i < dom->dcount && !bad; i++) {
        YeptrisValue dv = {YEP_V_DOC, 0, 0, 0, 0, 0, 0};
        yep_val_put(c, &dv);
        bad = lin_node(c, dom, dom->docs[i]) != 0;
    }
    yep_dom_destroy(dom);
    if (bad) {
        yep_value_ctx_free(c);
        return -1;
    }
    return ctx_finalize(c, out) == 0 ? 0 : -1;
}

/* Shared core of both drain flavors and the Marshal input path: one
 * record array + arena, ownership moves to the caller. */
int yep_values_from_input(const char* yaml, size_t len, int schema_compat, yep_value_ctx** out) {
    *out = NULL;
    /* The encoding gate the parse entry owns (parse.c): the marshal/
     * visit / value drains must not feed raw UTF-16/32 bytes to the
     * engine while yeptris_parse accepts them — the nightly's marshal
     * crasher (a UTF-16BE doc whose raw bytes contain "-s"). */
    yep_encoding enc = YEP_ENC_UNKNOWN;
    size_t bom = yep_bom_sniff((const unsigned char*)yaml, len, &enc);
    const char* data = yaml + bom;
    size_t data_len = len - bom;
    unsigned char* transcoded = NULL;
    const yep_allocator* sys = yep_system_allocator();
    if (enc == YEP_ENC_UTF16LE || enc == YEP_ENC_UTF16BE || enc == YEP_ENC_UTF32LE ||
        enc == YEP_ENC_UTF32BE) {
        size_t tlen = 0;
        size_t terr = 0;
        int rc = yep_transcode_to_utf8(sys, enc, (const unsigned char*)data, data_len, &transcoded,
                                       &tlen, &terr);
        if (rc != 0) {
            return -2;
        }
        size_t pverr = 0;
        if (!yep_printable_validate(transcoded, tlen, &pverr)) {
            yep_free(sys, transcoded);
            return -2;
        }
        data = (const char*)transcoded;
        data_len = tlen;
    }
    int prc_final = -2;
    do {
        if (looks_strict_json(data, data_len)) {
            int jrc = drain_json_route(data, data_len, out);
            if (jrc <= 0) {
                prc_final = (jrc == 0) ? 0 : -1;
                break;
            }
            /* fall through: engine */
        }
        yep_engine* eng = yep_engine_create(yep_system_allocator());
        if (eng == NULL) {
            return -1;
        }
        yep_engine_set_resolver(eng,
                                schema_compat ? yep_resolver_compat11() : yep_resolver_core12());
        if (schema_compat) {
            yep_engine_set_compat_grammar(eng, 1); /* libyaml grammar parity
                                                    * (the value drains ride the
                                                    * same compat contract) */
        }

        yep_value_ctx* c = ctx_create();
        if (c == NULL) {
            yep_engine_destroy(eng);
            return -1;
        }
        yep_rec_store store;
        yep_rec_init(&store);

        int prc = -2;
        yep_sink sink = {.on_event = yep_rec_on_event,
                         .ctx = &store,
                         .on_flow_build = NULL,
                         .on_flow_commit = NULL,
                         .on_flow_rollback = NULL,
                         .on_block_pair = NULL,
                         .on_block_open = NULL,
                         .on_block_item = NULL};
        /* strict JSON carries no anchors: no nametab reserve needed */
        yep_engine_front_done(eng);
        if (yep_engine_run(eng, data, data_len, &sink) == 0 && transform(c, &store) == 0) {
            prc = 0;
        } else if (c->oom) {
            prc = -1;
        }
        yep_engine_destroy(eng);
        yep_rec_free(&store);
        if (prc != 0) {
            yep_value_ctx_free(c);
            break;
        }
        prc_final = ctx_finalize(c, out) == 0 ? 0 : -1;
    } while (0);
    yep_free(sys, transcoded);
    if (prc_final != 0 && prc_final != -1) {
        return -2;
    }
    return prc_final;
}

static YeptrisStatus map_status(int rc) {
    return rc == 0 ? YEPTRIS_OK : (rc == -1 ? YEPTRIS_ERROR_MEMORY : YEPTRIS_ERROR_PARSE);
}

YEPTRIS_API YeptrisStatus yeptris_value_drain(const char* yaml, size_t len, YeptrisSchema schema,
                                              YeptrisValue** vals, size_t* count, char** arena,
                                              size_t* arena_len) {
    if (vals == NULL || count == NULL || arena == NULL || arena_len == NULL ||
        (yaml == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    *vals = NULL;
    *count = 0;
    *arena = NULL;
    *arena_len = 0;
    yep_value_ctx* c = NULL;
    int rc = yep_values_from_input(yaml, len, schema == YEPTRIS_SCHEMA_11_COMPAT, &c);
    if (rc != 0) {
        return map_status(rc);
    }
    *vals = c->vals;
    *count = c->n;
    *arena = c->arena;
    *arena_len = c->arena_len;
    free(c);
    return YEPTRIS_OK;
}

YEPTRIS_API void yeptris_value_free(YeptrisValue* vals, char* arena) {
    free(vals);
    free(arena);
}

YEPTRIS_API YeptrisStatus yeptris_value_drain_columns(const char* yaml, size_t len,
                                                      YeptrisSchema schema,
                                                      YeptrisValueColumns* cols) {
    if (cols == NULL || (yaml == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    memset(cols, 0, sizeof(*cols));
    yep_value_ctx* c = NULL;
    int rc = yep_values_from_input(yaml, len, schema == YEPTRIS_SCHEMA_11_COMPAT, &c);
    if (rc != 0) {
        return map_status(rc);
    }
    size_t n = c->n;
    /* one carved block, widest-first so every column is naturally
     * aligned: payloads(8n) offs(4n) lens(4n) kinds tags is_keys bools */
    size_t total = n * (8 + 4 + 4 + 1 + 1 + 1 + 1);
    int64_t* block = NULL;
    if (n > 0) {
        block = malloc(total);
        if (block == NULL) {
            yep_value_ctx_free(c);
            return YEPTRIS_ERROR_MEMORY;
        }
    }
    cols->count = n;
    cols->arena_len = c->arena_len;
    cols->arena = c->arena;
    /* a zero-entry drain keeps every column NULL (pointer arithmetic
     * on a null block is UB — caught by UBSan on the empty stream) */
    if (block != NULL) {
        cols->payloads = block;
        cols->offs = (uint32_t*)(block + n);
        cols->lens = cols->offs + n;
        cols->kinds = (uint8_t*)(cols->lens + n);
        cols->tags = cols->kinds + n;
        cols->is_keys = cols->tags + n;
        cols->bools = cols->is_keys + n;
    }
    for (size_t i = 0; i < n; i++) {
        const YeptrisValue* v = &c->vals[i];
        cols->payloads[i] = (int64_t)v->p;
        cols->offs[i] = v->off;
        cols->lens[i] = v->len;
        cols->kinds[i] = v->kind;
        cols->tags[i] = v->tag_id;
        cols->is_keys[i] = v->is_key;
        cols->bools[i] = v->b;
    }
    free(c->vals);
    free(c);
    return YEPTRIS_OK;
}

YEPTRIS_API void yeptris_value_free_columns(YeptrisValueColumns* cols) {
    if (cols == NULL) {
        return;
    }
    free(cols->payloads); /* the carved block base */
    free(cols->arena);
    cols->payloads = NULL;
    cols->offs = NULL;
    cols->lens = NULL;
    cols->kinds = NULL;
    cols->tags = NULL;
    cols->is_keys = NULL;
    cols->bools = NULL;
    cols->arena = NULL;
    cols->count = 0;
    cols->arena_len = 0;
}
