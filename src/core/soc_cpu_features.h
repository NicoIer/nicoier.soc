#ifndef SOC_CPU_FEATURES_H_INCLUDED
#define SOC_CPU_FEATURES_H_INCLUDED

#include <soc/soc_types.h>

#include <stdint.h>

typedef struct soc_cpu_features {
    soc_cpu_architecture architecture;
    /* Runtime-usable ISA features. AVX2 includes the required OS XCR0 state. */
    soc_cpu_feature_flags flags;
} soc_cpu_features;

/*
 * Raw x86 state is kept separate from the platform-specific CPUID calls so
 * the AVX2 OS-state checks can be unit tested without executing AVX code.
 */
typedef struct soc_x86_cpu_registers {
    uint32_t max_basic_leaf;
    uint32_t leaf1_ecx;
    uint32_t leaf1_edx;
    uint32_t leaf7_ebx;
    uint64_t xcr0;
} soc_x86_cpu_registers;

soc_cpu_features soc_cpu_features_detect(void);

soc_cpu_features soc_cpu_features_decode_x86(
    const soc_x86_cpu_registers* registers
);

static inline soc_bool soc_cpu_features_has(
    const soc_cpu_features* features,
    soc_cpu_feature_flags required
)
{
    return features != NULL && (features->flags & required) == required
        ? SOC_TRUE
        : SOC_FALSE;
}

#endif
