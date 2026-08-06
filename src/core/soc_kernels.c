#include "core/soc_kernels.h"

#include "occlusion/soc_hiz.h"
#include "occlusion/soc_visibility.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

void soc_kernel_clear_f32_scalar(
    float* destination,
    size_t count,
    float value
)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        destination[index] = value;
    }
}

void soc_kernel_merge_depth_planes_f32_scalar(
    float* level_zero,
    const float* scratch_planes,
    size_t element_count,
    size_t scratch_plane_stride,
    uint32_t lane_count,
    soc_depth_direction depth_direction
)
{
    const uint32_t scratch_plane_count = lane_count > 0u
        ? lane_count - 1u
        : 0u;
    size_t element_index = 0u;

    if (scratch_plane_count == 0u || element_count == 0u) {
        return;
    }

    if (depth_direction == SOC_DEPTH_REVERSED) {
        while (element_count - element_index >= 8u) {
            float merged0 = level_zero[element_index + 0u];
            float merged1 = level_zero[element_index + 1u];
            float merged2 = level_zero[element_index + 2u];
            float merged3 = level_zero[element_index + 3u];
            float merged4 = level_zero[element_index + 4u];
            float merged5 = level_zero[element_index + 5u];
            float merged6 = level_zero[element_index + 6u];
            float merged7 = level_zero[element_index + 7u];
            const float* scratch = scratch_planes + element_index;
            uint32_t plane_index;

            for (plane_index = 0u;
                 plane_index < scratch_plane_count;
                 ++plane_index) {
                const float candidate0 = scratch[0u];
                const float candidate1 = scratch[1u];
                const float candidate2 = scratch[2u];
                const float candidate3 = scratch[3u];
                const float candidate4 = scratch[4u];
                const float candidate5 = scratch[5u];
                const float candidate6 = scratch[6u];
                const float candidate7 = scratch[7u];

                if (candidate0 > merged0) {
                    merged0 = candidate0;
                }
                if (candidate1 > merged1) {
                    merged1 = candidate1;
                }
                if (candidate2 > merged2) {
                    merged2 = candidate2;
                }
                if (candidate3 > merged3) {
                    merged3 = candidate3;
                }
                if (candidate4 > merged4) {
                    merged4 = candidate4;
                }
                if (candidate5 > merged5) {
                    merged5 = candidate5;
                }
                if (candidate6 > merged6) {
                    merged6 = candidate6;
                }
                if (candidate7 > merged7) {
                    merged7 = candidate7;
                }
                scratch += scratch_plane_stride;
            }
            level_zero[element_index + 0u] = merged0;
            level_zero[element_index + 1u] = merged1;
            level_zero[element_index + 2u] = merged2;
            level_zero[element_index + 3u] = merged3;
            level_zero[element_index + 4u] = merged4;
            level_zero[element_index + 5u] = merged5;
            level_zero[element_index + 6u] = merged6;
            level_zero[element_index + 7u] = merged7;
            element_index += 8u;
        }
        for (; element_index < element_count; ++element_index) {
            float merged = level_zero[element_index];
            const float* scratch = scratch_planes + element_index;
            uint32_t plane_index;

            for (plane_index = 0u;
                 plane_index < scratch_plane_count;
                 ++plane_index) {
                const float candidate = *scratch;

                if (candidate > merged) {
                    merged = candidate;
                }
                scratch += scratch_plane_stride;
            }
            level_zero[element_index] = merged;
        }
    } else {
        while (element_count - element_index >= 8u) {
            float merged0 = level_zero[element_index + 0u];
            float merged1 = level_zero[element_index + 1u];
            float merged2 = level_zero[element_index + 2u];
            float merged3 = level_zero[element_index + 3u];
            float merged4 = level_zero[element_index + 4u];
            float merged5 = level_zero[element_index + 5u];
            float merged6 = level_zero[element_index + 6u];
            float merged7 = level_zero[element_index + 7u];
            const float* scratch = scratch_planes + element_index;
            uint32_t plane_index;

            for (plane_index = 0u;
                 plane_index < scratch_plane_count;
                 ++plane_index) {
                const float candidate0 = scratch[0u];
                const float candidate1 = scratch[1u];
                const float candidate2 = scratch[2u];
                const float candidate3 = scratch[3u];
                const float candidate4 = scratch[4u];
                const float candidate5 = scratch[5u];
                const float candidate6 = scratch[6u];
                const float candidate7 = scratch[7u];

                if (candidate0 < merged0) {
                    merged0 = candidate0;
                }
                if (candidate1 < merged1) {
                    merged1 = candidate1;
                }
                if (candidate2 < merged2) {
                    merged2 = candidate2;
                }
                if (candidate3 < merged3) {
                    merged3 = candidate3;
                }
                if (candidate4 < merged4) {
                    merged4 = candidate4;
                }
                if (candidate5 < merged5) {
                    merged5 = candidate5;
                }
                if (candidate6 < merged6) {
                    merged6 = candidate6;
                }
                if (candidate7 < merged7) {
                    merged7 = candidate7;
                }
                scratch += scratch_plane_stride;
            }
            level_zero[element_index + 0u] = merged0;
            level_zero[element_index + 1u] = merged1;
            level_zero[element_index + 2u] = merged2;
            level_zero[element_index + 3u] = merged3;
            level_zero[element_index + 4u] = merged4;
            level_zero[element_index + 5u] = merged5;
            level_zero[element_index + 6u] = merged6;
            level_zero[element_index + 7u] = merged7;
            element_index += 8u;
        }
        for (; element_index < element_count; ++element_index) {
            float merged = level_zero[element_index];
            const float* scratch = scratch_planes + element_index;
            uint32_t plane_index;

            for (plane_index = 0u;
                 plane_index < scratch_plane_count;
                 ++plane_index) {
                const float candidate = *scratch;

                if (candidate < merged) {
                    merged = candidate;
                }
                scratch += scratch_plane_stride;
            }
            level_zero[element_index] = merged;
        }
    }
}

