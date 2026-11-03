/* port.h — portability shims only; nothing YAML-specific lives here.
 *
 * Naming law: public API symbols are yeptris_*; internal symbols are yep_*.
 */
#ifndef YEP_PORT_H
#define YEP_PORT_H

#include <stddef.h>
#include <stdint.h>

#define YEP_UNUSED(x) ((void)(x))

#define YEP_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline unsigned yep_popcount32(uint32_t v) {
    return (unsigned)__popcnt(v);
}
static inline unsigned yep_ctz32(uint32_t v) {
    unsigned long i;
    _BitScanForward(&i, v);
    return (unsigned)i;
}
#else
static inline unsigned yep_popcount32(uint32_t v) {
    return (unsigned)__builtin_popcount(v);
}
static inline unsigned yep_ctz32(uint32_t v) {
    return (unsigned)__builtin_ctz(v);
}
#endif
/* Count-trailing-zeros of a nonzero u64. clang/gcc have the builtin;
 * MSVC spells it _BitScanForward64 (the windows leg pinned this). */
static inline unsigned yep_ctz64(uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i;
    _BitScanForward64(&i, v);
    return (unsigned)i;
#else
    return (unsigned)__builtin_ctzll(v);
#endif
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
/* SRWLOCK: the pthread_mutex stand-in on Windows — one shared shape
 * for the pool/mapindex locks (common/mutex.h wraps the calls). */
#include <synchapi.h>
typedef SRWLOCK yep_mutex_raw;
#else
#include <pthread.h>
typedef pthread_mutex_t yep_mutex_raw;
#endif

/* Architecture gates for the AOT SIMD TUs (TODO.impl/04). Both sides of
 * every extern pairing (TU and dispatch) use the same guards, so the link
 * always resolves regardless of which TUs CMake compiled. */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define YEP_ARCH_X86 1
#endif
/* _M_ARM64 excluded under MSVC: this TU family speaks GCC/clang NEON
 * (vector initializers, __attribute__) — MSVC-ARM64 defines __ARM_NEON
 * but cannot compile it, so Windows-ARM64 builds ride the scalar
 * kernels (a clang-cl build is the follow-up). clang-cl keeps the
 * NEON gate: it accepts the GNU-isms. */
#if (defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64)) &&                          \
    !(defined(_MSC_VER) && !defined(__clang__))
#define YEP_ARCH_AARCH64 1
#endif

#endif /* YEP_PORT_H */
