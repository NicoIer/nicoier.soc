#include "core/soc_pipeline.h"

#include "core/soc_context.h"
#include "core/soc_mesh.h"
#include "core/soc_snapshot.h"
#include "platform/soc_memory.h"
#include "platform/soc_thread_pool.h"
#include "raster/soc_rasterizer.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
    #define SOC_PIPELINE_FORCE_INLINE \
        static inline __attribute__((always_inline))
    #define SOC_PIPELINE_CACHE_ALIGNED __attribute__((aligned(64)))
    #define SOC_PIPELINE_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
    #define SOC_PIPELINE_FORCE_INLINE static inline
    #define SOC_PIPELINE_CACHE_ALIGNED
    #define SOC_PIPELINE_NOINLINE __declspec(noinline)
#else
    #define SOC_PIPELINE_FORCE_INLINE static inline
    #define SOC_PIPELINE_CACHE_ALIGNED
    #define SOC_PIPELINE_NOINLINE
#endif

#define SOC_PARALLEL_TILED_DENSE_MAX_TRIANGLES_PER_WORK_ITEM UINT32_C(1024)
#define SOC_PARALLEL_TILED_DENSE_MIN_TRIANGLES_PER_WORK_ITEM UINT32_C(128)
#define SOC_PARALLEL_TILED_DENSE_TARGET_WORK_ITEMS_PER_LANE UINT64_C(4)
#define SOC_PARALLEL_TILED_MASKED_TRIANGLES_PER_WORK_ITEM UINT32_C(1024)
#define SOC_PARALLEL_PRIVATE_TRIANGLES_PER_WORK_ITEM UINT32_C(256)
#define SOC_PARALLEL_DIRECT_REFERENCE_LIMIT ((size_t)256u)
#define SOC_PARALLEL_HOT_TILE_MINIMUM_REFERENCES ((size_t)1024u)
#define SOC_PARALLEL_TILE_REFERENCES_PER_LANE ((size_t)256u)
#define SOC_PARALLEL_TILE_ELEMENT_COUNT ((size_t)1024u)
#define SOC_PARALLEL_TILE_EARLY_Z_BLOCK_COUNT \
    ((size_t)(SOC_RASTER_LOCK_TILE_SIZE / SOC_KERNEL_RASTER_BLOCK_SIZE) * \
        (size_t)(SOC_RASTER_LOCK_TILE_SIZE / SOC_KERNEL_RASTER_BLOCK_SIZE))
#define SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT UINT32_C(3)
#define SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY UINT32_C(15)
#define SOC_PARALLEL_TILE_REFERENCE_CHUNK_INITIAL_CAPACITY UINT32_C(64)
#define SOC_PARALLEL_TILE_REFERENCE_CHUNK_INVALID UINT32_MAX
#define SOC_PARALLEL_TILE_EDGE_TEST_MINIMUM_TILES ((size_t)64u)
#define SOC_PARALLEL_TILE_SORT_MINIMUM_REFERENCES UINT32_C(8)
#define SOC_PARALLEL_TILE_SORT_MAXIMUM_REFERENCES UINT32_C(64)
#define SOC_PARALLEL_TILE_SORT_MINIMUM_DEPTH_SPAN 0.0625f
#define SOC_PARALLEL_TILED_MAX_LANE_COUNT UINT32_C(32)
#define SOC_PARALLEL_DEPTH_SCRATCH_BUDGET_BYTES \
    ((size_t)256u * 1024u * 1024u)
#define SOC_PARALLEL_TILED_TRAFFIC_FACTOR ((size_t)3u)
#define SOC_PARALLEL_SELECTOR_SAMPLE_SEGMENTS UINT32_C(8)
#define SOC_PARALLEL_SELECTOR_SAMPLE_TRIANGLES UINT32_C(32)
#if !defined(SOC_PARALLEL_TILED_WORK_ITEMS_PER_CLAIM)
    #define SOC_PARALLEL_TILED_WORK_ITEMS_PER_CLAIM ((size_t)1u)
#endif
#if !defined(SOC_PARALLEL_PRIVATE_WORK_ITEMS_PER_CLAIM)
    #define SOC_PARALLEL_PRIVATE_WORK_ITEMS_PER_CLAIM ((size_t)4u)
#endif
#if !defined(SOC_PARALLEL_PRIVATE_MAX_LANE_COUNT)
    #define SOC_PARALLEL_PRIVATE_MAX_LANE_COUNT UINT32_C(8)
#endif
#define SOC_PARALLEL_PRIVATE_MIN_WORK_ITEMS_PER_LANE UINT64_C(4)
#define SOC_PARALLEL_FUSED_HIZ_TARGET_ELEMENTS_PER_LANE ((size_t)393216u)
#define SOC_PARALLEL_FUSED_HIZ_MAX_LANE_COUNT UINT32_C(8)
/* Measured crossover on the primary Apple ARM64 workloads. */
#define SOC_MASKED_DIRECT_TRIANGLE_LIMIT UINT64_C(2049)
#define SOC_MASKED_PARALLEL_MIN_TRIANGLE_COUNT UINT64_C(4096)
#define SOC_MASKED_PARALLEL_MAX_LANE_COUNT UINT32_C(8)
#define SOC_MASKED_HIGH_RESOLUTION_PIXEL_THRESHOLD ((size_t)512u * 1024u)

#if !defined(SOC_EXPERIMENT_FORCE_PARALLEL_BACKEND)
    #define SOC_EXPERIMENT_FORCE_PARALLEL_BACKEND 0
#endif

static soc_bool checked_size_multiply(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || (right != 0u && left > SIZE_MAX / right)) {
        return SOC_FALSE;
    }

    *out_result = left * right;
    return SOC_TRUE;
}

static soc_bool checked_size_add(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || left > SIZE_MAX - right) {
        return SOC_FALSE;
    }

    *out_result = left + right;
    return SOC_TRUE;
}

static size_t saturating_size_multiply(size_t left, size_t right)
{
    size_t result;

    return checked_size_multiply(left, right, &result) == SOC_TRUE
        ? result
        : SIZE_MAX;
}

static soc_bool should_use_masked_backend(
    const soc_context* context,
    uint64_t input_triangle_count
)
{
    const size_t pixel_count =
        (size_t)context->width * (size_t)context->height;

    return input_triangle_count < SOC_MASKED_DIRECT_TRIANGLE_LIMIT ||
        pixel_count >= SOC_MASKED_HIGH_RESOLUTION_PIXEL_THRESHOLD
        ? SOC_TRUE
        : SOC_FALSE;
}

