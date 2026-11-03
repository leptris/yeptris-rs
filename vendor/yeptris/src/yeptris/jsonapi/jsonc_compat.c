/* jsonc_compat.c — json-c symbols over the yeptris core (TODO.impl/21).
 *
 * Read path: parse via strict JSON mode, query via the DOM API, output
 * via the JSON writer. The root object owns the YeptrisDocument;
 * children borrow it.
 *
 * Building path (v2, over DOM mutation 11/3): new_* objects are
 * PENDING — no document exists until the object is attached or first
 * queried. Attaching materializes the node directly in the parent's
 * document (O(1), no copies); a standalone object materializes its own
 * document on first query. json-c ownership rules hold: add transfers
 * ownership, the caller's pointer stays valid until the root is put.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/yeptris/dom.h"
#include "../../include/yeptris/json.h"
#include "../../include/yeptris/jsonc_compat.h"

#include "../emit/float/api.h"
#include "../emit/writer.h"

struct json_object {
    YeptrisDocument doc; /* set when materialized; owned only when owns_doc */
    YeptrisNode node;    /* NULL for the root placeholder */
    int owns_doc;
    int pending;     /* constructed, no document yet */
    json_type jtype; /* pending payload kind */
    char* sval;      /* pending json_type_string (malloc'd) */
    size_t slen;
    int64_t ival;             /* pending json_type_int */
    double dval;              /* pending json_type_double */
    int bval;                 /* pending json_type_boolean */
    char* json_out;           /* cached to_json_string result */
    struct json_object* root; /* the owning root (self for roots) */
    /* the root registers every wrapper it hands out; wrappers live
     * until the root is put (json-c lifetime: borrowed refs die with
     * the root) */
    struct json_object** children;
    size_t nchild;
    size_t capchild;
};

/* --- root registry (wrappers die with their root) --- */

