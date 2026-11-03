/* compile.c — the compiled-schema economy (#885 item 3).
 *
 * The leptris-XSD shape (~/src/leptris/leptris/src/leptris/xsd/):
 * parse the schema ONCE, retain the DOM and the derived state, validate
 * many instance streams against the retained model. The handle owns
 * everything it reads — the schema bytes (copied: the DOM's scalar
 * views borrow them), the parsed document (its DOM, pools and finish
 * pool die with yeptris_document_free), and the compiled pattern
 * programs (the XSD content-model analog: `(a+)+$` compiles once at
 * compile time, not once per validated string). One free call releases
 * it all — no refcounting, the document-ownership law.
 *
 * The pattern programs are populated EAGERLY by a DFS over the schema
 * tree (`pattern` values and `patternProperties` keys — the kernel's
 * only two consumers), so the handle is immutable after compile and
 * `yeptris_schema_validate` is a pure reader: one schema handle per
 * thread like documents; read-only sharing across threads is safe.
 * An invalid pattern is simply not retained — every use then recompiles
 * to the one-shot's -1 (ignored keyword), never a guess.
 *
 * The one-shot face (yeptris_json_schema_validate) routes through the
 * same compile+validate machinery internally: identical parse, walk
 * and report code paths, so its observable statuses and byte-for-byte
 * verdicts are preserved (the 1298-case suite port is the referee).
 */

#include <string.h>

#include <yeptris/dom.h>
#include <yeptris/validate.h>

#include "../common/error.h"
#include "../doc.h"
#include "../memory/allocator.h"
#include "../tape_in.h"
#include "internal.h"

/* The compiled schema. Opaque at the face (validate.h); everything it
 * reads dies with it. */
struct yeptris_schema {
    char* bytes;           /* owned: the schema bytes (the DOM's input views borrow them) */
    yeptris_document* doc; /* owned: the parsed schema document */
    const yep_dom* dom;    /* borrowed from doc (yep_doc_dom) */
    uint32_t sroot;        /* the schema document's root (docs[0]) */
    yep_re_cache* cache;   /* owned: pattern source -> compiled NFA; immutable after compile */
};

/* ---- the pattern collection (compile-time, eager) ----------------------- */

static int collect_patterns(yep_re_cache* t, const yep_dom* d, uint32_t id, int depth);

/* Collects every pattern source of one mapping pair set. 0 ok, -1 OOM. */
static int collect_map(yep_re_cache* t, const yep_dom* d, uint32_t map_id, int depth) {
    const yep_dnode* m = yep_dom_node(d, map_id);
    uint32_t k = m->first_child;
    while (k != UINT32_MAX) {
        uint32_t v = yep_dom_node(d, k) ? yep_dom_node(d, k)->next_sibling : UINT32_MAX;
        const yep_dnode* kn = yep_dom_node(d, k);
        const yep_dnode* vn = v != UINT32_MAX ? yep_dom_node(d, v) : NULL;
        if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
            yep_view kv = yep_dom_view(d, kn->value);
            /* the keyword names, matched by bytes like the walk's kw_get */
            if (kv.len == 7 && memcmp(kv.p, "pattern", 7) == 0) {
                /* "pattern": the value is the pattern source (a string
                 * scalar, schema_word's exact read) */
                if (vn != NULL && vn->kind == YEP_DOM_SCALAR &&
                    yep_validate_scalar_tag(vn) == YEPTRIS_TAG_STR) {
                    yep_view vv = yep_dom_view(d, vn->value);
                    if (vv.p != NULL && yep_re_cache_add(t, vv.p, vv.len) < 0) {
                        return -1;
                    }
                }
            } else if (kv.len == 17 && memcmp(kv.p, "patternProperties", 17) == 0) {
                /* "patternProperties": its KEYS are the pattern sources */
                if (vn != NULL && vn->kind == YEP_DOM_MAPPING) {
                    uint32_t pk = vn->first_child;
                    while (pk != UINT32_MAX) {
                        uint32_t pv =
                            yep_dom_node(d, pk) ? yep_dom_node(d, pk)->next_sibling : UINT32_MAX;
                        const yep_dnode* pkn = yep_dom_node(d, pk);
                        if (pkn != NULL && pkn->kind == YEP_DOM_SCALAR &&
                            yep_validate_scalar_tag(pkn) == YEPTRIS_TAG_STR) {
                            yep_view pw = yep_dom_view(d, pkn->value);
                            if (pw.p != NULL && yep_re_cache_add(t, pw.p, pw.len) < 0) {
                                return -1;
                            }
                        }
                        pk = pv;
                    }
                }
            }
        }
        /* the rest of the schema rides the values (keys are scalars —
         * nothing under them; the walk reaches nodes the same way) */
        if (v != UINT32_MAX && collect_patterns(t, d, v, depth + 1) != 0) {
            return -1;
        }
        k = v != UINT32_MAX && yep_dom_node(d, v) ? yep_dom_node(d, v)->next_sibling : UINT32_MAX;
    }
    return 0;
}

/* Depth-bounded DFS gathering every pattern the walk can reach. 0 ok,
 * -1 OOM. Aliases are skipped: the walk sees an alias node as opaque
 * (never a mapping), so no reachable pattern hides behind one. */
static int collect_patterns(yep_re_cache* t, const yep_dom* d, uint32_t id, int depth) {
    if (depth > YEP_DOM_MAX_DEPTH) {
        return 0; /* the walk's own guard: deeper nodes are unreachable to it too */
    }
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == YEP_DOM_MAPPING) {
        return collect_map(t, d, id, depth);
    }
    if (n->kind == YEP_DOM_SEQUENCE) {
        uint32_t ch = n->first_child;
        while (ch != UINT32_MAX) {
            uint32_t next = yep_dom_node(d, ch) ? yep_dom_node(d, ch)->next_sibling : UINT32_MAX;
            if (collect_patterns(t, d, ch, depth + 1) != 0) {
                return -1;
            }
            ch = next;
        }
    }
    return 0;
}

