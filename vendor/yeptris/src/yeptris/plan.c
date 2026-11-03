/* plan.c — the compiled plan walk (TODO.restructure/87 slice one,
 * yeptris#293). Compile: the spec parses through the strict JSON DOM,
 * then lands as a flat column array + the collection path. Walk: one
 * linear pass over the tape's records — find the rows container,
 * count rows, then fill the typed columns per row by comparing key
 * spans against the planned leaf names (no host dispatch, no tree). */

#include <stdlib.h>
#include <string.h>

#include <yeptris.h>
#include <yeptris/json.h>
#include <yeptris/plan.h>

#include "doc.h"
#include "dom/dom.h"
#include "parse/numbers.h"
#include "scan/json.h"

/* sview decode (the writer's wv, over the document's two regions) */
static yep_view sv_view(const yep_dom* d, yep_sview sv) {
    yep_view v = {NULL, 0};
    if (sv.len == 0) {
        return v;
    }
    v.len = sv.len;
    v.p = ((sv.off & YEP_SV_INPUT) ? d->str : d->input_base) + (sv.off & YEP_SV_OFF);
    return v;
}

typedef struct {
    char* name;
    uint32_t name_len;
    int kind;
} yep_plan_col;

struct yeptris_plan {
    yep_plan_col* cols;
    size_t ncols;
    char* path; /* the path segments' bytes, concatenated */
    size_t path_len;
    uint32_t* seg_off; /* nsegs entries: segment start inside path */
    uint32_t* seg_len;
    size_t nsegs; /* 0 = the root itself is the rows container */
};

typedef struct {
    int kind;
    int64_t* ints;
    double* floats;
    uint32_t* offs;
    uint32_t* lens;
    yeptris_plan_str* views; /* the DOM leg's str lane: (ptr,len) into the doc */
    uint8_t* nulls;
} yep_result_col;

struct yeptris_plan_result {
    size_t rows;
    size_t ncols;
    yep_result_col* cols;
    void* block; /* one carved allocation backs every array */
};

