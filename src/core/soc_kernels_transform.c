#include "core/soc_kernels.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static soc_kernel_clip_vertex transform_vertex_f32(
    const soc_kernel_mat4_f32* matrix,
    const soc_kernel_clip_vertex* vertex
)
{
    soc_kernel_clip_vertex result;

    result.x = matrix->columns[3][0] * vertex->w;
    result.x = fmaf(matrix->columns[0][0], vertex->x, result.x);
    result.x = fmaf(matrix->columns[1][0], vertex->y, result.x);
    result.x = fmaf(matrix->columns[2][0], vertex->z, result.x);

    result.y = matrix->columns[3][1] * vertex->w;
    result.y = fmaf(matrix->columns[0][1], vertex->x, result.y);
    result.y = fmaf(matrix->columns[1][1], vertex->y, result.y);
    result.y = fmaf(matrix->columns[2][1], vertex->z, result.y);

    result.z = matrix->columns[3][2] * vertex->w;
    result.z = fmaf(matrix->columns[0][2], vertex->x, result.z);
    result.z = fmaf(matrix->columns[1][2], vertex->y, result.z);
    result.z = fmaf(matrix->columns[2][2], vertex->z, result.z);

    result.w = matrix->columns[3][3] * vertex->w;
    result.w = fmaf(matrix->columns[0][3], vertex->x, result.w);
    result.w = fmaf(matrix->columns[1][3], vertex->y, result.w);
    result.w = fmaf(matrix->columns[2][3], vertex->z, result.w);
    return result;
}

void soc_kernel_mat4_f32_multiply(
    const soc_kernel_mat4_f32* left,
    const soc_kernel_mat4_f32* right,
    soc_kernel_mat4_f32* destination
)
{
    soc_kernel_mat4_f32 result;
    size_t column;

    for (column = 0u; column < 4u; ++column) {
        const soc_kernel_clip_vertex right_column = {
            right->columns[column][0],
            right->columns[column][1],
            right->columns[column][2],
            right->columns[column][3],
        };
        const soc_kernel_clip_vertex product = transform_vertex_f32(
            left,
            &right_column
        );

        result.columns[column][0] = product.x;
        result.columns[column][1] = product.y;
        result.columns[column][2] = product.z;
        result.columns[column][3] = product.w;
    }
    *destination = result;
}

static uint8_t compute_clip_outcode_f32(
    const soc_kernel_clip_vertex* vertex,
    soc_clip_depth_range depth_range
)
{
    uint8_t outcode = 0u;

    if (vertex->x + vertex->w < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 0u));
    }
    if (vertex->w - vertex->x < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 1u));
    }
    if (vertex->y + vertex->w < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 2u));
    }
    if (vertex->w - vertex->y < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 3u));
    }
    if ((depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
            ? vertex->z
            : vertex->z + vertex->w) < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 4u));
    }
    if (vertex->w - vertex->z < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 5u));
    }
    return outcode;
}

void soc_kernel_mat4_f32_from_f32(
    const soc_mat4* source,
    soc_kernel_mat4_f32* destination
)
{
    const float components[16] = {
        source->col0.x, source->col0.y, source->col0.z, source->col0.w,
        source->col1.x, source->col1.y, source->col1.z, source->col1.w,
        source->col2.x, source->col2.y, source->col2.z, source->col2.w,
        source->col3.x, source->col3.y, source->col3.z, source->col3.w,
    };
    size_t column;

    for (column = 0u; column < 4u; ++column) {
        size_t row;

        for (row = 0u; row < 4u; ++row) {
            const float value = components[column * 4u + row];

            destination->columns[column][row] = value;
        }
    }
}

void soc_kernel_transform_triangle_f32_scalar(
    const soc_kernel_mat4_f32* clip_from_object,
    const float* position0_xyz,
    const float* position1_xyz,
    const float* position2_xyz,
    soc_clip_depth_range depth_range,
    soc_kernel_clip_vertex out_clip[3],
    soc_kernel_clip_metadata* out_metadata
)
{
    const float* positions[3] = {
        position0_xyz,
        position1_xyz,
        position2_xyz,
    };
    size_t index;

    out_metadata->active_planes = 0u;
    out_metadata->common_planes = UINT8_C(0x3f);

    for (index = 0u; index < 3u; ++index) {
        const soc_kernel_clip_vertex object_position = {
            positions[index][0],
            positions[index][1],
            positions[index][2],
            1.0f,
        };
        out_clip[index] = transform_vertex_f32(
            clip_from_object,
            &object_position
        );
    }

    for (index = 0u; index < 3u; ++index) {
        const uint8_t outcode = compute_clip_outcode_f32(
            &out_clip[index],
            depth_range
        );
        out_metadata->active_planes = (uint8_t)(
            out_metadata->active_planes | outcode
        );
        out_metadata->common_planes = (uint8_t)(
            out_metadata->common_planes & outcode
        );
    }
}

void soc_kernel_transform_triangle_post_cache_f32_scalar(
    const soc_kernel_mat4_f32* clip_from_object,
    const float* position0_xyz,
    const float* position1_xyz,
    const float* position2_xyz,
    soc_clip_depth_range depth_range,
    soc_kernel_clip_vertex out_clip[3],
    soc_kernel_clip_metadata* out_metadata,
    uint8_t out_outcodes[3],
    uint8_t transform_mask
)
{
    const float* positions[3] = {
        position0_xyz,
        position1_xyz,
        position2_xyz,
    };
    size_t index;

    out_metadata->active_planes = 0u;
    out_metadata->common_planes = UINT8_C(0x3f);

    for (index = 0u; index < 3u; ++index) {
        if ((transform_mask & (UINT8_C(1) << index)) == 0u) {
            continue;
        }
        const soc_kernel_clip_vertex object_position = {
            positions[index][0],
            positions[index][1],
            positions[index][2],
            1.0f,
        };
        out_clip[index] = transform_vertex_f32(
            clip_from_object,
            &object_position
        );
        const uint8_t outcode = compute_clip_outcode_f32(
            &out_clip[index],
            depth_range
        );
        out_outcodes[index] = outcode;
    }
    for (index = 0u; index < 3u; ++index) {
        out_metadata->active_planes = (uint8_t)(
            out_metadata->active_planes | out_outcodes[index]
        );
        out_metadata->common_planes = (uint8_t)(
            out_metadata->common_planes & out_outcodes[index]
        );
    }
}
