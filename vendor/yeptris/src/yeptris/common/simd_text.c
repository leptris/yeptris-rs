/* simd_text.c — dispatch: pick the best kernel table once (atomic-lazy,
 * the yep_cpu_detect pattern). The ISA TUs are compiled by CMake with the
 * right -m flags on their architectures; the guards here mirror theirs so
 * every extern resolves on every platform.
 */

#include <stdatomic.h>

#include "cpu.h"
#include "port.h"
#include "simd_text.h"

extern const yep_text_kernels yep_text_kernels_scalar;

#if defined(YEP_ARCH_X86)
extern const yep_text_kernels yep_text_kernels_avx2;
#endif
#if defined(YEP_ARCH_AARCH64)
extern const yep_text_kernels yep_text_kernels_neon;
#endif

static const yep_text_kernels* yep_text_pick(void) {
    yep_cpu_features cpu = yep_cpu_detect();
    /* arm64-MSVC has no ISA branch: port.h keeps MSVC off the
     * GCC/clang NEON TUs, so it dispatches straight to scalar */
    (void)cpu;
#if defined(YEP_ARCH_X86)
    if (cpu.avx2) {
        return &yep_text_kernels_avx2;
    }
#endif
#if defined(YEP_ARCH_AARCH64)
    if (cpu.neon) {
        return &yep_text_kernels_neon;
    }
#endif
    return &yep_text_kernels_scalar;
}

static _Atomic(const yep_text_kernels*) yep_text_cached;

const yep_text_kernels* yep_text_active(void) {
    const yep_text_kernels* k = atomic_load_explicit(&yep_text_cached, memory_order_acquire);
    if (k == NULL) {
        k = yep_text_pick();
        atomic_store_explicit(&yep_text_cached, k, memory_order_release);
    }
    return k;
}