static soc_result validate_frame_desc(const soc_frame_desc* desc)
{
    if (desc == NULL || desc->struct_size < SOC_FRAME_DESC_SIZE_V1) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (desc->clip_depth_range != SOC_CLIP_DEPTH_ZERO_TO_ONE &&
        desc->clip_depth_range != SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (desc->front_face != SOC_FRONT_FACE_CCW &&
        desc->front_face != SOC_FRONT_FACE_CW) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (desc->flags != SOC_FRAME_FLAG_NONE) {
        return SOC_RESULT_UNSUPPORTED;
    }

    return SOC_RESULT_OK;
}

static void read_frame_desc(
    const soc_frame_desc* source,
    soc_frame_desc* out_frame
)
{
    const size_t copy_size = source->struct_size < sizeof(*out_frame)
        ? source->struct_size
        : sizeof(*out_frame);

    memset(out_frame, 0, sizeof(*out_frame));
    memcpy(out_frame, source, copy_size);
}

static void read_group(
    const soc_occlusion_build_desc* desc,
    uint32_t index,
    soc_occluder_group* out_group
)
{
    const unsigned char* source =
        (const unsigned char*)desc->groups +
        (size_t)index * desc->group_stride;
    const size_t copy_size = desc->group_stride < sizeof(*out_group)
        ? desc->group_stride
        : sizeof(*out_group);

    memset(out_group, 0, sizeof(*out_group));
    memcpy(out_group, source, copy_size);
}

static soc_result validate_build_desc(
    const soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_frame_desc* out_frame,
    uint64_t* out_input_triangle_count
)
{
    uint64_t input_triangle_count = 0u;
    size_t last_group_offset;
    size_t group_copy_size;
    uint32_t index;
    soc_result result;

    if (context == NULL ||
        desc == NULL ||
        desc->struct_size < SOC_OCCLUSION_BUILD_DESC_SIZE_V1 ||
        desc->frame == NULL ||
        out_frame == NULL ||
        out_input_triangle_count == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (desc->flags != SOC_OCCLUSION_BUILD_FLAG_NONE) {
        return SOC_RESULT_UNSUPPORTED;
    }

    result = validate_frame_desc(desc->frame);
    if (result != SOC_RESULT_OK) {
        return result;
    }
    read_frame_desc(desc->frame, out_frame);

    if (desc->group_count == 0u) {
        *out_input_triangle_count = 0u;
        return SOC_RESULT_OK;
    }
    group_copy_size = desc->group_stride < sizeof(soc_occluder_group)
        ? desc->group_stride
        : sizeof(soc_occluder_group);
    if (desc->groups == NULL ||
        desc->group_stride < SOC_OCCLUDER_GROUP_SIZE_V1 ||
        !checked_size_multiply(
            (size_t)(desc->group_count - 1u),
            (size_t)desc->group_stride,
            &last_group_offset
        ) ||
        last_group_offset > SIZE_MAX - group_copy_size) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (index = 0u; index < desc->group_count; ++index) {
        soc_occluder_group group;
        uint64_t group_triangle_count;
        size_t transform_byte_count;

        read_group(desc, index, &group);
        if (group.flags != SOC_OCCLUDER_GROUP_FLAG_NONE) {
            return SOC_RESULT_UNSUPPORTED;
        }
        if (group.instance_count == 0u) {
            continue;
        }
        if (group.mesh == NULL ||
            group.mesh->owner != context ||
            group.mesh->positions_xyz == NULL ||
            group.mesh->indices == NULL ||
            group.object_to_world == NULL ||
            !checked_size_multiply(
                (size_t)group.instance_count,
                sizeof(*group.object_to_world),
                &transform_byte_count
            )) {
            return SOC_RESULT_INVALID_ARGUMENT;
        }

        group_triangle_count =
            (uint64_t)(group.mesh->index_count / 3u) *
            group.instance_count;
        if (input_triangle_count > UINT64_MAX - group_triangle_count) {
            return SOC_RESULT_INVALID_ARGUMENT;
        }
        input_triangle_count += group_triangle_count;
    }

    *out_input_triangle_count = input_triangle_count;
    return SOC_RESULT_OK;
}

static soc_result rasterize_occluders_serial(
    const soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    float* depth,
    size_t depth_element_count,
    uint64_t* out_clipped_triangle_count,
    uint64_t* out_rasterized_triangle_count
)
{
    soc_rasterizer rasterizer;
    uint32_t index;
    soc_result result;
    soc_bool rasterizer_initialized = SOC_FALSE;
    soc_bool frame_active = SOC_FALSE;

    result = soc_rasterizer_initialize(
        &rasterizer,
        context->width,
        context->height,
        depth,
        depth_element_count,
        context->kernels
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }
    rasterizer_initialized = SOC_TRUE;

    result = soc_rasterizer_begin_frame(&rasterizer, frame);
    if (result != SOC_RESULT_OK) {
        goto cleanup;
    }
    frame_active = SOC_TRUE;

    for (index = 0u; index < desc->group_count; ++index) {
        soc_occluder_group group;

        read_group(desc, index, &group);
        if (group.instance_count == 0u) {
            continue;
        }

        result = soc_rasterizer_submit_occluders(
            &rasterizer,
            group.mesh,
            group.object_to_world,
            group.instance_count
        );
        if (result != SOC_RESULT_OK) {
            goto cleanup;
        }
    }

    result = soc_rasterizer_finish_occluders(&rasterizer);
    if (result != SOC_RESULT_OK) {
        goto cleanup;
    }

    *out_clipped_triangle_count = rasterizer.clipped_triangle_count;
    *out_rasterized_triangle_count = rasterizer.rasterized_triangle_count;

    result = soc_rasterizer_end_frame(&rasterizer);
    if (result != SOC_RESULT_OK) {
        goto cleanup;
    }
    frame_active = SOC_FALSE;

cleanup:
    if (frame_active == SOC_TRUE) {
        (void)soc_rasterizer_end_frame(&rasterizer);
    }
    if (rasterizer_initialized == SOC_TRUE) {
        soc_rasterizer_shutdown(&rasterizer);
    }
    return result;
}

static soc_result rasterize_occluders_serial_masked(
    const soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    soc_hiz* depth_pyramid,
    uint64_t* out_clipped_triangle_count,
    uint64_t* out_rasterized_triangle_count
)
{
    soc_rasterizer rasterizer;
    const soc_hiz_level* level_zero = &depth_pyramid->levels[0];
    uint32_t index;
    soc_result result;
    soc_bool rasterizer_initialized = SOC_FALSE;
    soc_bool frame_active = SOC_FALSE;

    result = soc_rasterizer_initialize_masked(
        &rasterizer,
        context->width,
        context->height,
        soc_hiz_level_data(depth_pyramid, 0u),
        depth_pyramid->working_depth,
        depth_pyramid->layer_masks,
        level_zero->width,
        level_zero->height,
        context->kernels
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }
    rasterizer_initialized = SOC_TRUE;

    result = soc_rasterizer_begin_frame(&rasterizer, frame);
    if (result != SOC_RESULT_OK) {
        goto cleanup;
    }
    frame_active = SOC_TRUE;

    for (index = 0u; index < desc->group_count; ++index) {
        soc_occluder_group group;

        read_group(desc, index, &group);
        if (group.instance_count == 0u) {
            continue;
        }

        result = soc_rasterizer_submit_occluders(
            &rasterizer,
            group.mesh,
            group.object_to_world,
            group.instance_count
        );
        if (result != SOC_RESULT_OK) {
            goto cleanup;
        }
    }

    result = soc_rasterizer_finish_occluders(&rasterizer);
    if (result != SOC_RESULT_OK) {
        goto cleanup;
    }

    *out_clipped_triangle_count = rasterizer.clipped_triangle_count;
    *out_rasterized_triangle_count = rasterizer.rasterized_triangle_count;

    result = soc_rasterizer_end_frame(&rasterizer);
    if (result != SOC_RESULT_OK) {
        goto cleanup;
    }
    frame_active = SOC_FALSE;

cleanup:
    if (frame_active == SOC_TRUE) {
        (void)soc_rasterizer_end_frame(&rasterizer);
    }
    if (rasterizer_initialized == SOC_TRUE) {
        soc_rasterizer_shutdown(&rasterizer);
    }
    return result;
}

typedef struct soc_parallel_work_item {
    const soc_mesh* mesh;
    const soc_mat4* object_to_world;
    uint32_t triangle_begin;
    uint32_t triangle_count;
} soc_parallel_work_item;

typedef struct soc_prepared_tile_range {
    size_t first_column;
    size_t first_row;
    size_t end_column;
    size_t end_row;
} soc_prepared_tile_range;

typedef struct soc_parallel_tile_reference_chunk {
    uint32_t next;
    uint32_t prepared_indices[
        SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY
    ];
} soc_parallel_tile_reference_chunk;

typedef struct soc_parallel_tile_reference_arena {
    soc_parallel_tile_reference_chunk* chunks;
    uint32_t count;
    uint32_t capacity;
} soc_parallel_tile_reference_arena;

typedef struct soc_parallel_tile_bin {
    uint32_t count;
    /*
     * Up to three lane-local prepared indices live inline.  Once promoted,
     * payload[0] and payload[1] are the first and last chunk ids.
     */
    uint32_t payload[SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT];
} soc_parallel_tile_bin;

typedef struct soc_parallel_tile_job {
    size_t tile_index;
    size_t reference_count;
    uint32_t lane_mask;
} soc_parallel_tile_job;

_Static_assert(
    sizeof(soc_parallel_tile_reference_chunk) == 64u,
    "tile reference chunks must remain one cache line"
);

_Static_assert(
    sizeof(soc_parallel_tile_bin) == 16u,
    "tile bins must remain compact"
);

_Static_assert(
    SOC_PARALLEL_TILED_MAX_LANE_COUNT <= 32u,
    "tiled lane masks hold at most 32 lanes"
);

typedef struct soc_parallel_prepare_state {
    soc_rasterizer* rasterizers;
    soc_result* lane_results;
    soc_raster_prepared_list* prepared_lists;
    const soc_parallel_work_item* work_items;
    size_t work_item_count;
    size_t tile_column_count;
    size_t tile_count;
    soc_parallel_tile_bin* tile_bins;
    soc_parallel_tile_reference_arena* tile_reference_arenas;
    atomic_size_t next_work_item;
} soc_parallel_prepare_state;

typedef struct soc_parallel_tile_row_state {
    size_t normal_tile_count;
    atomic_size_t remaining_normal_tiles;
    soc_bool has_hot_tile;
} soc_parallel_tile_row_state;

typedef struct soc_parallel_tile_state {
    soc_rasterizer* rasterizers;
    const soc_raster_prepared_list* prepared_lists;
    const soc_parallel_tile_bin* tile_bins;
    const soc_parallel_tile_reference_arena* tile_reference_arenas;
    size_t tile_count;
    uint32_t prepare_lane_count;
    const soc_parallel_tile_job* normal_tiles;
    size_t normal_tile_count;
    const soc_parallel_tile_job* hot_tiles;
    size_t hot_tile_count;
    float* hot_tile_scratch;
    soc_parallel_tile_row_state* tile_rows;
    const size_t* empty_tile_rows;
    size_t empty_tile_row_count;
    size_t tile_column_count;
    uint32_t width;
    uint32_t height;
    soc_hiz* depth_pyramid;
    const soc_kernel_table* kernels;
    uint32_t hiz_band_count;
    atomic_size_t next_job;
    atomic_size_t next_empty_tile_row;
} soc_parallel_tile_state;

_Static_assert(
    SOC_RASTER_LOCK_TILE_SIZE % SOC_HIZ_LOWER_BAND_HEIGHT == 0u,
    "raster tile rows must align to lower HiZ bands"
);

_Static_assert(
    SOC_RASTER_LOCK_TILE_SIZE % SOC_KERNEL_RASTER_BLOCK_SIZE == 0u,
    "raster tiles must contain complete early-Z blocks"
);

_Static_assert(
    SOC_RASTER_LOCK_TILE_SIZE % SOC_HIZ_MASK_BLOCK_WIDTH == 0u,
    "raster tiles must contain complete masked block columns"
);

_Static_assert(
    SOC_RASTER_LOCK_TILE_SIZE % SOC_HIZ_MASK_BLOCK_HEIGHT == 0u,
    "raster tiles must contain complete masked block rows"
);

static void build_parallel_tile_row_hiz(
    soc_parallel_tile_state* state,
    size_t tile_row
)
{
    const size_t bands_per_tile_row =
        (size_t)SOC_RASTER_LOCK_TILE_SIZE /
        (size_t)SOC_HIZ_LOWER_BAND_HEIGHT;
    const size_t first_band = tile_row * bands_per_tile_row;
    size_t band_offset;

    for (band_offset = 0u;
         band_offset < bands_per_tile_row;
         ++band_offset) {
        const size_t band_index = first_band + band_offset;
        if (band_index >= (size_t)state->hiz_band_count) {
            break;
        }
        soc_hiz_build_lower_band_unchecked_with_kernels(
            state->depth_pyramid,
            state->kernels,
            (uint32_t)band_index
        );
    }
}

static soc_bool calculate_parallel_work_item_count(
    const soc_occlusion_build_desc* desc,
    uint32_t triangles_per_work_item,
    uint64_t* out_work_item_count
)
{
    uint64_t work_item_count = 0u;
    uint32_t group_index;

    for (group_index = 0u;
         group_index < desc->group_count;
         ++group_index) {
        soc_occluder_group group;
        uint64_t instance_work_item_count;
        uint64_t group_work_item_count;
        uint32_t triangle_count;

        read_group(desc, group_index, &group);
        if (group.instance_count == 0u) {
            continue;
        }
        triangle_count = group.mesh->index_count / 3u;

        instance_work_item_count =
            triangle_count / triangles_per_work_item;
        if (triangle_count % triangles_per_work_item != 0u) {
            ++instance_work_item_count;
        }
        if (instance_work_item_count != 0u &&
            (uint64_t)group.instance_count >
                UINT64_MAX / instance_work_item_count) {
            return SOC_FALSE;
        }
        group_work_item_count =
            instance_work_item_count * group.instance_count;
        if (work_item_count > UINT64_MAX - group_work_item_count) {
            return SOC_FALSE;
        }
        work_item_count += group_work_item_count;
    }

    *out_work_item_count = work_item_count;
    return SOC_TRUE;
}

static void calculate_prepared_tile_range(
    const soc_raster_prepared_triangle* prepared,
    soc_prepared_tile_range* out_range
)
{
    out_range->first_column = prepared->first_tile_column;
    out_range->first_row = prepared->first_tile_row;
    out_range->end_column = prepared->end_tile_column;
    out_range->end_row = prepared->end_tile_row;
}

static void calculate_parallel_tile_region(
    size_t tile_index,
    size_t tile_column_count,
    uint32_t width,
    uint32_t height,
    soc_raster_prepared_region* out_region
)
{
    const size_t tile_column = tile_index % tile_column_count;
    const size_t tile_row = tile_index / tile_column_count;

    out_region->minimum_x = (uint32_t)(
        tile_column * (size_t)SOC_RASTER_LOCK_TILE_SIZE
    );
    out_region->minimum_y = (uint32_t)(
        tile_row * (size_t)SOC_RASTER_LOCK_TILE_SIZE
    );
    out_region->end_x = width - out_region->minimum_x <
            SOC_RASTER_LOCK_TILE_SIZE
        ? width
        : out_region->minimum_x + SOC_RASTER_LOCK_TILE_SIZE;
    out_region->end_y = height - out_region->minimum_y <
            SOC_RASTER_LOCK_TILE_SIZE
        ? height
        : out_region->minimum_y + SOC_RASTER_LOCK_TILE_SIZE;
}

static soc_result reserve_parallel_tile_reference_chunks(
    soc_parallel_tile_reference_arena* arena,
    uint32_t minimum_capacity
)
{
    soc_parallel_tile_reference_chunk* allocation;
    uint32_t new_capacity;
    size_t allocation_bytes;

    if (minimum_capacity <= arena->capacity) {
        return SOC_RESULT_OK;
    }
    new_capacity = arena->capacity == 0u
        ? SOC_PARALLEL_TILE_REFERENCE_CHUNK_INITIAL_CAPACITY
        : arena->capacity;
    while (new_capacity < minimum_capacity) {
        if (new_capacity <= UINT32_MAX / 2u) {
            new_capacity *= 2u;
        } else {
            new_capacity = UINT32_MAX;
        }
        if (new_capacity < minimum_capacity) {
            return SOC_RESULT_OUT_OF_MEMORY;
        }
    }
    if (!checked_size_multiply(
            (size_t)new_capacity,
            sizeof(*allocation),
            &allocation_bytes
        )) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    allocation = soc_aligned_alloc(64u, allocation_bytes);
    if (allocation == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    if (arena->count != 0u) {
        memcpy(
            allocation,
            arena->chunks,
            (size_t)arena->count * sizeof(*allocation)
        );
    }
    soc_aligned_free(arena->chunks);
    arena->chunks = allocation;
    arena->capacity = new_capacity;
    return SOC_RESULT_OK;
}

static soc_result allocate_parallel_tile_reference_chunk(
    soc_parallel_tile_reference_arena* arena,
    uint32_t* out_chunk_id
)
{
    soc_result result;

    if (arena->count == UINT32_MAX) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    result = reserve_parallel_tile_reference_chunks(
        arena,
        arena->count + 1u
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }
    *out_chunk_id = arena->count;
    ++arena->count;
    return SOC_RESULT_OK;
}

SOC_PIPELINE_FORCE_INLINE soc_result append_parallel_tile_reference(
    soc_parallel_tile_bin* bin,
    soc_parallel_tile_reference_arena* arena,
    uint32_t prepared_index
)
{
    const uint32_t old_count = bin->count;

    if (old_count == UINT32_MAX) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    if (old_count < SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT) {
        bin->payload[old_count] = prepared_index;
        bin->count = old_count + 1u;
        return SOC_RESULT_OK;
    }
    if (old_count == SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT) {
        soc_parallel_tile_reference_chunk* chunk;
        uint32_t chunk_id;
        uint32_t inline_index;
        soc_result result = allocate_parallel_tile_reference_chunk(
            arena,
            &chunk_id
        );

        if (result != SOC_RESULT_OK) {
            return result;
        }
        chunk = &arena->chunks[chunk_id];
        chunk->next = SOC_PARALLEL_TILE_REFERENCE_CHUNK_INVALID;
        for (inline_index = 0u;
             inline_index <
                SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT;
             ++inline_index) {
            chunk->prepared_indices[inline_index] =
                bin->payload[inline_index];
        }
        chunk->prepared_indices[old_count] = prepared_index;
        bin->payload[0] = chunk_id;
        bin->payload[1] = chunk_id;
        bin->count = old_count + 1u;
        return SOC_RESULT_OK;
    }
    {
        const uint32_t tail_entry_count =
            ((old_count - 1u) %
                SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY) + 1u;
        const uint32_t old_tail_id = bin->payload[1];

        if (old_tail_id >= arena->count) {
            return SOC_RESULT_INTERNAL_ERROR;
        }
        if (tail_entry_count ==
                SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY) {
            soc_parallel_tile_reference_chunk* old_tail;
            soc_parallel_tile_reference_chunk* new_tail;
            uint32_t new_tail_id;
            soc_result result = allocate_parallel_tile_reference_chunk(
                arena,
                &new_tail_id
            );

            if (result != SOC_RESULT_OK) {
                return result;
            }
            /* The allocation may have moved the arena. */
            old_tail = &arena->chunks[old_tail_id];
            new_tail = &arena->chunks[new_tail_id];
            new_tail->next =
                SOC_PARALLEL_TILE_REFERENCE_CHUNK_INVALID;
            new_tail->prepared_indices[0] = prepared_index;
            old_tail->next = new_tail_id;
            bin->payload[1] = new_tail_id;
        } else {
            arena->chunks[old_tail_id]
                .prepared_indices[tail_entry_count] = prepared_index;
        }
    }
    bin->count = old_count + 1u;
    return SOC_RESULT_OK;
}

SOC_PIPELINE_FORCE_INLINE soc_result bin_parallel_prepared_triangle(
    soc_parallel_prepare_state* state,
    size_t lane_tile_offset,
    soc_parallel_tile_reference_arena* tile_reference_arena,
    const soc_raster_prepared_triangle* prepared,
    uint32_t prepared_index
)
{
    soc_prepared_tile_range range;
    size_t tile_column_count;
    size_t tile_row_count;
    soc_bool test_tile_edges;
    size_t tile_row;

    calculate_prepared_tile_range(prepared, &range);
    tile_column_count = range.end_column - range.first_column;
    tile_row_count = range.end_row - range.first_row;
    test_tile_edges = tile_column_count * tile_row_count >=
            SOC_PARALLEL_TILE_EDGE_TEST_MINIMUM_TILES
        ? SOC_TRUE
        : SOC_FALSE;
    if (test_tile_edges != SOC_TRUE) {
        for (tile_row = range.first_row;
             tile_row < range.end_row;
             ++tile_row) {
            size_t tile_column;

            for (tile_column = range.first_column;
                 tile_column < range.end_column;
                 ++tile_column) {
                const size_t tile_index =
                    tile_row * state->tile_column_count + tile_column;
                const soc_result append_result =
                    append_parallel_tile_reference(
                        &state->tile_bins[
                            lane_tile_offset + tile_index
                        ],
                        tile_reference_arena,
                        prepared_index
                    );

                if (append_result != SOC_RESULT_OK) {
                    return append_result;
                }
            }
        }
        return SOC_RESULT_OK;
    }
    for (tile_row = range.first_row;
         tile_row < range.end_row;
         ++tile_row) {
        const uint32_t tile_minimum_y = (uint32_t)(
            tile_row * (size_t)SOC_RASTER_LOCK_TILE_SIZE
        );
        const uint32_t tile_end_y =
            tile_minimum_y + SOC_RASTER_LOCK_TILE_SIZE;
        soc_raster_prepared_region region;
        size_t tile_column;

        region.minimum_y = prepared->bounds.minimum_y > tile_minimum_y
            ? prepared->bounds.minimum_y
            : tile_minimum_y;
        region.end_y = prepared->bounds.end_y < tile_end_y
            ? prepared->bounds.end_y
            : tile_end_y;
        for (tile_column = range.first_column;
             tile_column < range.end_column;
             ++tile_column) {
            const uint32_t tile_minimum_x = (uint32_t)(
                tile_column * (size_t)SOC_RASTER_LOCK_TILE_SIZE
            );
            const uint32_t tile_end_x =
                tile_minimum_x + SOC_RASTER_LOCK_TILE_SIZE;
            const size_t tile_index =
                tile_row * state->tile_column_count + tile_column;
            soc_parallel_tile_bin* bin = &state->tile_bins[
                lane_tile_offset + tile_index
            ];

            region.minimum_x =
                prepared->bounds.minimum_x > tile_minimum_x
                ? prepared->bounds.minimum_x
                : tile_minimum_x;
            region.end_x = prepared->bounds.end_x < tile_end_x
                ? prepared->bounds.end_x
                : tile_end_x;
            if (soc_raster_prepared_region_is_edge_rejected(
                    prepared,
                    &region
                ) == SOC_TRUE) {
                continue;
            }
            {
                const soc_result append_result =
                    append_parallel_tile_reference(
                        bin,
                        tile_reference_arena,
                        prepared_index
                    );

                if (append_result != SOC_RESULT_OK) {
                    return append_result;
                }
            }
        }
    }
    return SOC_RESULT_OK;
}

typedef struct soc_parallel_tile_bin_iterator {
    const soc_parallel_tile_bin* bin;
    const soc_parallel_tile_reference_arena* arena;
    uint32_t remaining;
    uint32_t inline_offset;
    uint32_t chunk_id;
    uint32_t chunk_offset;
    uint32_t chunk_entry_count;
    uint32_t chain_remaining;
} soc_parallel_tile_bin_iterator;

static void initialize_parallel_tile_bin_iterator(
    soc_parallel_tile_bin_iterator* iterator,
    const soc_parallel_tile_bin* bin,
    const soc_parallel_tile_reference_arena* arena,
    uint32_t range_begin,
    uint32_t range_count
)
{
    uint32_t skip_count = range_begin;

    iterator->bin = bin;
    iterator->arena = arena;
    iterator->remaining = range_count;
    if (range_count == 0u) {
        return;
    }
    if (bin->count <=
            SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT) {
        iterator->inline_offset = range_begin;
        return;
    }

    iterator->chunk_id = bin->payload[0];
    iterator->chain_remaining = bin->count;
    for (;;) {
        const soc_parallel_tile_reference_chunk* chunk;

        iterator->chunk_entry_count =
            iterator->chain_remaining <
                SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY
            ? iterator->chain_remaining
            : SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY;
        if (skip_count < iterator->chunk_entry_count) {
            iterator->chunk_offset = skip_count;
            return;
        }
        skip_count -= iterator->chunk_entry_count;
        iterator->chain_remaining -= iterator->chunk_entry_count;
        chunk = &arena->chunks[iterator->chunk_id];
        iterator->chunk_id = chunk->next;
    }
}

static uint32_t next_parallel_tile_bin_reference(
    soc_parallel_tile_bin_iterator* iterator
)
{
    uint32_t prepared_index;

    if (iterator->bin->count <=
            SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT) {
        prepared_index =
            iterator->bin->payload[iterator->inline_offset++];
    } else {
        if (iterator->chunk_offset == iterator->chunk_entry_count) {
            const soc_parallel_tile_reference_chunk* old_chunk;

            old_chunk = &iterator->arena->chunks[iterator->chunk_id];
            iterator->chain_remaining -= iterator->chunk_entry_count;
            iterator->chunk_id = old_chunk->next;
            iterator->chunk_entry_count =
                iterator->chain_remaining <
                    SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY
                ? iterator->chain_remaining
                : SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY;
            iterator->chunk_offset = 0u;
        }
        prepared_index = iterator->arena
            ->chunks[iterator->chunk_id]
            .prepared_indices[iterator->chunk_offset++];
    }
    --iterator->remaining;
    return prepared_index;
}

SOC_PIPELINE_FORCE_INLINE uint32_t first_parallel_tile_bin_reference(
    const soc_parallel_tile_bin* bin,
    const soc_parallel_tile_reference_arena* arena
)
{
    return bin->count <= SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT
        ? bin->payload[0]
        : arena->chunks[bin->payload[0]].prepared_indices[0];
}

SOC_PIPELINE_FORCE_INLINE uint32_t last_parallel_tile_bin_reference(
    const soc_parallel_tile_bin* bin,
    const soc_parallel_tile_reference_arena* arena
)
{
    return bin->count <= SOC_PARALLEL_TILE_BIN_INLINE_REFERENCE_COUNT
        ? bin->payload[bin->count - 1u]
        : arena->chunks[bin->payload[1]].prepared_indices[
            (bin->count - 1u) %
                SOC_PARALLEL_TILE_REFERENCE_CHUNK_CAPACITY
        ];
}

SOC_PIPELINE_FORCE_INLINE float parallel_tile_depth_sort_key(
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* tile_region
)
{
    const uint32_t minimum_x =
        prepared->bounds.minimum_x > tile_region->minimum_x
        ? prepared->bounds.minimum_x
        : tile_region->minimum_x;
    const uint32_t minimum_y =
        prepared->bounds.minimum_y > tile_region->minimum_y
        ? prepared->bounds.minimum_y
        : tile_region->minimum_y;
    const uint32_t end_x = prepared->bounds.end_x < tile_region->end_x
        ? prepared->bounds.end_x
        : tile_region->end_x;
    const uint32_t end_y = prepared->bounds.end_y < tile_region->end_y
        ? prepared->bounds.end_y
        : tile_region->end_y;
    const uint32_t farthest_x = prepared->depth_step_x >= 0.0f
        ? minimum_x : end_x - 1u;
    const uint32_t farthest_y = prepared->depth_step_y >= 0.0f
        ? minimum_y : end_y - 1u;
    float depth;

    if (prepared->depth_step_x == 0.0f &&
        prepared->depth_step_y == 0.0f) {
        depth = prepared->depth_sample_origin;
    } else {
        depth = fmaf(
            prepared->depth_step_x,
            (float)farthest_x,
            prepared->depth_sample_origin
        );
        depth = fmaf(
            prepared->depth_step_y,
            (float)farthest_y,
            depth
        );
    }
    if (depth < 0.0f) {
        return 0.0f;
    }
    return depth > 1.0f ? 1.0f : depth;
}

static void rasterize_parallel_tile_bin_in_order(
    const soc_parallel_tile_bin* bin,
    const soc_parallel_tile_reference_arena* arena,
    const soc_raster_prepared_list* prepared_list,
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_region* region
)
{
    soc_parallel_tile_bin_iterator iterator;

    initialize_parallel_tile_bin_iterator(
        &iterator,
        bin,
        arena,
        0u,
        bin->count
    );
    while (iterator.remaining != 0u) {
        const uint32_t prepared_index =
            next_parallel_tile_bin_reference(&iterator);

        soc_rasterizer_rasterize_prepared_region_unchecked(
            rasterizer,
            &prepared_list->data[prepared_index],
            region
        );
    }
}

SOC_PIPELINE_FORCE_INLINE soc_bool
parallel_normal_tile_requires_front_to_back_sort(
    const soc_parallel_tile_state* state,
    size_t tile_index,
    uint32_t lane_mask
)
{
    const soc_raster_prepared_triangle* first_prepared = NULL;
    const soc_raster_prepared_triangle* last_prepared = NULL;
    uint32_t source_lane = 0u;

    while (lane_mask != 0u) {
        const soc_parallel_tile_bin* bin;
        const soc_parallel_tile_reference_arena* arena;
        const soc_raster_prepared_list* prepared_list;
        uint32_t prepared_index;

        while ((lane_mask & UINT32_C(1)) == 0u) {
            lane_mask >>= 1u;
            ++source_lane;
        }
        bin = &state->tile_bins[
            (size_t)source_lane * state->tile_count + tile_index
        ];
        arena = &state->tile_reference_arenas[source_lane];
        prepared_list = &state->prepared_lists[source_lane];
        prepared_index = first_parallel_tile_bin_reference(
            bin,
            arena
        );
        if (first_prepared == NULL) {
            first_prepared = &prepared_list->data[prepared_index];
        }
        prepared_index = last_parallel_tile_bin_reference(
            bin,
            arena
        );
        last_prepared = &prepared_list->data[prepared_index];
        lane_mask >>= 1u;
        ++source_lane;
    }
    return last_prepared->depth_sample_origin -
            first_prepared->depth_sample_origin >=
        SOC_PARALLEL_TILE_SORT_MINIMUM_DEPTH_SPAN
        ? SOC_TRUE
        : SOC_FALSE;
}

static SOC_PIPELINE_NOINLINE void
rasterize_parallel_normal_tile_front_to_back(
    const soc_parallel_tile_state* state,
    soc_rasterizer* rasterizer,
    size_t tile_index,
    size_t reference_count,
    uint32_t lane_mask,
    const soc_raster_prepared_region* region
)
{
    const soc_raster_prepared_triangle* references[
        SOC_PARALLEL_TILE_SORT_MAXIMUM_REFERENCES
    ];
    float depth_keys[SOC_PARALLEL_TILE_SORT_MAXIMUM_REFERENCES];
    size_t reference_index = 0u;
    uint32_t source_lane = 0u;

    while (lane_mask != 0u) {
        const soc_parallel_tile_bin* bin;
        const soc_raster_prepared_list* prepared_list;
        soc_parallel_tile_bin_iterator iterator;

        while ((lane_mask & UINT32_C(1)) == 0u) {
            lane_mask >>= 1u;
            ++source_lane;
        }
        bin = &state->tile_bins[
            (size_t)source_lane * state->tile_count + tile_index
        ];
        prepared_list = &state->prepared_lists[source_lane];
        initialize_parallel_tile_bin_iterator(
            &iterator,
            bin,
            &state->tile_reference_arenas[source_lane],
            0u,
            bin->count
        );
        while (iterator.remaining != 0u) {
            const uint32_t prepared_index =
                next_parallel_tile_bin_reference(&iterator);
            const soc_raster_prepared_triangle* prepared =
                &prepared_list->data[prepared_index];

            references[reference_index] = prepared;
            depth_keys[reference_index] = parallel_tile_depth_sort_key(
                prepared,
                region
            );
            ++reference_index;
        }
        lane_mask >>= 1u;
        ++source_lane;
    }

    for (reference_index = 1u;
         reference_index < reference_count;
         ++reference_index) {
        const soc_raster_prepared_triangle* prepared =
            references[reference_index];
        const float depth = depth_keys[reference_index];
        size_t insertion_index = reference_index;

        while (insertion_index != 0u &&
            depth > depth_keys[insertion_index - 1u]) {
            references[insertion_index] =
                references[insertion_index - 1u];
            depth_keys[insertion_index] =
                depth_keys[insertion_index - 1u];
            --insertion_index;
        }
        references[insertion_index] = prepared;
        depth_keys[insertion_index] = depth;
    }

    for (reference_index = 0u;
         reference_index < reference_count;
         ++reference_index) {
        soc_rasterizer_rasterize_prepared_region_unchecked(
            rasterizer,
            references[reference_index],
            region
        );
    }
}

static soc_bool prepare_parallel_work_item_range(
    soc_parallel_prepare_state* state,
    uint32_t worker_index,
    size_t work_item_begin,
    size_t work_item_end
)
{
    soc_rasterizer* rasterizer = &state->rasterizers[worker_index];
    soc_raster_prepared_list* prepared_list =
        &state->prepared_lists[worker_index];
    soc_parallel_tile_reference_arena* tile_reference_arena =
        &state->tile_reference_arenas[worker_index];
    const size_t lane_index = (size_t)worker_index;
    const size_t lane_tile_offset = lane_index * state->tile_count;
    size_t work_item_index;

    for (work_item_index = work_item_begin;
         work_item_index < work_item_end;
         ++work_item_index) {
        const soc_parallel_work_item* work_item =
            &state->work_items[work_item_index];
        const size_t prepared_begin = prepared_list->count;
        size_t prepared_index;
        soc_result result;

        result = soc_rasterizer_prepare_occluder_triangles(
            rasterizer,
            work_item->mesh,
            work_item->object_to_world,
            work_item->triangle_begin,
            work_item->triangle_count,
            prepared_list
        );
        if (result != SOC_RESULT_OK) {
            state->lane_results[worker_index] = result;
            return SOC_FALSE;
        }

        for (prepared_index = prepared_begin;
             prepared_index < prepared_list->count;
             ++prepared_index) {
            const soc_raster_prepared_triangle* prepared =
                &prepared_list->data[prepared_index];
            soc_result bin_result;

            if (prepared_index > (size_t)UINT32_MAX) {
                state->lane_results[worker_index] =
                    SOC_RESULT_OUT_OF_MEMORY;
                return SOC_FALSE;
            }
            bin_result = bin_parallel_prepared_triangle(
                state,
                lane_tile_offset,
                tile_reference_arena,
                prepared,
                (uint32_t)prepared_index
            );
            if (bin_result != SOC_RESULT_OK) {
                state->lane_results[worker_index] = bin_result;
                return SOC_FALSE;
            }
        }
    }
    return SOC_TRUE;
}

SOC_PIPELINE_CACHE_ALIGNED
static void parallel_prepare_ordered_work_items(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_parallel_prepare_state* state = user;
    const size_t lanes = (size_t)worker_count;
    const size_t lane = (size_t)worker_index;
    const size_t items_per_lane = state->work_item_count / lanes;
    const size_t extra_items = state->work_item_count % lanes;
    const size_t extra_before_lane = lane < extra_items
        ? lane
        : extra_items;
    const size_t work_item_begin =
        lane * items_per_lane + extra_before_lane;
    const size_t work_item_end = work_item_begin + items_per_lane +
        (lane < extra_items ? 1u : 0u);

    (void)prepare_parallel_work_item_range(
        state,
        worker_index,
        work_item_begin,
        work_item_end
    );
}

SOC_PIPELINE_CACHE_ALIGNED
static void parallel_prepare_work_items(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_parallel_prepare_state* state = user;
    soc_rasterizer* rasterizer;
    soc_raster_prepared_list* prepared_list;
    soc_parallel_tile_reference_arena* tile_reference_arena;
    const size_t lane_index = (size_t)worker_index;
    const size_t lane_tile_offset = lane_index * state->tile_count;

    (void)worker_count;

    rasterizer = &state->rasterizers[worker_index];
    prepared_list = &state->prepared_lists[worker_index];
    tile_reference_arena =
        &state->tile_reference_arenas[worker_index];
    for (;;) {
        const size_t work_item_begin = atomic_fetch_add_explicit(
            &state->next_work_item,
            SOC_PARALLEL_TILED_WORK_ITEMS_PER_CLAIM,
            memory_order_relaxed
        );
        const size_t remaining_work_item_count =
            work_item_begin < state->work_item_count
                ? state->work_item_count - work_item_begin
                : 0u;
        const size_t work_item_count = remaining_work_item_count <
                SOC_PARALLEL_TILED_WORK_ITEMS_PER_CLAIM
            ? remaining_work_item_count
            : SOC_PARALLEL_TILED_WORK_ITEMS_PER_CLAIM;
        const size_t work_item_end = work_item_begin + work_item_count;
        size_t work_item_index;

        if (work_item_count == 0u) {
            return;
        }
        for (work_item_index = work_item_begin;
             work_item_index < work_item_end;
             ++work_item_index) {
            const soc_parallel_work_item* work_item =
                &state->work_items[work_item_index];
            const size_t prepared_begin = prepared_list->count;
            size_t prepared_index;
            soc_result result;

            result = soc_rasterizer_prepare_occluder_triangles(
                rasterizer,
                work_item->mesh,
                work_item->object_to_world,
                work_item->triangle_begin,
                work_item->triangle_count,
                prepared_list
            );
            if (result != SOC_RESULT_OK) {
                state->lane_results[worker_index] = result;
                return;
            }

            for (prepared_index = prepared_begin;
                 prepared_index < prepared_list->count;
                 ++prepared_index) {
                const soc_raster_prepared_triangle* prepared =
                    &prepared_list->data[prepared_index];
                soc_result bin_result;

                if (prepared_index > (size_t)UINT32_MAX) {
                    state->lane_results[worker_index] =
                        SOC_RESULT_OUT_OF_MEMORY;
                    return;
                }
                bin_result = bin_parallel_prepared_triangle(
                    state,
                    lane_tile_offset,
                    tile_reference_arena,
                    prepared,
                    (uint32_t)prepared_index
                );
                if (bin_result != SOC_RESULT_OK) {
                    state->lane_results[worker_index] = bin_result;
                    return;
                }
            }
        }
    }
}

static void rasterize_parallel_normal_tile(
    const soc_parallel_tile_state* state,
    soc_rasterizer* rasterizer,
    size_t tile_index,
    uint32_t lane_mask,
    const soc_raster_prepared_region* region
)
{
    uint32_t source_lane = 0u;

    while (lane_mask != 0u) {
        while ((lane_mask & UINT32_C(1)) == 0u) {
            lane_mask >>= 1u;
            ++source_lane;
        }
        const size_t bin_index =
            (size_t)source_lane * state->tile_count + tile_index;
        const soc_parallel_tile_bin* bin =
            &state->tile_bins[bin_index];
        const soc_raster_prepared_list* prepared_list =
            &state->prepared_lists[source_lane];
        rasterize_parallel_tile_bin_in_order(
            bin,
            &state->tile_reference_arenas[source_lane],
            prepared_list,
            rasterizer,
            region
        );
        lane_mask >>= 1u;
        ++source_lane;
    }
}

static void rasterize_parallel_hot_tile_range(
    const soc_parallel_tile_state* state,
    soc_rasterizer* rasterizer,
    size_t tile_index,
    size_t reference_begin,
    size_t reference_count,
    uint32_t lane_mask,
    const soc_raster_prepared_region* region,
    const soc_raster_target* target
)
{
    size_t skip_count = reference_begin;
    size_t remaining_count = reference_count;
    uint32_t source_lane = 0u;

    while (lane_mask != 0u && remaining_count != 0u) {
        while ((lane_mask & UINT32_C(1)) == 0u) {
            lane_mask >>= 1u;
            ++source_lane;
        }
        const size_t bin_index =
            (size_t)source_lane * state->tile_count + tile_index;
        const soc_parallel_tile_bin* bin =
            &state->tile_bins[bin_index];
        const soc_raster_prepared_list* prepared_list =
            &state->prepared_lists[source_lane];
        const size_t bin_count = (size_t)bin->count;
        size_t lane_count;
        soc_parallel_tile_bin_iterator iterator;
        uint32_t prepared_index;

        if (skip_count >= bin_count) {
            skip_count -= bin_count;
            lane_mask >>= 1u;
            ++source_lane;
            continue;
        }
        lane_count = bin_count - skip_count;
        if (lane_count > remaining_count) {
            lane_count = remaining_count;
        }
        initialize_parallel_tile_bin_iterator(
            &iterator,
            bin,
            &state->tile_reference_arenas[source_lane],
            (uint32_t)skip_count,
            (uint32_t)lane_count
        );
        while (iterator.remaining != 0u) {
            prepared_index =
                next_parallel_tile_bin_reference(&iterator);
            soc_rasterizer_rasterize_prepared_region_to_target_unchecked(
                rasterizer,
                &prepared_list->data[prepared_index],
                region,
                target
            );
        }
        remaining_count -= lane_count;
        skip_count = 0u;
        lane_mask >>= 1u;
        ++source_lane;
    }
}

SOC_PIPELINE_CACHE_ALIGNED
static void parallel_rasterize_dense_tiles(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_parallel_tile_state* state = user;
    soc_rasterizer* rasterizer = &state->rasterizers[worker_index];
    const size_t lane_index = (size_t)worker_index;
    const size_t lane_count = (size_t)worker_count;
    float target_early_z_farthest_depths[
        SOC_PARALLEL_TILE_EARLY_Z_BLOCK_COUNT
    ];
    uint64_t target_early_z_pending_masks[
        SOC_PARALLEL_TILE_EARLY_Z_BLOCK_COUNT
    ];
    size_t hot_index;

    for (hot_index = 0u;
         hot_index < state->hot_tile_count;
         ++hot_index) {
        const soc_parallel_tile_job* hot_tile =
            &state->hot_tiles[hot_index];
        const size_t tile_index = hot_tile->tile_index;
        const size_t tile_reference_count =
            hot_tile->reference_count;
        const size_t references_per_lane =
            tile_reference_count / lane_count;
        const size_t extra_reference_count =
            tile_reference_count % lane_count;
        const size_t extra_before_lane =
            lane_index < extra_reference_count
                ? lane_index
                : extra_reference_count;
        const size_t lane_reference_begin =
            lane_index * references_per_lane + extra_before_lane;
        const size_t lane_reference_count = references_per_lane +
            (lane_index < extra_reference_count ? 1u : 0u);
        const size_t scratch_slice_index =
            hot_index * lane_count + lane_index;
        soc_raster_prepared_region region;
        soc_raster_target target;

        calculate_parallel_tile_region(
            tile_index,
            state->tile_column_count,
            state->width,
            state->height,
            &region
        );
        target.depth = state->hot_tile_scratch +
            scratch_slice_index * SOC_PARALLEL_TILE_ELEMENT_COUNT;
        target.row_stride = (size_t)SOC_RASTER_LOCK_TILE_SIZE;
        target.element_count = SOC_PARALLEL_TILE_ELEMENT_COUNT;
        target.origin_x = region.minimum_x;
        target.origin_y = region.minimum_y;
        target.width = region.end_x - region.minimum_x;
        target.height = region.end_y - region.minimum_y;
        target.early_z_farthest_depths =
            target_early_z_farthest_depths;
        target.early_z_pending_masks = target_early_z_pending_masks;
        target.early_z_column_count =
            (target.width + SOC_KERNEL_RASTER_BLOCK_SIZE - 1u) /
            SOC_KERNEL_RASTER_BLOCK_SIZE;
        target.early_z_block_count =
            (size_t)target.early_z_column_count *
            ((target.height + SOC_KERNEL_RASTER_BLOCK_SIZE - 1u) /
                SOC_KERNEL_RASTER_BLOCK_SIZE);
        soc_raster_target_reset_early_z_unchecked(&target);

        rasterize_parallel_hot_tile_range(
            state,
            rasterizer,
            tile_index,
            lane_reference_begin,
            lane_reference_count,
            hot_tile->lane_mask,
            &region,
            &target
        );
    }

    for (;;) {
        const size_t job_index = atomic_fetch_add_explicit(
            &state->next_job,
            1u,
            memory_order_relaxed
        );
        const soc_parallel_tile_job* tile_job;
        size_t tile_index;
        soc_raster_prepared_region region;

        if (job_index >= state->normal_tile_count) {
            break;
        }

        tile_job = &state->normal_tiles[job_index];
        tile_index = tile_job->tile_index;
        calculate_parallel_tile_region(
            tile_index,
            state->tile_column_count,
            state->width,
            state->height,
            &region
        );

        if (tile_job->reference_count >=
                (size_t)SOC_PARALLEL_TILE_SORT_MINIMUM_REFERENCES &&
            tile_job->reference_count <=
                (size_t)SOC_PARALLEL_TILE_SORT_MAXIMUM_REFERENCES &&
            parallel_normal_tile_requires_front_to_back_sort(
                state,
                tile_index,
                tile_job->lane_mask
            ) == SOC_TRUE) {
            rasterize_parallel_normal_tile_front_to_back(
                state,
                rasterizer,
                tile_index,
                tile_job->reference_count,
                tile_job->lane_mask,
                &region
            );
        } else {
            rasterize_parallel_normal_tile(
                state,
                rasterizer,
                tile_index,
                tile_job->lane_mask,
                &region
            );
        }

        {
            const size_t tile_row =
                tile_index / state->tile_column_count;
            soc_parallel_tile_row_state* row_state =
                &state->tile_rows[tile_row];

            if (atomic_fetch_sub_explicit(
                    &row_state->remaining_normal_tiles,
                    1u,
                    memory_order_release
                ) == 1u &&
                row_state->has_hot_tile != SOC_TRUE) {
                atomic_thread_fence(memory_order_acquire);
                build_parallel_tile_row_hiz(state, tile_row);
            }
        }
    }

    for (;;) {
        const size_t job_index = atomic_fetch_add_explicit(
            &state->next_empty_tile_row,
            1u,
            memory_order_relaxed
        );

        if (job_index >= state->empty_tile_row_count) {
            return;
        }
        build_parallel_tile_row_hiz(
            state,
            state->empty_tile_rows[job_index]
        );
    }
}

SOC_PIPELINE_CACHE_ALIGNED
static void parallel_rasterize_masked_tiles(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_parallel_tile_state* state = user;
    soc_rasterizer* rasterizer = &state->rasterizers[worker_index];

    (void)worker_count;
    for (;;) {
        const size_t job_index = atomic_fetch_add_explicit(
            &state->next_job,
            1u,
            memory_order_relaxed
        );
        const soc_parallel_tile_job* tile_job;
        size_t tile_index;
        soc_raster_prepared_region region;

        if (job_index >= state->normal_tile_count) {
            return;
        }

        tile_job = &state->normal_tiles[job_index];
        tile_index = tile_job->tile_index;
        calculate_parallel_tile_region(
            tile_index,
            state->tile_column_count,
            state->width,
            state->height,
            &region
        );
        rasterize_parallel_normal_tile(
            state,
            rasterizer,
            tile_index,
            tile_job->lane_mask,
            &region
        );
    }
}

static void merge_parallel_hot_tiles(
    float* level_zero,
    const float* hot_tile_scratch,
    const soc_parallel_tile_job* hot_tiles,
    size_t hot_tile_count,
    uint32_t lane_count,
    size_t tile_column_count,
    uint32_t width,
    uint32_t height,
    const soc_kernel_table* kernels
)
{
    size_t hot_index;

    for (hot_index = 0u; hot_index < hot_tile_count; ++hot_index) {
        const size_t tile_index = hot_tiles[hot_index].tile_index;
        const float* scratch_tile = hot_tile_scratch +
            hot_index * (size_t)lane_count *
                SOC_PARALLEL_TILE_ELEMENT_COUNT;
        soc_raster_prepared_region region;
        uint32_t pixel_y;

        calculate_parallel_tile_region(
            tile_index,
            tile_column_count,
            width,
            height,
            &region
        );
        for (pixel_y = region.minimum_y;
             pixel_y < region.end_y;
             ++pixel_y) {
            const size_t local_y =
                (size_t)(pixel_y - region.minimum_y);
            const size_t global_index =
                (size_t)pixel_y * (size_t)width +
                (size_t)region.minimum_x;

            kernels->merge_depth_planes_f32(
                level_zero + global_index,
                scratch_tile +
                    local_y * (size_t)SOC_RASTER_LOCK_TILE_SIZE,
                (size_t)(region.end_x - region.minimum_x),
                SOC_PARALLEL_TILE_ELEMENT_COUNT,
                lane_count + 1u
            );
        }
    }
}

typedef struct soc_private_raster_state {
    const soc_frame_desc* frame;
    soc_rasterizer* rasterizers;
    soc_result* lane_results;
    const soc_parallel_work_item* work_items;
    size_t work_item_count;
    atomic_size_t next_work_item;
} soc_private_raster_state;

typedef struct soc_private_merge_state {
    float* level_zero;
    const float* scratch_depth;
    size_t depth_element_count;
    uint32_t lane_count;
    const soc_kernel_table* kernels;
    soc_hiz* depth_pyramid;
    uint32_t hiz_band_count;
    atomic_uint next_hiz_band;
} soc_private_merge_state;

static void private_begin_frames(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_private_raster_state* state = user;

    (void)worker_count;
    state->lane_results[worker_index] = soc_rasterizer_begin_frame(
        &state->rasterizers[worker_index],
        state->frame
    );
}

static void private_rasterize_work_items(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_private_raster_state* state = user;
    soc_rasterizer* rasterizer;

    (void)worker_count;

    if (state->lane_results[worker_index] != SOC_RESULT_OK) {
        return;
    }

    rasterizer = &state->rasterizers[worker_index];
    for (;;) {
        const size_t work_item_begin = atomic_fetch_add_explicit(
            &state->next_work_item,
            SOC_PARALLEL_PRIVATE_WORK_ITEMS_PER_CLAIM,
            memory_order_relaxed
        );
        const size_t remaining_work_item_count =
            work_item_begin < state->work_item_count
                ? state->work_item_count - work_item_begin
                : 0u;
        const size_t work_item_count = remaining_work_item_count <
                SOC_PARALLEL_PRIVATE_WORK_ITEMS_PER_CLAIM
            ? remaining_work_item_count
            : SOC_PARALLEL_PRIVATE_WORK_ITEMS_PER_CLAIM;
        const size_t work_item_end = work_item_begin + work_item_count;
        size_t work_item_index;

        if (work_item_count == 0u) {
            break;
        }
        for (work_item_index = work_item_begin;
             work_item_index < work_item_end;
             ++work_item_index) {
            const soc_parallel_work_item* work_item =
                &state->work_items[work_item_index];
            const soc_result result =
                soc_rasterizer_submit_occluder_triangles(
                    rasterizer,
                    work_item->mesh,
                    work_item->object_to_world,
                    work_item->triangle_begin,
                    work_item->triangle_count
                );

            if (result != SOC_RESULT_OK) {
                state->lane_results[worker_index] = result;
                return;
            }
        }
        if (work_item_end == state->work_item_count) {
            break;
        }
    }

    state->lane_results[worker_index] =
        soc_rasterizer_finish_occluders(rasterizer);
}

static void private_merge_depth(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_private_merge_state* state = user;
    const size_t width = state->depth_pyramid->levels[0].width;
    const uint32_t height = state->depth_pyramid->levels[0].height;

    (void)worker_index;
    (void)worker_count;
    for (;;) {
        const uint32_t band_index = atomic_fetch_add_explicit(
            &state->next_hiz_band,
            1u,
            memory_order_relaxed
        );
        uint32_t row_begin;
        uint32_t row_end;
        size_t element_begin;
        size_t element_end;

        if (band_index >= state->hiz_band_count) {
            return;
        }
        row_begin = band_index * SOC_HIZ_LOWER_BAND_HEIGHT;
        row_end = height - row_begin < SOC_HIZ_LOWER_BAND_HEIGHT
            ? height
            : row_begin + SOC_HIZ_LOWER_BAND_HEIGHT;
        element_begin = (size_t)row_begin * width;
        element_end = (size_t)row_end * width;
        state->kernels->merge_depth_planes_f32(
            state->level_zero + element_begin,
            state->scratch_depth + element_begin,
            element_end - element_begin,
            state->depth_element_count,
            state->lane_count
        );
        soc_hiz_build_lower_band_unchecked_with_kernels(
            state->depth_pyramid,
            state->kernels,
            band_index
        );
    }
}

static void private_begin_and_rasterize(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_private_raster_state* state = user;

    private_begin_frames(state, worker_index, worker_count);
    private_rasterize_work_items(state, worker_index, worker_count);
}

static void cleanup_parallel_rasterizers(
    soc_rasterizer* rasterizers,
    uint32_t initialized_count
)
{
    uint32_t lane;

    for (lane = 0u; lane < initialized_count; ++lane) {
        if (rasterizers[lane].frame_active == SOC_TRUE) {
            (void)soc_rasterizer_end_frame(&rasterizers[lane]);
        }
        soc_rasterizer_shutdown(&rasterizers[lane]);
    }
}

static void cleanup_parallel_prepared_lists(
    soc_raster_prepared_list* prepared_lists,
    uint32_t list_count
)
{
    uint32_t lane;

    if (prepared_lists == NULL) {
        return;
    }
    for (lane = 0u; lane < list_count; ++lane) {
        soc_raster_prepared_list_shutdown(&prepared_lists[lane]);
    }
}

static void cleanup_parallel_tile_reference_arenas(
    soc_parallel_tile_reference_arena* arenas,
    uint32_t arena_count
)
{
    uint32_t lane;

    if (arenas == NULL) {
        return;
    }
    for (lane = 0u; lane < arena_count; ++lane) {
        soc_aligned_free(arenas[lane].chunks);
    }
}

/*
 * Returns SOC_FALSE when the parallel path is ineligible or cannot allocate
 * its optional working storage. The caller must then use the serial path.
 */
SOC_PIPELINE_CACHE_ALIGNED
static soc_bool try_rasterize_occluders_tiled_dense(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    soc_hiz* depth_pyramid,
    float* level_zero,
    size_t depth_element_count,
    uint64_t* out_clipped_triangle_count,
    uint64_t* out_rasterized_triangle_count,
    soc_bool* out_lower_hiz_built,
    soc_result* out_result
)
{
    uint64_t work_item_count_u64;
    size_t work_item_count;
    size_t work_item_bytes;
    size_t rasterizer_bytes;
    size_t lane_result_bytes;
    size_t prepared_list_bytes;
    size_t tile_bin_entry_count;
    size_t tile_bin_bytes;
    size_t tile_reference_arena_bytes;
    size_t nonempty_tile_bytes;
    size_t tile_row_state_bytes;
    size_t empty_tile_row_bytes;
    size_t hot_tile_bytes;
    size_t scratch_slice_count;
    size_t scratch_element_count;
    size_t scratch_bytes;
    size_t tile_column_count;
    size_t tile_row_count;
    size_t tile_count;
    size_t total_reference_count = 0u;
    size_t nonempty_tile_count = 0u;
    size_t normal_tile_count = 0u;
    size_t hot_tile_count = 0u;
    size_t empty_tile_row_count = 0u;
    size_t source_triangle_count = 0u;
    const uint32_t configured_lane_count =
        context->worker_count <
            SOC_PARALLEL_TILED_MAX_LANE_COUNT
        ? context->worker_count
        : SOC_PARALLEL_TILED_MAX_LANE_COUNT;
    uint32_t triangles_per_work_item =
        SOC_PARALLEL_TILED_DENSE_MAX_TRIANGLES_PER_WORK_ITEM;
    uint32_t prepare_lane_count;
    uint32_t raster_lane_count = 0u;
    uint32_t lane;
    uint32_t initialized_count = 0u;
    soc_parallel_work_item* work_items = NULL;
    soc_rasterizer* rasterizers = NULL;
    soc_result* lane_results = NULL;
    soc_raster_prepared_list* prepared_lists = NULL;
    soc_parallel_tile_bin* tile_bins = NULL;
    soc_parallel_tile_reference_arena* tile_reference_arenas = NULL;
    soc_parallel_tile_job* nonempty_tiles = NULL;
    soc_parallel_tile_row_state* tile_rows = NULL;
    size_t* empty_tile_rows = NULL;
    soc_parallel_tile_job* hot_tiles = NULL;
    float* hot_tile_scratch = NULL;
    soc_parallel_prepare_state prepare_state;
    soc_parallel_tile_state tile_state;
    soc_result result = SOC_RESULT_OK;
    soc_bool return_parallel_result = SOC_FALSE;
    size_t work_item_index = 0u;
    uint32_t group_index;

    *out_lower_hiz_built = SOC_FALSE;
    if (configured_lane_count <= 1u) {
        return SOC_FALSE;
    }
    if (!calculate_parallel_work_item_count(
            desc,
            triangles_per_work_item,
            &work_item_count_u64
        )) {
        return SOC_FALSE;
    }
#if defined(SOC_BUILD_AARCH32_NEON_FMA)
    while (work_item_count_u64 <
                (uint64_t)configured_lane_count *
                    SOC_PARALLEL_TILED_DENSE_TARGET_WORK_ITEMS_PER_LANE &&
        triangles_per_work_item >
            SOC_PARALLEL_TILED_DENSE_MIN_TRIANGLES_PER_WORK_ITEM) {
        triangles_per_work_item /= 2u;
        if (!calculate_parallel_work_item_count(
                desc,
                triangles_per_work_item,
                &work_item_count_u64
            )) {
            return SOC_FALSE;
        }
    }
#endif
    if (work_item_count_u64 == 0u ||
        work_item_count_u64 > (uint64_t)SIZE_MAX) {
        return SOC_FALSE;
    }

    prepare_lane_count = configured_lane_count;
    if (work_item_count_u64 < (uint64_t)prepare_lane_count) {
        prepare_lane_count = (uint32_t)work_item_count_u64;
    }

    work_item_count = (size_t)work_item_count_u64;
    tile_column_count = (size_t)(
        context->width / SOC_RASTER_LOCK_TILE_SIZE
    );
    if (context->width % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++tile_column_count;
    }
    tile_row_count = (size_t)(
        context->height / SOC_RASTER_LOCK_TILE_SIZE
    );
    if (context->height % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++tile_row_count;
    }

    if (!checked_size_multiply(
            work_item_count,
            sizeof(*work_items),
            &work_item_bytes
        ) ||
        !checked_size_multiply(
            (size_t)configured_lane_count,
            sizeof(*rasterizers),
            &rasterizer_bytes
        ) ||
        !checked_size_multiply(
            (size_t)prepare_lane_count,
            sizeof(*lane_results),
            &lane_result_bytes
        ) ||
        !checked_size_multiply(
            (size_t)prepare_lane_count,
            sizeof(*prepared_lists),
            &prepared_list_bytes
        ) ||
        !checked_size_multiply(
            tile_column_count,
            tile_row_count,
            &tile_count
        )) {
        return SOC_FALSE;
    }
    if (tile_count == 0u) {
        *out_result = SOC_RESULT_INTERNAL_ERROR;
        return SOC_TRUE;
    }
    if (!checked_size_multiply(
            (size_t)prepare_lane_count,
            tile_count,
            &tile_bin_entry_count
        ) ||
        !checked_size_multiply(
            tile_bin_entry_count,
            sizeof(*tile_bins),
            &tile_bin_bytes
        ) ||
        !checked_size_multiply(
            (size_t)prepare_lane_count,
            sizeof(*tile_reference_arenas),
            &tile_reference_arena_bytes
        ) ||
        !checked_size_multiply(
            tile_count,
            sizeof(*nonempty_tiles),
            &nonempty_tile_bytes
        ) ||
        !checked_size_multiply(
            tile_row_count,
            sizeof(*tile_rows),
            &tile_row_state_bytes
        ) ||
        !checked_size_multiply(
            tile_row_count,
            sizeof(*empty_tile_rows),
            &empty_tile_row_bytes
        )) {
        return SOC_FALSE;
    }

    work_items = malloc(work_item_bytes);
    rasterizers = calloc(1u, rasterizer_bytes);
    lane_results = malloc(lane_result_bytes);
    prepared_lists = calloc(1u, prepared_list_bytes);
    tile_bins = calloc(1u, tile_bin_bytes);
    tile_reference_arenas = calloc(1u, tile_reference_arena_bytes);
    nonempty_tiles = malloc(nonempty_tile_bytes);
    tile_rows = calloc(1u, tile_row_state_bytes);
    empty_tile_rows = malloc(empty_tile_row_bytes);
    if (work_items == NULL ||
        rasterizers == NULL ||
        lane_results == NULL ||
        prepared_lists == NULL ||
        tile_bins == NULL ||
        tile_reference_arenas == NULL ||
        nonempty_tiles == NULL ||
        tile_rows == NULL ||
        empty_tile_rows == NULL) {
        result = SOC_RESULT_OUT_OF_MEMORY;
        goto attempted_cleanup;
    }
    {
        size_t tile_row;

        for (tile_row = 0u; tile_row < tile_row_count; ++tile_row) {
            atomic_init(
                &tile_rows[tile_row].remaining_normal_tiles,
                0u
            );
        }
    }

    for (group_index = 0u;
         group_index < desc->group_count;
         ++group_index) {
        soc_occluder_group group;
        uint32_t instance;
        uint32_t triangle_count;

        read_group(desc, group_index, &group);
        if (group.instance_count == 0u) {
            continue;
        }
        triangle_count = group.mesh->index_count / 3u;

        for (instance = 0u; instance < group.instance_count; ++instance) {
            uint32_t triangle_begin = 0u;

            while (triangle_begin < triangle_count) {
                const uint32_t remaining =
                    triangle_count - triangle_begin;
                const uint32_t item_triangle_count =
                    remaining < triangles_per_work_item
                        ? remaining
                        : triangles_per_work_item;
                soc_parallel_work_item* work_item;

                if (work_item_index >= work_item_count) {
                    result = SOC_RESULT_INTERNAL_ERROR;
                    return_parallel_result = SOC_TRUE;
                    goto attempted_cleanup;
                }
                work_item = &work_items[work_item_index++];
                work_item->mesh = group.mesh;
                work_item->object_to_world =
                    &group.object_to_world[instance];
                work_item->triangle_begin = triangle_begin;
                work_item->triangle_count = item_triangle_count;
                if (!checked_size_add(
                        source_triangle_count,
                        (size_t)item_triangle_count,
                        &source_triangle_count
                    )) {
                    result = SOC_RESULT_OUT_OF_MEMORY;
                    goto attempted_cleanup;
                }
                triangle_begin += item_triangle_count;
            }
        }
    }
    if (work_item_index != work_item_count) {
        result = SOC_RESULT_INTERNAL_ERROR;
        return_parallel_result = SOC_TRUE;
        goto attempted_cleanup;
    }

    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = soc_rasterizer_initialize(
            &rasterizers[lane],
            context->width,
            context->height,
            level_zero,
            depth_element_count,
            context->kernels
        );
        if (result != SOC_RESULT_OK) {
            if (result != SOC_RESULT_OUT_OF_MEMORY) {
                return_parallel_result = SOC_TRUE;
            }
            goto attempted_cleanup;
        }
        ++initialized_count;
    }

    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = lane == 0u
            ? soc_rasterizer_begin_frame(&rasterizers[lane], frame)
            : soc_rasterizer_begin_frame_no_clear(
                &rasterizers[lane],
                frame
            );
        if (result != SOC_RESULT_OK) {
            if (result != SOC_RESULT_OUT_OF_MEMORY) {
                return_parallel_result = SOC_TRUE;
            }
            goto attempted_cleanup;
        }
    }

    {
        size_t prepared_reserve_count =
            source_triangle_count / (size_t)prepare_lane_count;

        if (source_triangle_count % (size_t)prepare_lane_count != 0u) {
            ++prepared_reserve_count;
        }
        if (!checked_size_add(
                prepared_reserve_count,
                (size_t)triangles_per_work_item,
                &prepared_reserve_count
            )) {
            result = SOC_RESULT_OUT_OF_MEMORY;
            goto attempted_cleanup;
        }
        for (lane = 0u; lane < prepare_lane_count; ++lane) {
            result = soc_raster_prepared_list_reserve(
                &prepared_lists[lane],
                prepared_reserve_count
            );
            if (result != SOC_RESULT_OK) {
                goto attempted_cleanup;
            }
        }
    }

    for (lane = 0u; lane < prepare_lane_count; ++lane) {
        lane_results[lane] = SOC_RESULT_OK;
    }

    prepare_state.rasterizers = rasterizers;
    prepare_state.lane_results = lane_results;
    prepare_state.prepared_lists = prepared_lists;
    prepare_state.work_items = work_items;
    prepare_state.work_item_count = work_item_count;
    prepare_state.tile_column_count = tile_column_count;
    prepare_state.tile_count = tile_count;
    prepare_state.tile_bins = tile_bins;
    prepare_state.tile_reference_arenas = tile_reference_arenas;
    atomic_init(&prepare_state.next_work_item, 0u);

    soc_thread_pool_run_active(
        &context->thread_pool,
        prepare_lane_count,
        parallel_prepare_work_items,
        &prepare_state
    );
    {
        soc_result prepare_result = SOC_RESULT_OK;

        for (lane = 0u; lane < prepare_lane_count; ++lane) {
            if (lane_results[lane] == SOC_RESULT_OK) {
                continue;
            }
            if (lane_results[lane] != SOC_RESULT_OUT_OF_MEMORY) {
                result = lane_results[lane];
                return_parallel_result = SOC_TRUE;
                goto attempted_cleanup;
            }
            prepare_result = SOC_RESULT_OUT_OF_MEMORY;
        }
        if (prepare_result != SOC_RESULT_OK) {
            result = prepare_result;
            goto attempted_cleanup;
        }
    }

    {
        size_t tile_index;

        for (tile_index = 0u; tile_index < tile_count; ++tile_index) {
            size_t tile_reference_count = 0u;
            uint32_t lane_mask = 0u;

            for (lane = 0u; lane < prepare_lane_count; ++lane) {
                const size_t bin_index =
                    (size_t)lane * tile_count + tile_index;
                const size_t lane_reference_count =
                    (size_t)tile_bins[bin_index].count;

                if (lane_reference_count != 0u) {
                    lane_mask |= UINT32_C(1) << lane;
                }
                if (!checked_size_add(
                        tile_reference_count,
                        lane_reference_count,
                        &tile_reference_count
                    )) {
                    result = SOC_RESULT_OUT_OF_MEMORY;
                    goto attempted_cleanup;
                }
            }
            if (tile_reference_count != 0u) {
                soc_parallel_tile_job* tile_job =
                    &nonempty_tiles[nonempty_tile_count++];

                tile_job->tile_index = tile_index;
                tile_job->reference_count = tile_reference_count;
                tile_job->lane_mask = lane_mask;
                if (!checked_size_add(
                        total_reference_count,
                        tile_reference_count,
                        &total_reference_count
                    )) {
                    result = SOC_RESULT_OUT_OF_MEMORY;
                    goto attempted_cleanup;
                }
            }
        }
    }

    if (total_reference_count <=
            SOC_PARALLEL_DIRECT_REFERENCE_LIMIT &&
        nonempty_tile_count <= 1u) {
        return_parallel_result = SOC_TRUE;
        for (lane = 0u; lane < prepare_lane_count; ++lane) {
            result = soc_rasterizer_rasterize_prepared_triangles(
                &rasterizers[lane],
                prepared_lists[lane].data,
                prepared_lists[lane].count
            );
            if (result != SOC_RESULT_OK) {
                goto attempted_cleanup;
            }
        }
    } else {
        size_t hot_reference_threshold =
            total_reference_count /
                (size_t)configured_lane_count;
        size_t nonempty_index;

        if (total_reference_count %
                (size_t)configured_lane_count != 0u) {
            ++hot_reference_threshold;
        }
        if (hot_reference_threshold <
            SOC_PARALLEL_HOT_TILE_MINIMUM_REFERENCES) {
            hot_reference_threshold =
                SOC_PARALLEL_HOT_TILE_MINIMUM_REFERENCES;
        }
        for (nonempty_index = 0u;
             nonempty_index < nonempty_tile_count;
             ++nonempty_index) {
            const size_t tile_reference_count =
                nonempty_tiles[nonempty_index].reference_count;

            if (tile_reference_count > hot_reference_threshold) {
                ++hot_tile_count;
            }
        }

        if (hot_tile_count != 0u) {
            if (!checked_size_multiply(
                    hot_tile_count,
                    sizeof(*hot_tiles),
                    &hot_tile_bytes
                ) ||
                !checked_size_multiply(
                    hot_tile_count,
                    (size_t)configured_lane_count,
                    &scratch_slice_count
                ) ||
                !checked_size_multiply(
                    scratch_slice_count,
                    SOC_PARALLEL_TILE_ELEMENT_COUNT,
                    &scratch_element_count
                ) ||
                !checked_size_multiply(
                    scratch_element_count,
                    sizeof(*hot_tile_scratch),
                    &scratch_bytes
                )) {
                result = SOC_RESULT_OUT_OF_MEMORY;
                goto attempted_cleanup;
            }
            hot_tiles = malloc(hot_tile_bytes);
            hot_tile_scratch = malloc(scratch_bytes);
            if (hot_tiles == NULL || hot_tile_scratch == NULL) {
                result = SOC_RESULT_OUT_OF_MEMORY;
                goto attempted_cleanup;
            }
            context->kernels->clear_f32(
                hot_tile_scratch,
                scratch_element_count,
                0.0f
            );
        }

        {
            size_t hot_index = 0u;

            for (nonempty_index = 0u;
                 nonempty_index < nonempty_tile_count;
                 ++nonempty_index) {
                const soc_parallel_tile_job tile_job =
                    nonempty_tiles[nonempty_index];
                const size_t tile_index = tile_job.tile_index;
                const size_t tile_reference_count =
                    tile_job.reference_count;
                const size_t tile_row =
                    tile_index / tile_column_count;

                if (tile_reference_count > hot_reference_threshold) {
                    hot_tiles[hot_index++] = tile_job;
                    tile_rows[tile_row].has_hot_tile = SOC_TRUE;
                } else {
                    nonempty_tiles[normal_tile_count++] = tile_job;
                    ++tile_rows[tile_row].normal_tile_count;
                }
            }
        }

        {
            size_t tile_row;

            for (tile_row = 0u;
                 tile_row < tile_row_count;
                 ++tile_row) {
                atomic_store_explicit(
                    &tile_rows[tile_row].remaining_normal_tiles,
                    tile_rows[tile_row].normal_tile_count,
                    memory_order_relaxed
                );
                if (tile_rows[tile_row].normal_tile_count == 0u &&
                    tile_rows[tile_row].has_hot_tile != SOC_TRUE) {
                    empty_tile_rows[empty_tile_row_count++] = tile_row;
                }
            }
        }

        tile_state.rasterizers = rasterizers;
        tile_state.prepared_lists = prepared_lists;
        tile_state.tile_bins = tile_bins;
        tile_state.tile_reference_arenas = tile_reference_arenas;
        tile_state.tile_count = tile_count;
        tile_state.prepare_lane_count = prepare_lane_count;
        tile_state.normal_tiles = nonempty_tiles;
        tile_state.normal_tile_count = normal_tile_count;
        tile_state.hot_tiles = hot_tiles;
        tile_state.hot_tile_count = hot_tile_count;
        tile_state.hot_tile_scratch = hot_tile_scratch;
        tile_state.tile_rows = tile_rows;
        tile_state.empty_tile_rows = empty_tile_rows;
        tile_state.empty_tile_row_count = empty_tile_row_count;
        tile_state.tile_column_count = tile_column_count;
        tile_state.width = context->width;
        tile_state.height = context->height;
        tile_state.depth_pyramid = depth_pyramid;
        tile_state.kernels = context->kernels;
        result = soc_hiz_lower_band_count(
            depth_pyramid,
            &tile_state.hiz_band_count
        );
        if (result != SOC_RESULT_OK) {
            return_parallel_result = SOC_TRUE;
            goto attempted_cleanup;
        }
        atomic_init(&tile_state.next_job, 0u);
        atomic_init(&tile_state.next_empty_tile_row, 0u);

        return_parallel_result = SOC_TRUE;
        if (hot_tile_count != 0u) {
            raster_lane_count = configured_lane_count;
        } else if (normal_tile_count <
            (size_t)configured_lane_count) {
            raster_lane_count = (uint32_t)normal_tile_count;
        } else {
            raster_lane_count = configured_lane_count;
        }
        {
            size_t reference_lane_count =
                total_reference_count /
                    SOC_PARALLEL_TILE_REFERENCES_PER_LANE;

            if (total_reference_count %
                    SOC_PARALLEL_TILE_REFERENCES_PER_LANE != 0u) {
                ++reference_lane_count;
            }
            if (reference_lane_count < (size_t)raster_lane_count) {
                raster_lane_count = (uint32_t)reference_lane_count;
            }
        }
        {
            size_t hiz_lane_count = depth_element_count /
                SOC_PARALLEL_FUSED_HIZ_TARGET_ELEMENTS_PER_LANE;

            if (depth_element_count %
                    SOC_PARALLEL_FUSED_HIZ_TARGET_ELEMENTS_PER_LANE !=
                0u) {
                ++hiz_lane_count;
            }
            if (hiz_lane_count >
                (size_t)SOC_PARALLEL_FUSED_HIZ_MAX_LANE_COUNT) {
                hiz_lane_count =
                    (size_t)SOC_PARALLEL_FUSED_HIZ_MAX_LANE_COUNT;
            }
            if (hiz_lane_count > (size_t)configured_lane_count) {
                hiz_lane_count = (size_t)configured_lane_count;
            }
            if (hiz_lane_count > tile_row_count) {
                hiz_lane_count = tile_row_count;
            }
            if (hiz_lane_count > (size_t)raster_lane_count) {
                raster_lane_count = (uint32_t)hiz_lane_count;
            }
        }
        soc_thread_pool_run_active(
            &context->thread_pool,
            raster_lane_count,
            parallel_rasterize_dense_tiles,
            &tile_state
        );
        merge_parallel_hot_tiles(
            level_zero,
            hot_tile_scratch,
            hot_tiles,
            hot_tile_count,
            raster_lane_count,
            tile_column_count,
            context->width,
            context->height,
            context->kernels
        );
        {
            size_t tile_row;

            for (tile_row = 0u;
                 tile_row < tile_row_count;
                 ++tile_row) {
                if (tile_rows[tile_row].has_hot_tile == SOC_TRUE) {
                    build_parallel_tile_row_hiz(
                        &tile_state,
                        tile_row
                    );
                }
            }
        }
        *out_lower_hiz_built = SOC_TRUE;
    }

    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = soc_rasterizer_finish_occluders(&rasterizers[lane]);
        if (result != SOC_RESULT_OK) {
            goto attempted_cleanup;
        }
    }

    *out_clipped_triangle_count = 0u;
    *out_rasterized_triangle_count = 0u;
    for (lane = 0u; lane < prepare_lane_count; ++lane) {
        *out_clipped_triangle_count +=
            rasterizers[lane].clipped_triangle_count;
        *out_rasterized_triangle_count +=
            rasterizers[lane].rasterized_triangle_count;
    }
    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = soc_rasterizer_end_frame(&rasterizers[lane]);
        if (result != SOC_RESULT_OK) {
            goto attempted_cleanup;
        }
    }
    return_parallel_result = SOC_TRUE;

