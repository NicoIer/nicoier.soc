#include "core/soc_pipeline.h"

#include "core/soc_context.h"
#include "core/soc_mesh.h"
#include "core/soc_snapshot.h"
#include "raster/soc_rasterizer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

soc_result soc_occlusion_build_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
)
{
    soc_snapshot* snapshot;
    soc_rasterizer rasterizer;
    soc_frame_desc frame;
    uint64_t input_triangle_count;
    uint32_t index;
    soc_result result;
    soc_bool rasterizer_initialized = SOC_FALSE;
    soc_bool frame_active = SOC_FALSE;

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

    result = soc_hiz_initialize(
        &snapshot->depth_pyramid,
        context->width,
        context->height
    );
    if (result != SOC_RESULT_OK) {
        soc_snapshot_destroy_internal(snapshot);
        return result;
    }

    result = soc_rasterizer_initialize(
        &rasterizer,
        context->width,
        context->height,
        soc_hiz_level_data(&snapshot->depth_pyramid, 0u),
        snapshot->depth_pyramid.levels[0].element_count
    );
    if (result != SOC_RESULT_OK) {
        soc_snapshot_destroy_internal(snapshot);
        return result;
    }
    rasterizer_initialized = SOC_TRUE;

    result = soc_rasterizer_begin_frame(&rasterizer, &frame);
    if (result != SOC_RESULT_OK) {
        goto fail;
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
            goto fail;
        }
    }

    result = soc_rasterizer_finish_occluders(&rasterizer);
    if (result != SOC_RESULT_OK) {
        goto fail;
    }
    result = soc_hiz_build(
        &snapshot->depth_pyramid,
        frame.depth_direction
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
        rasterizer.clipped_triangle_count;
    snapshot->build_stats.rasterized_triangle_count =
        rasterizer.rasterized_triangle_count;

    result = soc_rasterizer_end_frame(&rasterizer);
    if (result != SOC_RESULT_OK) {
        goto fail;
    }
    frame_active = SOC_FALSE;
    soc_rasterizer_shutdown(&rasterizer);
    *out_snapshot = snapshot;
    return SOC_RESULT_OK;

fail:
    if (frame_active == SOC_TRUE) {
        (void)soc_rasterizer_end_frame(&rasterizer);
    }
    if (rasterizer_initialized == SOC_TRUE) {
        soc_rasterizer_shutdown(&rasterizer);
    }
    soc_snapshot_destroy_internal(snapshot);
    return result;
}
