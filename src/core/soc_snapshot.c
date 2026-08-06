#include "core/soc_snapshot.h"

#include "occlusion/soc_visibility.h"

#include <float.h>
#include <stddef.h>
#include <stdlib.h>

void soc_snapshot_destroy_internal(soc_snapshot* snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    soc_hiz_shutdown(&snapshot->depth_pyramid);
    free(snapshot);
}

soc_result soc_snapshot_test_aabbs_internal(
    const soc_snapshot* snapshot,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_query_stats* out_stats
)
{
    soc_occlusion_query_counts counts;
    soc_result result;

    if (snapshot == NULL ||
        (out_stats != NULL &&
            out_stats->struct_size < SOC_QUERY_STATS_SIZE_V1)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (bounds_count != 0u &&
        (world_bounds == NULL || out_visibility == NULL)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    if (snapshot->kernels == NULL || snapshot->kernels->test_aabbs == NULL) {
        return SOC_RESULT_INTERNAL_ERROR;
    }
    result = snapshot->kernels->test_aabbs(
        &snapshot->depth_pyramid,
        &snapshot->query_context,
        world_bounds,
        bounds_count,
        out_visibility,
        &counts
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    if (out_stats != NULL) {
        out_stats->reserved = 0u;
        out_stats->tested_aabb_count = bounds_count;
        out_stats->visible_aabb_count = counts.visible;
        out_stats->occluded_aabb_count = counts.occluded;
        out_stats->unknown_aabb_count = counts.unknown;
    }
    return SOC_RESULT_OK;
}

soc_result soc_snapshot_get_build_stats_internal(
    const soc_snapshot* snapshot,
    soc_build_stats* out_stats
)
{
    if (snapshot == NULL ||
        out_stats == NULL ||
        out_stats->struct_size < SOC_BUILD_STATS_SIZE_V1) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    out_stats->hiz_level_count = snapshot->build_stats.hiz_level_count;
    out_stats->input_triangle_count =
        snapshot->build_stats.input_triangle_count;
    out_stats->clipped_triangle_count =
        snapshot->build_stats.clipped_triangle_count;
    out_stats->rasterized_triangle_count =
        snapshot->build_stats.rasterized_triangle_count;
    return SOC_RESULT_OK;
}

soc_result soc_snapshot_hiz_level_query_internal(
    const soc_snapshot* snapshot,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
)
{
    const soc_hiz* hiz;
    soc_result result;

    if (snapshot == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    hiz = &snapshot->depth_pyramid;
    if (hiz->masked == SOC_TRUE && level == 0u) {
        const soc_hiz_level* level_zero;
        const float* z0;
        uint64_t required_element_count;
        uint32_t block_column_count;
        uint32_t y;

        if (hiz->initialized != SOC_TRUE ||
            hiz->data == NULL ||
            out_info == NULL ||
            out_info->struct_size < SOC_HIZ_LEVEL_INFO_SIZE_V1 ||
            level >= hiz->level_count) {
            return SOC_RESULT_INVALID_ARGUMENT;
        }

        required_element_count =
            (uint64_t)hiz->pixel_width * (uint64_t)hiz->pixel_height;
        out_info->level = level;
        out_info->width = hiz->pixel_width;
        out_info->height = hiz->pixel_height;
        out_info->required_element_count = required_element_count;

        if (out_depth == NULL) {
            return out_depth_count == 0u
                ? SOC_RESULT_OK
                : SOC_RESULT_INVALID_ARGUMENT;
        }
        if (out_depth_count < required_element_count) {
            return SOC_RESULT_BUFFER_TOO_SMALL;
        }

        level_zero = &hiz->levels[0];
        z0 = hiz->data + level_zero->offset;
        block_column_count = level_zero->width;
        for (y = 0u; y < hiz->pixel_height; ++y) {
            const size_t block_row_offset =
                (size_t)(y / SOC_HIZ_MASK_BLOCK_HEIGHT) *
                block_column_count;
            const uint32_t mask_row_offset =
                (y % SOC_HIZ_MASK_BLOCK_HEIGHT) *
                SOC_HIZ_MASK_BLOCK_WIDTH;
            const size_t output_row_offset =
                (size_t)y * hiz->pixel_width;
            uint32_t x;

            for (x = 0u; x < hiz->pixel_width; ++x) {
                const size_t block_index = block_row_offset +
                    x / SOC_HIZ_MASK_BLOCK_WIDTH;
                const uint32_t mask_bit = mask_row_offset +
                    x % SOC_HIZ_MASK_BLOCK_WIDTH;
                float depth =
                    (hiz->layer_masks[block_index] &
                        (UINT32_C(1) << mask_bit)) != 0u
                        ? hiz->working_depth[block_index]
                        : z0[block_index];

                if (depth == -1.0f || depth == FLT_MAX) {
                    depth = 0.0f;
                }
                out_depth[output_row_offset + x] = depth;
            }
        }
        return SOC_RESULT_OK;
    }

    result = soc_hiz_query(
        hiz,
        level,
        out_info,
        out_depth,
        out_depth_count
    );
    if (result == SOC_RESULT_OK &&
        hiz->masked == SOC_TRUE &&
        out_depth != NULL) {
        uint64_t index;

        for (index = 0u;
             index < out_info->required_element_count;
             ++index) {
            if (out_depth[index] == -1.0f) {
                out_depth[index] = 0.0f;
            }
        }
    }
    return result;
}