YEPTRIS_API yeptris_plan* yeptris_plan_compile(const char* spec, size_t len, YeptrisStatus* st) {
    if (st != NULL) {
        *st = YEPTRIS_OK;
    }
    if (spec == NULL && len != 0) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_ARG;
        }
        return NULL;
    }
    YeptrisStatus pst = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse_json(spec, len, &pst);
    if (doc == NULL) {
        if (st != NULL) {
            *st = pst;
        }
        return NULL;
    }
    yeptris_document* d = (yeptris_document*)doc;
    yeptris_plan* plan = calloc(1, sizeof(*plan)); /* before any goto: out frees it */
    if (plan == NULL) {
        goto mem;
    }
    yep_dom* dd = yep_doc_dom(d); /* #342 lazy: the spec parse may defer */
    if (dd == NULL) {
        goto mem;
    }
    const yep_dnode* root = yep_dom_node(dd, dd->docs[0]);
    if (root == NULL || root->kind != 2) { /* the spec root is a mapping */
        goto bad;
    }
    /* kind: "seq" (rows under the path key or the root itself) or
     * "map" (rows under the path key); children: the leaf columns */
    const yep_dnode* path_node = NULL;
    const yep_dnode* children = NULL;
    int saw_kind = 0;
    uint32_t pair = root->first_child;
    while (pair != UINT32_MAX) {
        const yep_dnode* kn = yep_dom_node(dd, pair);
        const yep_dnode* vn = yep_dom_node(dd, kn->next_sibling);
        yep_view kv = sv_view(dd, kn->value);
        yep_view vv = sv_view(dd, vn->value);
        if (kv.len == 4 && memcmp(kv.p, "kind", 4) == 0) {
            saw_kind = 1;
            if (!(vv.len == 3 && (memcmp(vv.p, "seq", 3) == 0 || memcmp(vv.p, "map", 3) == 0))) {
                goto bad;
            }
        } else if (kv.len == 4 && memcmp(kv.p, "path", 4) == 0) {
            if (vn->kind != 0 && vn->kind != 1) { /* a string or a string seq */
                goto bad;
            }
            path_node = vn;
        } else if (kv.len == 8 && memcmp(kv.p, "children", 8) == 0) {
            if (vn->kind != 1) { /* children is a sequence */
                goto bad;
            }
            children = vn;
        } else {
            goto bad; /* unknown spec key */
        }
        pair = vn->next_sibling;
    }
    if (!saw_kind || children == NULL) {
        goto bad;
    }
    plan->path = calloc(1, 1);
    if (plan->path == NULL) {
        goto mem;
    }
    if (path_node != NULL && path_node->kind == 0) {
        if (path_node->tag_id != YEPTRIS_TAG_STR) {
            goto bad; /* the path is a string, not a number */
        }
        yep_view vv = sv_view(dd, path_node->value);
        if (vv.len != 0) { /* "" rides the seq root like no path */
            plan->seg_off = malloc(sizeof(uint32_t));
            plan->seg_len = malloc(sizeof(uint32_t));
            if (plan->seg_off == NULL || plan->seg_len == NULL) {
                goto mem;
            }
            free(plan->path);
            plan->path = malloc(vv.len + 1);
            if (plan->path == NULL) {
                goto mem;
            }
            memcpy(plan->path, vv.p, vv.len);
            plan->path[vv.len] = 0;
            plan->path_len = vv.len;
            plan->seg_off[0] = 0;
            plan->seg_len[0] = vv.len;
            plan->nsegs = 1;
        }
    } else if (path_node != NULL) {
        size_t n = path_node->count;
        plan->seg_off = calloc(n, sizeof(uint32_t));
        plan->seg_len = calloc(n, sizeof(uint32_t));
        if (n != 0 && (plan->seg_off == NULL || plan->seg_len == NULL)) {
            goto mem;
        }
        size_t off = 0;
        uint32_t cn = path_node->first_child;
        for (size_t i = 0; i < n; i++) {
            const yep_dnode* seg = yep_dom_node(dd, cn);
            if (seg == NULL || seg->kind != 0 || seg->tag_id != YEPTRIS_TAG_STR) {
                goto bad; /* every segment is a string */
            }
            yep_view sv = sv_view(dd, seg->value);
            if (sv.len == 0) {
                goto bad;
            }
            char* grown = realloc(plan->path, off + sv.len + 1);
            if (grown == NULL) {
                goto mem;
            }
            plan->path = grown;
            memcpy(plan->path + off, sv.p, sv.len);
            plan->seg_off[i] = (uint32_t)off;
            plan->seg_len[i] = sv.len;
            off += sv.len;
            cn = seg->next_sibling;
        }
        plan->path_len = off;
        plan->nsegs = n;
    }
    plan->ncols = children->count;
    if (plan->ncols == 0) {
        goto bad;
    }
    plan->cols = calloc(plan->ncols, sizeof(*plan->cols));
    if (plan->cols == NULL) {
        goto mem;
    }
    uint32_t cn = children->first_child;
    for (size_t i = 0; i < plan->ncols; i++) {
        const yep_dnode* leaf = yep_dom_node(dd, cn);
        if (leaf == NULL || leaf->kind != 2) {
            goto bad;
        }
        const char* name = NULL;
        uint32_t name_len = 0;
        int kind = -1;
        uint32_t lp = leaf->first_child;
        while (lp != UINT32_MAX) {
            const yep_dnode* lk = yep_dom_node(dd, lp);
            const yep_dnode* lv = yep_dom_node(dd, lk->next_sibling);
            yep_view lkv = sv_view(dd, lk->value);
            yep_view lvv = sv_view(dd, lv->value);
            if (lkv.len == 4 && memcmp(lkv.p, "name", 4) == 0) {
                if (lv->kind != 0 || lvv.len == 0) {
                    goto bad;
                }
                name = lvv.p;
                name_len = lvv.len;
            } else if (lkv.len == 4 && memcmp(lkv.p, "kind", 4) == 0) {
                if (lvv.len == 3 && memcmp(lvv.p, "int", 3) == 0) {
                    kind = YEP_PLAN_INT;
                } else if (lvv.len == 5 && memcmp(lvv.p, "float", 5) == 0) {
                    kind = YEP_PLAN_FLOAT;
                } else if (lvv.len == 3 && memcmp(lvv.p, "str", 3) == 0) {
                    kind = YEP_PLAN_STR;
                } else if (lvv.len == 4 && memcmp(lvv.p, "bool", 4) == 0) {
                    kind = YEP_PLAN_BOOL;
                } else {
                    goto bad;
                }
            } else {
                goto bad;
            }
            lp = lv->next_sibling;
        }
        if (name == NULL || kind < 0) {
            goto bad;
        }
        plan->cols[i].name = malloc(name_len + 1);
        if (plan->cols[i].name == NULL) {
            goto mem;
        }
        memcpy(plan->cols[i].name, name, name_len);
        plan->cols[i].name[name_len] = 0;
        plan->cols[i].name_len = name_len;
        plan->cols[i].kind = kind;
        cn = leaf->next_sibling;
    }
    if (cn != UINT32_MAX) {
        goto bad; /* more children than counted */
    }
    yeptris_document_free(doc);
    return plan;

