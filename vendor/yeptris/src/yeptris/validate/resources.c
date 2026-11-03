/* resources.c — the schema reference machinery's compile-time half
 * (the 2020-12 sota sweep).
 *
 * The draft's reference model is a URI space over the schema's own
 * identifiers: "$id" establishes a base URI and names a RESOURCE (the
 * schema object carrying it is that resource's root), "$anchor" and
 * "$dynamicAnchor" attach plain-name fragments to the containing
 * resource, and "$ref"/"$dynamicRef" sites resolve against the nearest
 * enclosing base. All of that is STATIC — a property of the schema
 * text — so the whole map is built once, here, at compile time, by
 * one bounded DFS over the schema DOM ("the identifier crawl"). The
 * compiled economy keeps the schema DOM alive, so the map rides the
 * same handle and dies with it; validation is a pure reader.
 *
 * What the crawl records:
 *   - resources: canonical URI (fragment-free) -> the resource root
 *     node (the "$id" node; the document root registers the empty
 *     base "" when the schema has no "$id");
 *   - anchors: canonical URI + '#' + plain name -> the anchor node,
 *     flagged when created by "$dynamicAnchor" (the dynamic scope's
 *     extension points) vs "$anchor";
 *   - ref sites: every "$ref"/"$dynamicRef" object -> the canonical
 *     base URI and base resource root it resolves against;
 *   - rindex: node id -> owning resource index, for the runtime
 *     dynamic-scope stack ($dynamicRef, jsonschema.c).
 *
 * Bounded tables: an identifier past a cap is simply not registered —
 * the reference then fails to resolve at validation (DIAG_REF), the
 * same verdict as any other unresolvable target. Nothing guesses.
 *
 * URI handling is RFC 3986 section 5.2 to the level the conformance
 * suite exercises: schemes, authorities, path merge with dot-segment
 * removal, query carry, fragment split. The JSON Pointer percent
 * layer is decoded by the pointer walk (jsonschema.c). No network: a
 * target outside the schema's own identifier space is unresolvable,
 * never fetched.
 */

#include <string.h>

#include <yeptris/dom.h>

#include "../memory/allocator.h"
#include "internal.h"

/* ---- the tables ----------------------------------------------------------- */

/* 1 found (*idx_out), 0 absent. */
int js_res_find(const js_resources* r, const char* uri, uint32_t* idx_out) {
    for (uint32_t i = 0; i < r->res_n; i++) {
        if (strcmp(r->res[i].uri, uri) == 0) {
            *idx_out = i;
            return 1;
        }
    }
    return 0;
}

/* 1 found (node + dynamic flag), 0 absent. */
int js_anchor_find(const js_resources* r, const char* key, size_t key_len, uint32_t* node_out,
                   uint8_t* dyn_out) {
    for (uint32_t i = 0; i < r->anc_n; i++) {
        if (strlen(r->anc[i].key) == key_len && memcmp(r->anc[i].key, key, key_len) == 0) {
            *node_out = r->anc[i].node;
            *dyn_out = r->anc[i].dynamic;
            return 1;
        }
    }
    return 0;
}

/* The first DYNAMIC anchor with this plain name owned by resource
 * `res_idx` (the dynamic-scope search's per-resource probe). */
int js_anchor_dynamic_in_res(const js_resources* r, uint32_t res_idx, const char* name,
                             size_t name_len, uint32_t* node_out) {
    for (uint32_t i = 0; i < r->anc_n; i++) {
        if (r->anc[i].dynamic != 1 || r->anc[i].res != res_idx) {
            continue;
        }
        const char* hash = strchr(r->anc[i].key, '#');
        if (hash == NULL) {
            continue;
        }
        size_t nl = strlen(hash + 1);
        if (nl == name_len && (name_len == 0 || memcmp(hash + 1, name, name_len) == 0)) {
            *node_out = r->anc[i].node;
            return 1;
        }
    }
    return 0;
}

