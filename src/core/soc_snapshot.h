#ifndef SOC_SNAPSHOT_H_INCLUDED
#define SOC_SNAPSHOT_H_INCLUDED

#include <soc/soc.h>

#include "occlusion/soc_hiz.h"

struct soc_snapshot {
    soc_hiz depth_pyramid;
    soc_frame_desc frame;
    soc_build_stats build_stats;
};

void soc_snapshot_destroy_internal(soc_snapshot* snapshot);

soc_result soc_snapshot_test_aabbs_internal(
    const soc_snapshot* snapshot,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_query_stats* out_stats
);

soc_result soc_snapshot_get_build_stats_internal(
    const soc_snapshot* snapshot,
    soc_build_stats* out_stats
);

soc_result soc_snapshot_hiz_level_query_internal(
    const soc_snapshot* snapshot,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
);

#endif