/* ---- the seam with the explicit status (internal.h) --------------------- */

yeptris_schema* yep_schema_compile_st(const char* bytes, size_t n, YeptrisStatus* st) {
    *st = YEPTRIS_OK;
    if (bytes == NULL && n != 0) {
        *st = YEPTRIS_ERROR_ARG;
        return NULL;
    }
    const yep_allocator* sys = yep_system_allocator();

    /* the schema bytes are COPIED: the DOM's scalar views borrow its
     * input, and the handle outlives this call */
    char* copy = NULL;
    if (n > 0) {
        copy = yep_alloc(sys, n);
        if (copy == NULL) {
            *st = YEPTRIS_ERROR_MEMORY;
            return NULL;
        }
        memcpy(copy, bytes, n);
    }

    /* document 0 of the stream is the schema (the one-shot's exact rule) */
    YeptrisParseOptions opts;
    memset(&opts, 0, sizeof(opts)); /* core12: the YAML->JSON data model */
    YeptrisStatus ps = YEPTRIS_OK;
    yeptris_document* doc = (yeptris_document*)yeptris_parse_eager_ex(copy, n, &opts, &ps);
    if (doc == NULL) {
        yep_free(sys, copy);
        if (ps == YEPTRIS_OK) { /* empty bytes: the stream holds no document */
            yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                          "the schema bytes hold no document");
            *st = YEPTRIS_ERROR_ARG;
        } else {
            *st = ps; /* the schema did not parse — the engine's own status */
        }
        return NULL;
    }
    const yep_dom* dom = yep_doc_dom(doc);
    if (dom == NULL || dom->dcount == 0) {
        yeptris_document_free(doc);
        yep_free(sys, copy);
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, 0,
                      "the schema bytes hold no document");
        *st = YEPTRIS_ERROR_ARG;
        return NULL;
    }

    /* the pattern programs: compiled once, here (a failure to retain is
     * an allocation failure — loud, like every constructor's OOM) */
    yep_re_cache* cache = yep_re_cache_new();
    if (cache == NULL || collect_patterns(cache, dom, dom->docs[0], 0) != 0) {
        yep_re_cache_free(cache);
        yeptris_document_free(doc);
        yep_free(sys, copy);
        yep_error_set(yep_error_tls(), YEP_ERR_MEMORY, 0, 0, 0,
                      "the schema's pattern programs failed to compile");
        *st = YEPTRIS_ERROR_MEMORY;
        return NULL;
    }

    yeptris_schema* s = yep_alloc(sys, sizeof(*s));
    if (s == NULL) {
        yep_re_cache_free(cache);
        yeptris_document_free(doc);
        yep_free(sys, copy);
        *st = YEPTRIS_ERROR_MEMORY;
        return NULL;
    }
    s->bytes = copy;
    s->doc = doc;
    s->dom = dom;
    s->sroot = dom->docs[0];
    s->cache = cache;
    return s;
}

/* ---- the public face (validate.h) --------------------------------------- */

YEPTRIS_API yeptris_schema* yeptris_schema_compile(const char* bytes, size_t n) {
    YeptrisStatus st = YEPTRIS_OK;
    return yep_schema_compile_st(bytes, n, &st); /* the error channel carries the why */
}

YEPTRIS_API void yeptris_schema_free(yeptris_schema* s) {
    if (s == NULL) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    yep_re_cache_free(s->cache);
    yeptris_document_free(s->doc); /* the DOM and every pool under it */
    yep_free(sys, s->bytes);       /* the views' home: freed after its readers */
    yep_free(sys, s);
}

YEPTRIS_API YeptrisStatus yeptris_schema_validate(yeptris_schema* s, const char* buf, size_t len,
                                                  yeptris_validate_report** out) {
    if (out == NULL || s == NULL || (buf == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    *out = NULL;

    /* the instance stream (core12, the one-shot's exact parse) */
    YeptrisParseOptions opts;
    memset(&opts, 0, sizeof(opts));
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument idoc = yeptris_parse_eager_ex(buf, len, &opts, &st);
    if (idoc == NULL) {
        if (st == YEPTRIS_OK) {
            /* an empty instance stream: nothing to invalidate */
            yeptris_validate_report* r = yep_validate_report_new();
            if (r == NULL) {
                return YEPTRIS_ERROR_MEMORY;
            }
            *out = r;
            return YEPTRIS_OK;
        }
        return st; /* the parse's own error */
    }
    yep_dom* idom = yep_doc_dom((yeptris_document*)idoc);
    if (idom == NULL) {
        yeptris_document_free(idoc);
        return YEPTRIS_ERROR_MEMORY;
    }

    yeptris_validate_report* r = yep_validate_report_new();
    if (r == NULL) {
        yeptris_document_free(idoc);
        return YEPTRIS_ERROR_MEMORY;
    }
    /* the handle is a pure reader here (its cache is immutable): the
     * same walk, the same report shape, the one-shot's verdicts */
    int rc = yep_jsonschema_run(s->dom, s->sroot, idom, idom->docs, idom->dcount, r, s->cache);
    yeptris_document_free(idoc);
    if (rc != 0) {
        yeptris_validate_report_free(r);
        return YEPTRIS_ERROR_MEMORY;
    }
    *out = r;
    return r->count > 0 ? YEPTRIS_ERROR_SCHEMA : YEPTRIS_OK;
}