mem:
    if (st != NULL) {
        *st = YEPTRIS_ERROR_MEMORY;
    }
    goto out;
bad:
    if (st != NULL) {
        *st = YEPTRIS_ERROR_ARG;
    }
out:
    if (doc != NULL) {
        yeptris_document_free(doc);
    }
    yeptris_plan_free(plan);
    return NULL;
}

YEPTRIS_API void yeptris_plan_free(yeptris_plan* plan) {
    if (plan == NULL) {
        return;
    }
    for (size_t i = 0; i < plan->ncols; i++) {
        free(plan->cols[i].name);
    }
    free(plan->cols);
    free(plan->path);
    free(plan->seg_off);
    free(plan->seg_len);
    free(plan);
}

YEPTRIS_API size_t yeptris_plan_column_count(const yeptris_plan* plan) {
    return plan == NULL ? 0 : plan->ncols;
}

/* Finds the rows container's OPEN record: the root OPEN's child
 * matching each path segment in turn (a map root), or the root OPEN
 * itself (a seq root). Returns the record index of the container
 * OPEN, or 0 (the DOC record) when the shape disagrees. */
static size_t find_rows_open(const yeptris_json_tape* t, const yeptris_plan* plan) {
    size_t root = 1; /* the root OPEN (tape records start after DOC) */
    if (root >= t->count) {
        return 0;
    }
    if (t->kinds[root] != YEP_T_SEQ_OPEN && t->kinds[root] != YEP_T_MAP_OPEN) {
        return 0;
    }
    if (plan->nsegs == 0) {
        return t->kinds[root] == YEP_T_SEQ_OPEN ? root : 0;
    }
    /* map root: walk the segments. Pairs are (STR key, value record);
     * a container value spans to its CLOSE (offs link) */
    size_t cur = root;
    for (size_t s = 0; s < plan->nsegs; s++) {
        if (t->kinds[cur] != YEP_T_MAP_OPEN) {
            return 0;
        }
        const char* seg = plan->path + plan->seg_off[s];
        uint32_t seg_len = plan->seg_len[s];
        size_t i = cur + 1;
        int found = 0;
        while (i < t->count && i < t->offs[cur]) {
            if (t->kinds[i] != YEP_T_STR) {
                return 0; /* not a key position */
            }
            if (t->lens[i] == seg_len &&
                memcmp((const char*)t->_src + t->offs[i], seg, seg_len) == 0) {
                size_t v = i + 1;
                if (v >= t->count) {
                    return 0;
                }
                if (s + 1 < plan->nsegs) {
                    if (t->kinds[v] != YEP_T_MAP_OPEN) {
                        return 0; /* the next segment needs a mapping */
                    }
                    cur = v;
                } else {
                    return v; /* the rows container's OPEN (or scalar) */
                }
                found = 1;
                break;
            }
            i++; /* the value */
            uint8_t vk = t->kinds[i];
            if (vk == YEP_T_MAP_OPEN || vk == YEP_T_SEQ_OPEN) {
                i = t->offs[i]; /* skip to its CLOSE */
            }
            i++;
        }
        if (!found) {
            return 0;
        }
    }
    return 0; /* unreachable with nsegs > 0 */
}

/* Carves the result: cols array, then the typed lanes (8*rows bytes
 * each — ints/floats directly, tape-str = offs+lens, DOM-str =
 * views), then the null bitmaps last so every typed base stays
 * 8-aligned. str_row_bytes is the per-row footprint of a str
 * column's lane (8 for the tape leg, sizeof(yeptris_plan_str) for the DOM
 * leg). Returns NULL with *st on overflow/allocation failure. */
