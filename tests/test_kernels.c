#include "core/soc_context.h"
#include "core/soc_kernels.h"
#include "core/soc_pipeline.h"
#include "core/soc_snapshot.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float float_from_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t make_full_depth_block_mask(
    uint32_t width,
    uint32_t height
)
{
    const uint64_t row_mask = (UINT64_C(1) << width) - UINT64_C(1);
    uint64_t mask = 0u;
    uint32_t row;

    for (row = 0u; row < height; ++row) {
        mask |= row_mask << (row * SOC_KERNEL_RASTER_BLOCK_SIZE);
    }
    return mask;
}

static float reduce_depth_block_reference(
    const float* depth,
    size_t row_stride,
    uint32_t width,
    uint32_t height,
    soc_depth_direction depth_direction
)
{
    float summary = depth[0];
    uint32_t row;

    for (row = 0u; row < height; ++row) {
        const float* depth_row = depth + (size_t)row * row_stride;
        uint32_t column;

        for (column = 0u; column < width; ++column) {
            const float candidate = depth_row[column];

            if (depth_direction == SOC_DEPTH_REVERSED) {
                if (candidate < summary) {
                    summary = candidate;
                }
            } else if (candidate > summary) {
                summary = candidate;
            }
        }
    }
    return summary == 0.0f ? 0.0f : summary;
}

static int initialize_depth_summary_storage(
    float* storage,
    size_t row_stride,
    uint32_t width,
    uint32_t height,
    float padding
)
{
    static const float logical_values[] = {
        0.10f, 0.90f, 0.20f, 0.80f, 0.30f,
        0.70f, 0.40f, 0.60f, 0.50f, 0.15f,
        0.85f, 0.25f, 0.75f, 0.35f, 0.65f,
    };
    const size_t storage_count = row_stride * (size_t)height;
    size_t index;
    uint32_t row;

    CHECK(width * height ==
        (uint32_t)(sizeof(logical_values) / sizeof(logical_values[0])));
    for (index = 0u; index < storage_count; ++index) {
        storage[index] = padding;
    }
    for (row = 0u; row < height; ++row) {
        uint32_t column;

        for (column = 0u; column < width; ++column) {
            storage[(size_t)row * row_stride + column] =
                logical_values[(size_t)row * width + column];
        }
    }
    return 0;
}

static int check_depth_summary_padding(
    const float* storage,
    size_t row_stride,
    uint32_t width,
    uint32_t height,
    float expected_padding
)
{
    uint32_t row;

    for (row = 0u; row < height; ++row) {
        size_t column;

        for (column = width; column < row_stride; ++column) {
            CHECK(float_bits(storage[(size_t)row * row_stride + column]) ==
                float_bits(expected_padding));
        }
    }
    return 0;
}

