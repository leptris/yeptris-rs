/* build.c — the public from-scratch construction API (TODO.impl/11
 * phase 3). Thin: argument checking + handle wrapping over the dom
 * mutation primitives. The emitter and every query API work on
 * synthesized documents unchanged — same nodes, same invariants. */

#include <stdio.h>
#include <string.h>

#include <yeptris.h>

#include "doc.h"
#include "dom/dom.h"
#include "memory/allocator.h"
#include "resolve/resolver.h"

static yeptris_document* doc_of(YeptrisDocument handle) {
    return (yeptris_document*)handle;
}

static yeptris_node* node_of(YeptrisNode handle) {
    return (yeptris_node*)handle;
}

YEPTRIS_API YeptrisDocument yeptris_document_new(void) {
    const yep_allocator* sys = yep_system_allocator();
    yep_dom* dom = yep_dom_create(sys);
    if (dom == NULL) {
        return NULL;
    }
    yeptris_document* doc = yep_alloc(sys, sizeof(yeptris_document));
    if (doc == NULL) {
        yep_dom_destroy(dom);
        return NULL;
    }
    yep_mutex_init(&doc->lazy_mu);
    memset(doc, 0, sizeof(*doc));
    doc->dom = dom;
    doc->sys = sys;
    return (YeptrisDocument)doc;
}

/* An empty document whose plain scalars type through the given
 * schema's resolver (yeptris_document_new rides the core-12 default).
 * The CBOR/dump faces create compat-11 documents so a host String the
 * tree builder kept plain ("1e0" — Psych's dot-required float law)
 * stays a text string instead of resolving as a float (#293's shape). */
YEPTRIS_API YeptrisDocument yeptris_document_new_schema(YeptrisSchema schema) {
    YeptrisDocument doc = yeptris_document_new();
    if (doc != NULL) {
        yeptris_document* d = (yeptris_document*)doc;
        d->dom->resolver =
            schema == YEPTRIS_SCHEMA_11_COMPAT ? yep_resolver_compat11() : yep_resolver_core12();
    }
    return doc;
}

YEPTRIS_API int yeptris_document_set_root(YeptrisDocument handle, YeptrisNode root) {
    yeptris_document* doc = doc_of(handle);
    yeptris_node* n = node_of(root);
    if (doc == NULL || n == NULL || n->doc != doc) {
        return YEPTRIS_ERROR_PARSE;
    }
    int rc = yep_mut_add_root(doc->dom, n->id);
    return rc == 0 ? YEPTRIS_OK
                   : (rc == -1 ? YEPTRIS_ERROR_MEMORY
                               : (rc == -3 ? YEPTRIS_ERROR_DEPTH : YEPTRIS_ERROR_ARG));
}

YEPTRIS_API YeptrisNode yeptris_node_new_mapping(YeptrisDocument handle) {
    yeptris_document* doc = doc_of(handle);
    if (doc == NULL) {
        return NULL;
    }
    uint32_t id = yep_mut_map(doc->dom, 0);
    return id == UINT32_MAX ? NULL : (YeptrisNode)yep_handle_new(doc, id);
}

YEPTRIS_API YeptrisNode yeptris_node_new_sequence(YeptrisDocument handle) {
    yeptris_document* doc = doc_of(handle);
    if (doc == NULL) {
        return NULL;
    }
    uint32_t id = yep_mut_seq(doc->dom, 0);
    return id == UINT32_MAX ? NULL : (YeptrisNode)yep_handle_new(doc, id);
}

/* Memory: the value is copied into the document (nothing is borrowed
 * from the caller). Plain style types the copy through the resolver,
 * quoted styles force string — identical to parse-time typing. */
YEPTRIS_API YeptrisNode yeptris_node_new_scalar(YeptrisDocument handle, const char* value,
                                                size_t len, YeptrisScalarStyle style) {
    yeptris_document* doc = doc_of(handle);
    if (doc == NULL || (value == NULL && len > 0) || style < YEPTRIS_STYLE_PLAIN ||
        style > YEPTRIS_STYLE_FOLDED) {
        return NULL;
    }
    uint32_t id = yep_mut_scalar(doc->dom, value, len, (uint8_t)style);
    return id == UINT32_MAX ? NULL : (YeptrisNode)yep_handle_new(doc, id);
}