/* 1 found (base URI and that base's resource root), 0 absent (the
 * caller falls back to the document root). */
int js_refsite_find(const js_resources* r, uint32_t site_node, const char** base,
                    uint32_t* base_root) {
    for (uint32_t i = 0; i < r->site_n; i++) {
        if (r->site[i].site == site_node) {
            *base = r->site[i].base;
            *base_root = r->site[i].base_root;
            return 1;
        }
    }
    return 0;
}

void yep_js_resources_free(js_resources* r) {
    if (r == NULL) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    for (uint32_t i = 0; i < r->res_n; i++) {
        yep_free(sys, r->res[i].uri);
    }
    for (uint32_t i = 0; i < r->anc_n; i++) {
        yep_free(sys, r->anc[i].key);
    }
    for (uint32_t i = 0; i < r->site_n; i++) {
        yep_free(sys, r->site[i].base);
    }
    yep_free(sys, r->res);
    yep_free(sys, r->anc);
    yep_free(sys, r->site);
    yep_free(sys, r->rindex);
    yep_free(sys, r);
}

/* ---- URI machinery (RFC 3986 section 5.2, the exercised subset) ---------- */

typedef struct {
    size_t scheme; /* offset, URI_NOSCHEME when absent; length below */
    size_t scheme_len;
    size_t auth; /* offset of the authority (after "//"); length below */
    size_t auth_len;
    size_t path;
    size_t path_len;
    size_t query; /* offset (after '?'); length below; 0 when absent */
    size_t query_len;
} uri_parts;

#define URI_NOSCHEME ((size_t) - 1)

/* Splits s[0..n). Every part carries an offset and a length; absent
 * parts are 0. The fragment (after '#') is ignored — canonical forms
 * are fragment-free. */
