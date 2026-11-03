/* schema.c — the fused schema-descriptor materialization
 * (issue #238, TODO.restructure/83).
 *
 * One parse (the same engine every surface uses) + one fused walk of
 * the DOM against the caller's descriptor: typed columns come out,
 * the intermediate generic document never exists. The DOM side runs
 * first because it is the cheapest complete-materialization engine;
 * when the recorder path learns descriptor projection it can drop in
 * under the same ABI (the columns are the contract, not the walk).
 */

#include <stdlib.h>
#include <string.h>

#include <yeptris/schema.h>
#include <yeptris/values.h> /* YeptrisValue: ST_ANY records (#238) */

#include "dom/dom.h"
#include "memory/allocator.h"
#include "parse/engine.h"
#include "resolve/resolver.h"
#include "scan/json.h"

typedef struct {
    const yeptris_desc_node* desc;
    uint32_t desc_len;
    yeptris_schema_column* cols;
    const yep_dom* dom;
    int oom;
    int overflow;
    int type_mismatch;  /* node index */
    uint32_t* seen_map; /* per-node: the mapping instance that last
                         * emitted — YEP_SF_FIRST_WINS skips a repeat */
} yep_schema_ctx;

static int col_put(yep_schema_ctx* c, uint32_t node, const void* rec, size_t size) {
    yeptris_schema_column* col = &c->cols[node];
    if (col->count >= col->capacity) {
        c->overflow = 1;
        return 0;
    }
    memcpy((char*)col->data + (size_t)col->count * size, rec, size);
    col->count++;
    return 1;
}

static void put_error_schema(uint32_t node) {
    yep_error_set(yep_error_tls(), YEP_ERR_SCHEMA, 0, 0, node,
                  "required descriptor node %u missing", node);
}

/* Materializes the DOM node under plan `node` into its column. */
static int emit(yep_schema_ctx* c, uint32_t node, uint32_t at);

static int emit_scalar(yep_schema_ctx* c, uint32_t node, uint32_t at) {
    const yeptris_desc_node* d = &c->desc[node];
    const yep_dnode* n = yep_dom_node(c->dom, at);
    yep_view v = yep_dom_view(c->dom, n->value);
    switch (d->type_tag) {
    case YEP_ST_STR: {
        yeptris_span2 s = {(uint32_t)((const char*)v.p - c->dom->input_base), v.len, 0, 0};
        return col_put(c, node, &s, sizeof(s));
    }
    case YEP_ST_INT:
    case YEP_ST_FLOAT: {
        int64_t iv = 0;
        double dv = 0;
        int shape = 0;
        size_t end = 0;
        if (v.len == 0 || v.p == NULL ||
            yep_json_number_scan(v.p, v.len, &end, &shape, &iv, &dv) == 0 || end != v.len) {
            c->type_mismatch = (int)node;
            return -1;
        }
        if (d->type_tag == YEP_ST_INT) {
            int64_t out = shape == 0 ? iv : (int64_t)dv;
            return col_put(c, node, &out, sizeof(out));
        }
        double fd = shape == 0 ? (double)iv : dv;
        return col_put(c, node, &fd, sizeof(fd));
    }
    case YEP_ST_BOOL: {
        uint8_t b = (uint8_t)(v.len > 0 && (v.p[0] == 't' || v.p[0] == 'T' || v.p[0] == 'y' ||
                                            v.p[0] == 'Y' || v.p[0] == 'o' || v.p[0] == '1'));
        return col_put(c, node, &b, sizeof(b));
    }
    case YEP_ST_NULL:
        col_put(c, node, &(uint8_t){0}, 0);
        return 1;
    case YEP_ST_ANY: {
        /* the resolver's verdict, verbatim: a YeptrisValue record
         * (#238's headroom) — kind/tag_id from the node, the span
         * borrowed, and the numeric payload converted when the
         * verdict says so (the values-drain's conversion, DOM-side) */
        YeptrisValue r = {0, 0, 0, 0, 0, 0, 0};
        r.tag_id = n->tag_id;
        r.off = (uint32_t)((const char*)v.p - c->dom->input_base);
        r.len = v.len;
        switch (n->tag_id) {
        case YEPTRIS_TAG_NULL:
            r.kind = YEP_V_NULL;
            break;
        case YEPTRIS_TAG_BOOL: {
            r.kind = YEP_V_BOOL;
            r.b = (uint8_t)(v.len > 0 && (v.p[0] == 't' || v.p[0] == 'T' || v.p[0] == 'y' ||
                                          v.p[0] == 'Y' || v.p[0] == 'o' || v.p[0] == '1'));
            break;
        }
        case YEPTRIS_TAG_INT:
        case YEPTRIS_TAG_FLOAT: {
            int64_t iv = 0;
            double dv = 0;
            int shape = 0;
            size_t end = 0;
            if (v.len != 0 && v.p != NULL &&
                yep_json_number_scan(v.p, v.len, &end, &shape, &iv, &dv) != 0 && end == v.len) {
                if (n->tag_id == YEPTRIS_TAG_INT) {
                    r.kind = YEP_V_INT;
                    r.p = (uint64_t)iv;
                } else {
                    r.kind = YEP_V_FLOAT;
                    double fd = shape == 0 ? (double)iv : dv;
                    memcpy(&r.p, &fd, sizeof(fd));
                }
            } else {
                r.kind = YEP_V_STR; /* the resolver said number, the
                                      bytes disagree: keep the span */
            }
            break;
        }
        case YEPTRIS_TAG_TIMESTAMP:
            r.kind = YEP_V_TIMESTAMP;
            break;
        default:
            r.kind = YEP_V_STR;
            break;
        }
        return col_put(c, node, &r, sizeof(r));
    }
    default:
        return -1;
    }
}

