/* json_visit.c — fused RFC 8259 scan → visitor (TODO.restructure/24).
 *
 * One pass, no DOM, no records: the JSON kernels tokenize and the
 * vtable receives values directly. Escaped strings decode into a
 * small scratch that lives only for the on_string callback. */

#include <stdlib.h>
#include <string.h>

#include "../parse/numbers.h"
#include "../parse/scalars.h"
#include "../scan/json.h"

#include <yeptris/visit.h>

typedef struct {
    const char* p;
    size_t len;
    size_t i;
    const YeptrisVisitVTable* vt;
    void* ctx;
    char* scratch;
    size_t scratch_cap;
    int depth;
    int err; /* 0 ok, -1 oom/abort, -2 parse */
} jv;

#define YEP_JV_MAX_DEPTH 1000

static int jv_emit_str(jv* j, const char* s, size_t n) {
    if (j->vt->on_string == NULL) {
        return 0;
    }
    return j->vt->on_string(j->ctx, s, n);
}

static int jv_value(jv* j);

static void jv_ws(jv* j) {
    while (j->i < j->len) {
        char c = j->p[j->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            j->i++;
        } else {
            break;
        }
    }
}

static int jv_string(jv* j, int as_key) {
    size_t start = j->i;
    size_t close = 0;
    int has_esc = 0;
    if (!yep_json_string(j->p, j->len, &j->i, &close, &has_esc)) {
        j->err = -2;
        return -1;
    }
    const char* sp;
    size_t sl;
    if (has_esc) {
        uint32_t span = (uint32_t)(close - start - 1);
        if (span + 1 > j->scratch_cap) {
            size_t cap = j->scratch_cap ? j->scratch_cap : 64;
            while (cap < span + 1) {
                cap *= 2;
            }
            char* ns = realloc(j->scratch, cap);
            if (ns == NULL) {
                j->err = -1;
                return -1;
            }
            j->scratch = ns;
            j->scratch_cap = cap;
        }
        sl = yep_finish_double_into(j->p, (uint32_t)(start + 1), (uint32_t)close, j->scratch, span);
        sp = j->scratch;
    } else {
        sp = j->p + start + 1;
        sl = close - start - 1;
    }
    int rc;
    if (as_key && j->vt->on_key != NULL) {
        rc = j->vt->on_key(j->ctx, sp, sl);
    } else {
        rc = jv_emit_str(j, sp, sl);
    }
    if (rc != 0) {
        j->err = -1;
        return -1;
    }
    return 0;
}

static int jv_number(jv* j) {
    size_t start = j->i;
    if (!yep_json_number(j->p, j->len, &j->i)) {
        j->err = -2;
        return -1;
    }
    const char* s = j->p + start;
    uint32_t n = (uint32_t)(j->i - start);
    /* integer if no '.' and no exponent — matches JSON.parse */
    int is_float = 0;
    for (uint32_t k = 0; k < n; k++) {
        char c = s[k];
        if (c == '.' || c == 'e' || c == 'E') {
            is_float = 1;
            break;
        }
    }
    if (is_float) {
        double d = 0.0;
        if (yep_num_f64(s, n, &d) != 0) {
            j->err = -2;
            return -1;
        }
        if (j->vt->on_float != NULL && j->vt->on_float(j->ctx, d) != 0) {
            j->err = -1;
            return -1;
        }
    } else {
        int64_t v = 0;
        if (yep_num_i64(s, n, &v) != 0) {
            /* overflow: hand up as double (JSON.parse does this past
             * 2^53 roughly; we keep int64 max then float) */
            double d = 0.0;
            if (yep_num_f64(s, n, &d) != 0) {
                j->err = -2;
                return -1;
            }
            if (j->vt->on_float != NULL && j->vt->on_float(j->ctx, d) != 0) {
                j->err = -1;
                return -1;
            }
        } else if (j->vt->on_int != NULL && j->vt->on_int(j->ctx, v) != 0) {
            j->err = -1;
            return -1;
        }
    }
    return 0;
}

