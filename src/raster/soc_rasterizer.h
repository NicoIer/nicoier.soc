#ifndef SOC_RASTERIZER_H_INCLUDED
#define SOC_RASTERIZER_H_INCLUDED

#include <soc/soc.h>

typedef struct soc_rasterizer {
    uint32_t width;
    uint32_t height;
    uint32_t hiz_level_count;
    soc_bool initialized;
    soc_bool frame_active;
    soc_frame_desc frame;
} soc_rasterizer;

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
);

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_resize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
);

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
);

soc_result soc_rasterizer_submit_occluders(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
);

soc_result soc_rasterizer_finish_occluders(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_test_aabbs(
    soc_rasterizer* rasterizer,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility
);

soc_result soc_rasterizer_end_frame(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_query_hiz_level(
    const soc_rasterizer* rasterizer,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
);

#endif