static yeptris_plan_result* plan_result_carve(const yeptris_plan* plan, size_t rows,
                                              size_t str_row_bytes, YeptrisStatus* st) {
    size_t ncols = plan->ncols;
    size_t col_bytes = ncols * sizeof(yep_result_col);
    if (ncols != 0 && rows != 0 && rows > (SIZE_MAX - col_bytes) / (str_row_bytes + 9)) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_MEMORY;
        }
        return NULL;
    }
    size_t need = col_bytes;
    for (size_t c = 0; c < ncols; c++) {
        size_t lane = plan->cols[c].kind == YEP_PLAN_STR ? str_row_bytes : 8;
        need += rows * lane + rows;
    }
    char* block = malloc(need);
    if (block == NULL) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_MEMORY;
        }
        return NULL;
    }
    yeptris_plan_result* r = calloc(1, sizeof(*r));
    if (r == NULL) {
        free(block);
        if (st != NULL) {
            *st = YEPTRIS_ERROR_MEMORY;
        }
        return NULL;
    }
    r->rows = rows;
    r->ncols = ncols;
    r->block = block;
    r->cols = (yep_result_col*)block;
    memset(block, 0, col_bytes);
    char* p = block + col_bytes;
    for (size_t c = 0; c < ncols; c++) {
        yep_result_col* col = &r->cols[c];
        col->kind = plan->cols[c].kind;
        if (col->kind == YEP_PLAN_FLOAT) {
            col->floats = (double*)p;
            p += rows * sizeof(double);
        } else if (col->kind == YEP_PLAN_STR) {
            if (str_row_bytes == sizeof(yeptris_plan_str)) {
                col->views = (yeptris_plan_str*)p;
                p += rows * sizeof(yep_view);
            } else {
                col->offs = (uint32_t*)p;
                p += rows * sizeof(uint32_t);
                col->lens = (uint32_t*)p;
                p += rows * sizeof(uint32_t);
            }
        } else {
            col->ints = (int64_t*)p; /* int and bool share the lane */
            p += rows * sizeof(int64_t);
        }
    }
    for (size_t c = 0; c < ncols; c++) {
        r->cols[c].nulls = (uint8_t*)p;
        memset(r->cols[c].nulls, 1, rows); /* missing until matched */
        p += rows;
    }
    return r;
}

