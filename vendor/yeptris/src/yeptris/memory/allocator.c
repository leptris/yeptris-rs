/* allocator.c — the system default. */

#include <stdlib.h>

#include "allocator.h"

static void* yep_sys_alloc(void* ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void yep_sys_free(void* ctx, void* ptr) {
    (void)ctx;
    free(ptr);
}

const yep_allocator* yep_system_allocator(void) {
    static const yep_allocator sys = {yep_sys_alloc, yep_sys_free, NULL};
    return &sys;
}