static int test_scalar_depth_block_summaries(void)
{
    enum {
        WIDTH = 5,
        HEIGHT = 3,
        ROW_STRIDE = 8,
        STORAGE_COUNT = ROW_STRIDE * HEIGHT,
    };
    static const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
    };
    const uint64_t partial_mask =
        UINT64_C(1) |
        (UINT64_C(1) << (SOC_KERNEL_RASTER_BLOCK_SIZE + 2u)) |
        (UINT64_C(1) << (2u * SOC_KERNEL_RASTER_BLOCK_SIZE + 4u));
    const uint64_t masks[] = {
        UINT64_C(0),
        partial_mask,
        make_full_depth_block_mask(WIDTH, HEIGHT),
    };
    const soc_kernel_table* scalar = soc_kernel_table_scalar();
    size_t direction_index;

    for (direction_index = 0u;
         direction_index <
            sizeof(depth_directions) / sizeof(depth_directions[0]);
         ++direction_index) {
        const soc_depth_direction depth_direction =
            depth_directions[direction_index];
        const float padding = depth_direction == SOC_DEPTH_REVERSED
            ? -10.0f
            : 10.0f;
        const float constant_candidate =
            depth_direction == SOC_DEPTH_REVERSED ? 0.55f : 0.45f;
        size_t mask_index;

        for (mask_index = 0u;
             mask_index < sizeof(masks) / sizeof(masks[0]);
             ++mask_index) {
            float storage[STORAGE_COUNT];
            float summary;
            float expected_summary;

            CHECK(initialize_depth_summary_storage(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                padding
            ) == 0);
            summary = scalar->store_constant_depth_block_f32(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                masks[mask_index],
                constant_candidate,
                depth_direction
            );
            expected_summary = reduce_depth_block_reference(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                depth_direction
            );
            CHECK(float_bits(summary) == float_bits(expected_summary));
            CHECK(check_depth_summary_padding(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                padding
            ) == 0);

            if (depth_direction == SOC_DEPTH_REVERSED) {
                const float expected_by_mask[] = {0.10f, 0.15f, 0.55f};

                CHECK(float_bits(summary) ==
                    float_bits(expected_by_mask[mask_index]));
            } else {
                const float expected_by_mask[] = {0.90f, 0.90f, 0.45f};

                CHECK(float_bits(summary) ==
                    float_bits(expected_by_mask[mask_index]));
            }
        }

        for (mask_index = 0u;
             mask_index < sizeof(masks) / sizeof(masks[0]);
             ++mask_index) {
            float storage[STORAGE_COUNT];
            float summary;
            float expected_summary;

            CHECK(initialize_depth_summary_storage(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                padding
            ) == 0);
            summary = scalar->store_depth_plane_block_f32(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                masks[mask_index],
                depth_direction == SOC_DEPTH_REVERSED ? 0.80f : 0.20f,
                depth_direction == SOC_DEPTH_REVERSED ? -0.05f : 0.05f,
                depth_direction == SOC_DEPTH_REVERSED ? -0.07f : 0.07f,
                depth_direction
            );
            expected_summary = reduce_depth_block_reference(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                depth_direction
            );
            CHECK(float_bits(summary) == float_bits(expected_summary));
            CHECK(check_depth_summary_padding(
                storage,
                ROW_STRIDE,
                WIDTH,
                HEIGHT,
                padding
            ) == 0);
        }
    }
    return 0;
}

static void merge_depth_planes_reference(
    float* level_zero,
    const float* scratch_planes,
    size_t element_count,
    size_t scratch_plane_stride,
    uint32_t lane_count,
    soc_depth_direction depth_direction
)
{
    size_t element_index;

    for (element_index = 0u;
         element_index < element_count;
         ++element_index) {
        float merged = level_zero[element_index];
        uint32_t lane_index;

        for (lane_index = 1u; lane_index < lane_count; ++lane_index) {
            const float candidate = scratch_planes[
                (size_t)(lane_index - 1u) * scratch_plane_stride +
                element_index
            ];

            if (depth_direction == SOC_DEPTH_REVERSED) {
                if (candidate > merged) {
                    merged = candidate;
                }
            } else if (candidate < merged) {
                merged = candidate;
            }
        }
        level_zero[element_index] = merged;
    }
}

