#include "core/soc_cpu_features.h"

#include <stddef.h>
#include <stdint.h>

#if defined(_M_X64) || defined(_M_IX86) || \
    defined(__x86_64__) || defined(__i386__)
    #define SOC_CPU_TARGET_X86 1
#else
    #define SOC_CPU_TARGET_X86 0
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
    #define SOC_CPU_TARGET_ARM64 1
#else
    #define SOC_CPU_TARGET_ARM64 0
#endif

#if defined(_M_ARM) || defined(__arm__)
    #define SOC_CPU_TARGET_ARM32 1
#else
    #define SOC_CPU_TARGET_ARM32 0
#endif

#if SOC_CPU_TARGET_X86
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define SOC_CPU_HAS_X86_INTRINSICS 1
    #elif defined(__GNUC__) || defined(__clang__)
        #if defined(__has_include)
            #if __has_include(<cpuid.h>)
                #include <cpuid.h>
                #define SOC_CPU_HAS_X86_INTRINSICS 1
            #else
                #define SOC_CPU_HAS_X86_INTRINSICS 0
            #endif
        #else
            #include <cpuid.h>
            #define SOC_CPU_HAS_X86_INTRINSICS 1
        #endif
    #else
        #define SOC_CPU_HAS_X86_INTRINSICS 0
    #endif
#endif

#if (SOC_CPU_TARGET_ARM32 || SOC_CPU_TARGET_ARM64) && \
    (defined(__linux__) || defined(__ANDROID__))
    #include <sys/auxv.h>
#endif

#define SOC_X86_LEAF1_ECX_SSE4_1 (UINT32_C(1) << 19u)
#define SOC_X86_LEAF1_ECX_OSXSAVE (UINT32_C(1) << 27u)
#define SOC_X86_LEAF1_ECX_AVX (UINT32_C(1) << 28u)
#define SOC_X86_LEAF1_EDX_SSE2 (UINT32_C(1) << 26u)
#define SOC_X86_LEAF7_EBX_AVX2 (UINT32_C(1) << 5u)
#define SOC_X86_XCR0_XMM (UINT64_C(1) << 1u)
#define SOC_X86_XCR0_YMM (UINT64_C(1) << 2u)

/* Linux UAPI values from asm/hwcap.h. Keep them local to avoid depending on
 * architecture-specific headers which are not consistently exposed by NDKs.
 */
#define SOC_ARM32_HWCAP_NEON (1ul << 12u)
#define SOC_ARM64_HWCAP_ASIMD (1ul << 1u)

soc_cpu_features soc_cpu_features_decode_x86(
    const soc_x86_cpu_registers* registers
)
{
    soc_cpu_features features = {
        .architecture = SOC_CPU_ARCHITECTURE_X86,
        .flags = SOC_CPU_FEATURE_NONE,
    };

    if (registers == NULL || registers->max_basic_leaf < 1u) {
        return features;
    }

    if ((registers->leaf1_edx & SOC_X86_LEAF1_EDX_SSE2) != 0u) {
        features.flags |= SOC_CPU_FEATURE_SSE2;
    }
    if ((registers->leaf1_ecx & SOC_X86_LEAF1_ECX_SSE4_1) != 0u) {
        features.flags |= SOC_CPU_FEATURE_SSE4_1;
    }

    const uint32_t avx_os_mask =
        SOC_X86_LEAF1_ECX_AVX | SOC_X86_LEAF1_ECX_OSXSAVE;
    const uint64_t required_xcr0 = SOC_X86_XCR0_XMM | SOC_X86_XCR0_YMM;
    if (registers->max_basic_leaf >= 7u &&
        (registers->leaf1_ecx & avx_os_mask) == avx_os_mask &&
        (registers->xcr0 & required_xcr0) == required_xcr0 &&
        (registers->leaf7_ebx & SOC_X86_LEAF7_EBX_AVX2) != 0u) {
        features.flags |= SOC_CPU_FEATURE_AVX2;
    }

    return features;
}

