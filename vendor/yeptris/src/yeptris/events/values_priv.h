/* values_priv.h — the record-building machinery, shared with the
 * Marshal emitter (TODO.restructure/21).
 *
 * The value stream's SSOT lives in values.c; marshal.c consumes the
 * SAME ctx (records + arena) the drains produce, and the DOM
 * linearizer produces the same records the engine transform does, so
 * every route through the value format is byte-identical.
 */
#ifndef YEP_EVENTS_VALUES_PRIV_H
#define YEP_EVENTS_VALUES_PRIV_H

#include "../../include/yeptris/values.h"

struct yep_dom;

#define YEP_V_MAX_DEPTH 1024

typedef struct yep_value_ctx {
    /* records + arena as handed to hosts by the public drains */
    YeptrisValue* vals;
    size_t n, cap;
    char* arena;
    size_t arena_len, arena_cap;
    int oom;
    int unsupported;                   /* linearize: construct the emitter cannot express */
    uint8_t key_pend[YEP_V_MAX_DEPTH]; /* per open map: key slot taken */
    uint8_t is_seq[YEP_V_MAX_DEPTH];   /* per open frame: 1 = sequence —
                                        * entries never take key slots */
    int depth;
} yep_value_ctx;

/* values.c owns these; declared here for marshal.c and the linearizer. */
void yep_val_put(yep_value_ctx* c, const YeptrisValue* v);
uint32_t yep_val_arena_put(yep_value_ctx* c, const char* p, uint32_t len);
void yep_val_slot(yep_value_ctx* c, YeptrisValue* v);

/* engine route — now with the strict-JSON sniff: first non-space
 * byte '{'/'[' builds through the JSON scanner and the linearizer,
 * falling back to the engine on any grammar failure. Both routes
 * produce identical records (the differential gate). Returns 0 ok,
 * -1 oom, -2 parse (detail via the TLS error channel). */
int yep_values_from_input(const char* yaml, size_t len, int schema_compat, yep_value_ctx** out);

/* DOM route: preorder walk of a subtree producing the same records
 * the engine transform produces (21's linearizer). root_id may be
 * UINT32_MAX for an empty document; with_doc emits YEP_V_DOC records
 * around the root (the input path always documents). */
int yep_values_from_dom(const struct yep_dom* d, uint32_t root_id, int with_doc,
                        yep_value_ctx** out);

void yep_value_ctx_free(yep_value_ctx* c); /* frees records, arena, ctx */

#endif /* YEP_EVENTS_VALUES_PRIV_H */