static json_object* reg_grow(json_object* root) {
    if (root->nchild == root->capchild) {
        size_t cap = root->capchild ? root->capchild * 2 : 8;
        struct json_object** grown = realloc(root->children, cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        root->children = grown;
        root->capchild = cap;
    }
    return root;
}

static void register_child(json_object* root, json_object* child) {
    if (reg_grow(root) == NULL) {
        return; /* registry full = the wrapper leaks at put only */
    }
    root->children[root->nchild++] = child;
}

static json_object* child_of(json_object* from, YeptrisNode node) {
    if (from == NULL || node == NULL) {
        return NULL;
    }
    json_object* root = from->root;
    if (reg_grow(root) == NULL) {
        return NULL;
    }
    json_object* child = calloc(1, sizeof(*child));
    if (child == NULL) {
        return NULL;
    }
    child->doc = root->doc;
    child->node = node;
    child->root = root;
    register_child(root, child);
    return child;
}

/* --- pending payload -> DOM node --- */

/* The scalar text a pending double materializes as: shortest
 * round-trip (finite) or the resolver's non-finite word. */
static int double_text(double d, char* buf) {
    if (isfinite(d)) {
        return yep_d2s_shortest(d, buf);
    }
    return yep_d2s_nonfinite(d, buf);
}

static YeptrisNode pending_node(YeptrisDocument doc, json_object* o) {
    char buf[32];
    switch (o->jtype) {
    case json_type_object:
        return yeptris_node_new_mapping(doc);
    case json_type_array:
        return yeptris_node_new_sequence(doc);
    case json_type_string:
        return yeptris_node_new_scalar(doc, o->sval ? o->sval : "", o->slen,
                                       YEPTRIS_STYLE_DOUBLE_QUOTED);
    case json_type_int:
        snprintf(buf, sizeof(buf), "%lld", (long long)o->ival);
        return yeptris_node_new_scalar(doc, buf, strlen(buf), YEPTRIS_STYLE_PLAIN);
    case json_type_double:
        return yeptris_node_new_scalar(doc, buf, (size_t)double_text(o->dval, buf),
                                       YEPTRIS_STYLE_PLAIN);
    case json_type_boolean:
        return yeptris_node_new_scalar(doc, o->bval ? "true" : "false", o->bval ? 4 : 5,
                                       YEPTRIS_STYLE_PLAIN);
    default:
        return yeptris_node_new_scalar(doc, "null", 4, YEPTRIS_STYLE_PLAIN);
    }
}

/* A pending object becomes its own document's root. */
static int materialize_root(json_object* o) {
    if (!o->pending) {
        return 0;
    }
    YeptrisDocument doc = yeptris_document_new();
    if (doc == NULL) {
        return -1;
    }
    YeptrisNode node = pending_node(doc, o);
    if (node == NULL || yeptris_document_set_root(doc, node) != YEPTRIS_OK) {
        yeptris_document_free(doc);
        return -1;
    }
    free(o->sval);
    o->sval = NULL;
    o->pending = 0;
    o->doc = doc;
    o->node = node;
    o->owns_doc = 1;
    return 0;
}

/* Every read on a standalone pending object materializes it first. */
static int ensure_materialized(json_object* o) {
    if (o == NULL) {
        return -1;
    }
    return o->pending ? materialize_root(o) : 0;
}

/* Duplicates an attached subtree into `doc` via the public builders —
 * the rare path where a standalone object was materialized by a query
 * before being added to a parent. Styles ride along, so a quoted "12"
 * stays a string and a bare 12 stays an int. */
static YeptrisNode dup_into(YeptrisDocument doc, YeptrisNode src) {
    switch (yeptris_node_kind(src)) {
    case YEPTRIS_NODE_SEQUENCE: {
        YeptrisNode s = yeptris_node_new_sequence(doc);
        if (s == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < yeptris_node_seq_count(src); i++) {
            YeptrisNode e = dup_into(doc, yeptris_node_seq_at(src, i));
            if (e == NULL || yeptris_node_seq_add(s, e) != YEPTRIS_OK) {
                return NULL;
            }
        }
        return s;
    }
    case YEPTRIS_NODE_MAPPING: {
        YeptrisNode m = yeptris_node_new_mapping(doc);
        if (m == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < yeptris_node_map_count(src); i++) {
            YeptrisNode k, v;
            size_t klen = 0;
            if (yeptris_node_map_at(src, i, &k, &v) != 0) {
                return NULL;
            }
            const char* kt = yeptris_node_value(k, &klen);
            YeptrisNode dv = dup_into(doc, v);
            if (dv == NULL || yeptris_node_map_set(m, kt ? kt : "", klen, dv) != YEPTRIS_OK) {
                return NULL;
            }
        }
        return m;
    }
    default: {
        size_t len = 0;
        const char* v = yeptris_node_value(src, &len);
        return yeptris_node_new_scalar(doc, v ? v : "", len, yeptris_node_style(src));
    }
    }
}

/* Attaches val under parent: materializes pending payloads in the
 * parent's document (O(1)); standalone materialized vals are
 * duplicated in and their private document released. Retargets the
 * wrapper so the caller's pointer stays valid (json-c contract). */
static int attach(json_object* parent, json_object* val, const char* key) {
    if (ensure_materialized(parent) != 0) {
        return -1;
    }
    json_object* root = parent->root;
    YeptrisNode vnode;
    if (val->pending) {
        vnode = pending_node(root->doc, val);
        if (vnode == NULL) {
            return -1;
        }
        free(val->sval);
        val->sval = NULL;
        val->pending = 0;
    } else if (val->owns_doc) {
        vnode = dup_into(root->doc, val->node);
        if (vnode == NULL) {
            return -1;
        }
        yeptris_document_free(val->doc);
        /* the standalone root died with its document: its old registry
         * and cached output die too (borrowed refs die with the root —
         * the caller's val pointer is the one that stays valid) */
        for (size_t i = 0; i < val->nchild; i++) {
            free(val->children[i]->json_out);
            free(val->children[i]);
        }
        free(val->children);
        val->children = NULL;
        val->nchild = 0;
        val->capchild = 0;
        free(val->json_out);
        val->json_out = NULL;
    } else {
        return -1; /* already attached to a parent: two parents is a bug */
    }
    int rc;
    if (key != NULL) {
        /* added vs replaced: json-c's return contract */
        rc = yeptris_node_map_get(parent->node, key, strlen(key)) != NULL;
        if (yeptris_node_map_set(parent->node, key, strlen(key), vnode) != YEPTRIS_OK) {
            return -1;
        }
    } else {
        rc = yeptris_node_seq_add(parent->node, vnode) == YEPTRIS_OK ? 0 : -1;
        if (rc != 0) {
            return -1;
        }
    }
    val->doc = root->doc;
    val->node = vnode;
    val->root = root;
    val->owns_doc = 0;
    register_child(root, val);
    return key != NULL ? rc : 0;
}

/* --- parse / lifetime --- */

json_object* json_tokener_parse(const char* str) {
    if (str == NULL) {
        return NULL;
    }
    return json_tokener_parse_ex(str, (int)strlen(str));
}

json_object* json_tokener_parse_ex(const char* buf, int len) {
    if (buf == NULL || len < 0) {
        return NULL;
    }
    /* json-c tolerates trailing NUL: honor the explicit length only */
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse_json(buf, (size_t)len, &st);
    if (doc == NULL) {
        return NULL;
    }
    if (yeptris_document_count(doc) == 0) {
        yeptris_document_free(doc);
        return NULL;
    }
    json_object* obj = calloc(1, sizeof(*obj));
    if (obj == NULL) {
        yeptris_document_free(doc);
        return NULL;
    }
    obj->doc = doc;
    obj->node = yeptris_document_root(doc, 0);
    obj->owns_doc = 1;
    obj->root = obj;
    return obj;
}

void json_tokener_free(json_object* obj) {
    json_object_put(obj);
}

int json_object_put(json_object* obj) {
    if (obj == NULL) {
        return 0;
    }
    if (obj->pending) {
        free(obj->sval);
        free(obj->json_out);
        free(obj);
        return 1;
    }
    if (obj->owns_doc) {
        free(obj->json_out);
        for (size_t i = 0; i < obj->nchild; i++) {
            free(obj->children[i]->json_out);
            free(obj->children[i]);
        }
        free(obj->children);
        yeptris_document_free(obj->doc);
        free(obj);
    }
    /* children are owned by their root */
    return 1;
}

json_object* json_object_get(json_object* obj) {
    return obj;
}

/* --- building (json_object_new_*) --- */

static json_object* new_pending(json_type t) {
    json_object* o = calloc(1, sizeof(*o));
    if (o != NULL) {
        o->pending = 1;
        o->jtype = t;
        o->root = o;
    }
    return o;
}

json_object* json_object_new_object(void) {
    return new_pending(json_type_object);
}

json_object* json_object_new_array(void) {
    return new_pending(json_type_array);
}

json_object* json_object_new_string(const char* s) {
    return json_object_new_string_len(s, s ? (int)strlen(s) : 0);
}

json_object* json_object_new_string_len(const char* s, int len) {
    if (s == NULL || len < 0) {
        return NULL;
    }
    json_object* o = new_pending(json_type_string);
    if (o == NULL) {
        return NULL;
    }
    o->sval = malloc((size_t)len + 1);
    if (o->sval == NULL) {
        free(o);
        return NULL;
    }
    memcpy(o->sval, s, (size_t)len);
    o->sval[len] = '\0';
    o->slen = (size_t)len;
    return o;
}

json_object* json_object_new_int(int32_t i) {
    return json_object_new_int64(i);
}

json_object* json_object_new_int64(int64_t i) {
    json_object* o = new_pending(json_type_int);
    if (o != NULL) {
        o->ival = i;
    }
    return o;
}

json_object* json_object_new_double(double d) {
    json_object* o = new_pending(json_type_double);
    if (o != NULL) {
        o->dval = d;
    }
    return o;
}

json_object* json_object_new_boolean(int b) {
    json_object* o = new_pending(json_type_boolean);
    if (o != NULL) {
        o->bval = b != 0;
    }
    return o;
}

int json_object_object_add(json_object* obj, const char* key, json_object* val) {
    if (obj == NULL || key == NULL) {
        return -1;
    }
    if (val == NULL) {
        /* legacy json-c: NULL val deletes the key */
        return json_object_object_del(obj, key) == 0 ? 0 : -1;
    }
    if (ensure_materialized(obj) != 0 || yeptris_node_kind(obj->node) != YEPTRIS_NODE_MAPPING) {
        return -1;
    }
    return attach(obj, val, key);
}

int json_object_object_del(json_object* obj, const char* key) {
    if (obj == NULL || key == NULL || ensure_materialized(obj) != 0 ||
        yeptris_node_kind(obj->node) != YEPTRIS_NODE_MAPPING) {
        return -1;
    }
    return yeptris_node_map_del(obj->node, key, strlen(key)) == YEPTRIS_OK ? 0 : -1;
}

int json_object_array_add(json_object* obj, json_object* val) {
    if (obj == NULL || val == NULL || ensure_materialized(obj) != 0 ||
        yeptris_node_kind(obj->node) != YEPTRIS_NODE_SEQUENCE) {
        return -1;
    }
    return attach(obj, val, NULL);
}

/* json-c: index < len replaces in place; index == len appends;
 * beyond that errors. */
int json_object_array_put_idx(json_object* obj, size_t idx, json_object* val) {
    if (obj == NULL || val == NULL || ensure_materialized(obj) != 0 ||
        yeptris_node_kind(obj->node) != YEPTRIS_NODE_SEQUENCE) {
        return -1;
    }
    size_t len = yeptris_node_seq_count(obj->node);
    if (idx > len) {
        return -1;
    }
    if (idx == len) {
        return attach(obj, val, NULL);
    }
    /* replace in place: duplicate-then-splice for pending/standalone
     * values, mirroring attach's retarget contract */
    if (ensure_materialized(val) != 0) {
        return -1;
    }
    json_object* root = obj->root;
    YeptrisNode vnode;
    if (val->pending) {
        vnode = pending_node(root->doc, val);
        if (vnode == NULL) {
            return -1;
        }
        free(val->sval);
        val->sval = NULL;
        val->pending = 0;
    } else if (val->owns_doc) {
        vnode = dup_into(root->doc, val->node);
        if (vnode == NULL) {
            return -1;
        }
        yeptris_document_free(val->doc);
        for (size_t i = 0; i < val->nchild; i++) {
            free(val->children[i]->json_out);
            free(val->children[i]);
        }
        free(val->children);
        val->children = NULL;
        val->nchild = 0;
        val->capchild = 0;
        free(val->json_out);
        val->json_out = NULL;
    } else {
        return -1;
    }
    if (yeptris_node_seq_set(obj->node, idx, vnode) != YEPTRIS_OK) {
        return -1;
    }
    val->doc = root->doc;
    val->node = vnode;
    val->root = root;
    val->owns_doc = 0;
    register_child(root, val);
    return 0;
}

int json_object_array_del_idx(json_object* obj, size_t idx, size_t count) {
    if (obj == NULL || ensure_materialized(obj) != 0 ||
        yeptris_node_kind(obj->node) != YEPTRIS_NODE_SEQUENCE) {
        return -1;
    }
    if (idx + count > yeptris_node_seq_count(obj->node)) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (yeptris_node_seq_del(obj->node, idx) != YEPTRIS_OK) {
            return -1;
        }
    }
    return 0;
}