#if SOC_CPU_TARGET_X86
static void soc_cpu_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t* out_eax,
    uint32_t* out_ebx,
    uint32_t* out_ecx,
    uint32_t* out_edx
)
{
#if SOC_CPU_HAS_X86_INTRINSICS && defined(_MSC_VER)
    int registers[4];
    __cpuidex(registers, (int)leaf, (int)subleaf);
    *out_eax = (uint32_t)registers[0];
    *out_ebx = (uint32_t)registers[1];
    *out_ecx = (uint32_t)registers[2];
    *out_edx = (uint32_t)registers[3];
#elif SOC_CPU_HAS_X86_INTRINSICS && \
    (defined(__GNUC__) || defined(__clang__))
    uint32_t eax = 0u;
    uint32_t ebx = 0u;
    uint32_t ecx = 0u;
    uint32_t edx = 0u;
    (void)__get_cpuid_count(leaf, subleaf, &eax, &ebx, &ecx, &edx);
    *out_eax = eax;
    *out_ebx = ebx;
    *out_ecx = ecx;
    *out_edx = edx;
#else
    (void)leaf;
    (void)subleaf;
    *out_eax = 0u;
    *out_ebx = 0u;
    *out_ecx = 0u;
    *out_edx = 0u;
#endif
}

static uint64_t soc_cpu_xgetbv_zero(void)
{
#if SOC_CPU_HAS_X86_INTRINSICS && defined(_MSC_VER)
    return (uint64_t)_xgetbv(0u);
#elif SOC_CPU_HAS_X86_INTRINSICS && \
    (defined(__GNUC__) || defined(__clang__))
    uint32_t eax;
    uint32_t edx;
    __asm__ volatile(
        "xgetbv"
        : "=a"(eax), "=d"(edx)
        : "c"(0u)
    );
    return ((uint64_t)edx << 32u) | (uint64_t)eax;
#else
    return 0u;
#endif
}

static soc_cpu_features soc_cpu_features_detect_x86(void)
{
    soc_x86_cpu_registers state = {0};
    uint32_t eax = 0u;
    uint32_t ebx = 0u;
    uint32_t ecx = 0u;
    uint32_t edx = 0u;

    soc_cpu_cpuid(0u, 0u, &eax, &ebx, &ecx, &edx);
    state.max_basic_leaf = eax;

    if (state.max_basic_leaf >= 1u) {
        soc_cpu_cpuid(1u, 0u, &eax, &ebx, &ecx, &edx);
        state.leaf1_ecx = ecx;
        state.leaf1_edx = edx;

        const uint32_t avx_os_mask =
            SOC_X86_LEAF1_ECX_AVX | SOC_X86_LEAF1_ECX_OSXSAVE;
        if ((state.leaf1_ecx & avx_os_mask) == avx_os_mask) {
            state.xcr0 = soc_cpu_xgetbv_zero();
        }
    }

    if (state.max_basic_leaf >= 7u) {
        soc_cpu_cpuid(7u, 0u, &eax, &ebx, &ecx, &edx);
        state.leaf7_ebx = ebx;
    }

    return soc_cpu_features_decode_x86(&state);
}
#endif

soc_cpu_features soc_cpu_features_detect(void)
{
#if SOC_CPU_TARGET_X86
    return soc_cpu_features_detect_x86();
#elif SOC_CPU_TARGET_ARM64
    soc_cpu_features features = {
        .architecture = SOC_CPU_ARCHITECTURE_ARM64,
        .flags = SOC_CPU_FEATURE_NONE,
    };

    /* Advanced SIMD is part of the arm64 ABI on Apple, Android, and Windows. */
    #if defined(__APPLE__) || defined(__ANDROID__) || defined(_WIN32)
        features.flags |= SOC_CPU_FEATURE_NEON;
    #elif defined(__linux__)
        if ((getauxval(AT_HWCAP) & SOC_ARM64_HWCAP_ASIMD) != 0ul) {
            features.flags |= SOC_CPU_FEATURE_NEON;
        }
    #elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        features.flags |= SOC_CPU_FEATURE_NEON;
    #endif

    return features;
#elif SOC_CPU_TARGET_ARM32
    soc_cpu_features features = {
        .architecture = SOC_CPU_ARCHITECTURE_ARM32,
        .flags = SOC_CPU_FEATURE_NONE,
    };

    #if defined(__linux__) || defined(__ANDROID__)
        if ((getauxval(AT_HWCAP) & SOC_ARM32_HWCAP_NEON) != 0ul) {
            features.flags |= SOC_CPU_FEATURE_NEON;
        }
    #elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        features.flags |= SOC_CPU_FEATURE_NEON;
    #endif

    return features;
#else
    const soc_cpu_features features = {
        .architecture = SOC_CPU_ARCHITECTURE_UNKNOWN,
        .flags = SOC_CPU_FEATURE_NONE,
    };
    return features;
#endif
}