static int emit(yep_schema_ctx* c, uint32_t node, uint32_t at) {
    const yeptris_desc_node* d = &c->desc[node];
    const yep_dnode* n = yep_dom_node(c->dom, at);
    switch (d->kind) {
    case YEP_SK_SCALAR:
        return emit_scalar(c, node, at);
    case YEP_SK_CALLBACK: {
        /* the escape hatch: raw span + the node's first byte —
         * scalar content by span, collections by their extent (the
         * caller that needs the tree takes the generic surface) */
        yep_view v = yep_dom_view(c->dom, n->value);
        yeptris_span2 s = {(uint32_t)((const char*)v.p - c->dom->input_base), v.len,
                           (uint32_t)((const char*)v.p - c->dom->input_base), 0};
        if (n->kind == YEP_DOM_MAPPING || n->kind == YEP_DOM_SEQUENCE) {
            s.off = 0; /* collections have no span: position only */
            s.len = 0;
        }
        return col_put(c, node, &s, sizeof(s));
    }
    case YEP_SK_SEQUENCE: {
        if (n->kind != YEP_DOM_SEQUENCE) {
            return 0; /* not this shape: the column stays empty */
        }
        uint32_t ch = d->child_index;
        if (ch >= c->desc_len) {
            return -1;
        }
        uint32_t e = n->first_child;
        while (e != UINT32_MAX) {
            int rc = emit(c, ch, e);
            if (rc != 1) {
                return rc;
            }
            e = c->dom->nodes[e].next_sibling;
        }
        return 1;
    }
    case YEP_SK_MAPPING: {
        if (n->kind != YEP_DOM_MAPPING) {
            return 0;
        }
        uint32_t k = n->first_child;
        while (k != UINT32_MAX) {
            uint32_t val = c->dom->nodes[k].next_sibling;
            yep_view key = yep_dom_view(c->dom, c->dom->nodes[k].value);
            for (uint32_t ci = d->child_index;
                 ci < d->child_index + d->child_count && ci < c->desc_len; ci++) {
                const yeptris_desc_node* cd = &c->desc[ci];
                size_t wl = strlen(cd->wire_name ? cd->wire_name : "");
                if (cd->wire_name != NULL && key.len == wl &&
                    memcmp(key.p, cd->wire_name, wl) == 0) {
                    if ((cd->flags & YEP_SF_FIRST_WINS) && c->seen_map != NULL &&
                        c->seen_map[ci] == at + 1) {
                        break; /* a repeat within THIS mapping: first wins */
                    }
                    int rc = emit(c, ci, val);
                    if (rc != 1) {
                        return rc;
                    }
                    if ((cd->flags & YEP_SF_FIRST_WINS) && c->seen_map != NULL) {
                        c->seen_map[ci] = at + 1;
                    }
                    break;
                }
            }
            k = c->dom->nodes[val].next_sibling;
        }
        return 1;
    }
    default:
        return -1;
    }
}