YEPTRIS_API yeptris_plan_result*
yeptris_tape_plan_walk(const yeptris_json_tape* tape, const yeptris_plan* plan, YeptrisStatus* st) {
    /* item 07: the lenient tape's columns are lazy — materialize (a
     * cache write through a const view; idempotent) */
    yeptris_tape_columns((yeptris_json_tape*)tape);
    if (st != NULL) {
        *st = YEPTRIS_OK;
    }
    if (tape == NULL || plan == NULL || tape->count < 2) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_ARG;
        }
        return NULL;
    }
    size_t rows_open = find_rows_open(tape, plan);
    if (rows_open == 0) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_PARSE; /* document shape disagreement */
        }
        return NULL;
    }
    uint8_t rows_kind = tape->kinds[rows_open];
    if (rows_kind != YEP_T_SEQ_OPEN && rows_kind != YEP_T_MAP_OPEN) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_PARSE;
        }
        return NULL;
    }
    size_t rows_close = tape->offs[rows_open];

    /* pass one: count the row MAP_OPENs directly inside the container */
    size_t rows = 0;
    {
        int depth = 0;
        for (size_t i = rows_open + 1; i < rows_close; i++) {
            uint8_t k = tape->kinds[i];
            if (depth == 0 && k == YEP_T_MAP_OPEN) {
                rows++;
            }
            if (k == YEP_T_MAP_OPEN || k == YEP_T_SEQ_OPEN) {
                depth++;
            } else if (k == YEP_T_CLOSE) {
                depth--;
            }
        }
    }

    size_t ncols = plan->ncols;
    yeptris_plan_result* r = plan_result_carve(plan, rows, 8, st);
    if (r == NULL) {
        return NULL;
    }

    /* pass two: fill. Rows are MAP_OPEN..CLOSE at container depth 0;
     * each pair's key span matches a planned leaf name. */
    const char* src = (const char*)tape->_src;
    size_t row = 0;
    int depth = 0;
    int row_start = 0;
    for (size_t i = rows_open + 1; i < rows_close && row < rows; i++) {
        uint8_t k = tape->kinds[i];
        if (k == YEP_T_MAP_OPEN || k == YEP_T_SEQ_OPEN) {
            if (depth == 0 && k == YEP_T_MAP_OPEN) {
                row_start = 1; /* inside a row */
            }
            depth++;
            continue;
        }
        if (k == YEP_T_CLOSE) {
            depth--;
            if (depth == 0 && row_start) {
                row++;
                row_start = 0;
            }
            continue;
        }
        if (depth != 1 || !row_start || k != YEP_T_STR) {
            continue; /* not a key position inside a row map */
        }
        /* the key: match against the plan leaves */
        int col_idx = -1;
        for (size_t c = 0; c < ncols; c++) {
            if (tape->lens[i] == plan->cols[c].name_len &&
                memcmp(src + tape->offs[i], plan->cols[c].name, plan->cols[c].name_len) == 0) {
                col_idx = (int)c;
                break;
            }
        }
        if (col_idx < 0) {
            continue; /* not a planned leaf: skip its value below */
        }
        size_t v = i + 1;
        if (v >= rows_close) {
            break;
        }
        uint8_t vk = tape->kinds[v];
        yep_result_col* col = &r->cols[col_idx];
        switch (vk) {
        case YEP_T_NULL:
            break; /* stays null */
        case YEP_T_TRUE:
        case YEP_T_FALSE:
            if (col->kind == YEP_PLAN_BOOL) {
                col->ints[row] = vk == YEP_T_TRUE ? 1 : 0;
                col->nulls[row] = 0;
            } else if (col->kind == YEP_PLAN_STR) {
                col->offs[row] = tape->offs[v];
                col->lens[row] = tape->lens[v];
                col->nulls[row] = 0;
            }
            break;
        case YEP_T_STR:
            if (col->kind == YEP_PLAN_STR) {
                col->offs[row] = tape->offs[v];
                col->lens[row] = tape->lens[v];
                col->nulls[row] = 0;
            }
            break;
        case YEP_T_INT:
        case YEP_T_FLOAT:
        case YEP_T_NUM: {
            size_t j = 0;
            int64_t iv = 0;
            double dv = 0;
            int shape = 0;
            if (!yep_json_number_scan(src + tape->offs[v], tape->lens[v], &j, &shape, &iv, &dv) ||
                j != tape->lens[v]) {
                goto bad_row_value;
            }
            if (col->kind == YEP_PLAN_INT && shape == 0) {
                col->ints[row] = iv;
                col->nulls[row] = 0;
            } else if (col->kind == YEP_PLAN_FLOAT) {
                col->floats[row] = shape == 0 ? (double)iv : dv;
                col->nulls[row] = 0;
            } else if (col->kind == YEP_PLAN_STR) {
                col->offs[row] = tape->offs[v];
                col->lens[row] = tape->lens[v];
                col->nulls[row] = 0;
            }
            break;
        }
        default:
            /* container or mismatched scalar at a planned leaf: mark
             * the row value unusable but keep walking */
            break;
        }
        i = v; /* the value record is consumed */
        continue;
    bad_row_value:
        i = v;
        continue;
    }
    return r;
}

/* ---- the DOM leg (the YAML form of #293): the same compiled plan
 * applied to a parsed yeptris document. Typed extraction rides the
 * parse-time tag_id (the typing SSOT) plus the number kernels; str
 * columns expose (ptr,len) views into the document's regions — the
 * result borrows the document, free it first. ---- */

/* Follows alias chains to their target (bounded: a cycle fails). */
static const yep_dnode* dom_alias_final(const yep_dom* d, const yep_dnode* n) {
    for (int hops = 0; n != NULL && n->kind == YEP_DOM_ALIAS; hops++) {
        if (hops >= 64 || n->target == UINT32_MAX) {
            return NULL;
        }
        n = yep_dom_node(d, n->target);
    }
    return n;
}

/* The rows container: the root (no path), or the mapping reached by
 * walking the path segments. NULL when the shape disagrees. */
