#ifndef SOC_KERNELS_H_INCLUDED
#define SOC_KERNELS_H_INCLUDED

#include <soc/soc.h>

#include "core/soc_cpu_features.h"

#include <stddef.h>
#include <stdint.h>

struct soc_aabb_query_context;
struct soc_hiz;
struct soc_occlusion_query_counts;

typedef uint32_t soc_kernel_backend;

#define SOC_KERNEL_BACKEND_SCALAR ((soc_kernel_backend)0u)
#define SOC_KERNEL_BACKEND_NEON ((soc_kernel_backend)1u)

#define SOC_KERNEL_RASTER_BLOCK_SIZE 8u

typedef struct soc_kernel_table {
    soc_kernel_backend backend;
    void (*clear_f32)(float* destination, size_t count, float value);
    /* block dimensions are at most 8; mask bit = row * 8 + column. */
    void (*store_constant_depth_block_f32)(
        float* destination,
        size_t row_stride,
        uint32_t block_width,
        uint32_t block_height,
        uint64_t coverage_mask,
        float candidate_depth,
        soc_depth_direction depth_direction
    );
    void (*reduce_hiz_level_f32)(
        const float* source,
        uint32_t source_width,
        uint32_t source_height,
        float* destination,
        soc_depth_direction depth_direction
    );
    soc_result (*test_aabbs)(
        const struct soc_hiz* hiz,
        const struct soc_aabb_query_context* query,
        const soc_aabb* world_bounds,
        uint32_t bounds_count,
        soc_visibility* out_visibility,
        struct soc_occlusion_query_counts* out_counts
    );
} soc_kernel_table;

void soc_kernel_clear_f32_scalar(
    float* destination,
    size_t count,
    float value
);

void soc_kernel_store_constant_depth_block_f32_scalar(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float candidate_depth,
    soc_depth_direction depth_direction
);

const soc_kernel_table* soc_kernel_table_scalar(void);

/* Returns null on build slices which do not contain AArch64 NEON kernels. */
const soc_kernel_table* soc_kernel_table_neon(void);

const soc_kernel_table* soc_kernel_table_select(
    const soc_cpu_features* features
);

/* Returns null when the requested backend is not compiled into this build. */
const soc_kernel_table* soc_kernel_table_for_backend(
    soc_kernel_backend backend
);

#endif