YEPTRIS_API YeptrisStatus yeptris_schema_load(const char* source, size_t len, YeptrisSchema schema,
                                              const yeptris_desc_node* desc, uint32_t desc_len,
                                              uint32_t desc_abi, yeptris_schema_column* cols) {
    if ((source == NULL && len != 0) || desc == NULL || desc_len == 0 || cols == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    if (desc_abi != YEPTRIS_DESC_ABI) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, desc_abi,
                      "descriptor ABI %u != %u (recompile the descriptor)", desc_abi,
                      YEPTRIS_DESC_ABI);
        return YEPTRIS_ERROR_ARG;
    }
    for (uint32_t i = 0; i < desc_len; i++) {
        if (desc[i].reserved != 0) {
            return YEPTRIS_ERROR_ARG;
        }
        cols[i].count = 0;
    }

    const yep_allocator* sys = yep_system_allocator();
    yep_engine* eng = yep_engine_create(sys);
    yep_dom* dom = yep_dom_create(sys);
    uint32_t* seen_map_ptr = NULL;
    YeptrisStatus st = YEPTRIS_OK;
    yep_sink sink = {.on_event = yep_dom_on_event,
                     .ctx = dom,
                     .on_flow_build = dom_on_flow_build,
                     .on_flow_commit = dom_on_flow_commit,
                     .on_flow_rollback = dom_on_flow_rollback,
                     .on_block_pair = dom_on_block_pair,
                     .on_scalar = dom_on_scalar,
                     .on_block_open = dom_on_block_open,
                     .on_block_item = dom_on_block_item,
                     .compose = 1};
    if (eng == NULL || dom == NULL) {
        st = YEPTRIS_ERROR_MEMORY;
        goto out;
    }
    if (schema == YEPTRIS_SCHEMA_11_COMPAT) {
        yep_engine_set_resolver(eng, yep_resolver_compat11());
        dom->resolver = yep_resolver_compat11(); /* the DOM's direct
                                  builders resolve their own tag_ids —
                                  ST_ANY reads the NODE's verdict */
    }
    dom->input_base = source;
    dom->input_len = len;
    yep_dom_prepare_len(dom, len);
    if (yep_engine_run(eng, source, len, &sink) != 0 || dom->dcount == 0) {
        st = YEPTRIS_ERROR_PARSE;
        goto out;
    }

    {
        uint32_t* seen = seen_map_ptr = calloc(desc_len, sizeof(uint32_t));
        if (seen == NULL) {
            st = YEPTRIS_ERROR_MEMORY;
            goto out;
        }
        yep_schema_ctx c = {desc, desc_len, cols, dom, 0, 0, -1, seen};
        int rc = emit(&c, 0, dom->docs[0]);
        if (rc < 0 && c.type_mismatch >= 0) {
            yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, (uint32_t)c.type_mismatch,
                          "node %d: value is not a number", c.type_mismatch);
            st = YEPTRIS_ERROR_PARSE;
            goto out;
        }
        if (rc < 0) {
            st = YEPTRIS_ERROR_ARG;
            goto out;
        }
        for (uint32_t i = 0; i < desc_len; i++) {
            if ((desc[i].flags & YEP_SF_REQUIRED) && cols[i].count == 0 &&
                desc[i].wire_name != NULL) {
                put_error_schema(i);
                st = YEPTRIS_ERROR_SCHEMA;
                goto out;
            }
        }
        if (c.overflow) {
            st = YEPTRIS_ERROR_MEMORY; /* caller buffers too small */
            yep_error_set(yep_error_tls(), YEP_ERR_MEMORY, 0, 0, 0,
                          "output column capacity exceeded");
        }
    }

out:
    free(seen_map_ptr);
    yep_dom_destroy(dom);
    yep_engine_destroy(eng);
    return st;
}