static int mut_status(int rc) {
    switch (rc) {
    case 0:
    case 1: /* map_set replaced — success for the caller */
        return YEPTRIS_OK;
    case -1:
        return YEPTRIS_ERROR_MEMORY;
    case -3:
        return YEPTRIS_ERROR_DEPTH;
    default: /* -2 duplicate key, -4 double-attach */
        return YEPTRIS_ERROR_PARSE;
    }
}

static int same_doc(YeptrisNode parent, YeptrisNode child) {
    yeptris_node* p = node_of(parent);
    yeptris_node* c = node_of(child);
    return p != NULL && c != NULL && p->doc == c->doc;
}

YEPTRIS_API int yeptris_node_map_add(YeptrisNode handle, const char* key, size_t key_len,
                                     YeptrisNode value) {
    yeptris_node* m = node_of(handle);
    if (m == NULL || key == NULL || !same_doc(handle, value)) {
        return YEPTRIS_ERROR_ARG;
    }
    return mut_status(yep_mut_map_add(m->doc->dom, m->id, key, key_len, node_of(value)->id));
}

/* Replace-or-add (json-c semantics): an existing key keeps its
 * position and swaps the value; a new key appends. */
YEPTRIS_API int yeptris_node_map_set(YeptrisNode handle, const char* key, size_t key_len,
                                     YeptrisNode value) {
    yeptris_node* m = node_of(handle);
    if (m == NULL || key == NULL || !same_doc(handle, value)) {
        return YEPTRIS_ERROR_ARG;
    }
    return mut_status(yep_mut_map_set(m->doc->dom, m->id, key, key_len, node_of(value)->id));
}

YEPTRIS_API int yeptris_node_seq_add(YeptrisNode handle, YeptrisNode value) {
    yeptris_node* s = node_of(handle);
    if (s == NULL || !same_doc(handle, value)) {
        return YEPTRIS_ERROR_ARG;
    }
    return mut_status(yep_mut_seq_add(s->doc->dom, s->id, node_of(value)->id));
}

YEPTRIS_API int yeptris_node_seq_set(YeptrisNode handle, size_t index, YeptrisNode value) {
    yeptris_node* s = node_of(handle);
    if (s == NULL || !same_doc(handle, value)) {
        return YEPTRIS_ERROR_ARG;
    }
    return mut_status(yep_mut_seq_set(s->doc->dom, s->id, (uint32_t)index, node_of(value)->id));
}