/* --- queries --- */

static int kind_of(const json_object* obj) {
    if (obj == NULL || obj->node == NULL) {
        return -1;
    }
    return (int)yeptris_node_kind(obj->node);
}

json_type json_object_get_type(const json_object* obj) {
    if (obj == NULL || ensure_materialized((json_object*)obj) != 0) {
        return json_type_null;
    }
    if (obj->node == NULL) {
        return json_type_null;
    }
    switch (kind_of(obj)) {
    case YEPTRIS_NODE_MAPPING:
        return json_type_object;
    case YEPTRIS_NODE_SEQUENCE:
        return json_type_array;
    case YEPTRIS_NODE_ALIAS:
        return json_type_null;
    default:
        break;
    }
    /* Typed accessors respect implicitness: a QUOTED "12" in strict
     * JSON is a string (the accessors reject it), a bare 12 is int —
     * the discrimination comes free from the resolver */
    int64_t i;
    double d;
    int b;
    if (yeptris_node_int(obj->node, &i) == YEPTRIS_OK) {
        return json_type_int;
    }
    if (yeptris_node_float(obj->node, &d) == YEPTRIS_OK) {
        return json_type_double;
    }
    if (yeptris_node_bool(obj->node, &b) == YEPTRIS_OK) {
        return json_type_boolean;
    }
    size_t len = 0;
    const char* v = yeptris_node_value(obj->node, &len);
    if (len == 4 && v != NULL && memcmp(v, "null", 4) == 0) {
        return json_type_null;
    }
    return json_type_string;
}