attempted_cleanup:
    cleanup_parallel_rasterizers(rasterizers, initialized_count);
    cleanup_parallel_prepared_lists(
        prepared_lists,
        prepare_lane_count
    );
    free(hot_tile_scratch);
    free(hot_tiles);
    free(empty_tile_rows);
    free(tile_rows);
    free(nonempty_tiles);
    cleanup_parallel_tile_reference_arenas(
        tile_reference_arenas,
        prepare_lane_count
    );
    free(tile_reference_arenas);
    free(tile_bins);
    free(prepared_lists);
    free(lane_results);
    free(rasterizers);
    free(work_items);
    if (return_parallel_result == SOC_TRUE) {
        *out_result = result;
        return SOC_TRUE;
    }
    return SOC_FALSE;
}

/*
 * Masked Quick-Mask variant. Its state transitions are order-sensitive, so
 * preparation and tile replay deliberately use their specialized callbacks.
 */
SOC_PIPELINE_CACHE_ALIGNED
static soc_bool try_rasterize_occluders_tiled_masked(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    soc_hiz* depth_pyramid,
    float* level_zero,
    size_t depth_element_count,
    uint64_t* out_clipped_triangle_count,
    uint64_t* out_rasterized_triangle_count,
    soc_bool* out_lower_hiz_built,
    soc_result* out_result
)
{
    uint64_t work_item_count_u64;
    size_t work_item_count;
    size_t work_item_bytes;
    size_t rasterizer_bytes;
    size_t lane_result_bytes;
    size_t prepared_list_bytes;
    size_t tile_bin_entry_count;
    size_t tile_bin_bytes;
    size_t tile_reference_arena_bytes;
    size_t nonempty_tile_bytes;
    size_t tile_row_state_bytes;
    size_t empty_tile_row_bytes;
    size_t hot_tile_bytes;
    size_t scratch_slice_count;
    size_t scratch_element_count;
    size_t scratch_bytes;
    size_t tile_column_count;
    size_t tile_row_count;
    size_t tile_count;
    size_t total_reference_count = 0u;
    size_t nonempty_tile_count = 0u;
    size_t normal_tile_count = 0u;
    size_t hot_tile_count = 0u;
    size_t empty_tile_row_count = 0u;
    size_t source_triangle_count = 0u;
    const soc_bool masked_backend = SOC_TRUE;
    uint32_t configured_lane_count;
    uint32_t prepare_lane_count;
    uint32_t raster_lane_count = 0u;
    uint32_t lane;
    uint32_t initialized_count = 0u;
    soc_parallel_work_item* work_items = NULL;
    soc_rasterizer* rasterizers = NULL;
    soc_result* lane_results = NULL;
    soc_raster_prepared_list* prepared_lists = NULL;
    soc_parallel_tile_bin* tile_bins = NULL;
    soc_parallel_tile_reference_arena* tile_reference_arenas = NULL;
    soc_parallel_tile_job* nonempty_tiles = NULL;
    soc_parallel_tile_row_state* tile_rows = NULL;
    size_t* empty_tile_rows = NULL;
    soc_parallel_tile_job* hot_tiles = NULL;
    float* hot_tile_scratch = NULL;
    soc_parallel_prepare_state prepare_state;
    soc_parallel_tile_state tile_state;
    soc_result result = SOC_RESULT_OK;
    soc_bool return_parallel_result = SOC_FALSE;
    size_t work_item_index = 0u;
    uint32_t group_index;

    *out_lower_hiz_built = SOC_FALSE;
    if (context->worker_count <= 1u) {
        return SOC_FALSE;
    }
    if (!calculate_parallel_work_item_count(
            desc,
            SOC_PARALLEL_TILED_MASKED_TRIANGLES_PER_WORK_ITEM,
            &work_item_count_u64
        ) ||
        work_item_count_u64 == 0u ||
        work_item_count_u64 > (uint64_t)SIZE_MAX) {
        return SOC_FALSE;
    }
    configured_lane_count = context->worker_count <
            SOC_MASKED_PARALLEL_MAX_LANE_COUNT
        ? context->worker_count
        : SOC_MASKED_PARALLEL_MAX_LANE_COUNT;

    prepare_lane_count = configured_lane_count;
    if (work_item_count_u64 < (uint64_t)prepare_lane_count) {
        prepare_lane_count = (uint32_t)work_item_count_u64;
    }

    work_item_count = (size_t)work_item_count_u64;
    tile_column_count = (size_t)(
        context->width / SOC_RASTER_LOCK_TILE_SIZE
    );
    if (context->width % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++tile_column_count;
    }
    tile_row_count = (size_t)(
        context->height / SOC_RASTER_LOCK_TILE_SIZE
    );
    if (context->height % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++tile_row_count;
    }

    if (!checked_size_multiply(
            work_item_count,
            sizeof(*work_items),
            &work_item_bytes
        ) ||
        !checked_size_multiply(
            (size_t)configured_lane_count,
            sizeof(*rasterizers),
            &rasterizer_bytes
        ) ||
        !checked_size_multiply(
            (size_t)prepare_lane_count,
            sizeof(*lane_results),
            &lane_result_bytes
        ) ||
        !checked_size_multiply(
            (size_t)prepare_lane_count,
            sizeof(*prepared_lists),
            &prepared_list_bytes
        ) ||
        !checked_size_multiply(
            tile_column_count,
            tile_row_count,
            &tile_count
        )) {
        return SOC_FALSE;
    }
    if (tile_count == 0u) {
        *out_result = SOC_RESULT_INTERNAL_ERROR;
        return SOC_TRUE;
    }
    if (!checked_size_multiply(
            (size_t)prepare_lane_count,
            tile_count,
            &tile_bin_entry_count
        ) ||
        !checked_size_multiply(
            tile_bin_entry_count,
            sizeof(*tile_bins),
            &tile_bin_bytes
        ) ||
        !checked_size_multiply(
            (size_t)prepare_lane_count,
            sizeof(*tile_reference_arenas),
            &tile_reference_arena_bytes
        ) ||
        !checked_size_multiply(
            tile_count,
            sizeof(*nonempty_tiles),
            &nonempty_tile_bytes
        ) ||
        !checked_size_multiply(
            tile_row_count,
            sizeof(*tile_rows),
            &tile_row_state_bytes
        ) ||
        !checked_size_multiply(
            tile_row_count,
            sizeof(*empty_tile_rows),
            &empty_tile_row_bytes
        )) {
        return SOC_FALSE;
    }

    work_items = malloc(work_item_bytes);
    rasterizers = calloc(1u, rasterizer_bytes);
    lane_results = malloc(lane_result_bytes);
    prepared_lists = calloc(1u, prepared_list_bytes);
    tile_bins = calloc(1u, tile_bin_bytes);
    tile_reference_arenas = calloc(1u, tile_reference_arena_bytes);
    nonempty_tiles = malloc(nonempty_tile_bytes);
    tile_rows = calloc(1u, tile_row_state_bytes);
    empty_tile_rows = malloc(empty_tile_row_bytes);
    if (work_items == NULL ||
        rasterizers == NULL ||
        lane_results == NULL ||
        prepared_lists == NULL ||
        tile_bins == NULL ||
        tile_reference_arenas == NULL ||
        nonempty_tiles == NULL ||
        tile_rows == NULL ||
        empty_tile_rows == NULL) {
        result = SOC_RESULT_OUT_OF_MEMORY;
        goto attempted_cleanup;
    }
    {
        size_t tile_row;

        for (tile_row = 0u; tile_row < tile_row_count; ++tile_row) {
            atomic_init(
                &tile_rows[tile_row].remaining_normal_tiles,
                0u
            );
        }
    }

    for (group_index = 0u;
         group_index < desc->group_count;
         ++group_index) {
        soc_occluder_group group;
        uint32_t instance;
        uint32_t triangle_count;

        read_group(desc, group_index, &group);
        if (group.instance_count == 0u) {
            continue;
        }
        triangle_count = group.mesh->index_count / 3u;

        for (instance = 0u; instance < group.instance_count; ++instance) {
            uint32_t triangle_begin = 0u;

            while (triangle_begin < triangle_count) {
                const uint32_t remaining =
                    triangle_count - triangle_begin;
                const uint32_t item_triangle_count =
                    remaining <
                        SOC_PARALLEL_TILED_MASKED_TRIANGLES_PER_WORK_ITEM
                        ? remaining
                        : SOC_PARALLEL_TILED_MASKED_TRIANGLES_PER_WORK_ITEM;
                soc_parallel_work_item* work_item;

                if (work_item_index >= work_item_count) {
                    result = SOC_RESULT_INTERNAL_ERROR;
                    return_parallel_result = SOC_TRUE;
                    goto attempted_cleanup;
                }
                work_item = &work_items[work_item_index++];
                work_item->mesh = group.mesh;
                work_item->object_to_world =
                    &group.object_to_world[instance];
                work_item->triangle_begin = triangle_begin;
                work_item->triangle_count = item_triangle_count;
                if (!checked_size_add(
                        source_triangle_count,
                        (size_t)item_triangle_count,
                        &source_triangle_count
                    )) {
                    result = SOC_RESULT_OUT_OF_MEMORY;
                    goto attempted_cleanup;
                }
                triangle_begin += item_triangle_count;
            }
        }
    }
    if (work_item_index != work_item_count) {
        result = SOC_RESULT_INTERNAL_ERROR;
        return_parallel_result = SOC_TRUE;
        goto attempted_cleanup;
    }

    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = masked_backend == SOC_TRUE
            ? soc_rasterizer_initialize_masked(
                &rasterizers[lane],
                context->width,
                context->height,
                level_zero,
                depth_pyramid->working_depth,
                depth_pyramid->layer_masks,
                depth_pyramid->levels[0].width,
                depth_pyramid->levels[0].height,
                context->kernels
            )
            : soc_rasterizer_initialize(
                &rasterizers[lane],
                context->width,
                context->height,
                level_zero,
                depth_element_count,
                context->kernels
            );
        if (result != SOC_RESULT_OK) {
            if (result != SOC_RESULT_OUT_OF_MEMORY) {
                return_parallel_result = SOC_TRUE;
            }
            goto attempted_cleanup;
        }
        ++initialized_count;
    }

    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = lane == 0u
            ? soc_rasterizer_begin_frame(&rasterizers[lane], frame)
            : soc_rasterizer_begin_frame_no_clear(
                &rasterizers[lane],
                frame
            );
        if (result != SOC_RESULT_OK) {
            if (result != SOC_RESULT_OUT_OF_MEMORY) {
                return_parallel_result = SOC_TRUE;
            }
            goto attempted_cleanup;
        }
    }
    if (masked_backend == SOC_TRUE) {
        for (lane = 0u; lane < configured_lane_count; ++lane) {
            rasterizers[lane].masked_reference_count = SIZE_MAX;
        }
    }

    {
        size_t prepared_reserve_count =
            source_triangle_count / (size_t)prepare_lane_count;

        if (source_triangle_count % (size_t)prepare_lane_count != 0u) {
            ++prepared_reserve_count;
        }
        if (!checked_size_add(
                prepared_reserve_count,
                (size_t)SOC_PARALLEL_TILED_MASKED_TRIANGLES_PER_WORK_ITEM,
                &prepared_reserve_count
            )) {
            result = SOC_RESULT_OUT_OF_MEMORY;
            goto attempted_cleanup;
        }
        for (lane = 0u; lane < prepare_lane_count; ++lane) {
            result = soc_raster_prepared_list_reserve(
                &prepared_lists[lane],
                prepared_reserve_count
            );
            if (result != SOC_RESULT_OK) {
                goto attempted_cleanup;
            }
        }
    }

    for (lane = 0u; lane < prepare_lane_count; ++lane) {
        lane_results[lane] = SOC_RESULT_OK;
    }

    prepare_state.rasterizers = rasterizers;
    prepare_state.lane_results = lane_results;
    prepare_state.prepared_lists = prepared_lists;
    prepare_state.work_items = work_items;
    prepare_state.work_item_count = work_item_count;
    prepare_state.tile_column_count = tile_column_count;
    prepare_state.tile_count = tile_count;
    prepare_state.tile_bins = tile_bins;
    prepare_state.tile_reference_arenas = tile_reference_arenas;
    atomic_init(&prepare_state.next_work_item, 0u);

    soc_thread_pool_run_active(
        &context->thread_pool,
        prepare_lane_count,
        masked_backend == SOC_TRUE
            ? parallel_prepare_ordered_work_items
            : parallel_prepare_work_items,
        &prepare_state
    );
    {
        soc_result prepare_result = SOC_RESULT_OK;

        for (lane = 0u; lane < prepare_lane_count; ++lane) {
            if (lane_results[lane] == SOC_RESULT_OK) {
                continue;
            }
            if (lane_results[lane] != SOC_RESULT_OUT_OF_MEMORY) {
                result = lane_results[lane];
                return_parallel_result = SOC_TRUE;
                goto attempted_cleanup;
            }
            prepare_result = SOC_RESULT_OUT_OF_MEMORY;
        }
        if (prepare_result != SOC_RESULT_OK) {
            result = prepare_result;
            goto attempted_cleanup;
        }
    }

    {
        size_t tile_index;

        for (tile_index = 0u; tile_index < tile_count; ++tile_index) {
            size_t tile_reference_count = 0u;
            uint32_t lane_mask = 0u;

            for (lane = 0u; lane < prepare_lane_count; ++lane) {
                const size_t bin_index =
                    (size_t)lane * tile_count + tile_index;
                const size_t lane_reference_count =
                    (size_t)tile_bins[bin_index].count;

                if (lane_reference_count != 0u) {
                    lane_mask |= UINT32_C(1) << lane;
                }
                if (!checked_size_add(
                        tile_reference_count,
                        lane_reference_count,
                        &tile_reference_count
                    )) {
                    result = SOC_RESULT_OUT_OF_MEMORY;
                    goto attempted_cleanup;
                }
            }
            if (tile_reference_count != 0u) {
                soc_parallel_tile_job* tile_job =
                    &nonempty_tiles[nonempty_tile_count++];

                tile_job->tile_index = tile_index;
                tile_job->reference_count = tile_reference_count;
                tile_job->lane_mask = lane_mask;
                if (!checked_size_add(
                        total_reference_count,
                        tile_reference_count,
                        &total_reference_count
                    )) {
                    result = SOC_RESULT_OUT_OF_MEMORY;
                    goto attempted_cleanup;
                }
            }
        }
    }

    if (total_reference_count <=
            SOC_PARALLEL_DIRECT_REFERENCE_LIMIT &&
        nonempty_tile_count <= 1u) {
        return_parallel_result = SOC_TRUE;
        for (lane = 0u; lane < prepare_lane_count; ++lane) {
            result = soc_rasterizer_rasterize_prepared_triangles(
                &rasterizers[lane],
                prepared_lists[lane].data,
                prepared_lists[lane].count
            );
            if (result != SOC_RESULT_OK) {
                goto attempted_cleanup;
            }
        }
    } else {
        size_t hot_reference_threshold =
            total_reference_count /
                (size_t)configured_lane_count;
        size_t nonempty_index;

        if (total_reference_count %
                (size_t)configured_lane_count != 0u) {
            ++hot_reference_threshold;
        }
        if (hot_reference_threshold <
            SOC_PARALLEL_HOT_TILE_MINIMUM_REFERENCES) {
            hot_reference_threshold =
                SOC_PARALLEL_HOT_TILE_MINIMUM_REFERENCES;
        }
        if (masked_backend != SOC_TRUE) {
            for (nonempty_index = 0u;
                 nonempty_index < nonempty_tile_count;
                 ++nonempty_index) {
                const size_t tile_reference_count =
                    nonempty_tiles[nonempty_index].reference_count;

                if (tile_reference_count > hot_reference_threshold) {
                    ++hot_tile_count;
                }
            }
        }

        if (hot_tile_count != 0u) {
            if (!checked_size_multiply(
                    hot_tile_count,
                    sizeof(*hot_tiles),
                    &hot_tile_bytes
                ) ||
                !checked_size_multiply(
                    hot_tile_count,
                    (size_t)configured_lane_count,
                    &scratch_slice_count
                ) ||
                !checked_size_multiply(
                    scratch_slice_count,
                    SOC_PARALLEL_TILE_ELEMENT_COUNT,
                    &scratch_element_count
                ) ||
                !checked_size_multiply(
                    scratch_element_count,
                    sizeof(*hot_tile_scratch),
                    &scratch_bytes
                )) {
                result = SOC_RESULT_OUT_OF_MEMORY;
                goto attempted_cleanup;
            }
            hot_tiles = malloc(hot_tile_bytes);
            hot_tile_scratch = malloc(scratch_bytes);
            if (hot_tiles == NULL || hot_tile_scratch == NULL) {
                result = SOC_RESULT_OUT_OF_MEMORY;
                goto attempted_cleanup;
            }
            context->kernels->clear_f32(
                hot_tile_scratch,
                scratch_element_count,
                0.0f
            );
        }

        {
            size_t hot_index = 0u;

            for (nonempty_index = 0u;
                 nonempty_index < nonempty_tile_count;
                 ++nonempty_index) {
                const soc_parallel_tile_job tile_job =
                    nonempty_tiles[nonempty_index];
                const size_t tile_index = tile_job.tile_index;
                const size_t tile_reference_count =
                    tile_job.reference_count;
                const size_t tile_row =
                    tile_index / tile_column_count;

                if (masked_backend != SOC_TRUE &&
                    tile_reference_count > hot_reference_threshold) {
                    hot_tiles[hot_index++] = tile_job;
                    tile_rows[tile_row].has_hot_tile = SOC_TRUE;
                } else {
                    nonempty_tiles[normal_tile_count++] = tile_job;
                    ++tile_rows[tile_row].normal_tile_count;
                }
            }
        }

        {
            size_t tile_row;

            for (tile_row = 0u;
                 tile_row < tile_row_count;
                 ++tile_row) {
                atomic_store_explicit(
                    &tile_rows[tile_row].remaining_normal_tiles,
                    tile_rows[tile_row].normal_tile_count,
                    memory_order_relaxed
                );
                if (tile_rows[tile_row].normal_tile_count == 0u &&
                    tile_rows[tile_row].has_hot_tile != SOC_TRUE) {
                    empty_tile_rows[empty_tile_row_count++] = tile_row;
                }
            }
        }

        tile_state.rasterizers = rasterizers;
        tile_state.prepared_lists = prepared_lists;
        tile_state.tile_bins = tile_bins;
        tile_state.tile_reference_arenas = tile_reference_arenas;
        tile_state.tile_count = tile_count;
        tile_state.prepare_lane_count = prepare_lane_count;
        tile_state.normal_tiles = nonempty_tiles;
        tile_state.normal_tile_count = normal_tile_count;
        tile_state.hot_tiles = hot_tiles;
        tile_state.hot_tile_count = hot_tile_count;
        tile_state.hot_tile_scratch = hot_tile_scratch;
        tile_state.tile_rows = tile_rows;
        tile_state.empty_tile_rows = empty_tile_rows;
        tile_state.empty_tile_row_count = empty_tile_row_count;
        tile_state.tile_column_count = tile_column_count;
        tile_state.width = context->width;
        tile_state.height = context->height;
        tile_state.depth_pyramid = depth_pyramid;
        tile_state.kernels = context->kernels;
        tile_state.hiz_band_count = 0u;
        if (masked_backend != SOC_TRUE) {
            result = soc_hiz_lower_band_count(
                depth_pyramid,
                &tile_state.hiz_band_count
            );
            if (result != SOC_RESULT_OK) {
                return_parallel_result = SOC_TRUE;
                goto attempted_cleanup;
            }
        }
        atomic_init(&tile_state.next_job, 0u);
        atomic_init(&tile_state.next_empty_tile_row, 0u);

        return_parallel_result = SOC_TRUE;
        if (hot_tile_count != 0u) {
            raster_lane_count = configured_lane_count;
        } else if (normal_tile_count <
            (size_t)configured_lane_count) {
            raster_lane_count = (uint32_t)normal_tile_count;
        } else {
            raster_lane_count = configured_lane_count;
        }
        {
            size_t reference_lane_count =
                total_reference_count /
                    SOC_PARALLEL_TILE_REFERENCES_PER_LANE;

            if (total_reference_count %
                    SOC_PARALLEL_TILE_REFERENCES_PER_LANE != 0u) {
                ++reference_lane_count;
            }
            if (reference_lane_count < (size_t)raster_lane_count) {
                raster_lane_count = (uint32_t)reference_lane_count;
            }
        }
        if (masked_backend != SOC_TRUE) {
            size_t hiz_lane_count = depth_element_count /
                SOC_PARALLEL_FUSED_HIZ_TARGET_ELEMENTS_PER_LANE;

            if (depth_element_count %
                    SOC_PARALLEL_FUSED_HIZ_TARGET_ELEMENTS_PER_LANE !=
                0u) {
                ++hiz_lane_count;
            }
            if (hiz_lane_count >
                (size_t)SOC_PARALLEL_FUSED_HIZ_MAX_LANE_COUNT) {
                hiz_lane_count =
                    (size_t)SOC_PARALLEL_FUSED_HIZ_MAX_LANE_COUNT;
            }
            if (hiz_lane_count > (size_t)configured_lane_count) {
                hiz_lane_count = (size_t)configured_lane_count;
            }
            if (hiz_lane_count > tile_row_count) {
                hiz_lane_count = tile_row_count;
            }
            if (hiz_lane_count > (size_t)raster_lane_count) {
                raster_lane_count = (uint32_t)hiz_lane_count;
            }
        }
        soc_thread_pool_run_active(
            &context->thread_pool,
            raster_lane_count,
            masked_backend == SOC_TRUE
                ? parallel_rasterize_masked_tiles
                : parallel_rasterize_dense_tiles,
            &tile_state
        );
        if (masked_backend != SOC_TRUE) {
            size_t tile_row;

            merge_parallel_hot_tiles(
                level_zero,
                hot_tile_scratch,
                hot_tiles,
                hot_tile_count,
                raster_lane_count,
                tile_column_count,
                context->width,
                context->height,
                context->kernels
            );
            for (tile_row = 0u;
                 tile_row < tile_row_count;
                 ++tile_row) {
                if (tile_rows[tile_row].has_hot_tile == SOC_TRUE) {
                    build_parallel_tile_row_hiz(
                        &tile_state,
                        tile_row
                    );
                }
            }
            *out_lower_hiz_built = SOC_TRUE;
        }
    }

    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = soc_rasterizer_finish_occluders(&rasterizers[lane]);
        if (result != SOC_RESULT_OK) {
            goto attempted_cleanup;
        }
    }

    *out_clipped_triangle_count = 0u;
    *out_rasterized_triangle_count = 0u;
    for (lane = 0u; lane < prepare_lane_count; ++lane) {
        *out_clipped_triangle_count +=
            rasterizers[lane].clipped_triangle_count;
        *out_rasterized_triangle_count +=
            rasterizers[lane].rasterized_triangle_count;
    }
    for (lane = 0u; lane < configured_lane_count; ++lane) {
        result = soc_rasterizer_end_frame(&rasterizers[lane]);
        if (result != SOC_RESULT_OK) {
            goto attempted_cleanup;
        }
    }
    return_parallel_result = SOC_TRUE;