void soc_kernel_store_constant_depth_block_f32_scalar(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float candidate_depth,
    soc_depth_direction depth_direction
)
{
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        float* destination_row = destination + (size_t)row * row_stride;
        uint32_t column;

        for (column = 0u; column < block_width; ++column) {
            const uint32_t bit =
                row * SOC_KERNEL_RASTER_BLOCK_SIZE + column;
            const float stored_depth = destination_row[column];

            if ((coverage_mask & (UINT64_C(1) << bit)) != 0u) {
                const soc_bool passes_depth =
                    depth_direction == SOC_DEPTH_REVERSED
                        ? (candidate_depth > stored_depth
                            ? SOC_TRUE
                            : SOC_FALSE)
                        : (candidate_depth < stored_depth
                            ? SOC_TRUE
                            : SOC_FALSE);

                if (passes_depth == SOC_TRUE) {
                    destination_row[column] = candidate_depth;
                }
            }
        }
    }
}

static float make_far_biased_plane_depth_scalar(
    float depth,
    soc_depth_direction depth_direction
)
{
    const uint32_t one_bits = UINT32_C(0x3f800000);
    uint32_t bits;

    if (depth <= 0.0f) {
        depth = 0.0f;
    } else if (depth > 1.0f) {
        depth = 1.0f;
    }
    memcpy(&bits, &depth, sizeof(bits));
    if (depth_direction == SOC_DEPTH_REVERSED) {
        bits = bits > SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            ? bits - SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            : 0u;
    } else {
        bits = bits < one_bits - SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            ? bits + SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            : one_bits;
    }
    memcpy(&depth, &bits, sizeof(depth));
    return depth;
}

void soc_kernel_store_depth_plane_block_f32_scalar(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float depth_origin,
    float depth_step_x,
    float depth_step_y,
    soc_depth_direction depth_direction
)
{
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        float* destination_row = destination + (size_t)row * row_stride;
        const uint32_t row_mask = (uint32_t)(
            coverage_mask >> (row * SOC_KERNEL_RASTER_BLOCK_SIZE)
        );
        const float row_depth = fmaf(
            depth_step_y,
            (float)row,
            depth_origin
        );
        uint32_t column;

        for (column = 0u; column < block_width; ++column) {
            const float stored_depth = destination_row[column];

            if ((row_mask & (UINT32_C(1) << column)) != 0u) {
                const float candidate_depth =
                    make_far_biased_plane_depth_scalar(
                        fmaf(depth_step_x, (float)column, row_depth),
                        depth_direction
                    );
                const soc_bool passes_depth =
                    depth_direction == SOC_DEPTH_REVERSED
                        ? (candidate_depth > stored_depth
                            ? SOC_TRUE
                            : SOC_FALSE)
                        : (candidate_depth < stored_depth
                            ? SOC_TRUE
                            : SOC_FALSE);

                if (passes_depth == SOC_TRUE) {
                    destination_row[column] = candidate_depth;
                }
            }
        }
    }
}

static const soc_kernel_table scalar_kernels = {
    .backend = SOC_KERNEL_BACKEND_SCALAR,
    .clear_f32 = soc_kernel_clear_f32_scalar,
    .merge_depth_planes_f32 = soc_kernel_merge_depth_planes_f32_scalar,
    .store_constant_depth_block_f32 =
        soc_kernel_store_constant_depth_block_f32_scalar,
    .store_depth_plane_block_f32 =
        soc_kernel_store_depth_plane_block_f32_scalar,
    .reduce_hiz_level_f32 = soc_hiz_reduce_level_scalar,
    .transform_triangle_f64 = soc_kernel_transform_triangle_f64_scalar,
    .test_aabbs = soc_occlusion_test_aabbs,
};

const soc_kernel_table* soc_kernel_table_scalar(void)
{
    return &scalar_kernels;
}

const soc_kernel_table* soc_kernel_table_select(
    const soc_cpu_features* features
)
{
    const soc_kernel_table* neon = soc_kernel_table_neon();

    if (features != NULL && neon != NULL &&
        features->architecture == SOC_CPU_ARCHITECTURE_ARM64 &&
        soc_cpu_features_has(features, SOC_CPU_FEATURE_NEON)) {
        return neon;
    }
    return soc_kernel_table_scalar();
}

const soc_kernel_table* soc_kernel_table_for_backend(
    soc_kernel_backend backend
)
{
    if (backend == SOC_KERNEL_BACKEND_SCALAR) {
        return soc_kernel_table_scalar();
    }
    if (backend == SOC_KERNEL_BACKEND_NEON) {
        return soc_kernel_table_neon();
    }
    return NULL;
}