static int jv_literal(jv* j, const char* word, int kind) {
    if (!yep_json_literal(j->p, j->len, &j->i, word)) {
        j->err = -2;
        return -1;
    }
    int rc = 0;
    if (kind == 0) { /* null */
        if (j->vt->on_null) {
            rc = j->vt->on_null(j->ctx);
        }
    } else if (j->vt->on_bool) {
        rc = j->vt->on_bool(j->ctx, kind > 0);
    }
    if (rc != 0) {
        j->err = -1;
        return -1;
    }
    return 0;
}

static int jv_container(jv* j, int is_map) {
    if (j->depth >= YEP_JV_MAX_DEPTH) {
        j->err = -2;
        return -1;
    }
    j->i++; /* skip [ or { */
    j->depth++;
    if (is_map) {
        if (j->vt->on_map_start && j->vt->on_map_start(j->ctx) != 0) {
            j->err = -1;
            return -1;
        }
    } else if (j->vt->on_seq_start && j->vt->on_seq_start(j->ctx) != 0) {
        j->err = -1;
        return -1;
    }
    jv_ws(j);
    char close = is_map ? '}' : ']';
    if (j->i < j->len && j->p[j->i] == close) {
        j->i++;
        j->depth--;
        if (is_map) {
            if (j->vt->on_map_end && j->vt->on_map_end(j->ctx) != 0) {
                j->err = -1;
                return -1;
            }
        } else if (j->vt->on_seq_end && j->vt->on_seq_end(j->ctx) != 0) {
            j->err = -1;
            return -1;
        }
        return 0;
    }
    for (;;) {
        if (is_map) {
            jv_ws(j);
            if (j->i >= j->len || j->p[j->i] != '"') {
                j->err = -2;
                return -1;
            }
            if (jv_string(j, 1) != 0) {
                return -1;
            }
            jv_ws(j);
            if (j->i >= j->len || j->p[j->i] != ':') {
                j->err = -2;
                return -1;
            }
            j->i++;
            if (jv_value(j) != 0) {
                return -1;
            }
        } else if (jv_value(j) != 0) {
            return -1;
        }
        jv_ws(j);
        if (j->i >= j->len) {
            j->err = -2;
            return -1;
        }
        if (j->p[j->i] == ',') {
            j->i++;
            continue;
        }
        if (j->p[j->i] == close) {
            j->i++;
            j->depth--;
            if (is_map) {
                if (j->vt->on_map_end && j->vt->on_map_end(j->ctx) != 0) {
                    j->err = -1;
                    return -1;
                }
            } else if (j->vt->on_seq_end && j->vt->on_seq_end(j->ctx) != 0) {
                j->err = -1;
                return -1;
            }
            return 0;
        }
        j->err = -2;
        return -1;
    }
}

static int jv_value(jv* j) {
    jv_ws(j);
    if (j->i >= j->len) {
        j->err = -2;
        return -1;
    }
    char c = j->p[j->i];
    if (c == '{') {
        return jv_container(j, 1);
    }
    if (c == '[') {
        return jv_container(j, 0);
    }
    if (c == '"') {
        return jv_string(j, 0);
    }
    if (c == 't') {
        return jv_literal(j, "true", 1);
    }
    if (c == 'f') {
        return jv_literal(j, "false", -1);
    }
    if (c == 'n') {
        return jv_literal(j, "null", 0);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return jv_number(j);
    }
    j->err = -2;
    return -1;
}

YEPTRIS_API YeptrisStatus yeptris_visit_json(const char* data, size_t len,
                                             const YeptrisVisitVTable* vt, void* ctx) {
    if (vt == NULL || (data == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    jv j = {data, len, 0, vt, ctx, NULL, 0, 0, 0};
    if (jv_value(&j) != 0) {
        free(j.scratch);
        return j.err == -1 ? YEPTRIS_ERROR_MEMORY : YEPTRIS_ERROR_PARSE;
    }
    jv_ws(&j);
    free(j.scratch);
    if (j.i != len) {
        return YEPTRIS_ERROR_PARSE;
    }
    return YEPTRIS_OK;
}
