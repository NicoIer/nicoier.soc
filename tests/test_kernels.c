#include "core/soc_context.h"
#include "core/soc_kernels.h"
#include "core/soc_pipeline.h"
#include "core/soc_snapshot.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return 1; \
        } \
    } while (0)

static soc_mat4 identity_matrix(void)
{
    const soc_mat4 matrix = {
        .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
        .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
        .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
        .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    return matrix;
}

static int test_scalar_table_contract(void)
{
    const soc_kernel_table* scalar = soc_kernel_table_scalar();

    CHECK(scalar != NULL);
    CHECK(scalar->backend == SOC_KERNEL_BACKEND_SCALAR);
    CHECK(scalar->clear_f32 != NULL);
    CHECK(scalar->reduce_hiz_level_f32 != NULL);
    CHECK(scalar->test_aabbs != NULL);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_SCALAR) == scalar);
    CHECK(soc_kernel_table_select(NULL) == scalar);

#if defined(__aarch64__) || defined(_M_ARM64)
    const soc_kernel_table* neon = soc_kernel_table_neon();

    CHECK(neon != NULL);
    CHECK(neon->backend == SOC_KERNEL_BACKEND_NEON);
    CHECK(neon->clear_f32 != NULL);
    CHECK(neon->reduce_hiz_level_f32 != NULL);
    CHECK(neon->test_aabbs != NULL);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_NEON) == neon);
#else
    CHECK(soc_kernel_table_neon() == NULL);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_NEON) == NULL);
#endif
    return 0;
}

static int test_kernel_selection(void)
{
    soc_cpu_features features = {0};
    const soc_kernel_table* scalar = soc_kernel_table_scalar();

    CHECK(soc_kernel_table_select(&features) == scalar);

    features.architecture = SOC_CPU_ARCHITECTURE_X86;
    features.flags = SOC_CPU_FEATURE_NEON;
    CHECK(soc_kernel_table_select(&features) == scalar);

    features.architecture = SOC_CPU_ARCHITECTURE_ARM32;
    CHECK(soc_kernel_table_select(&features) == scalar);

    features.architecture = SOC_CPU_ARCHITECTURE_ARM64;
    features.flags = SOC_CPU_FEATURE_NONE;
    CHECK(soc_kernel_table_select(&features) == scalar);

    features.flags = SOC_CPU_FEATURE_NEON;
#if defined(__aarch64__) || defined(_M_ARM64)
    CHECK(soc_kernel_table_select(&features) == soc_kernel_table_neon());
#else
    CHECK(soc_kernel_table_select(&features) == scalar);
#endif
    return 0;
}

static int test_context_backend_constructor(void)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 7u,
        .height = 5u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    soc_context* context = NULL;

    CHECK(soc_context_create_internal(&config, &context) == SOC_RESULT_OK);
    CHECK(context != NULL);
    CHECK(context->kernels ==
        soc_kernel_table_select(&context->cpu_features));
    soc_context_destroy_internal(context);
    context = NULL;

    CHECK(soc_context_create_for_backend_for_testing_internal(
        &config,
        SOC_KERNEL_BACKEND_SCALAR,
        &context
    ) == SOC_RESULT_OK);
    CHECK(context != NULL);
    CHECK(context->kernels == soc_kernel_table_scalar());
    soc_context_destroy_internal(context);
    context = NULL;

#if defined(__aarch64__) || defined(_M_ARM64)
    CHECK(soc_context_create_for_backend_for_testing_internal(
        &config,
        SOC_KERNEL_BACKEND_NEON,
        &context
    ) == SOC_RESULT_OK);
    CHECK(context != NULL);
    CHECK(context->kernels == soc_kernel_table_neon());
    soc_context_destroy_internal(context);
    context = NULL;
#else
    CHECK(soc_context_create_for_backend_for_testing_internal(
        &config,
        SOC_KERNEL_BACKEND_NEON,
        &context
    ) == SOC_RESULT_UNSUPPORTED);
    CHECK(context == NULL);
#endif

    CHECK(soc_context_create_for_backend_for_testing_internal(
        &config,
        (soc_kernel_backend)UINT32_MAX,
        &context
    ) == SOC_RESULT_UNSUPPORTED);
    CHECK(context == NULL);
    CHECK(soc_context_create_for_backend_for_testing_internal(
        &config,
        SOC_KERNEL_BACKEND_SCALAR,
        NULL
    ) == SOC_RESULT_INVALID_ARGUMENT);
    return 0;
}

static int test_snapshot_keeps_kernel_table(void)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 7u,
        .height = 5u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    const soc_frame_desc frame = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = identity_matrix(),
        .clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE,
        .depth_direction = SOC_DEPTH_FORWARD,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    const soc_occlusion_build_desc build = {
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = &frame,
        .groups = NULL,
        .group_count = 0u,
        .group_stride = sizeof(soc_occluder_group),
    };
    soc_query_stats stats = {.struct_size = sizeof(soc_query_stats)};
    soc_context* context = NULL;
    soc_snapshot* snapshot = NULL;
    const soc_kernel_table* selected;

    CHECK(soc_context_create_for_backend_for_testing_internal(
        &config,
        SOC_KERNEL_BACKEND_SCALAR,
        &context
    ) == SOC_RESULT_OK);
    CHECK(context != NULL);
    selected = context->kernels;
    CHECK(selected == soc_kernel_table_scalar());

    CHECK(soc_occlusion_build_internal(context, &build, &snapshot) ==
        SOC_RESULT_OK);
    CHECK(snapshot != NULL);
    CHECK(snapshot->kernels == selected);

    soc_context_destroy_internal(context);
    context = NULL;
    CHECK(soc_snapshot_test_aabbs_internal(
        snapshot,
        NULL,
        0u,
        NULL,
        &stats
    ) == SOC_RESULT_OK);
    CHECK(stats.tested_aabb_count == 0u);

    soc_snapshot_destroy_internal(snapshot);
    return 0;
}

int main(void)
{
    if (test_scalar_table_contract() != 0) {
        return 1;
    }
    if (test_kernel_selection() != 0) {
        return 1;
    }
    if (test_context_backend_constructor() != 0) {
        return 1;
    }
    if (test_snapshot_keeps_kernel_table() != 0) {
        return 1;
    }
    return 0;
}