attempted_cleanup:
    cleanup_parallel_rasterizers(rasterizers, initialized_count);
    cleanup_parallel_prepared_lists(
        prepared_lists,
        prepare_lane_count
    );
    free(hot_tile_scratch);
    free(hot_tiles);
    free(empty_tile_rows);
    free(tile_rows);
    free(nonempty_tiles);
    cleanup_parallel_tile_reference_arenas(
        tile_reference_arenas,
        prepare_lane_count
    );
    free(tile_reference_arenas);
    free(tile_bins);
    free(prepared_lists);
    free(lane_results);
    free(rasterizers);
    free(work_items);
    if (return_parallel_result == SOC_TRUE) {
        *out_result = result;
        return SOC_TRUE;
    }
    return SOC_FALSE;
}

/*
 * Immediate backend: every active lane rasterizes directly into a private
 * full-resolution Level 0 image, then the images are reduced in parallel.
 * It deliberately does not create or consume tiled prepared records.
 */
static soc_bool try_rasterize_occluders_private(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    soc_hiz* depth_pyramid,
    float* level_zero,
    size_t depth_element_count,
    uint64_t* out_clipped_triangle_count,
    uint64_t* out_rasterized_triangle_count,
    soc_bool* out_lower_hiz_built,
    soc_result* out_result
)
{
    uint64_t work_item_count_u64;
    size_t work_item_count;
    size_t work_item_bytes;
    size_t rasterizer_bytes;
    size_t lane_result_bytes;
    size_t depth_buffer_bytes;
    size_t scratch_depth_bytes;
    size_t scratch_lane_capacity;
    uint32_t desired_lane_count;
    uint32_t active_lane_count;
    uint32_t lane;
    uint32_t initialized_count = 0u;
    soc_parallel_work_item* work_items = NULL;
    soc_rasterizer* rasterizers = NULL;
    soc_result* lane_results = NULL;
    float* scratch_depth = NULL;
    soc_private_raster_state raster_state;
    soc_private_merge_state merge_state;
    soc_result result = SOC_RESULT_OK;
    soc_bool return_private_result = SOC_FALSE;
    size_t work_item_index = 0u;
    uint32_t group_index;

    *out_lower_hiz_built = SOC_FALSE;
    if (context->worker_count <= 1u ||
        !calculate_parallel_work_item_count(
            desc,
            SOC_PARALLEL_PRIVATE_TRIANGLES_PER_WORK_ITEM,
            &work_item_count_u64
        ) ||
        work_item_count_u64 < 2u ||
        work_item_count_u64 > (uint64_t)SIZE_MAX ||
        !checked_size_multiply(
            depth_element_count,
            sizeof(*scratch_depth),
            &depth_buffer_bytes
        ) ||
        depth_buffer_bytes == 0u) {
        return SOC_FALSE;
    }

    desired_lane_count = context->worker_count;
    if (work_item_count_u64 < (uint64_t)desired_lane_count) {
        desired_lane_count = (uint32_t)work_item_count_u64;
    }
    if (desired_lane_count > SOC_PARALLEL_PRIVATE_MAX_LANE_COUNT) {
        desired_lane_count = SOC_PARALLEL_PRIVATE_MAX_LANE_COUNT;
    }
    scratch_lane_capacity =
        SOC_PARALLEL_DEPTH_SCRATCH_BUDGET_BYTES / depth_buffer_bytes;
    active_lane_count = desired_lane_count;
    if (scratch_lane_capacity < (size_t)(active_lane_count - 1u)) {
        active_lane_count = (uint32_t)scratch_lane_capacity + 1u;
    }
    if (active_lane_count < 2u) {
        return SOC_FALSE;
    }

    work_item_count = (size_t)work_item_count_u64;
    if (!checked_size_multiply(
            work_item_count,
            sizeof(*work_items),
            &work_item_bytes
        ) ||
        !checked_size_multiply(
            (size_t)active_lane_count,
            sizeof(*rasterizers),
            &rasterizer_bytes
        ) ||
        !checked_size_multiply(
            (size_t)active_lane_count,
            sizeof(*lane_results),
            &lane_result_bytes
        ) ||
        !checked_size_multiply(
            (size_t)(active_lane_count - 1u),
            depth_buffer_bytes,
            &scratch_depth_bytes
        )) {
        return SOC_FALSE;
    }

    work_items = malloc(work_item_bytes);
    rasterizers = calloc(1u, rasterizer_bytes);
    lane_results = malloc(lane_result_bytes);
    scratch_depth = malloc(scratch_depth_bytes);
    if (work_items == NULL ||
        rasterizers == NULL ||
        lane_results == NULL ||
        scratch_depth == NULL) {
        result = SOC_RESULT_OUT_OF_MEMORY;
        goto private_cleanup;
    }

    for (group_index = 0u;
         group_index < desc->group_count;
         ++group_index) {
        soc_occluder_group group;
        uint32_t instance;
        uint32_t triangle_count;

        read_group(desc, group_index, &group);
        if (group.instance_count == 0u) {
            continue;
        }
        triangle_count = group.mesh->index_count / 3u;

        for (instance = 0u; instance < group.instance_count; ++instance) {
            uint32_t triangle_begin = 0u;

            while (triangle_begin < triangle_count) {
                const uint32_t remaining =
                    triangle_count - triangle_begin;
                const uint32_t item_triangle_count =
                    remaining <
                        SOC_PARALLEL_PRIVATE_TRIANGLES_PER_WORK_ITEM
                        ? remaining
                        : SOC_PARALLEL_PRIVATE_TRIANGLES_PER_WORK_ITEM;
                soc_parallel_work_item* work_item;

                if (work_item_index >= work_item_count) {
                    result = SOC_RESULT_INTERNAL_ERROR;
                    return_private_result = SOC_TRUE;
                    goto private_cleanup;
                }
                work_item = &work_items[work_item_index++];
                work_item->mesh = group.mesh;
                work_item->object_to_world =
                    &group.object_to_world[instance];
                work_item->triangle_begin = triangle_begin;
                work_item->triangle_count = item_triangle_count;
                triangle_begin += item_triangle_count;
            }
        }
    }
    if (work_item_index != work_item_count) {
        result = SOC_RESULT_INTERNAL_ERROR;
        return_private_result = SOC_TRUE;
        goto private_cleanup;
    }

    for (lane = 0u; lane < active_lane_count; ++lane) {
        float* lane_depth = lane == 0u
            ? level_zero
            : scratch_depth +
                (size_t)(lane - 1u) * depth_element_count;

        result = soc_rasterizer_initialize(
            &rasterizers[lane],
            context->width,
            context->height,
            lane_depth,
            depth_element_count,
            context->kernels
        );
        if (result != SOC_RESULT_OK) {
            if (result != SOC_RESULT_OUT_OF_MEMORY) {
                return_private_result = SOC_TRUE;
            }
            goto private_cleanup;
        }
        ++initialized_count;
        lane_results[lane] = SOC_RESULT_OK;
    }

    raster_state.frame = frame;
    raster_state.rasterizers = rasterizers;
    raster_state.lane_results = lane_results;
    raster_state.work_items = work_items;
    raster_state.work_item_count = work_item_count;
    atomic_init(&raster_state.next_work_item, 0u);

    merge_state.level_zero = level_zero;
    merge_state.scratch_depth = scratch_depth;
    merge_state.depth_element_count = depth_element_count;
    merge_state.lane_count = active_lane_count;
    merge_state.kernels = context->kernels;
    merge_state.depth_pyramid = depth_pyramid;
    result = soc_hiz_lower_band_count(
        depth_pyramid,
        &merge_state.hiz_band_count
    );
    if (result != SOC_RESULT_OK) {
        return_private_result = SOC_TRUE;
        goto private_cleanup;
    }
    atomic_init(&merge_state.next_hiz_band, 0u);

    return_private_result = SOC_TRUE;
    soc_thread_pool_run_active(
        &context->thread_pool,
        active_lane_count,
        private_begin_and_rasterize,
        &raster_state
    );
    for (lane = 0u; lane < active_lane_count; ++lane) {
        if (lane_results[lane] != SOC_RESULT_OK) {
            result = lane_results[lane];
            goto private_cleanup;
        }
    }
    soc_thread_pool_run_active(
        &context->thread_pool,
        active_lane_count,
        private_merge_depth,
        &merge_state
    );
    *out_lower_hiz_built = SOC_TRUE;

    *out_clipped_triangle_count = 0u;
    *out_rasterized_triangle_count = 0u;
    for (lane = 0u; lane < active_lane_count; ++lane) {
        *out_clipped_triangle_count +=
            rasterizers[lane].clipped_triangle_count;
        *out_rasterized_triangle_count +=
            rasterizers[lane].rasterized_triangle_count;
        result = soc_rasterizer_end_frame(&rasterizers[lane]);
        if (result != SOC_RESULT_OK) {
            goto private_cleanup;
        }
    }

private_cleanup:
    cleanup_parallel_rasterizers(rasterizers, initialized_count);
    free(scratch_depth);
    free(lane_results);
    free(rasterizers);
    free(work_items);
    if (return_private_result == SOC_TRUE) {
        *out_result = result;
        return SOC_TRUE;
    }
    return SOC_FALSE;
}

