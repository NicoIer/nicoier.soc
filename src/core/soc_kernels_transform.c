#include "core/soc_kernels.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t nonfinite_f32_mask(float value)
{
    uint32_t bits;
    uint32_t exponent;

    memcpy(&bits, &value, sizeof(bits));
    exponent = bits & UINT32_C(0x7f800000);
    return (exponent + UINT32_C(0x00800000)) & UINT32_C(0x80000000);
}

static soc_kernel_clip_vertex transform_vertex_f64(
    const soc_kernel_mat4_f64* matrix,
    const soc_kernel_clip_vertex* vertex
)
{
    soc_kernel_clip_vertex result;

    result.x = matrix->columns[0][0] * vertex->x;
    result.x = result.x + matrix->columns[1][0] * vertex->y;
    result.x = result.x + matrix->columns[2][0] * vertex->z;
    result.x = result.x + matrix->columns[3][0] * vertex->w;

    result.y = matrix->columns[0][1] * vertex->x;
    result.y = result.y + matrix->columns[1][1] * vertex->y;
    result.y = result.y + matrix->columns[2][1] * vertex->z;
    result.y = result.y + matrix->columns[3][1] * vertex->w;

    result.z = matrix->columns[0][2] * vertex->x;
    result.z = result.z + matrix->columns[1][2] * vertex->y;
    result.z = result.z + matrix->columns[2][2] * vertex->z;
    result.z = result.z + matrix->columns[3][2] * vertex->w;

    result.w = matrix->columns[0][3] * vertex->x;
    result.w = result.w + matrix->columns[1][3] * vertex->y;
    result.w = result.w + matrix->columns[2][3] * vertex->z;
    result.w = result.w + matrix->columns[3][3] * vertex->w;
    return result;
}

static soc_bool finite_f64(double value)
{
    return value == value && value >= -DBL_MAX && value <= DBL_MAX
        ? SOC_TRUE
        : SOC_FALSE;
}

void soc_kernel_mat4_f64_multiply(
    const soc_kernel_mat4_f64* left,
    const soc_kernel_mat4_f64* right,
    soc_kernel_mat4_f64* destination
)
{
    soc_kernel_mat4_f64 result;
    size_t column;

    result.all_finite = left->all_finite == UINT64_C(1) &&
            right->all_finite == UINT64_C(1)
        ? UINT64_C(1)
        : UINT64_C(0);
    for (column = 0u; column < 4u; ++column) {
        const soc_kernel_clip_vertex right_column = {
            right->columns[column][0],
            right->columns[column][1],
            right->columns[column][2],
            right->columns[column][3],
        };
        const soc_kernel_clip_vertex product = transform_vertex_f64(
            left,
            &right_column
        );

        result.columns[column][0] = product.x;
        result.columns[column][1] = product.y;
        result.columns[column][2] = product.z;
        result.columns[column][3] = product.w;
        if (finite_f64(product.x) != SOC_TRUE ||
            finite_f64(product.y) != SOC_TRUE ||
            finite_f64(product.z) != SOC_TRUE ||
            finite_f64(product.w) != SOC_TRUE) {
            result.all_finite = UINT64_C(0);
        }
    }
    *destination = result;
}

static uint8_t compute_clip_outcode_f64(
    const soc_kernel_clip_vertex* vertex,
    soc_clip_depth_range depth_range
)
{
    uint8_t outcode = 0u;

    if (vertex->x + vertex->w < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 0u));
    }
    if (vertex->w - vertex->x < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 1u));
    }
    if (vertex->y + vertex->w < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 2u));
    }
    if (vertex->w - vertex->y < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 3u));
    }
    if ((depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
            ? vertex->z
            : vertex->z + vertex->w) < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 4u));
    }
    if (vertex->w - vertex->z < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 5u));
    }
    return outcode;
}

void soc_kernel_mat4_f64_from_f32(
    const soc_mat4* source,
    soc_kernel_mat4_f64* destination
)
{
    const float components[16] = {
        source->col0.x, source->col0.y, source->col0.z, source->col0.w,
        source->col1.x, source->col1.y, source->col1.z, source->col1.w,
        source->col2.x, source->col2.y, source->col2.z, source->col2.w,
        source->col3.x, source->col3.y, source->col3.z, source->col3.w,
    };
    uint32_t nonfinite_mask = 0u;
    size_t column;

    for (column = 0u; column < 4u; ++column) {
        size_t row;

        for (row = 0u; row < 4u; ++row) {
            const float value = components[column * 4u + row];

            destination->columns[column][row] = value;
            nonfinite_mask |= nonfinite_f32_mask(value);
        }
    }
    destination->all_finite = nonfinite_mask == 0u
        ? UINT64_C(1)
        : UINT64_C(0);
}

void soc_kernel_transform_triangle_f64_scalar(
    const soc_kernel_mat4_f64* clip_from_object,
    const float* position0_xyz,
    const float* position1_xyz,
    const float* position2_xyz,
    soc_bool positions_all_finite,
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

    (void)positions_all_finite;

    out_metadata->active_planes = 0u;
    out_metadata->common_planes = UINT8_C(0x3f);
    out_metadata->all_finite = SOC_TRUE;

    for (index = 0u; index < 3u; ++index) {
        const soc_kernel_clip_vertex object_position = {
            positions[index][0],
            positions[index][1],
            positions[index][2],
            1.0,
        };
        out_clip[index] = transform_vertex_f64(
            clip_from_object,
            &object_position
        );
    }

    for (index = 0u; index < 3u; ++index) {
        uint8_t outcode;

        if (finite_f64(out_clip[index].x) != SOC_TRUE ||
            finite_f64(out_clip[index].y) != SOC_TRUE ||
            finite_f64(out_clip[index].z) != SOC_TRUE ||
            finite_f64(out_clip[index].w) != SOC_TRUE) {
            out_metadata->active_planes = 0u;
            out_metadata->common_planes = 0u;
            out_metadata->all_finite = SOC_FALSE;
            return;
        }
        outcode = compute_clip_outcode_f64(&out_clip[index], depth_range);
        out_metadata->active_planes = (uint8_t)(
            out_metadata->active_planes | outcode
        );
        out_metadata->common_planes = (uint8_t)(
            out_metadata->common_planes & outcode
        );
    }
}
