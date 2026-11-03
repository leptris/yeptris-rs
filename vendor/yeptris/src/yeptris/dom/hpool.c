/* hpool.c — the thread-safe handle arena (TODO.impl/19).
 *
 * Query APIs allocate one YeptrisNode wrapper per call with document
 * lifetime; read-only document sharing across threads makes those
 * allocations concurrent. A mutex-guarded bump arena (uncontended lock
 * per small allocation; blocks freed at document destruction) keeps
 * the parse-path pools entirely lock-free.
 */

#include <stdint.h>
#include <stdlib.h>

#include "dom/dom.h"
#include "memory/allocator.h"

#include "common/mutex.h"

#define HPOOL_BLOCK (16u * 1024u)

typedef struct hblock {
    struct hblock* next;
    size_t size;
    size_t used;
} hblock;

struct yep_hpool {
    const yep_allocator* sys;
    hblock* head;
    yep_mutex_raw mu;
};

static hblock* hb_new(const yep_allocator* sys, size_t payload) {
    hblock* b = yep_alloc(sys, sizeof(hblock) + payload);
    if (b == NULL) {
        return NULL;
    }
    b->next = NULL;
    b->size = payload;
    b->used = 0;
    return b;
}

struct yep_hpool* yep_hpool_create(const yep_allocator* sys) {
    if (sys == NULL) {
        return NULL;
    }
    struct yep_hpool* p = yep_alloc(sys, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->sys = sys;
    p->head = NULL;
    if (yep_mutex_init(&p->mu) != 0) {
        yep_free(sys, p);
        return NULL;
    }
    p->head = hb_new(sys, HPOOL_BLOCK);
    if (p->head == NULL) {
        yep_mutex_destroy(&p->mu);
        yep_free(sys, p);
        return NULL;
    }
    return p;
}

void yep_hpool_destroy(struct yep_hpool* p) {
    if (p == NULL) {
        return;
    }
    yep_mutex_destroy(&p->mu);
    hblock* b = p->head;
    while (b != NULL) {
        hblock* next = b->next;
        yep_free(p->sys, b);
        b = next;
    }
    yep_free(p->sys, p);
}

void* yep_hpool_alloc(struct yep_hpool* p, size_t size, size_t align) {
    if (p == NULL || size == 0 || align == 0 || (align & (align - 1)) != 0) {
        return NULL;
    }
    yep_mutex_lock(&p->mu);
    hblock* b = p->head;
    uintptr_t base = (uintptr_t)b + sizeof(hblock);
    uintptr_t start = (base + b->used + align - 1) & ~(uintptr_t)(align - 1);
    if (start + size > base + b->size) {
        size_t payload = HPOOL_BLOCK;
        if (payload < size + align + sizeof(hblock)) {
            payload = size + align + sizeof(hblock);
        }
        hblock* nb = hb_new(p->sys, payload);
        if (nb == NULL) {
            yep_mutex_unlock(&p->mu);
            return NULL;
        }
        nb->next = b;
        p->head = nb;
        base = (uintptr_t)nb + sizeof(hblock);
        start = (base + align - 1) & ~(uintptr_t)(align - 1);
        b = nb;
    }
    b->used = start + size - base;
    yep_mutex_unlock(&p->mu);
    return (void*)start;
}