#if SOC_EXPERIMENT_FORCE_PARALLEL_BACKEND == 0
/*
 * Samples setup density without clearing or rasterizing Level 0.  Global
 * triangle space is split by quotient/remainder instead of multiplying the
 * total by a sample index, and group ranges are located by subtraction, so
 * the mapping remains valid even when the input count approaches UINT64_MAX.
 * Any failure only makes the selector hint unavailable.
 */
static soc_bool try_sample_prepared_density(
    const soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    uint64_t input_triangle_count,
    float* level_zero,
    size_t depth_element_count,
    size_t* out_source_triangle_count,
    size_t* out_prepared_record_count
)
{
    soc_rasterizer rasterizer;
    soc_raster_prepared_list prepared = {0};
    const uint64_t segment_count =
        (uint64_t)SOC_PARALLEL_SELECTOR_SAMPLE_SEGMENTS;
    const uint64_t triangles_per_segment =
        input_triangle_count / segment_count;
    const uint64_t extra_triangle_count =
        input_triangle_count % segment_count;
    size_t sampled_source_count = 0u;
    soc_bool hint_available = SOC_FALSE;
    uint32_t sample_index;
    soc_result result;

    *out_source_triangle_count = 0u;
    *out_prepared_record_count = 0u;
    if (input_triangle_count == 0u) {
        return SOC_FALSE;
    }

    /*
     * The selector only runs transform, clipping and setup.  Construct the
     * preparation view directly instead of allocating and clearing a
     * framebuffer-sized early-Z sidecar that can never be consumed here.
     */
    memset(&rasterizer, 0, sizeof(rasterizer));
    rasterizer.width = context->width;
    rasterizer.height = context->height;
    rasterizer.depth_element_count = depth_element_count;
    rasterizer.depth = level_zero;
    rasterizer.kernels = context->kernels;
    rasterizer.initialized = SOC_TRUE;
    rasterizer.frame_active = SOC_TRUE;
    rasterizer.frame = *frame;

    for (sample_index = 0u;
         sample_index < SOC_PARALLEL_SELECTOR_SAMPLE_SEGMENTS;
         ++sample_index) {
        const uint64_t lane = (uint64_t)sample_index;
        const uint64_t segment_length = triangles_per_segment +
            (lane < extra_triangle_count ? UINT64_C(1) : UINT64_C(0));
        const uint64_t extra_before_segment =
            lane < extra_triangle_count ? lane : extra_triangle_count;
        const uint64_t segment_begin =
            lane * triangles_per_segment + extra_before_segment;
        uint64_t remaining_triangle;
        uint32_t group_index;
        soc_bool sample_found = SOC_FALSE;

        if (segment_length == 0u) {
            continue;
        }
        remaining_triangle =
            segment_begin + segment_length / UINT64_C(2);

        for (group_index = 0u;
             group_index < desc->group_count;
             ++group_index) {
            soc_occluder_group group;
            uint64_t group_triangle_count;
            uint32_t mesh_triangle_count;
            uint32_t instance_index;
            uint32_t center_triangle;
            uint32_t sample_triangle_count;
            uint32_t triangle_begin;
            size_t next_sampled_source_count;

            read_group(desc, group_index, &group);
            if (group.instance_count == 0u) {
                continue;
            }
            mesh_triangle_count = group.mesh->index_count / 3u;
            if (mesh_triangle_count == 0u) {
                continue;
            }
            group_triangle_count =
                (uint64_t)mesh_triangle_count * group.instance_count;
            if (remaining_triangle >= group_triangle_count) {
                remaining_triangle -= group_triangle_count;
                continue;
            }

            instance_index = (uint32_t)(
                remaining_triangle / (uint64_t)mesh_triangle_count
            );
            center_triangle = (uint32_t)(
                remaining_triangle % (uint64_t)mesh_triangle_count
            );
            sample_triangle_count = mesh_triangle_count <
                    SOC_PARALLEL_SELECTOR_SAMPLE_TRIANGLES
                ? mesh_triangle_count
                : SOC_PARALLEL_SELECTOR_SAMPLE_TRIANGLES;
            triangle_begin = center_triangle >=
                    sample_triangle_count / 2u
                ? center_triangle - sample_triangle_count / 2u
                : 0u;
            if (triangle_begin >
                mesh_triangle_count - sample_triangle_count) {
                triangle_begin =
                    mesh_triangle_count - sample_triangle_count;
            }

            result = soc_rasterizer_prepare_occluder_triangles(
                &rasterizer,
                group.mesh,
                &group.object_to_world[instance_index],
                triangle_begin,
                sample_triangle_count,
                &prepared
            );
            if (result != SOC_RESULT_OK ||
                !checked_size_add(
                    sampled_source_count,
                    (size_t)sample_triangle_count,
                    &next_sampled_source_count
                )) {
                goto sample_cleanup;
            }
            sampled_source_count = next_sampled_source_count;
            sample_found = SOC_TRUE;
            break;
        }
        if (sample_found != SOC_TRUE) {
            goto sample_cleanup;
        }
    }

    if (sampled_source_count != 0u) {
        *out_source_triangle_count = sampled_source_count;
        *out_prepared_record_count = prepared.count;
        hint_available = SOC_TRUE;
    }

sample_cleanup:
    soc_raster_prepared_list_shutdown(&prepared);
    return hint_available;
}
#endif

