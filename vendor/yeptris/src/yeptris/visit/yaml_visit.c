/* yaml_visit.c — engine → visitor (TODO.restructure/24).
 *
 * Reuses the value-record pipeline (the conversion SSOT) and replays
 * records into the vtable. One allocation of records is still far
 * cheaper than host-side walks; the JSON fused path is the one that
 * must beat JSON.parse and skips records entirely. */

#include <string.h>

#include "../events/values_priv.h"

#include <yeptris/visit.h>

/* CLOSE needs open-kind: replay with a small kind stack. */
static int play_stacked(const yep_value_ctx* c, const YeptrisVisitVTable* vt, void* ctx) {
    const YeptrisValue* vals = c->vals;
    const char* arena = c->arena ? c->arena : "";
    unsigned char stack[1024];
    int sp = 0;
    for (size_t i = 0; i < c->n; i++) {
        const YeptrisValue* v = &vals[i];
        int rc = 0;
        switch (v->kind) {
        case YEP_V_DOC:
            if (vt->on_doc) {
                rc = vt->on_doc(ctx);
            }
            break;
        case YEP_V_NULL:
            if (vt->on_null) {
                rc = vt->on_null(ctx);
            }
            break;
        case YEP_V_BOOL:
            if (vt->on_bool) {
                rc = vt->on_bool(ctx, v->b);
            }
            break;
        case YEP_V_INT:
            if (vt->on_int) {
                rc = vt->on_int(ctx, (int64_t)v->p);
            }
            break;
        case YEP_V_FLOAT: {
            double d;
            memcpy(&d, &v->p, sizeof(d));
            if (vt->on_float) {
                rc = vt->on_float(ctx, d);
            }
            break;
        }
        case YEP_V_STR:
        case YEP_V_TIMESTAMP: {
            const char* s = arena + v->off;
            /* is_key alternation only MEANS something inside a map:
             * the slot rule marks a container's first record
             * spuriously when the parent is a sequence */
            int in_map = sp > 0 && stack[sp - 1] == 1;
            if (in_map && v->is_key && vt->on_key) {
                rc = vt->on_key(ctx, s, v->len);
            } else if (vt->on_string) {
                rc = vt->on_string(ctx, s, v->len);
            }
            break;
        }
        case YEP_V_SEQ_OPEN:
            if (sp < (int)sizeof(stack)) {
                stack[sp++] = 0;
            }
            if (vt->on_seq_start) {
                rc = vt->on_seq_start(ctx);
            }
            break;
        case YEP_V_MAP_OPEN:
            if (sp < (int)sizeof(stack)) {
                stack[sp++] = 1;
            }
            if (vt->on_map_start) {
                rc = vt->on_map_start(ctx);
            }
            break;
        case YEP_V_CLOSE:
            if (sp > 0) {
                unsigned char kind = stack[--sp];
                if (kind) {
                    if (vt->on_map_end) {
                        rc = vt->on_map_end(ctx);
                    }
                } else if (vt->on_seq_end) {
                    rc = vt->on_seq_end(ctx);
                }
            }
            break;
        case YEP_V_ANCHOR:
            if (vt->on_anchor) {
                rc = vt->on_anchor(ctx, arena + v->off, v->len);
            }
            break;
        case YEP_V_ALIAS:
            if (vt->on_alias) {
                rc = vt->on_alias(ctx, arena + v->off, v->len);
            }
            break;
        default:
            break;
        }
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

YEPTRIS_API YeptrisStatus yeptris_visit(const char* data, size_t len, YeptrisSchema schema,
                                        const YeptrisVisitVTable* vt, void* ctx) {
    if (vt == NULL || (data == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    yep_value_ctx* c = NULL;
    int rc = yep_values_from_input(data, len, schema == YEPTRIS_SCHEMA_11_COMPAT, &c);
    if (rc != 0) {
        return rc == -1 ? YEPTRIS_ERROR_MEMORY : YEPTRIS_ERROR_PARSE;
    }
    int prc = play_stacked(c, vt, ctx);
    yep_value_ctx_free(c);
    return prc == 0 ? YEPTRIS_OK : YEPTRIS_ERROR_INTERNAL;
}