int json_object_is_type(const json_object* obj, json_type type) {
    return json_object_get_type(obj) == type;
}

const char* json_object_get_string(json_object* obj) {
    if (obj == NULL || ensure_materialized(obj) != 0) {
        return NULL;
    }
    size_t len = 0;
    const char* v = yeptris_node_value(obj->node, &len);
    if (v == NULL) {
        return NULL;
    }
    char* out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, v, len);
    out[len] = '\0';
    /* attach to the object's slot: freed on the NEXT call or at put —
     * json-c returns owned storage; we approximate with a per-object
     * cache (the last getString result) */
    free(obj->json_out);
    obj->json_out = out;
    return out;
}

int32_t json_object_get_int(const json_object* obj) {
    return (int32_t)json_object_get_int64(obj);
}

int64_t json_object_get_int64(const json_object* obj) {
    int64_t v = 0;
    if (obj != NULL && ensure_materialized((json_object*)obj) == 0 && obj->node != NULL) {
        if (yeptris_node_int(obj->node, &v) != YEPTRIS_OK) {
            double d = 0; /* json-c truncates doubles in getInt */
            if (yeptris_node_float(obj->node, &d) == YEPTRIS_OK) {
                v = (int64_t)d;
            }
        }
    }
    return v;
}

double json_object_get_double(const json_object* obj) {
    double v = 0.0;
    if (obj != NULL && ensure_materialized((json_object*)obj) == 0 && obj->node != NULL) {
        yeptris_node_float(obj->node, &v);
    }
    return v;
}

