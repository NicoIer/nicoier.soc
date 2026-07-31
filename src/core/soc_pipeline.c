#include "core/soc_pipeline.h"

#include "core/soc_context.h"
#include "core/soc_mesh.h"

#include <stddef.h>
#include <string.h>

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

soc_result soc_frame_begin_internal(
    soc_context* context,
    const soc_frame_desc* desc
)
{
    soc_result result;

    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_IDLE) {
        return SOC_RESULT_INVALID_STATE;
    }

    result = validate_frame_desc(desc);
    if (result != SOC_RESULT_OK) {
        return result;
    }

    result = soc_rasterizer_begin_frame(&context->rasterizer, desc);
    if (result != SOC_RESULT_OK) {
        return result;
    }

    memset(&context->stats, 0, sizeof(context->stats));
    context->stats.struct_size = sizeof(context->stats);
    context->state = SOC_CONTEXT_STATE_RECORDING_OCCLUDERS;
    return SOC_RESULT_OK;
}

soc_result soc_occluders_submit_internal(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
)
{
    soc_result result;

    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_RECORDING_OCCLUDERS) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (instance_count == 0u) {
        return SOC_RESULT_OK;
    }
    if (mesh == NULL ||
        mesh->owner != context ||
        object_to_world == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    result = soc_rasterizer_submit_occluders(
        &context->rasterizer,
        mesh,
        object_to_world,
        instance_count
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    context->stats.input_triangle_count +=
        (uint64_t)(mesh->index_count / 3u) * instance_count;
    context->stats.clipped_triangle_count =
        context->rasterizer.clipped_triangle_count;
    context->stats.rasterized_triangle_count =
        context->rasterizer.rasterized_triangle_count;
    return SOC_RESULT_OK;
}

soc_result soc_occluders_finish_internal(soc_context* context)
{
    soc_result result;

    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_RECORDING_OCCLUDERS) {
        return SOC_RESULT_INVALID_STATE;
    }

    result = soc_rasterizer_finish_occluders(&context->rasterizer);
    if (result != SOC_RESULT_OK) {
        return result;
    }

    result = soc_hiz_build(
        &context->depth_pyramid,
        context->rasterizer.frame.depth_direction
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    context->stats.hiz_level_count =
        context->depth_pyramid.level_count;
    context->state = SOC_CONTEXT_STATE_QUERY_READY;
    return SOC_RESULT_OK;
}

soc_result soc_visibility_test_aabbs_internal(
    soc_context* context,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility
)
{
    soc_result result;

    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_QUERY_READY) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (bounds_count == 0u) {
        return SOC_RESULT_OK;
    }
    if (world_bounds == NULL || out_visibility == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    result = soc_rasterizer_test_aabbs(
        &context->rasterizer,
        world_bounds,
        bounds_count,
        out_visibility
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    context->stats.tested_aabb_count += bounds_count;
    return SOC_RESULT_OK;
}

soc_result soc_frame_end_internal(soc_context* context)
{
    soc_result result;

    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_QUERY_READY) {
        return SOC_RESULT_INVALID_STATE;
    }

    result = soc_rasterizer_end_frame(&context->rasterizer);
    if (result != SOC_RESULT_OK) {
        return result;
    }

    context->state = SOC_CONTEXT_STATE_IDLE;
    return SOC_RESULT_OK;
}

soc_result soc_hiz_level_query_internal(
    const soc_context* context,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
)
{
    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_QUERY_READY) {
        return SOC_RESULT_INVALID_STATE;
    }

    return soc_hiz_query(
        &context->depth_pyramid,
        level,
        out_info,
        out_depth,
        out_depth_count
    );
}
