#ifndef SOC_PIPELINE_H_INCLUDED
#define SOC_PIPELINE_H_INCLUDED

#include <soc/soc.h>

soc_result soc_frame_begin_internal(
    soc_context* context,
    const soc_frame_desc* desc
);

soc_result soc_occluders_submit_internal(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
);

soc_result soc_occluders_finish_internal(soc_context* context);

soc_result soc_visibility_test_aabbs_internal(
    soc_context* context,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility
);

soc_result soc_frame_end_internal(soc_context* context);

soc_result soc_hiz_level_query_internal(
    const soc_context* context,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
);

#endif
