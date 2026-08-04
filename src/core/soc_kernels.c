#include "core/soc_kernels.h"

#include "occlusion/soc_hiz.h"
#include "occlusion/soc_visibility.h"

#include <stddef.h>

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

            if ((coverage_mask & (UINT64_C(1) << bit)) != 0u) {
                const float stored_depth = destination_row[column];
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
    .store_constant_depth_block_f32 =
        soc_kernel_store_constant_depth_block_f32_scalar,
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
