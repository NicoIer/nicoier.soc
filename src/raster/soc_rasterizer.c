#include "raster/soc_rasterizer.h"

#include <stddef.h>
#include <string.h>

static uint32_t halve_ceil(uint32_t value)
{
    return value / 2u + value % 2u;
}

static uint32_t calculate_hiz_level_count(uint32_t width, uint32_t height)
{
    uint32_t level_count = 1u;

    while (width > 1u || height > 1u) {
        width = halve_ceil(width);
        height = halve_ceil(height);
        ++level_count;
    }

    return level_count;
}

static void calculate_hiz_level_dimensions(
    const soc_rasterizer* rasterizer,
    uint32_t level,
    uint32_t* out_width,
    uint32_t* out_height
)
{
    uint32_t width = rasterizer->width;
    uint32_t height = rasterizer->height;
    uint32_t current_level;

    for (current_level = 0u; current_level < level; ++current_level) {
        width = halve_ceil(width);
        height = halve_ceil(height);
    }

    *out_width = width;
    *out_height = height;
}

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
)
{
    if (rasterizer == NULL || width == 0u || height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->hiz_level_count = calculate_hiz_level_count(width, height);
    rasterizer->initialized = SOC_TRUE;
    rasterizer->frame_active = SOC_FALSE;
    return SOC_RESULT_OK;
}

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL) {
        return;
    }

    memset(rasterizer, 0, sizeof(*rasterizer));
}

soc_result soc_rasterizer_resize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
)
{
    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        width == 0u ||
        height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->hiz_level_count = calculate_hiz_level_count(width, height);
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
)
{
    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        desc == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    rasterizer->frame = *desc;
    rasterizer->frame_active = SOC_TRUE;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_submit_occluders(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
)
{
    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        mesh == NULL ||
        object_to_world == NULL ||
        instance_count == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    /*
     * Framework only: transform, clipping, binning, and depth writes will be
     * implemented here.
     */
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_finish_occluders(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL || rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    /* Framework only: the Hi-Z hierarchy will be built here. */
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_test_aabbs(
    soc_rasterizer* rasterizer,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility
)
{
    uint32_t index;

    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        world_bounds == NULL ||
        out_visibility == NULL ||
        bounds_count == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    /*
     * Framework only: UNKNOWN is fail-open and prevents false occlusion until
     * projection and Hi-Z testing are implemented.
     */
    for (index = 0u; index < bounds_count; ++index) {
        out_visibility[index] = SOC_VISIBILITY_UNKNOWN;
    }
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_end_frame(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL || rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    rasterizer->frame_active = SOC_FALSE;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_query_hiz_level(
    const soc_rasterizer* rasterizer,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
)
{
    uint32_t width;
    uint32_t height;
    uint64_t required_count;
    uint64_t index;
    float clear_depth;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        rasterizer->frame_active != SOC_TRUE ||
        out_info == NULL ||
        out_info->struct_size < SOC_HIZ_LEVEL_INFO_SIZE_V1) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (level >= rasterizer->hiz_level_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    calculate_hiz_level_dimensions(rasterizer, level, &width, &height);
    required_count = (uint64_t)width * height;

    out_info->level = level;
    out_info->width = width;
    out_info->height = height;
    out_info->required_element_count = required_count;

    if (out_depth == NULL) {
        return out_depth_count == 0u
            ? SOC_RESULT_OK
            : SOC_RESULT_INVALID_ARGUMENT;
    }
    if (out_depth_count < required_count) {
        return SOC_RESULT_BUFFER_TOO_SMALL;
    }

    clear_depth = rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
        ? 0.0f
        : 1.0f;

    /*
     * Framework only: replace this clear image with a copy from the selected
     * Hi-Z level after depth storage is implemented.
     */
    for (index = 0u; index < required_count; ++index) {
        out_depth[index] = clear_depth;
    }

    return SOC_RESULT_OK;
}