static soc_bool try_rasterize_occluders_parallel(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    uint64_t input_triangle_count,
    soc_hiz* depth_pyramid,
    float* level_zero,
    size_t depth_element_count,
    uint64_t* out_clipped_triangle_count,
    uint64_t* out_rasterized_triangle_count,
    soc_bool* out_lower_hiz_built,
    soc_result* out_result
)
{
    uint64_t work_item_count_u64;
    size_t depth_buffer_bytes;
    size_t score_triangle_count;
    size_t tiled_score;
    size_t private_cost;
    size_t scratch_lane_capacity;
    uint32_t desired_lane_count;
    uint32_t private_lane_count;
    soc_bool private_eligible;
    soc_bool prefer_private;

    *out_lower_hiz_built = SOC_FALSE;
    if (context->worker_count <= 1u ||
        !calculate_parallel_work_item_count(
            desc,
            SOC_PARALLEL_PRIVATE_TRIANGLES_PER_WORK_ITEM,
            &work_item_count_u64
        ) ||
        work_item_count_u64 == 0u ||
        work_item_count_u64 > (uint64_t)SIZE_MAX) {
        return SOC_FALSE;
    }

    desired_lane_count = context->worker_count;
    if (work_item_count_u64 < (uint64_t)desired_lane_count) {
        desired_lane_count = (uint32_t)work_item_count_u64;
    }
    if (desired_lane_count > SOC_PARALLEL_PRIVATE_MAX_LANE_COUNT) {
        desired_lane_count = SOC_PARALLEL_PRIVATE_MAX_LANE_COUNT;
    }
    private_eligible = checked_size_multiply(
        depth_element_count,
        sizeof(float),
        &depth_buffer_bytes
    );
    if (private_eligible == SOC_TRUE && depth_buffer_bytes != 0u &&
        desired_lane_count >= 2u) {
        scratch_lane_capacity =
            SOC_PARALLEL_DEPTH_SCRATCH_BUDGET_BYTES /
            depth_buffer_bytes;
        private_lane_count = desired_lane_count;
        if (scratch_lane_capacity <
            (size_t)(private_lane_count - 1u)) {
            private_lane_count =
                (uint32_t)scratch_lane_capacity + 1u;
        }
        private_eligible = private_lane_count >= 2u
            ? SOC_TRUE
            : SOC_FALSE;
    } else {
        private_eligible = SOC_FALSE;
    }

    score_triangle_count = input_triangle_count > (uint64_t)SIZE_MAX
        ? SIZE_MAX
        : (size_t)input_triangle_count;
    tiled_score = saturating_size_multiply(
        SOC_PARALLEL_TILED_TRAFFIC_FACTOR,
        score_triangle_count
    );
    tiled_score = saturating_size_multiply(
        tiled_score,
        sizeof(soc_raster_prepared_triangle)
    );
    private_cost = private_eligible == SOC_TRUE
        ? saturating_size_multiply(
            (size_t)(desired_lane_count - 1u),
            depth_buffer_bytes
        )
        : SIZE_MAX;

    /*
     * Prepared tiling touches each record several times (count, fill and
     * replay).  The factor above is intentionally named and tunable; compare
     * against the desired private-lane footprint even when the 256 MiB cap
     * later reduces the number of lanes that can actually be allocated.
     */
    prefer_private = private_eligible == SOC_TRUE &&
        tiled_score >= private_cost
        ? SOC_TRUE
        : SOC_FALSE;
    if (work_item_count_u64 <
        (uint64_t)desired_lane_count *
            SOC_PARALLEL_PRIVATE_MIN_WORK_ITEMS_PER_LANE) {
        prefer_private = SOC_FALSE;
    }

#if SOC_EXPERIMENT_FORCE_PARALLEL_BACKEND == 1
    prefer_private = SOC_FALSE;
#elif SOC_EXPERIMENT_FORCE_PARALLEL_BACKEND == 2
    if (private_eligible == SOC_TRUE) {
        prefer_private = SOC_TRUE;
    }
#elif SOC_EXPERIMENT_FORCE_PARALLEL_BACKEND == 0
    if (prefer_private == SOC_TRUE) {
        size_t sampled_source_count;
        size_t sampled_prepared_count;

        if (try_sample_prepared_density(
                context,
                desc,
                frame,
                input_triangle_count,
                level_zero,
                depth_element_count,
                &sampled_source_count,
                &sampled_prepared_count
            ) == SOC_TRUE) {
            const size_t adjusted_score_numerator =
                saturating_size_multiply(
                    tiled_score,
                    sampled_prepared_count
                );
            const size_t private_cost_numerator =
                saturating_size_multiply(
                    private_cost,
                    sampled_source_count
                );

            /*
             * Compare tiled_score * prepared/source with private_cost by
             * cross multiplication.  Saturation is conservative: ambiguous
             * overflow keeps the original private hint instead of changing
             * backend choice from incomplete sampling evidence.
             */
            if (adjusted_score_numerator < private_cost_numerator) {
                prefer_private = SOC_FALSE;
            }
        }
    }
#endif

    if (prefer_private == SOC_TRUE) {
        if (try_rasterize_occluders_private(
                context,
                desc,
                frame,
                depth_pyramid,
                level_zero,
                depth_element_count,
                out_clipped_triangle_count,
                out_rasterized_triangle_count,
                out_lower_hiz_built,
                out_result
            )) {
            return SOC_TRUE;
        }
        return try_rasterize_occluders_tiled_dense(
            context,
            desc,
            frame,
            depth_pyramid,
            level_zero,
            depth_element_count,
            out_clipped_triangle_count,
            out_rasterized_triangle_count,
            out_lower_hiz_built,
            out_result
        );
    }

    if (try_rasterize_occluders_tiled_dense(
            context,
            desc,
            frame,
            depth_pyramid,
            level_zero,
            depth_element_count,
            out_clipped_triangle_count,
            out_rasterized_triangle_count,
            out_lower_hiz_built,
            out_result
        )) {
        return SOC_TRUE;
    }
    if (private_eligible == SOC_TRUE) {
        return try_rasterize_occluders_private(
            context,
            desc,
            frame,
            depth_pyramid,
            level_zero,
            depth_element_count,
            out_clipped_triangle_count,
            out_rasterized_triangle_count,
            out_lower_hiz_built,
            out_result
        );
    }
    return SOC_FALSE;
}

