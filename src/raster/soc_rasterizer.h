#ifndef SOC_RASTERIZER_H_INCLUDED
#define SOC_RASTERIZER_H_INCLUDED

#include <soc/soc.h>

#include "core/soc_kernels.h"

#include <stddef.h>
#include <stdatomic.h>

#define SOC_RASTER_LOCK_TILE_SIZE UINT32_C(32)

/*
 * A borrowed lock grid shared by every rasterizer writing one Level 0 image.
 * Each lock exclusively owns one 32x32 half-open pixel region.
 */
typedef struct soc_raster_tile_locks {
    uint32_t column_count;
    uint32_t row_count;
    size_t lock_count;
    atomic_uint* locks;
} soc_raster_tile_locks;

/*
 * Immutable fixed-point setup produced by the prepare path.  These types are
 * internal to soc_core for now; keeping the complete setup makes prepared
 * replay identical to immediate rasterization without repeating setup work.
 */
typedef struct soc_raster_prepared_edge {
    int64_t start_x;
    int64_t start_y;
    int64_t delta_x;
    int64_t delta_y;
    int64_t step_x;
    int64_t step_y;
    int64_t bias;
} soc_raster_prepared_edge;

typedef struct soc_raster_prepared_region {
    uint32_t minimum_x;
    uint32_t minimum_y;
    uint32_t end_x;
    uint32_t end_y;
} soc_raster_prepared_region;

typedef struct soc_raster_prepared_triangle {
    soc_raster_prepared_edge edges[3];
    soc_raster_prepared_region bounds;
    double depth_anchor_x;
    double depth_anchor_y;
    double depth_anchor;
    double depth_step_x;
    double depth_step_y;
    double depth_error_bound;
} soc_raster_prepared_triangle;

/* Zero initialization produces an empty, usable list. */
typedef struct soc_raster_prepared_list {
    soc_raster_prepared_triangle* data;
    size_t count;
    size_t capacity;
} soc_raster_prepared_list;

typedef struct soc_rasterizer {
    uint32_t width;
    uint32_t height;
    size_t depth_element_count;
    /* Borrowed Level 0 storage owned by the in-progress snapshot. */
    float* depth;
    const soc_kernel_table* kernels;
    /* Optional borrowed grid; NULL keeps the original lock-free path. */
    soc_raster_tile_locks* tile_locks;
    uint64_t clipped_triangle_count;
    uint64_t rasterized_triangle_count;
    soc_bool initialized;
    soc_bool frame_active;
    soc_frame_desc frame;
} soc_rasterizer;

soc_result soc_raster_tile_locks_initialize(
    soc_raster_tile_locks* tile_locks,
    uint32_t width,
    uint32_t height
);

void soc_raster_tile_locks_shutdown(
    soc_raster_tile_locks* tile_locks
);

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count,
    const soc_kernel_table* kernels
);

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_resize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count
);

soc_result soc_rasterizer_configure_tile_locks(
    soc_rasterizer* rasterizer,
    soc_raster_tile_locks* tile_locks
);

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
);

/* Begins a frame without touching the caller-initialized depth image. */
soc_result soc_rasterizer_begin_frame_no_clear(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
);

soc_result soc_rasterizer_submit_occluders(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
);

soc_result soc_rasterizer_submit_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count
);

soc_result soc_raster_prepared_list_reserve(
    soc_raster_prepared_list* list,
    size_t minimum_capacity
);

void soc_raster_prepared_list_shutdown(
    soc_raster_prepared_list* list
);

/*
 * Runs transform, clipping and fixed-point setup, appending only READY
 * triangles.  Rasterization statistics are accounted here; later replay does
 * not change them.
 */
soc_result soc_rasterizer_prepare_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count,
    soc_raster_prepared_list* prepared
);

/* Full-frame replay of immutable setup records; performs no allocation. */
soc_result soc_rasterizer_rasterize_prepared_triangles(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    size_t prepared_count
);

soc_result soc_rasterizer_finish_occluders(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_end_frame(soc_rasterizer* rasterizer);

#endif
