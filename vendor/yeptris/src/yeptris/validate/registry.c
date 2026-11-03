/* registry.c — the tag-driven schema registry (#906 slice 4).
 *
 * The dialect table's reserved slot: user tag -> compiled schema.
 * A small borrowed-pointer table (the caller's compiled schemas are
 * REFERENCED, never copied — the schema bytes/DOM/programs stay the
 * handle's single home, so the compile economy holds and no refcount
 * exists; THE REGISTRY LAW on validate.h pins the free order:
 * registry first, schemas after).
 *
 * Lookup is a linear scan — registries are hand-built (a handful of
 * domain tags), so O(n) probes cost nothing next to a validation walk,
 * and registration order is the only state to reason about (last
 * registration wins). MECE: this TU owns the table; validate.c owns
 * when the walk consults it; jsonschema.c owns the subtree walk it
 * triggers. Nothing here edits the core.
 */

#include <string.h>

#include <yeptris/validate.h>

#include "../common/error.h"
#include "../memory/allocator.h"
#include "internal.h"

typedef struct {
    char* tag; /* owned copy of the caller's bytes */
    uint32_t tag_len;
    const yeptris_schema* schema; /* borrowed (THE REGISTRY LAW) */
} reg_entry;

struct yeptris_schema_registry {
    reg_entry* entries;
    uint32_t count, cap;
};

YEPTRIS_API yeptris_schema_registry* yeptris_schema_registry_new(void) {
    const yep_allocator* sys = yep_system_allocator();
    yeptris_schema_registry* r = yep_alloc(sys, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->entries = NULL;
    r->count = 0;
    r->cap = 0;
    return r;
}

YEPTRIS_API YeptrisStatus yeptris_schema_registry_register(yeptris_schema_registry* reg,
                                                           const char* tag, size_t tag_len,
                                                           const yeptris_schema* schema) {
    if (reg == NULL || tag == NULL || tag_len == 0 || tag_len > UINT32_MAX || schema == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    const yep_allocator* sys = yep_system_allocator();

    /* replace-on-reregister: the table stays a FUNCTION (one schema
     * per tag), never a multiset a lookup would have to order */
    for (uint32_t i = 0; i < reg->count; i++) {
        if (reg->entries[i].tag_len == tag_len && memcmp(reg->entries[i].tag, tag, tag_len) == 0) {
            reg->entries[i].schema = schema; /* the old binding drops */
            return YEPTRIS_OK;
        }
    }

    if (reg->count == reg->cap) {
        uint32_t ncap = reg->cap ? reg->cap * 2 : 8;
        reg_entry* ne = yep_alloc(sys, (size_t)ncap * sizeof(reg_entry));
        if (ne == NULL) {
            yep_error_set(yep_error_tls(), YEP_ERR_MEMORY, 0, 0, 0,
                          "the schema registry failed to grow");
            return YEPTRIS_ERROR_MEMORY;
        }
        if (reg->entries != NULL) {
            memcpy(ne, reg->entries, (size_t)reg->count * sizeof(reg_entry));
            yep_free(sys, reg->entries);
        }
        reg->entries = ne;
        reg->cap = ncap;
    }
    char* copy = yep_alloc(sys, tag_len);
    if (copy == NULL) {
        yep_error_set(yep_error_tls(), YEP_ERR_MEMORY, 0, 0, 0,
                      "the schema registry failed to copy a tag");
        return YEPTRIS_ERROR_MEMORY;
    }
    memcpy(copy, tag, tag_len);
    reg_entry* e = &reg->entries[reg->count++];
    e->tag = copy;
    e->tag_len = (uint32_t)tag_len;
    e->schema = schema;
    return YEPTRIS_OK;
}

YEPTRIS_API const yeptris_schema* yeptris_schema_registry_lookup(const yeptris_schema_registry* reg,
                                                                 const char* tag, size_t tag_len) {
    if (reg == NULL || tag == NULL || tag_len == 0 || tag_len > UINT32_MAX) {
        return NULL;
    }
    for (uint32_t i = 0; i < reg->count; i++) {
        if (reg->entries[i].tag_len == tag_len && memcmp(reg->entries[i].tag, tag, tag_len) == 0) {
            return reg->entries[i].schema;
        }
    }
    return NULL;
}

YEPTRIS_API void yeptris_schema_registry_free(yeptris_schema_registry* reg) {
    if (reg == NULL) {
        return;
    }
    const yep_allocator* sys = yep_system_allocator();
    for (uint32_t i = 0; i < reg->count; i++) {
        yep_free(sys, reg->entries[i].tag); /* the borrowed schemas stay the caller's */
    }
    yep_free(sys, reg->entries);
    yep_free(sys, reg);
}