static const yep_dnode* dom_find_rows(const yep_dom* d, const yep_dnode* root,
                                      const yeptris_plan* plan) {
    const yep_dnode* cur = dom_alias_final(d, root);
    if (plan->nsegs == 0) {
        return (cur != NULL && cur->kind == YEP_DOM_SEQUENCE) ? cur : NULL;
    }
    for (size_t s = 0; s < plan->nsegs; s++) {
        if (cur == NULL || cur->kind != YEP_DOM_MAPPING) {
            return NULL;
        }
        const char* seg = plan->path + plan->seg_off[s];
        uint32_t seg_len = plan->seg_len[s];
        const yep_dnode* hit = NULL;
        uint32_t cn = cur->first_child;
        while (cn != UINT32_MAX) {
            const yep_dnode* k = yep_dom_node(d, cn);
            const yep_dnode* v = yep_dom_node(d, k->next_sibling);
            yep_view kv = sv_view(d, k->value);
            if (k->kind == YEP_DOM_SCALAR && kv.len == seg_len && memcmp(kv.p, seg, seg_len) == 0) {
                hit = dom_alias_final(d, v);
                break;
            }
            cn = v->next_sibling;
        }
        if (hit == NULL) {
            return NULL;
        }
        cur = hit;
    }
    if (cur == NULL || (cur->kind != YEP_DOM_SEQUENCE && cur->kind != YEP_DOM_MAPPING)) {
        return NULL;
    }
    return cur;
}

/* One row iteration: a SEQUENCE container yields its mapping
 * children, a MAPPING container its mapping values. Returns the
 * next cursor (UINT32_MAX at the end); *out is the row (NULL when
 * the child is not a mapping — callers skip it). */
static uint32_t dom_rows_next(const yep_dom* d, const yep_dnode* container, uint32_t cur,
                              const yep_dnode** out) {
    *out = NULL;
    if (cur == UINT32_MAX) {
        return UINT32_MAX;
    }
    if (container->kind == YEP_DOM_SEQUENCE) {
        const yep_dnode* self = yep_dom_node(d, cur);
        const yep_dnode* child = dom_alias_final(d, self);
        uint32_t next = self->next_sibling;
        if (child != NULL && child->kind == YEP_DOM_MAPPING) {
            *out = child;
        }
        return next;
    }
    /* mapping: pairs (key, value) — yield values */
    const yep_dnode* k = yep_dom_node(d, cur);
    const yep_dnode* v = yep_dom_node(d, k->next_sibling);
    const yep_dnode* val = dom_alias_final(d, v);
    if (val != NULL && val->kind == YEP_DOM_MAPPING) {
        *out = val;
    }
    return v->next_sibling;
}

