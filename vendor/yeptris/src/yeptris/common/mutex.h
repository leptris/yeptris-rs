/* mutex.h — the one lock shape for internal short-critical-section
 * mutexes (dom pool, mapindex). pthread on POSIX; SRWLOCK on Windows
 * (no trylock consumer exists — SRWLOCK's shape is a superset of what
 * we use: init/destroy/lock/unlock).
 *
 * Naming law: internal symbols are yep_*. */
#ifndef YEP_MUTEX_H
#define YEP_MUTEX_H

#include "port.h"

static inline int yep_mutex_init(yep_mutex_raw* m) {
#if defined(_WIN32)
    InitializeSRWLock(m);
    return 0;
#else
    return pthread_mutex_init(m, NULL);
#endif
}

static inline void yep_mutex_destroy(yep_mutex_raw* m) {
#if !defined(_WIN32)
    pthread_mutex_destroy(m);
#else
    (void)m; /* SRWLOCK needs no destroy */
#endif
}

static inline void yep_mutex_lock(yep_mutex_raw* m) {
#if defined(_WIN32)
    AcquireSRWLockExclusive(m);
#else
    pthread_mutex_lock(m);
#endif
}

static inline void yep_mutex_unlock(yep_mutex_raw* m) {
#if defined(_WIN32)
    ReleaseSRWLockExclusive(m);
#else
    pthread_mutex_unlock(m);
#endif
}

#endif /* YEP_MUTEX_H */
