/* cpu.c — feature detection via compiler builtins (no inline asm, no
 * platform headers). SSE2 is architectural on x86-64; NEON on AArch64.
 * Advanced flags come from __builtin_cpu_supports where available.
 */

#include <stdatomic.h>

#include "cpu.h"

#if defined(__x86_64__) || defined(__i386__)
#define YEP_X86 1
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#define YEP_ARM_NEON_OK 1
#endif

/* Raw CPUID (TODO.restructure/82): __builtin_cpu_supports returned 0
 * for AVX/AVX2 on real CI Xeons (the bench artifact's kernels line
 * said scalar(sse2) — the whole kernel campaign had never run there).
 * The builtins depend on the runtime's __cpu_model initialization;
 * cpuid has no such dependency. <cpuid.h> intrinsics need no -m
 * flags; xgetbv is raw asm behind the OSXSAVE gate. */
#if defined(YEP_X86) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>

static uint64_t yep_xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}
#endif

static yep_cpu_features yep_cpu_compute(void) {
    yep_cpu_features f = {0};

#if defined(YEP_X86)
    f.sse2 = 1; /* architectural on x86-64 */
#if defined(__GNUC__) || defined(__clang__)
    {
        uint32_t a, b, c, d;
        if (__get_cpuid(1, &a, &b, &c, &d)) {
            f.sse3 = (c & (1u << 0)) != 0;
            f.ssse3 = (c & (1u << 9)) != 0;
            f.sse41 = (c & (1u << 19)) != 0;
            f.popcnt = (c & (1u << 23)) != 0;
            int osxsave = (c & (1u << 27)) != 0;
            int cpu_avx = (c & (1u << 28)) != 0;
            if (osxsave && cpu_avx && (yep_xgetbv0() & 0x6u) == 0x6u) {
                f.avx = 1;
            }
        }
        uint32_t a7, b7, c7, d7;
        if (__get_cpuid_count(7, 0, &a7, &b7, &c7, &d7)) {
            f.bmi1 = (b7 & (1u << 3)) != 0;
            f.bmi2 = (b7 & (1u << 8)) != 0;
            f.avx2 = f.avx && (b7 & (1u << 5)) != 0;
        }
    }
#endif
#elif defined(YEP_ARM_NEON_OK)
    f.neon = 1;
#if defined(__ARM_FEATURE_CRC32)
    f.crc32 = 1;
#endif
#endif

    return f;
}

static _Atomic yep_cpu_features yep_cpu_cached;

yep_cpu_features yep_cpu_detect(void) {
    yep_cpu_features f = atomic_load_explicit(&yep_cpu_cached, memory_order_acquire);
    if (!f.detected) {
        f = yep_cpu_compute();
        f.detected = 1;
        atomic_store_explicit(&yep_cpu_cached, f, memory_order_release);
    }
    return f;
}