YEPTRIS_API yeptris_plan_result*
yeptris_document_plan_walk(YeptrisDocument doc, const yeptris_plan* plan, YeptrisStatus* st) {
    if (st != NULL) {
        *st = YEPTRIS_OK;
    }
    const yep_dom* dd = doc == NULL ? NULL : yep_doc_dom((yeptris_document*)doc);
    if (doc == NULL || plan == NULL || dd == NULL || dd->dcount == 0) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_ARG;
        }
        return NULL;
    }
    const yep_dnode* root = yep_dom_node(dd, dd->docs[0]);
    const yep_dnode* container = dom_find_rows(dd, root, plan);
    if (container == NULL) {
        if (st != NULL) {
            *st = YEPTRIS_ERROR_PARSE; /* document shape disagreement */
        }
        return NULL;
    }

    size_t rows = 0;
    for (uint32_t cur = container->first_child; cur != UINT32_MAX;) {
        const yep_dnode* row;
        cur = dom_rows_next(dd, container, cur, &row);
        if (row != NULL) {
            rows++;
        }
    }

    yeptris_plan_result* r = plan_result_carve(plan, rows, sizeof(yeptris_plan_str), st);
    if (r == NULL) {
        return NULL;
    }

    size_t row_i = 0;
    for (uint32_t cur = container->first_child; cur != UINT32_MAX && row_i < rows;) {
        const yep_dnode* row;
        cur = dom_rows_next(dd, container, cur, &row);
        if (row == NULL) {
            continue;
        }
        uint32_t cn = row->first_child;
        while (cn != UINT32_MAX) {
            const yep_dnode* k = yep_dom_node(dd, cn);
            const yep_dnode* v = yep_dom_node(dd, k->next_sibling);
            yep_view kv = sv_view(dd, k->value);
            int col_idx = -1;
            if (k->kind == YEP_DOM_SCALAR) {
                for (size_t c = 0; c < plan->ncols; c++) {
                    if (kv.len == plan->cols[c].name_len &&
                        memcmp(kv.p, plan->cols[c].name, plan->cols[c].name_len) == 0) {
                        col_idx = (int)c;
                        break;
                    }
                }
            }
            if (col_idx >= 0) {
                const yep_dnode* val = dom_alias_final(dd, v);
                yep_result_col* col = &r->cols[col_idx];
                if (val != NULL && val->kind == YEP_DOM_SCALAR && val->tag_id != YEPTRIS_TAG_NULL) {
                    yep_view sv = sv_view(dd, val->value);
                    int filled = 0;
                    switch (col->kind) {
                    case YEP_PLAN_STR:
                        col->views[row_i].p = sv.p;
                        col->views[row_i].len = sv.len;
                        filled = 1;
                        break;
                    case YEP_PLAN_INT: {
                        int64_t iv;
                        if (val->tag_id == YEPTRIS_TAG_INT && yep_num_i64(sv.p, sv.len, &iv) == 0) {
                            col->ints[row_i] = iv;
                            filled = 1;
                        }
                        break;
                    }
                    case YEP_PLAN_FLOAT: {
                        double dv;
                        if (val->tag_id == YEPTRIS_TAG_FLOAT) {
                            if (yep_num_f64(sv.p, sv.len, &dv) == 0) {
                                col->floats[row_i] = dv;
                                filled = 1;
                            }
                        } else if (val->tag_id == YEPTRIS_TAG_INT) {
                            int64_t iv;
                            if (yep_num_i64(sv.p, sv.len, &iv) == 0) {
                                col->floats[row_i] = (double)iv;
                                filled = 1;
                            }
                        }
                        break;
                    }
                    case YEP_PLAN_BOOL:
                        if (val->tag_id == YEPTRIS_TAG_BOOL) {
                            col->ints[row_i] = yep_num_bool(sv.p, sv.len);
                            filled = 1;
                        }
                        break;
                    }
                    if (filled) {
                        col->nulls[row_i] = 0;
                    }
                }
                /* null / missing / tag disagreement / a container at a
                 * planned leaf: the slot stays null, the walk goes on */
            }
            cn = v->next_sibling;
        }
        row_i++;
    }
    return r;
}

YEPTRIS_API void yeptris_plan_result_free(yeptris_plan_result* r) {
    if (r == NULL) {
        return;
    }
    free(r->block);
    free(r);
}

YEPTRIS_API size_t yeptris_plan_result_rows(const yeptris_plan_result* r) {
    return r == NULL ? 0 : r->rows;
}

YEPTRIS_API int yeptris_plan_result_kind(const yeptris_plan_result* r, size_t col) {
    if (r == NULL || col >= r->ncols) {
        return -1;
    }
    return r->cols[col].kind;
}

#define YEP_RESULT_ARR(name, type)                                                                 \
    YEPTRIS_API const type* yeptris_plan_result_##name(const yeptris_plan_result* r, size_t col) { \
        if (r == NULL || col >= r->ncols) {                                                        \
            return NULL;                                                                           \
        }                                                                                          \
        return r->cols[col].name;                                                                  \
    }

YEP_RESULT_ARR(ints, int64_t)
YEP_RESULT_ARR(floats, double)
YEP_RESULT_ARR(nulls, uint8_t)

YEPTRIS_API const uint32_t* yeptris_plan_result_str_offs(const yeptris_plan_result* r, size_t col) {
    if (r == NULL || col >= r->ncols) {
        return NULL;
    }
    return r->cols[col].offs;
}

YEPTRIS_API const yeptris_plan_str* yeptris_plan_result_strs(const yeptris_plan_result* r,
                                                             size_t col) {
    if (r == NULL || col >= r->ncols) {
        return NULL;
    }
    return r->cols[col].views;
}

YEPTRIS_API const uint32_t* yeptris_plan_result_str_lens(const yeptris_plan_result* r, size_t col) {
    if (r == NULL || col >= r->ncols) {
        return NULL;
    }
    return r->cols[col].lens;
}