static soc_result build_dense_validated(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    uint64_t input_triangle_count,
    soc_snapshot** out_snapshot
)
{
    soc_snapshot* snapshot;
    uint64_t clipped_triangle_count = 0u;
    uint64_t rasterized_triangle_count = 0u;
    soc_bool lower_hiz_built = SOC_FALSE;
    soc_result result;

    snapshot = calloc(1u, sizeof(*snapshot));
    if (snapshot == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    snapshot->kernels = context->kernels;

    result = soc_hiz_initialize(
        &snapshot->depth_pyramid,
        context->width,
        context->height
    );
    if (result != SOC_RESULT_OK) {
        soc_snapshot_destroy_internal(snapshot);
        return result;
    }

    if (!try_rasterize_occluders_parallel(
            context,
            desc,
            frame,
            input_triangle_count,
            &snapshot->depth_pyramid,
            soc_hiz_level_data(&snapshot->depth_pyramid, 0u),
            snapshot->depth_pyramid.levels[0].element_count,
            &clipped_triangle_count,
            &rasterized_triangle_count,
            &lower_hiz_built,
            &result
        )) {
        result = rasterize_occluders_serial(
            context,
            desc,
            frame,
            soc_hiz_level_data(&snapshot->depth_pyramid, 0u),
            snapshot->depth_pyramid.levels[0].element_count,
            &clipped_triangle_count,
            &rasterized_triangle_count
        );
    }
    if (result != SOC_RESULT_OK) {
        goto fail;
    }
    result = lower_hiz_built == SOC_TRUE
        ? soc_hiz_build_upper_levels_with_kernels(
            &snapshot->depth_pyramid,
            snapshot->kernels
        )
        : soc_hiz_build_parallel_with_kernels(
            &snapshot->depth_pyramid,
            snapshot->kernels,
            &context->thread_pool
        );
    if (result != SOC_RESULT_OK) {
        goto fail;
    }

    snapshot->frame = *frame;
    soc_aabb_query_context_initialize(
        &snapshot->frame,
        &snapshot->query_context
    );
    snapshot->build_stats.struct_size = sizeof(snapshot->build_stats);
    snapshot->build_stats.hiz_level_count =
        snapshot->depth_pyramid.level_count;
    snapshot->build_stats.input_triangle_count = input_triangle_count;
    snapshot->build_stats.clipped_triangle_count =
        clipped_triangle_count;
    snapshot->build_stats.rasterized_triangle_count =
        rasterized_triangle_count;

    *out_snapshot = snapshot;
    return SOC_RESULT_OK;

fail:
    soc_snapshot_destroy_internal(snapshot);
    return result;
}

soc_result soc_occlusion_build_dense_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
)
{
    soc_frame_desc frame;
    uint64_t input_triangle_count;
    soc_result result;

    if (out_snapshot == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    *out_snapshot = NULL;

    result = validate_build_desc(
        context,
        desc,
        &frame,
        &input_triangle_count
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }
    return build_dense_validated(
        context,
        desc,
        &frame,
        input_triangle_count,
        out_snapshot
    );
}

static soc_result build_masked_validated(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    uint64_t input_triangle_count,
    soc_snapshot** out_snapshot
)
{
    soc_snapshot* snapshot;
    uint64_t clipped_triangle_count = 0u;
    uint64_t rasterized_triangle_count = 0u;
    soc_bool lower_hiz_built = SOC_FALSE;
    soc_bool rasterized_parallel = SOC_FALSE;
    soc_result result;

    snapshot = calloc(1u, sizeof(*snapshot));
    if (snapshot == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    snapshot->kernels = context->kernels;

    result = soc_hiz_initialize_masked(
        &snapshot->depth_pyramid,
        context->width,
        context->height
    );
    if (result != SOC_RESULT_OK) {
        goto fail;
    }

    if (input_triangle_count >=
        SOC_MASKED_PARALLEL_MIN_TRIANGLE_COUNT) {
        rasterized_parallel = try_rasterize_occluders_tiled_masked(
            context,
            desc,
            frame,
            &snapshot->depth_pyramid,
            soc_hiz_level_data(&snapshot->depth_pyramid, 0u),
            snapshot->depth_pyramid.levels[0].element_count,
            &clipped_triangle_count,
            &rasterized_triangle_count,
            &lower_hiz_built,
            &result
        );
    }
    if (rasterized_parallel != SOC_TRUE) {
        result = rasterize_occluders_serial_masked(
            context,
            desc,
            frame,
            &snapshot->depth_pyramid,
            &clipped_triangle_count,
            &rasterized_triangle_count
        );
    }
    if (result != SOC_RESULT_OK) {
        goto fail;
    }
    result = lower_hiz_built == SOC_TRUE
        ? soc_hiz_build_upper_levels_with_kernels(
            &snapshot->depth_pyramid,
            snapshot->kernels
        )
        : soc_hiz_build_with_kernels(
            &snapshot->depth_pyramid,
            snapshot->kernels
        );
    if (result != SOC_RESULT_OK) {
        goto fail;
    }

    snapshot->masked_parallel = rasterized_parallel;
    snapshot->frame = *frame;
    soc_aabb_query_context_initialize(
        &snapshot->frame,
        &snapshot->query_context
    );
    snapshot->build_stats.struct_size = sizeof(snapshot->build_stats);
    snapshot->build_stats.hiz_level_count =
        snapshot->depth_pyramid.level_count;
    snapshot->build_stats.input_triangle_count = input_triangle_count;
    snapshot->build_stats.clipped_triangle_count =
        clipped_triangle_count;
    snapshot->build_stats.rasterized_triangle_count =
        rasterized_triangle_count;

    *out_snapshot = snapshot;
    return SOC_RESULT_OK;

fail:
    soc_snapshot_destroy_internal(snapshot);
    return result;
}

soc_result soc_occlusion_build_masked_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
)
{
    soc_frame_desc frame;
    uint64_t input_triangle_count;
    soc_result result;

    if (out_snapshot == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    *out_snapshot = NULL;

    result = validate_build_desc(
        context,
        desc,
        &frame,
        &input_triangle_count
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }
    return build_masked_validated(
        context,
        desc,
        &frame,
        input_triangle_count,
        out_snapshot
    );
}

soc_result soc_occlusion_build_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
)
{
    soc_frame_desc frame;
    uint64_t input_triangle_count;
    soc_result result;

    if (out_snapshot == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    *out_snapshot = NULL;

    result = validate_build_desc(
        context,
        desc,
        &frame,
        &input_triangle_count
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }
    return should_use_masked_backend(context, input_triangle_count) ==
        SOC_TRUE
        ? build_masked_validated(
            context,
            desc,
            &frame,
            input_triangle_count,
            out_snapshot
        )
        : build_dense_validated(
            context,
            desc,
            &frame,
            input_triangle_count,
            out_snapshot
        );
}
