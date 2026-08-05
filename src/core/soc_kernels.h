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
#define SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS 4u

typedef struct soc_kernel_mat4_f64 {
    double columns[4][4];
} soc_kernel_mat4_f64;

typedef struct soc_kernel_clip_vertex {
    double x;
    double y;
    double z;
    double w;
} soc_kernel_clip_vertex;

typedef struct soc_kernel_clip_metadata {
    uint8_t active_planes;
    uint8_t common_planes;
} soc_kernel_clip_metadata;

typedef struct soc_kernel_table {
    soc_kernel_backend backend;
    void (*clear_f32)(float* destination, size_t count, float value);
    void (*merge_depth_planes_f32)(
        float* level_zero,
        const float* scratch_planes,
        size_t element_count,
        size_t scratch_plane_stride,
        uint32_t lane_count,
        soc_depth_direction depth_direction
    );
    /*
     * Block dimensions are at most 8; mask bit = row * 8 + column.
     * Stores return the farthest depth remaining in the addressed rectangle:
     * maximum for forward-Z and minimum for reversed-Z.
     */
    float (*store_constant_depth_block_f32)(
        float* destination,
        size_t row_stride,
        uint32_t block_width,
        uint32_t block_height,
        uint64_t coverage_mask,
        float candidate_depth,
        soc_depth_direction depth_direction
    );
    float (*store_depth_plane_block_f32)(
        float* destination,
        size_t row_stride,
        uint32_t block_width,
        uint32_t block_height,
        uint64_t coverage_mask,
        float depth_origin,
        float depth_step_x,
        float depth_step_y,
        soc_depth_direction depth_direction
    );
    void (*reduce_hiz_level_f32)(
        const float* source,
        uint32_t source_width,
        uint32_t source_height,
        float* destination,
        soc_depth_direction depth_direction
    );
    void (*transform_triangle_f64)(
        const soc_kernel_mat4_f64* clip_from_object,
        const float* position0_xyz,
        const float* position1_xyz,
        const float* position2_xyz,
        soc_clip_depth_range depth_range,
        soc_kernel_clip_vertex out_clip[3],
        soc_kernel_clip_metadata* out_metadata
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

/* lane_count includes level_zero; scratch_planes contains lanes 1..N-1. */
void soc_kernel_merge_depth_planes_f32_scalar(
    float* level_zero,
    const float* scratch_planes,
    size_t element_count,
    size_t scratch_plane_stride,
    uint32_t lane_count,
    soc_depth_direction depth_direction
);

float soc_kernel_store_constant_depth_block_f32_scalar(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float candidate_depth,
    soc_depth_direction depth_direction
);

float soc_kernel_store_depth_plane_block_f32_scalar(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float depth_origin,
    float depth_step_x,
    float depth_step_y,
    soc_depth_direction depth_direction
);

void soc_kernel_mat4_f64_from_f32(
    const soc_mat4* source,
    soc_kernel_mat4_f64* destination
);

/* Column-major destination = left * right; destination may alias either input. */
void soc_kernel_mat4_f64_multiply(
    const soc_kernel_mat4_f64* left,
    const soc_kernel_mat4_f64* right,
    soc_kernel_mat4_f64* destination
);

void soc_kernel_transform_triangle_f64_scalar(
    const soc_kernel_mat4_f64* clip_from_object,
    const float* position0_xyz,
    const float* position1_xyz,
    const float* position2_xyz,
    soc_clip_depth_range depth_range,
    soc_kernel_clip_vertex out_clip[3],
    soc_kernel_clip_metadata* out_metadata
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
