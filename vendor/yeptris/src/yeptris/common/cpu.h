/* cpu.h — CPU feature facts for the SIMD dispatch table (TODO.impl/04).
 *
 * Detection is atomic-lazy: the first caller computes, later callers read.
 * Concurrent first calls are benign — every writer stores identical bytes
 * through atomic stores, so there is no data race under the C11 memory
 * model (and none reported by TSAN).
 */
#ifndef YEP_CPU_H
#define YEP_CPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yep_cpu_features {
    uint16_t sse2 : 1;
    uint16_t sse3 : 1;
    uint16_t ssse3 : 1;
    uint16_t sse41 : 1;
    uint16_t popcnt : 1;
    uint16_t avx : 1;
    uint16_t avx2 : 1;
    uint16_t bmi1 : 1;
    uint16_t bmi2 : 1;
    uint16_t neon : 1;
    uint16_t crc32 : 1;
    uint16_t detected : 1; /* internal: set once computed */
} yep_cpu_features;

/* Returns the feature snapshot (computing it on first use). */
yep_cpu_features yep_cpu_detect(void);

#ifdef __cplusplus
}
#endif

#endif /* YEP_CPU_H */
