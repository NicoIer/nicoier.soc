#include "core/soc_context.h"
#include "core/soc_cpu_features.h"

#include <stdint.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return 1; \
        } \
    } while (0)

#define TEST_X86_SSE4_1 (UINT32_C(1) << 19u)
#define TEST_X86_OSXSAVE (UINT32_C(1) << 27u)
#define TEST_X86_AVX (UINT32_C(1) << 28u)
#define TEST_X86_SSE2 (UINT32_C(1) << 26u)
#define TEST_X86_AVX2 (UINT32_C(1) << 5u)

static int test_x86_decode(void)
{
    soc_x86_cpu_registers state = {0};
    soc_cpu_features features = soc_cpu_features_decode_x86(&state);

    CHECK(features.architecture == SOC_CPU_ARCHITECTURE_X86);
    CHECK(features.flags == SOC_CPU_FEATURE_NONE);

    state.max_basic_leaf = 7u;
    state.leaf1_edx = TEST_X86_SSE2;
    state.leaf1_ecx = TEST_X86_SSE4_1 | TEST_X86_AVX;
    state.leaf7_ebx = TEST_X86_AVX2;
    state.xcr0 = UINT64_C(6);
    features = soc_cpu_features_decode_x86(&state);
    CHECK(soc_cpu_features_has(&features, SOC_CPU_FEATURE_SSE2));
    CHECK(soc_cpu_features_has(&features, SOC_CPU_FEATURE_SSE4_1));
    CHECK(!soc_cpu_features_has(&features, SOC_CPU_FEATURE_AVX2));

    state.leaf1_ecx |= TEST_X86_OSXSAVE;
    state.xcr0 = UINT64_C(2);
    features = soc_cpu_features_decode_x86(&state);
    CHECK(!soc_cpu_features_has(&features, SOC_CPU_FEATURE_AVX2));

    state.xcr0 = UINT64_C(6);
    features = soc_cpu_features_decode_x86(&state);
    CHECK(soc_cpu_features_has(&features, SOC_CPU_FEATURE_AVX2));

    state.max_basic_leaf = 6u;
    features = soc_cpu_features_decode_x86(&state);
    CHECK(!soc_cpu_features_has(&features, SOC_CPU_FEATURE_AVX2));
    CHECK(!soc_cpu_features_has(NULL, SOC_CPU_FEATURE_SSE2));
    return 0;
}

static int test_native_detection(void)
{
    const soc_cpu_features features = soc_cpu_features_detect();

    CHECK((features.flags & ~SOC_CPU_FEATURE_ALL_KNOWN) == 0u);

#if defined(_M_X64) || defined(_M_IX86) || \
    defined(__x86_64__) || defined(__i386__)
    CHECK(features.architecture == SOC_CPU_ARCHITECTURE_X86);
    #if defined(_M_X64) || defined(__x86_64__)
        CHECK(soc_cpu_features_has(&features, SOC_CPU_FEATURE_SSE2));
    #endif
#elif defined(_M_ARM64) || defined(__aarch64__)
    CHECK(features.architecture == SOC_CPU_ARCHITECTURE_ARM64);
    #if defined(__APPLE__) || defined(__ANDROID__) || defined(_WIN32)
        CHECK(soc_cpu_features_has(&features, SOC_CPU_FEATURE_NEON));
    #endif
#elif defined(_M_ARM) || defined(__arm__)
    CHECK(features.architecture == SOC_CPU_ARCHITECTURE_ARM32);
#else
    CHECK(features.architecture == SOC_CPU_ARCHITECTURE_UNKNOWN);
    CHECK(features.flags == SOC_CPU_FEATURE_NONE);
#endif

    return 0;
}

static int test_context_captures_detection(void)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    const soc_cpu_features detected = soc_cpu_features_detect();
    soc_runtime_info info = {
        .struct_size = sizeof(soc_runtime_info),
    };
    soc_context* context = NULL;

    CHECK(soc_context_create_internal(&config, &context) == SOC_RESULT_OK);
    CHECK(context != NULL);
    CHECK(context->cpu_features.architecture == detected.architecture);
    CHECK(context->cpu_features.flags == detected.flags);
    CHECK(
        soc_context_get_runtime_info_internal(context, &info) ==
            SOC_RESULT_OK
    );
    CHECK(info.cpu_architecture == context->cpu_features.architecture);
    CHECK(info.cpu_features == context->cpu_features.flags);
    CHECK(info.worker_count == context->worker_count);
    CHECK(info.worker_count >= 1u);
    CHECK(info.worker_count <= SOC_MAX_WORKER_COUNT);
    CHECK(
        info.execution_backend ==
            (context->kernels->backend == SOC_KERNEL_BACKEND_NEON
                ? SOC_EXECUTION_BACKEND_NEON
                : SOC_EXECUTION_BACKEND_SCALAR)
    );

    soc_context_destroy_internal(context);
    return 0;
}

static int test_runtime_info_reports_forced_scalar(void)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    soc_runtime_info info = {
        .struct_size = sizeof(soc_runtime_info),
    };
    soc_context* context = NULL;

    CHECK(
        soc_context_create_for_backend_for_testing_internal(
            &config,
            SOC_KERNEL_BACKEND_SCALAR,
            &context
        ) == SOC_RESULT_OK
    );
    CHECK(context != NULL);
    CHECK(
        soc_context_get_runtime_info_internal(context, &info) ==
            SOC_RESULT_OK
    );
    CHECK(info.execution_backend == SOC_EXECUTION_BACKEND_SCALAR);

    soc_context_destroy_internal(context);
    return 0;
}

int main(void)
{
    if (test_x86_decode() != 0) {
        return 1;
    }
    if (test_native_detection() != 0) {
        return 1;
    }
    if (test_context_captures_detection() != 0) {
        return 1;
    }
    if (test_runtime_info_reports_forced_scalar() != 0) {
        return 1;
    }
    return 0;
}