YEPTRIS_API int yeptris_node_seq_del(YeptrisNode handle, size_t index) {
    yeptris_node* s = node_of(handle);
    if (s == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    return yep_mut_seq_del(s->doc->dom, s->id, (uint32_t)index) == 0 ? YEPTRIS_OK
                                                                     : YEPTRIS_ERROR_PARSE;
}

/* The removed subtree stays document-owned (arena lifetime); its
 * handle remains valid but unreachable from the root. */
YEPTRIS_API int yeptris_node_set_anchor(YeptrisNode handle, const char* name, size_t len) {
    yeptris_node* n = node_of(handle);
    if (n == NULL || name == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    return yep_mut_set_anchor(n->doc->dom, n->id, name, len) == 0 ? YEPTRIS_OK : YEPTRIS_ERROR_ARG;
}

YEPTRIS_API int yeptris_node_set_tag(YeptrisNode handle, const char* tag, size_t len) {
    yeptris_node* n = node_of(handle);
    if (n == NULL || tag == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    return yep_mut_set_tag(n->doc->dom, n->id, tag, len) == 0 ? YEPTRIS_OK : YEPTRIS_ERROR_ARG;
}

YEPTRIS_API YeptrisNode yeptris_node_new_alias(YeptrisDocument handle, YeptrisNode target,
                                               const char* name, size_t len) {
    yeptris_document* doc = doc_of(handle);
    yeptris_node* t = node_of(target);
    if (doc == NULL || t == NULL || t->doc != doc || name == NULL) {
        return NULL;
    }
    uint32_t id = yep_mut_new_alias(doc->dom, t->id, name, len);
    return id == UINT32_MAX ? NULL : (YeptrisNode)yep_handle_new(doc, id);
}

YEPTRIS_API int yeptris_node_map_add_node(YeptrisNode handle, YeptrisNode key, YeptrisNode value) {
    yeptris_node* m = node_of(handle);
    if (m == NULL || !same_doc(handle, key) || !same_doc(handle, value)) {
        return YEPTRIS_ERROR_ARG;
    }
    return mut_status(
        yep_mut_map_add_node(m->doc->dom, m->id, node_of(key)->id, node_of(value)->id));
}

YEPTRIS_API int yeptris_node_map_del(YeptrisNode handle, const char* key, size_t key_len) {
    yeptris_node* m = node_of(handle);
    if (m == NULL || key == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    return yep_mut_map_del(m->doc->dom, m->id, key, key_len) == 0 ? YEPTRIS_OK
                                                                  : YEPTRIS_ERROR_PARSE;
}

/* ---- bulk build --------------------------------------------------------
 *
 * One call, one tree: entries walk in document order (the same shape
 * as the event stream), pairing and linking ride the mutation
 * primitives unchanged — dup checks, depth caps, and per-link depth
 * fixups all come along for free (DRY with the public builder). */
YEPTRIS_API YeptrisStatus yeptris_document_build(YeptrisDocument handle,
                                                 const YeptrisBuildEntry* entries, size_t count,
                                                 const char* blob, size_t blob_len) {
    yeptris_document* doc = doc_of(handle);
    if (doc == NULL || (entries == NULL && count != 0) || (blob == NULL && blob_len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    struct frame {
        uint32_t id;
        uint8_t is_map;
        uint8_t key_pending;
    };
    if (count > (SIZE_MAX / 2) / sizeof(struct frame)) {
        return YEPTRIS_ERROR_ARG;
    }
    struct frame* st = yep_alloc(doc->dom->sys, (count + 1) * sizeof(*st));
    if (st == NULL) {
        return YEPTRIS_ERROR_MEMORY;
    }
    int depth = 0;
    uint32_t root = UINT32_MAX;
    uint32_t root_closed = 0;
    uint32_t root_tagged = 0;          /* one trailing TAG on the closed root (#168) */
    uint32_t pending_key = UINT32_MAX; /* map frame's buffered key */
    uint32_t last_id = UINT32_MAX;     /* TAG applies here (#300) */
    YeptrisStatus rc = YEPTRIS_OK;
    for (size_t i = 0; i < count && rc == YEPTRIS_OK; i++) {
        const YeptrisBuildEntry* e = &entries[i];
        /* A closed SCALAR root admits exactly one trailing TAG —
         * psych's binary root is `--- !binary |-` (#168); everything
         * after (more ops, a second tag) still rejects */
        if (root_closed && !(e->op == YEPTRIS_BUILD_TAG && !root_tagged && last_id == root)) {
            rc = YEPTRIS_ERROR_PARSE; /* the document ended; more entries */
            break;
        }
        if (e->off > blob_len || e->len > blob_len - e->off) {
            rc = YEPTRIS_ERROR_PARSE; /* slice out of range */
            break;
        }
        uint32_t id = UINT32_MAX; /* set by every arm that reaches the place; MSVC wants the init */
        switch (e->op) {
        case YEPTRIS_BUILD_SCALAR: {
            id = yep_mut_scalar(doc->dom, blob + e->off, e->len, e->style);
            if (id == UINT32_MAX) {
                rc = YEPTRIS_ERROR_MEMORY;
                break;
            }
            break;
        }
        case YEPTRIS_BUILD_SEQ:
            id = yep_mut_seq(doc->dom, 0);
            if (id == UINT32_MAX) {
                rc = YEPTRIS_ERROR_MEMORY;
                break;
            }
            break;
        case YEPTRIS_BUILD_MAP:
            id = yep_mut_map(doc->dom, 0);
            if (id == UINT32_MAX) {
                rc = YEPTRIS_ERROR_MEMORY;
                break;
            }
            break;
        case YEPTRIS_BUILD_END:
            if (depth == 0) {
                rc = YEPTRIS_ERROR_PARSE; /* END with nothing open */
                break;
            }
            depth--;
            if (depth == 0) {
                root_closed = 1; /* the root just completed */
            }
            if (st[depth].is_map && st[depth].key_pending) {
                st[depth].key_pending = 0; /* a closed container was the value */
            }
            continue;
        case YEPTRIS_BUILD_TAG:
            /* #300: apply an explicit tag to the last-placed node —
             * the bulk ABI otherwise cannot express tags. Placed
             * immediately after the entry it tags. */
            if (last_id == UINT32_MAX) {
                rc = YEPTRIS_ERROR_PARSE; /* TAG with nothing placed */
                break;
            }
            if (yep_mut_set_tag(doc->dom, last_id, blob + e->off, e->len) != 0) {
                rc = YEPTRIS_ERROR_MEMORY;
                break;
            }
            root_tagged = root_closed;
            continue;
        default:
            rc = YEPTRIS_ERROR_PARSE; /* unknown op */
            break;
        }
        if (rc != YEPTRIS_OK) {
            break;
        }
        last_id = id; /* TAG entries target this (#300) */
        /* place the fresh node */
        if (depth == 0) {
            if (root != UINT32_MAX) {
                rc = YEPTRIS_ERROR_PARSE; /* two roots */
                break;
            }
            root = id; /* a single scalar root needs no END */
            root_closed = (e->op != YEPTRIS_BUILD_SEQ && e->op != YEPTRIS_BUILD_MAP) ? 1 : 0;
            if (!root_closed) {
                st[depth].id = id;
                st[depth].is_map = (e->op == YEPTRIS_BUILD_MAP);
                st[depth].key_pending = 0;
                depth = 1;
            }
            continue;
        }
        struct frame* top = &st[depth - 1];
        if (top->is_map) {
            if (!top->key_pending) {
                top->key_pending = 1;
                pending_key = id;
                continue;
            }
            /* the check-free pairing twin: document-order entries
             * from a host mapping cannot duplicate, and map_add's
             * linear dup scan is O(n^2) on wide maps */
            int mrc = yep_mut_map_append(doc->dom, top->id, pending_key, id);
            if (mrc == 0) {
                top->key_pending = 0;
                pending_key = UINT32_MAX;
            } else {
                rc = (mrc == -3) ? YEPTRIS_ERROR_DEPTH
                                 : (mrc == -1 ? YEPTRIS_ERROR_ARG : YEPTRIS_ERROR_MEMORY);
            }
        } else {
            int src_ = yep_mut_seq_add(doc->dom, top->id, id);
            if (src_ != 0) {
                rc = (src_ == -3) ? YEPTRIS_ERROR_DEPTH
                                  : (src_ == -1 ? YEPTRIS_ERROR_ARG : YEPTRIS_ERROR_MEMORY);
            }
        }
        if (rc == YEPTRIS_OK && (e->op == YEPTRIS_BUILD_SEQ || e->op == YEPTRIS_BUILD_MAP)) {
            if ((size_t)depth >= (size_t)YEP_DOM_MAX_DEPTH) {
                rc = YEPTRIS_ERROR_DEPTH;
                break;
            }
            st[depth].id = id;
            st[depth].is_map = (e->op == YEPTRIS_BUILD_MAP);
            st[depth].key_pending = 0;
            depth++;
        }
    }
    if (rc == YEPTRIS_OK) {
        if (depth > 1 || (depth == 1)) {
            rc = YEPTRIS_ERROR_PARSE; /* an unclosed container */
        } else if (root == UINT32_MAX) {
            rc = YEPTRIS_ERROR_PARSE; /* no root */
        } else if (pending_key != UINT32_MAX) {
            rc = YEPTRIS_ERROR_PARSE; /* a key without its value */
        } else {
            int arc = yep_mut_add_root(doc->dom, root);
            if (arc == 0) {
                rc = YEPTRIS_OK;
            } else if (arc == -4) {
                rc = YEPTRIS_ERROR_PARSE;
            } else if (arc == -3) {
                rc = YEPTRIS_ERROR_DEPTH;
            } else if (arc == -1) {
                rc = YEPTRIS_ERROR_MEMORY;
            } else {
                rc = YEPTRIS_ERROR_ARG;
            }
        }
    }
    yep_free(doc->dom->sys, st);
    return rc;
}
