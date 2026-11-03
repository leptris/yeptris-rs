/* dom_visit.c — DOM subtree → visitor (TODO.restructure/24). */

#include <string.h>

#include "../dom/dom.h"
#include "../parse/numbers.h"
#include "doc.h"

#include <yeptris/resolve.h>
#include <yeptris/visit.h>

static int visit_id(const yep_dom* d, uint32_t id, const YeptrisVisitVTable* vt, void* ctx);

static int visit_id(const yep_dom* d, uint32_t id, const YeptrisVisitVTable* vt, void* ctx) {
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return -1;
    }
    if (n->anchor.len != 0 && n->kind != YEP_DOM_ALIAS && vt->on_anchor) {
        yep_view a = yep_dom_view(d, n->anchor);
        if (vt->on_anchor(ctx, a.p, a.len) != 0) {
            return -1;
        }
    }
    switch (n->kind) {
    case YEP_DOM_ALIAS: {
        yep_view a = yep_dom_view(d, n->value);
        if (vt->on_alias && vt->on_alias(ctx, a.p, a.len) != 0) {
            return -1;
        }
        return 0;
    }
    case YEP_DOM_SEQUENCE:
        if (vt->on_seq_start && vt->on_seq_start(ctx) != 0) {
            return -1;
        }
        for (uint32_t c = n->first_child; c != UINT32_MAX; c = d->nodes[c].next_sibling) {
            if (visit_id(d, c, vt, ctx) != 0) {
                return -1;
            }
        }
        if (vt->on_seq_end && vt->on_seq_end(ctx) != 0) {
            return -1;
        }
        return 0;
    case YEP_DOM_MAPPING:
        if (vt->on_map_start && vt->on_map_start(ctx) != 0) {
            return -1;
        }
        for (uint32_t c = n->first_child; c != UINT32_MAX;) {
            uint32_t k = c;
            uint32_t v = d->nodes[c].next_sibling;
            if (v == UINT32_MAX) {
                return -1;
            }
            /* key */
            const yep_dnode* kn = yep_dom_node(d, k);
            if (kn != NULL && kn->kind == YEP_DOM_SCALAR && vt->on_key) {
                yep_view kv = yep_dom_view(d, kn->value);
                if (vt->on_key(ctx, kv.p, kv.len) != 0) {
                    return -1;
                }
            } else if (visit_id(d, k, vt, ctx) != 0) {
                return -1;
            }
            if (visit_id(d, v, vt, ctx) != 0) {
                return -1;
            }
            c = d->nodes[v].next_sibling;
        }
        if (vt->on_map_end && vt->on_map_end(ctx) != 0) {
            return -1;
        }
        return 0;
    default: { /* scalar */
        yep_view s = yep_dom_view(d, n->value);
        switch (n->tag_id) {
        case YEPTRIS_TAG_NULL:
            return vt->on_null ? vt->on_null(ctx) : 0;
        case YEPTRIS_TAG_BOOL: {
            int b = yep_num_bool_ci(s.p, (uint32_t)s.len);
            return vt->on_bool ? vt->on_bool(ctx, b) : 0;
        }
        case YEPTRIS_TAG_INT: {
            int64_t v = 0;
            if (yep_num_i64(s.p, (uint32_t)s.len, &v) != 0) {
                return vt->on_string ? vt->on_string(ctx, s.p, s.len) : 0;
            }
            return vt->on_int ? vt->on_int(ctx, v) : 0;
        }
        case YEPTRIS_TAG_FLOAT: {
            double v = 0.0;
            if (yep_num_f64(s.p, (uint32_t)s.len, &v) != 0) {
                return vt->on_string ? vt->on_string(ctx, s.p, s.len) : 0;
            }
            return vt->on_float ? vt->on_float(ctx, v) : 0;
        }
        default:
            return vt->on_string ? vt->on_string(ctx, s.p, s.len) : 0;
        }
    }
    }
}

YEPTRIS_API YeptrisStatus yeptris_visit_node(YeptrisNode node, const YeptrisVisitVTable* vt,
                                             void* ctx) {
    if (node == NULL || vt == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    yeptris_node* h = (yeptris_node*)node;
    if (h->doc == NULL || h->doc->dom == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    return visit_id(h->doc->dom, h->id, vt, ctx) == 0 ? YEPTRIS_OK : YEPTRIS_ERROR_INTERNAL;
}
