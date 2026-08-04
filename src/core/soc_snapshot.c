#include "core/soc_snapshot.h"

#include "occlusion/soc_visibility.h"

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
    if (snapshot == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    return soc_hiz_query(
        &snapshot->depth_pyramid,
        level,
        out_info,
        out_depth,
        out_depth_count
    );
}
