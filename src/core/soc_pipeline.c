#include "core/soc_pipeline.h"

#include "core/soc_context.h"
#include "core/soc_mesh.h"
#include "core/soc_snapshot.h"
#include "platform/soc_thread_pool.h"
#include "raster/soc_rasterizer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SOC_PARALLEL_TRIANGLES_PER_WORK_ITEM UINT32_C(256)
#define SOC_PARALLEL_DEPTH_SCRATCH_BUDGET_BYTES \
    ((size_t)256u * 1024u * 1024u)

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

static soc_result validate_frame_desc(const soc_frame_desc* desc)
{
    if (desc == NULL || desc->struct_size < SOC_FRAME_DESC_SIZE_V1) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (desc->clip_depth_range != SOC_CLIP_DEPTH_ZERO_TO_ONE &&
        desc->clip_depth_range != SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (desc->depth_direction != SOC_DEPTH_FORWARD &&
        desc->depth_direction != SOC_DEPTH_REVERSED) {
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

typedef struct soc_parallel_work_item {
    const soc_mesh* mesh;
    const soc_mat4* object_to_world;
    uint32_t triangle_begin;
    uint32_t triangle_count;
} soc_parallel_work_item;

typedef struct soc_parallel_raster_state {
    const soc_frame_desc* frame;
    soc_rasterizer* rasterizers;
    soc_result* lane_results;
    const soc_parallel_work_item* work_items;
    size_t work_item_count;
    uint32_t active_lane_count;
} soc_parallel_raster_state;

typedef struct soc_parallel_merge_state {
    float* level_zero;
    const float* scratch_depth;
    size_t depth_element_count;
    uint32_t active_lane_count;
    soc_depth_direction depth_direction;
} soc_parallel_merge_state;

static soc_bool calculate_parallel_work_item_count(
    const soc_occlusion_build_desc* desc,
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
            triangle_count / SOC_PARALLEL_TRIANGLES_PER_WORK_ITEM;
        if (triangle_count % SOC_PARALLEL_TRIANGLES_PER_WORK_ITEM != 0u) {
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

static void parallel_begin_frame(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_parallel_raster_state* state = user;

    (void)worker_count;
    if (worker_index >= state->active_lane_count) {
        return;
    }
    state->lane_results[worker_index] = soc_rasterizer_begin_frame(
        &state->rasterizers[worker_index],
        state->frame
    );
}

static void parallel_rasterize_work_items(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_parallel_raster_state* state = user;
    soc_rasterizer* rasterizer;
    size_t work_item_index;
    soc_result result;

    (void)worker_count;
    if (worker_index >= state->active_lane_count ||
        state->lane_results[worker_index] != SOC_RESULT_OK) {
        return;
    }

    rasterizer = &state->rasterizers[worker_index];
    for (work_item_index = worker_index;
         work_item_index < state->work_item_count;
         work_item_index += state->active_lane_count) {
        const soc_parallel_work_item* work_item =
            &state->work_items[work_item_index];

        result = soc_rasterizer_submit_occluder_triangles(
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

    state->lane_results[worker_index] =
        soc_rasterizer_finish_occluders(rasterizer);
}

static void parallel_merge_depth(
    void* user,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_parallel_merge_state* state = user;
    const size_t elements_per_worker =
        state->depth_element_count / worker_count;
    const size_t extra_elements =
        state->depth_element_count % worker_count;
    const size_t begin =
        elements_per_worker * worker_index +
        (worker_index < extra_elements ? worker_index : extra_elements);
    const size_t end = begin + elements_per_worker +
        (worker_index < extra_elements ? 1u : 0u);
    size_t element_index;

    for (element_index = begin; element_index < end; ++element_index) {
        float merged_depth = state->level_zero[element_index];
        uint32_t lane;

        for (lane = 1u; lane < state->active_lane_count; ++lane) {
            const float candidate_depth = state->scratch_depth[
                (size_t)(lane - 1u) * state->depth_element_count +
                element_index
            ];

            if ((state->depth_direction == SOC_DEPTH_REVERSED &&
                    candidate_depth > merged_depth) ||
                (state->depth_direction == SOC_DEPTH_FORWARD &&
                    candidate_depth < merged_depth)) {
                merged_depth = candidate_depth;
            }
        }
        state->level_zero[element_index] = merged_depth;
    }
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

/*
 * Returns SOC_FALSE when the parallel path is ineligible or cannot allocate
 * its optional working storage. The caller must then use the serial path.
 */
static soc_bool try_rasterize_occluders_parallel(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    const soc_frame_desc* frame,
    float* level_zero,
    size_t depth_element_count,
    uint64_t* out_clipped_triangle_count,
    uint64_t* out_rasterized_triangle_count,
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
    uint32_t active_lane_count;
    uint32_t lane;
    uint32_t initialized_count = 0u;
    soc_parallel_work_item* work_items = NULL;
    soc_rasterizer* rasterizers = NULL;
    soc_result* lane_results = NULL;
    float* scratch_depth = NULL;
    soc_parallel_raster_state raster_state;
    soc_parallel_merge_state merge_state;
    soc_result result = SOC_RESULT_OK;
    size_t work_item_index = 0u;
    uint32_t group_index;

    if (context->worker_count <= 1u ||
        !calculate_parallel_work_item_count(desc, &work_item_count_u64) ||
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

    scratch_lane_capacity =
        SOC_PARALLEL_DEPTH_SCRATCH_BUDGET_BYTES / depth_buffer_bytes;
    if (scratch_lane_capacity == 0u) {
        return SOC_FALSE;
    }

    active_lane_count = context->worker_count;
    if (work_item_count_u64 < active_lane_count) {
        active_lane_count = (uint32_t)work_item_count_u64;
    }
    if (scratch_lane_capacity < (size_t)(active_lane_count - 1u)) {
        active_lane_count = (uint32_t)scratch_lane_capacity + 1u;
    }
    if (active_lane_count <= 1u) {
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
        free(scratch_depth);
        free(lane_results);
        free(rasterizers);
        free(work_items);
        return SOC_FALSE;
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
                    remaining < SOC_PARALLEL_TRIANGLES_PER_WORK_ITEM
                        ? remaining
                        : SOC_PARALLEL_TRIANGLES_PER_WORK_ITEM;
                soc_parallel_work_item* work_item;

                if (work_item_index >= work_item_count) {
                    result = SOC_RESULT_INTERNAL_ERROR;
                    goto attempted_cleanup;
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
        goto attempted_cleanup;
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
            goto attempted_cleanup;
        }
        ++initialized_count;
        lane_results[lane] = SOC_RESULT_OK;
    }

    raster_state.frame = frame;
    raster_state.rasterizers = rasterizers;
    raster_state.lane_results = lane_results;
    raster_state.work_items = work_items;
    raster_state.work_item_count = work_item_count;
    raster_state.active_lane_count = active_lane_count;

    soc_thread_pool_run(
        &context->thread_pool,
        parallel_begin_frame,
        &raster_state
    );
    for (lane = 0u; lane < active_lane_count; ++lane) {
        if (lane_results[lane] != SOC_RESULT_OK) {
            result = lane_results[lane];
            goto attempted_cleanup;
        }
    }

    soc_thread_pool_run(
        &context->thread_pool,
        parallel_rasterize_work_items,
        &raster_state
    );
    for (lane = 0u; lane < active_lane_count; ++lane) {
        if (lane_results[lane] != SOC_RESULT_OK) {
            result = lane_results[lane];
            goto attempted_cleanup;
        }
    }

    merge_state.level_zero = level_zero;
    merge_state.scratch_depth = scratch_depth;
    merge_state.depth_element_count = depth_element_count;
    merge_state.active_lane_count = active_lane_count;
    merge_state.depth_direction = frame->depth_direction;
    soc_thread_pool_run(
        &context->thread_pool,
        parallel_merge_depth,
        &merge_state
    );

    *out_clipped_triangle_count = 0u;
    *out_rasterized_triangle_count = 0u;
    for (lane = 0u; lane < active_lane_count; ++lane) {
        *out_clipped_triangle_count +=
            rasterizers[lane].clipped_triangle_count;
        *out_rasterized_triangle_count +=
            rasterizers[lane].rasterized_triangle_count;
        result = soc_rasterizer_end_frame(&rasterizers[lane]);
        if (result != SOC_RESULT_OK) {
            goto attempted_cleanup;
        }
    }

attempted_cleanup:
    cleanup_parallel_rasterizers(rasterizers, initialized_count);
    free(scratch_depth);
    free(lane_results);
    free(rasterizers);
    free(work_items);
    *out_result = result;
    return SOC_TRUE;
}

soc_result soc_occlusion_build_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
)
{
    soc_snapshot* snapshot;
    soc_frame_desc frame;
    uint64_t input_triangle_count;
    uint64_t clipped_triangle_count = 0u;
    uint64_t rasterized_triangle_count = 0u;
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
            &frame,
            soc_hiz_level_data(&snapshot->depth_pyramid, 0u),
            snapshot->depth_pyramid.levels[0].element_count,
            &clipped_triangle_count,
            &rasterized_triangle_count,
            &result
        )) {
        result = rasterize_occluders_serial(
            context,
            desc,
            &frame,
            soc_hiz_level_data(&snapshot->depth_pyramid, 0u),
            snapshot->depth_pyramid.levels[0].element_count,
            &clipped_triangle_count,
            &rasterized_triangle_count
        );
    }
    if (result != SOC_RESULT_OK) {
        goto fail;
    }
    result = soc_hiz_build_with_kernels(
        &snapshot->depth_pyramid,
        frame.depth_direction,
        snapshot->kernels
    );
    if (result != SOC_RESULT_OK) {
        goto fail;
    }

    snapshot->frame = frame;
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
