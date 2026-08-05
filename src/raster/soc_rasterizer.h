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
    /* Edge value, including top-left bias, at pixel sample (0.5, 0.5). */
    int64_t sample_origin;
    int64_t step_x;
    int64_t step_y;
} soc_raster_prepared_edge;

typedef struct soc_raster_prepared_region {
    uint32_t minimum_x;
    uint32_t minimum_y;
    uint32_t end_x;
    uint32_t end_y;
} soc_raster_prepared_region;

/* A depth image view whose origin is expressed in global framebuffer pixels. */
typedef struct soc_raster_target {
    float* depth;
    size_t row_stride;
    size_t element_count;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t width;
    uint32_t height;
    /*
     * Optional borrowed farthest-depth summaries for the global 8x8 cells
     * intersecting this target, packed from the cell containing the origin.
     * The caller initializes each entry to a conservative bound for depth.
     */
    float* block_depth_summaries;
    size_t block_depth_summary_count;
} soc_raster_target;

typedef struct soc_raster_prepared_triangle {
    soc_raster_prepared_edge edges[3];
    soc_raster_prepared_region bounds;
    /* Depth at pixel sample (0.5, 0.5). */
    double depth_sample_origin;
    double depth_step_x;
    double depth_step_y;
    double depth_error_bound;
    uint16_t first_tile_column;
    uint16_t first_tile_row;
    uint16_t end_tile_column;
    uint16_t end_tile_row;
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
    uint32_t block_column_count;
    uint32_t block_row_count;
    size_t block_depth_summary_count;
    /* Owned farthest-depth summaries for the fixed 8x8 block grid. */
    float* block_depth_summaries;
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

/*
 * Replays one setup record inside a global half-open pixel region.  The
 * caller owns writes to that region, so this path deliberately bypasses tile
 * locks.  Empty intersections are successful no-ops.
 */
soc_result soc_rasterizer_rasterize_prepared_region(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region
);

/*
 * Hot-path variant for an already validated active raster phase.  The caller
 * guarantees non-NULL arguments, valid prepared bounds and a well-formed
 * region; empty intersections remain no-ops.
 */
void soc_rasterizer_rasterize_prepared_region_unchecked(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region
);

/*
 * Region replay into an explicit depth view.  Geometry remains in global
 * framebuffer coordinates; only destination addressing is target-relative.
 */
soc_result soc_rasterizer_rasterize_prepared_region_to_target(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region,
    const soc_raster_target* target
);

/*
 * Hot-path target replay for an already validated active raster phase.  The
 * caller guarantees non-NULL arguments, valid prepared bounds, a well-formed
 * region inside the framebuffer and a valid target covering that region.
 * Empty prepared/region intersections remain no-ops.
 */
void soc_rasterizer_rasterize_prepared_region_to_target_unchecked(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region,
    const soc_raster_target* target
);

soc_result soc_rasterizer_finish_occluders(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_end_frame(soc_rasterizer* rasterizer);

#endif