static void uri_split(const char* s, size_t n, uri_parts* p) {
    memset(p, 0, sizeof(*p));
    p->scheme = URI_NOSCHEME;
    size_t i = 0;
    /* scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
    if (n > 0 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
        size_t j = 1;
        while (j < n &&
               (s[j] == '+' || s[j] == '-' || s[j] == '.' || (s[j] >= 'a' && s[j] <= 'z') ||
                (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9'))) {
            j++;
        }
        if (j < n && s[j] == ':') {
            p->scheme = 0;
            p->scheme_len = j;
            i = j + 1;
        }
    }
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
        i += 2;
        p->auth = i;
        while (i < n && s[i] != '/' && s[i] != '?' && s[i] != '#') {
            i++;
        }
        p->auth_len = i - p->auth;
    }
    p->path = i;
    while (i < n && s[i] != '?' && s[i] != '#') {
        i++;
    }
    p->path_len = i - p->path;
    if (i < n && s[i] == '?') {
        p->query = i + 1;
        size_t q = p->query;
        while (q < n && s[q] != '#') {
            q++;
        }
        p->query_len = q - p->query;
    }
}

/* Appends [s, s+n) to out (NUL kept terminated); -1 overlong. */
static int uri_put(char* out, size_t cap, size_t* n, const char* s, size_t len) {
    if (*n + len + 1 > cap) {
        return -1;
    }
    memcpy(out + *n, s, len);
    *n += len;
    out[*n] = '\0';
    return 0;
}

/* 1 when the path segment is "." or ".." (*dotdot set for the latter). */
static int uri_dot_seg(const char* p, size_t len, int* dotdot) {
    *dotdot = 0;
    if (len == 1 && p[0] == '.') {
        return 1;
    }
    if (len == 2 && p[0] == '.' && p[1] == '.') {
        *dotdot = 1;
        return 1;
    }
    return 0;
}

/* remove_dot_segments (RFC 3986 5.2.4): emits the path's segments
 * slash-prefixed, dropping "." and popping on ".."; a trailing slash
 * survives as an empty last segment. */
static int uri_normalize_path(const char* p, size_t n, char* out, size_t cap, size_t* on) {
    typedef struct {
        size_t off;
        size_t len;
    } seg;
    seg segs[64];
    int seg_n = 0;
    int trailing_slash = n > 0 && p[n - 1] == '/';
    size_t i = 0;
    while (i < n) {
        while (i < n && p[i] == '/') {
            i++;
        }
        size_t s = i;
        while (i < n && p[i] != '/') {
            i++;
        }
        if (i == s) {
            break;
        }
        int dotdot = 0;
        if (uri_dot_seg(p + s, i - s, &dotdot)) {
            if (dotdot && seg_n > 0) {
                seg_n--;
            }
            continue;
        }
        if (seg_n >= (int)(sizeof(segs) / sizeof(segs[0]))) {
            return -1;
        }
        segs[seg_n].off = s;
        segs[seg_n].len = i - s;
        seg_n++;
    }
    for (int k = 0; k < seg_n; k++) {
        if (uri_put(out, cap, on, "/", 1) != 0 ||
            uri_put(out, cap, on, p + segs[k].off, segs[k].len) != 0) {
            return -1;
        }
    }
    if (trailing_slash && uri_put(out, cap, on, "/", 1) != 0) {
        return -1;
    }
    return 0;
}

/* Resolves the URI-reference `ref` against the base URI `base`,
 * producing the canonical form WITHOUT its fragment (the caller owns
 * fragment handling — plain-name fragments never reach this). 0 ok,
 * -1 overlong (out holds "" on failure). */
int js_uri_resolve(const char* base, const char* ref, size_t ref_len, char* out, size_t cap) {
    out[0] = '\0';
    size_t bn = strlen(base);
    if (bn >= YEP_JS_URI_MAX) {
        return -1;
    }

    /* the reference minus its fragment */
    char r[YEP_JS_URI_MAX];
    size_t rn = 0;
    {
        const char* hash = memchr(ref, '#', ref_len);
        size_t rl = hash != NULL ? (size_t)(hash - ref) : ref_len;
        if (uri_put(r, sizeof(r), &rn, ref, rl) != 0) {
            return -1;
        }
    }
    uri_parts rp;
    uri_split(r, rn, &rp);

    /* an empty reference is the base itself */
    if (rn == 0) {
        size_t n = 0;
        return uri_put(out, cap, &n, base, bn);
    }
    if (rp.scheme != URI_NOSCHEME) { /* absolute (URN included): verbatim */
        size_t n = 0;
        return uri_put(out, cap, &n, r, rn);
    }

    /* every remaining form prefixes the base's scheme[:authority] */
    uri_parts bp;
    uri_split(base, bn, &bp);
    size_t on = 0;
    if (bp.scheme != URI_NOSCHEME && uri_put(out, cap, &on, base, bp.scheme_len + 1) != 0) {
        return -1;
    }
    if (rn > 1 && r[0] == '/' && r[1] == '/') { /* network-path: keep the ref's authority */
        return uri_put(out, cap, &on, r, rn) == 0 ? 0 : -1;
    }
    if (bp.auth_len > 0 && (uri_put(out, cap, &on, "//", 2) != 0 ||
                            uri_put(out, cap, &on, base + bp.auth, bp.auth_len) != 0)) {
        return -1;
    }
    if (rn > 0 && r[0] == '/') { /* path-absolute: replaces the base path */
        if (uri_normalize_path(r + rp.path, rp.path_len, out, cap, &on) != 0) {
            return -1;
        }
    } else { /* relative path: merge against the base path's directory */
        char merged[YEP_JS_URI_MAX];
        size_t mn = 0;
        size_t dir = bp.path_len;
        while (dir > 0 && base[bp.path + dir - 1] != '/') {
            dir--;
        }
        if (dir > 0 && uri_put(merged, sizeof(merged), &mn, base + bp.path, dir) != 0) {
            return -1;
        }
        if (uri_put(merged, sizeof(merged), &mn, r + rp.path, rp.path_len) != 0) {
            return -1;
        }
        if (uri_normalize_path(merged, mn, out, cap, &on) != 0) {
            return -1;
        }
    }
    /* the reference's query replaces the base's */
    if (rp.query_len > 0 && (uri_put(out, cap, &on, "?", 1) != 0 ||
                             uri_put(out, cap, &on, r + rp.query, rp.query_len) != 0)) {
        return -1;
    }
    return 0;
}

/* ---- the identifier crawl ------------------------------------------------- */

typedef struct {
    const yep_dom* d;
    js_resources* r;
} crawl;

/* Schema-side string keyword read (the walk's exact rules: a scalar
 * key by bytes, a string-scalar value). 1 + *val_out, 0 absent. */
static int crawl_kw_str(const crawl* c, uint32_t map_id, const char* name, const char** p,
                        size_t* len) {
    const yep_dnode* m = yep_dom_node(c->d, map_id);
    if (m == NULL || m->kind != YEP_DOM_MAPPING) {
        return 0;
    }
    size_t nl = strlen(name);
    uint32_t k = m->first_child;
    while (k != UINT32_MAX) {
        uint32_t v = yep_dom_node(c->d, k) ? yep_dom_node(c->d, k)->next_sibling : UINT32_MAX;
        const yep_dnode* kn = yep_dom_node(c->d, k);
        if (kn != NULL && kn->kind == YEP_DOM_SCALAR) {
            yep_view kv = yep_dom_view(c->d, kn->value);
            if (kv.len == nl && (nl == 0 || memcmp(kv.p, name, nl) == 0)) {
                const yep_dnode* vn = v != UINT32_MAX ? yep_dom_node(c->d, v) : NULL;
                if (vn != NULL && vn->kind == YEP_DOM_SCALAR &&
                    yep_validate_scalar_tag(vn) == YEPTRIS_TAG_STR) {
                    yep_view vv = yep_dom_view(c->d, vn->value);
                    if (vv.p != NULL) {
                        *p = vv.p;
                        *len = vv.len;
                        return 1;
                    }
                }
                return 0; /* the keyword is present but unreadable */
            }
        }
        k = v != UINT32_MAX && yep_dom_node(c->d, v) ? yep_dom_node(c->d, v)->next_sibling
                                                     : UINT32_MAX;
    }
    return 0;
}

/* Registers a resource (first registration wins; the same canonical
 * URI names exactly one resource). UINT32_MAX: over cap / OOM — the
 * resource stays unregistered and its references unresolvable. */
static uint32_t crawl_resource(crawl* c, const char* uri, uint32_t root) {
    uint32_t idx = 0;
    if (js_res_find(c->r, uri, &idx)) {
        return idx;
    }
    if (c->r->res_n >= YEP_JS_MAX_RESOURCES) {
        return UINT32_MAX;
    }
    const yep_allocator* sys = yep_system_allocator();
    size_t n = strlen(uri);
    char* copy = yep_alloc(sys, n + 1);
    js_resource* nr = yep_alloc(sys, (size_t)(c->r->res_n + 1) * sizeof(*nr));
    if (copy == NULL || nr == NULL) {
        yep_free(sys, copy);
        yep_free(sys, nr);
        return UINT32_MAX;
    }
    memcpy(copy, uri, n + 1);
    if (c->r->res_n > 0) {
        memcpy(nr, c->r->res, (size_t)c->r->res_n * sizeof(*nr));
    }
    yep_free(sys, c->r->res);
    c->r->res = nr;
    c->r->res[c->r->res_n].uri = copy;
    c->r->res[c->r->res_n].root = root;
    c->r->res_n++;
    return c->r->res_n - 1;
}

/* Registers an anchor: key = base + '#' + name. Same-key duplicates
 * keep the first (the draft leaves repeated fragments undefined). */
static void crawl_anchor(crawl* c, const char* base, uint32_t res_idx, const char* name,
                         size_t name_len, uint32_t node, uint8_t dynamic) {
    if (c->r->anc_n >= YEP_JS_MAX_ANCHORS) {
        return;
    }
    char key[YEP_JS_URI_MAX];
    size_t bn = strlen(base);
    if (bn + 1 + name_len + 1 > sizeof(key)) {
        return;
    }
    size_t kl = 0;
    if (uri_put(key, sizeof(key), &kl, base, bn) != 0 ||
        uri_put(key, sizeof(key), &kl, "#", 1) != 0 ||
        uri_put(key, sizeof(key), &kl, name, name_len) != 0) {
        return;
    }
    uint32_t dup_node = 0;
    uint8_t dup_dyn = 0;
    if (js_anchor_find(c->r, key, kl, &dup_node, &dup_dyn)) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    char* copy = yep_alloc(sys, kl + 1);
    js_anchor* na = yep_alloc(sys, (size_t)(c->r->anc_n + 1) * sizeof(*na));
    if (copy == NULL || na == NULL) {
        yep_free(sys, copy);
        yep_free(sys, na);
        return;
    }
    memcpy(copy, key, kl + 1);
    if (c->r->anc_n > 0) {
        memcpy(na, c->r->anc, (size_t)c->r->anc_n * sizeof(*na));
    }
    yep_free(sys, c->r->anc);
    c->r->anc = na;
    c->r->anc[c->r->anc_n].key = copy;
    c->r->anc[c->r->anc_n].node = node;
    c->r->anc[c->r->anc_n].res = res_idx;
    c->r->anc[c->r->anc_n].dynamic = dynamic;
    c->r->anc_n++;
}

/* Registers a ref site (the object's base and that base's resource
 * root — where its fragment-only references start walking). */
static void crawl_refsite(crawl* c, uint32_t site, const char* base, uint32_t base_root) {
    for (uint32_t i = 0; i < c->r->site_n; i++) {
        if (c->r->site[i].site == site) {
            return;
        }
    }
    if (c->r->site_n >= YEP_JS_MAX_REFSITES) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    size_t bn = strlen(base);
    char* copy = yep_alloc(sys, bn + 1);
    js_refsite* ns = yep_alloc(sys, (size_t)(c->r->site_n + 1) * sizeof(*ns));
    if (copy == NULL || ns == NULL) {
        yep_free(sys, copy);
        yep_free(sys, ns);
        return;
    }
    memcpy(copy, base, bn + 1);
    if (c->r->site_n > 0) {
        memcpy(ns, c->r->site, (size_t)c->r->site_n * sizeof(*ns));
    }
    yep_free(sys, c->r->site);
    c->r->site = ns;
    c->r->site[c->r->site_n].site = site;
    c->r->site[c->r->site_n].base = copy;
    c->r->site[c->r->site_n].base_root = base_root;
    c->r->site_n++;
}

static int crawl_node(crawl* c, uint32_t id, const char* base, uint32_t res_idx, int depth);

/* One mapping (a schema object): the identifier keywords first (the
 * draft's "$id must be evaluated before $ref" order), then the
 * values. 0 ok, -1 OOM. */
static int crawl_map(crawl* c, uint32_t map_id, const char* base, uint32_t res_idx, int depth) {
    const char* my_base = base;
    uint32_t my_res = res_idx;
    const char* idv = NULL;
    size_t idl = 0;
    char canon[YEP_JS_URI_MAX];
    if (crawl_kw_str(c, map_id, "$id", &idv, &idl) &&
        js_uri_resolve(base, idv, idl, canon, sizeof(canon)) == 0) {
        uint32_t ri = crawl_resource(c, canon, map_id);
        if (ri != UINT32_MAX) {
            my_base = c->r->res[ri].uri;
            my_res = ri;
        }
    }
    if (map_id < c->r->rindex_n) {
        c->r->rindex[map_id] = my_res;
    }
    const char* av = NULL;
    size_t al = 0;
    if (crawl_kw_str(c, map_id, "$anchor", &av, &al)) {
        crawl_anchor(c, my_base, my_res, av, al, map_id, 0);
    }
    if (crawl_kw_str(c, map_id, "$dynamicAnchor", &av, &al)) {
        crawl_anchor(c, my_base, my_res, av, al, map_id, 1);
    }
    if (crawl_kw_str(c, map_id, "$ref", &av, &al) ||
        crawl_kw_str(c, map_id, "$dynamicRef", &av, &al)) {
        crawl_refsite(c, map_id, my_base, c->r->res[my_res].root);
    }
    /* the values ride the subtree */
    const yep_dnode* m = yep_dom_node(c->d, map_id);
    uint32_t k = m->first_child;
    while (k != UINT32_MAX) {
        uint32_t v = yep_dom_node(c->d, k) ? yep_dom_node(c->d, k)->next_sibling : UINT32_MAX;
        if (v != UINT32_MAX && crawl_node(c, v, my_base, my_res, depth + 1) != 0) {
            return -1;
        }
        k = v != UINT32_MAX && yep_dom_node(c->d, v) ? yep_dom_node(c->d, v)->next_sibling
                                                     : UINT32_MAX;
    }
    return 0;
}

static int crawl_node(crawl* c, uint32_t id, const char* base, uint32_t res_idx, int depth) {
    if (depth > YEP_DOM_MAX_DEPTH) {
        return 0; /* the walk's own guard: deeper nodes are unreachable to it too */
    }
    const yep_dnode* n = yep_dom_node(c->d, id);
    if (n == NULL) {
        return 0;
    }
    if (id < c->r->rindex_n && c->r->rindex[id] == UINT32_MAX) {
        c->r->rindex[id] = res_idx;
    }
    if (n->kind == YEP_DOM_MAPPING) {
        return crawl_map(c, id, base, res_idx, depth);
    }
    if (n->kind == YEP_DOM_SEQUENCE) {
        uint32_t ch = n->first_child;
        while (ch != UINT32_MAX) {
            uint32_t next =
                yep_dom_node(c->d, ch) ? yep_dom_node(c->d, ch)->next_sibling : UINT32_MAX;
            if (crawl_node(c, ch, base, res_idx, depth + 1) != 0) {
                return -1;
            }
            ch = next;
        }
    }
    return 0;
}

/* The constructor: one bounded DFS over the schema DOM. NULL on OOM
 * (the caller reports YEPTRIS_ERROR_MEMORY). */
js_resources* yep_js_resources_crawl(const yep_dom* d, uint32_t root) {
    const yep_allocator* sys = yep_system_allocator();
    js_resources* r = yep_alloc(sys, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->rindex_n = d->ncount;
    r->rindex = yep_alloc(sys, (size_t)d->ncount * sizeof(uint32_t));
    if (r->rindex == NULL) {
        yep_free(sys, r);
        return NULL;
    }
    memset(r->rindex, 0xFF, (size_t)d->ncount * sizeof(uint32_t)); /* UINT32_MAX */

    crawl c = {d, r};
    /* resource 0: the document itself (the empty base, or the root's
     * own "$id") */
    const char* idv = NULL;
    size_t idl = 0;
    char canon[YEP_JS_URI_MAX];
    const char* root_base = "";
    if (crawl_kw_str(&c, root, "$id", &idv, &idl) &&
        js_uri_resolve("", idv, idl, canon, sizeof(canon)) == 0) {
        root_base = canon;
    }
    uint32_t ri = crawl_resource(&c, root_base, root);
    if (ri == UINT32_MAX) {
        yep_js_resources_free(r);
        return NULL;
    }
    if (crawl_node(&c, root, r->res[ri].uri, ri, 0) != 0) {
        yep_js_resources_free(r);
        return NULL;
    }
    return r;
}