static int test_merge_depth_planes_for_table(
    const soc_kernel_table* kernels,
    soc_depth_direction depth_direction
)
{
    enum {
        ELEMENT_COUNT = 71,
        SCRATCH_PLANE_STRIDE = 83,
        LANE_COUNT = 6,
        SCRATCH_ELEMENT_COUNT =
            SCRATCH_PLANE_STRIDE * (LANE_COUNT - 1)
    };
    float initial[ELEMENT_COUNT];
    float expected[ELEMENT_COUNT];
    float actual[ELEMENT_COUNT];
    float scratch[SCRATCH_ELEMENT_COUNT];
    size_t element_index;
    uint32_t lane_index;

    for (element_index = 0u;
         element_index < ELEMENT_COUNT;
         ++element_index) {
        initial[element_index] =
            (float)((element_index * 17u + 13u) % 101u) / 100.0f;
    }
    for (lane_index = 1u; lane_index < LANE_COUNT; ++lane_index) {
        float* plane = scratch +
            (size_t)(lane_index - 1u) * SCRATCH_PLANE_STRIDE;

        for (element_index = 0u;
             element_index < SCRATCH_PLANE_STRIDE;
             ++element_index) {
            plane[element_index] = (float)(
                (element_index * 29u + (size_t)lane_index * 11u + 7u) %
                103u
            ) / 102.0f;
        }
    }

    initial[3u] = float_from_bits(UINT32_C(0x7fc12345));
    scratch[5u] = float_from_bits(UINT32_C(0x7fc54321));
    initial[7u] = 0.0f;
    initial[9u] = float_from_bits(UINT32_C(0x80000000));
    for (lane_index = 1u; lane_index < LANE_COUNT; ++lane_index) {
        float* plane = scratch +
            (size_t)(lane_index - 1u) * SCRATCH_PLANE_STRIDE;

        plane[7u] = float_from_bits(UINT32_C(0x80000000));
        plane[9u] = 0.0f;
    }

    memcpy(expected, initial, sizeof(expected));
    memcpy(actual, initial, sizeof(actual));
    merge_depth_planes_reference(
        expected,
        scratch,
        ELEMENT_COUNT,
        SCRATCH_PLANE_STRIDE,
        LANE_COUNT,
        depth_direction
    );
    kernels->merge_depth_planes_f32(
        actual,
        scratch,
        ELEMENT_COUNT,
        SCRATCH_PLANE_STRIDE,
        LANE_COUNT,
        depth_direction
    );
    for (element_index = 0u;
         element_index < ELEMENT_COUNT;
         ++element_index) {
        CHECK(float_bits(actual[element_index]) ==
            float_bits(expected[element_index]));
    }

    memcpy(actual, initial, sizeof(actual));
    kernels->merge_depth_planes_f32(
        actual,
        NULL,
        ELEMENT_COUNT,
        0u,
        1u,
        depth_direction
    );
    for (element_index = 0u;
         element_index < ELEMENT_COUNT;
         ++element_index) {
        CHECK(float_bits(actual[element_index]) ==
            float_bits(initial[element_index]));
    }
    return 0;
}

static int test_merge_depth_planes(void)
{
    const soc_kernel_table* scalar = soc_kernel_table_scalar();

    CHECK(test_merge_depth_planes_for_table(
        scalar,
        SOC_DEPTH_FORWARD
    ) == 0);
    CHECK(test_merge_depth_planes_for_table(
        scalar,
        SOC_DEPTH_REVERSED
    ) == 0);
#if defined(__aarch64__) || defined(_M_ARM64)
    const soc_kernel_table* neon = soc_kernel_table_neon();

    CHECK(neon != NULL);
    CHECK(test_merge_depth_planes_for_table(
        neon,
        SOC_DEPTH_FORWARD
    ) == 0);
    CHECK(test_merge_depth_planes_for_table(
        neon,
        SOC_DEPTH_REVERSED
    ) == 0);
#endif
    return 0;
}

static int test_scalar_table_contract(void)
{
    const soc_kernel_table* scalar = soc_kernel_table_scalar();

    CHECK(scalar != NULL);
    CHECK(scalar->backend == SOC_KERNEL_BACKEND_SCALAR);
    CHECK(scalar->clear_f32 != NULL);
    CHECK(scalar->merge_depth_planes_f32 != NULL);
    CHECK(scalar->store_constant_depth_block_f32 != NULL);
    CHECK(scalar->store_depth_plane_block_f32 != NULL);
    CHECK(scalar->reduce_hiz_level_f32 != NULL);
    CHECK(scalar->transform_triangle_f64 != NULL);
    CHECK(scalar->test_aabbs != NULL);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_SCALAR) == scalar);
    CHECK(soc_kernel_table_select(NULL) == scalar);

#if defined(__aarch64__) || defined(_M_ARM64)
    const soc_kernel_table* neon = soc_kernel_table_neon();

    CHECK(neon != NULL);
    CHECK(neon->backend == SOC_KERNEL_BACKEND_NEON);
    CHECK(neon->clear_f32 != NULL);
    CHECK(neon->merge_depth_planes_f32 != NULL);
    CHECK(neon->store_constant_depth_block_f32 != NULL);
    CHECK(neon->store_depth_plane_block_f32 != NULL);
    CHECK(neon->reduce_hiz_level_f32 != NULL);
    CHECK(neon->transform_triangle_f64 != NULL);
    CHECK(neon->test_aabbs != NULL);
    CHECK(neon->test_aabbs != scalar->test_aabbs);
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
    if (test_scalar_depth_block_summaries() != 0) {
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
    if (test_merge_depth_planes() != 0) {
        return 1;
    }
    return 0;
}