int json_object_get_boolean(const json_object* obj) {
    int v = 0;
    if (obj != NULL && ensure_materialized((json_object*)obj) == 0 && obj->node != NULL) {
        yeptris_node_bool(obj->node, &v);
    }
    return v;
}

int json_object_object_get_ex(const json_object* obj, const char* key, json_object** value) {
    if (obj == NULL || key == NULL || kind_of(obj) != YEPTRIS_NODE_MAPPING) {
        if (value != NULL) {
            *value = NULL;
        }
        return 0;
    }
    YeptrisNode n = yeptris_node_map_get(obj->node, key, strlen(key));
    if (n == NULL) {
        if (value != NULL) {
            *value = NULL;
        }
        return 0;
    }
    if (value != NULL) {
        json_object* child = child_of((json_object*)obj, n);
        *value = child;
        return child != NULL;
    }
    return 1;
}

size_t json_object_object_length(const json_object* obj) {
    if (obj == NULL || kind_of(obj) != YEPTRIS_NODE_MAPPING) {
        return 0;
    }
    return yeptris_node_map_count(obj->node);
}

size_t json_object_array_length(const json_object* obj) {
    if (obj == NULL || kind_of(obj) != YEPTRIS_NODE_SEQUENCE) {
        return 0;
    }
    return yeptris_node_seq_count(obj->node);
}

json_object* json_object_array_get_idx(const json_object* obj, size_t idx) {
    if (obj == NULL || kind_of(obj) != YEPTRIS_NODE_SEQUENCE) {
        return NULL;
    }
    if (idx >= yeptris_node_seq_count(obj->node)) {
        return NULL;
    }
    return child_of((json_object*)obj, yeptris_node_seq_at(obj->node, idx));
}

const char* json_object_to_json_string_ext(json_object* obj, int flags) {
    if (obj == NULL) {
        return NULL;
    }
    if (obj->pending && materialize_root(obj) != 0) {
        return NULL;
    }
    if (obj->json_out != NULL && obj->owns_doc) {
        free(obj->json_out);
    }
    /* children serialize their SUBTREE: emit from the node's document
     * requires a whole-document writer; v1 supports roots only */
    if (obj->node == NULL || !obj->owns_doc) {
        return NULL;
    }
    size_t len = 0;
    int pretty = (flags & JSON_C_TO_STRING_PRETTY) != 0 && !(flags & JSON_C_TO_STRING_PLAIN);
    obj->json_out = pretty ? yep_serialize_json_pretty((const yeptris_document*)obj->doc, &len)
                           : yep_serialize_json_compact((const yeptris_document*)obj->doc, &len);
    if (obj->json_out != NULL && len > 0 && obj->json_out[len - 1] == '\n') {
        obj->json_out[len - 1] = '\0'; /* json-c output carries no newline */
    }
    return obj->json_out;
}

const char* json_object_to_json_string(json_object* obj) {
    return json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
}
