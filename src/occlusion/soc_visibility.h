#ifndef SOC_VISIBILITY_H_INCLUDED
#define SOC_VISIBILITY_H_INCLUDED

#include <soc/soc.h>

#include "occlusion/soc_hiz.h"

soc_result soc_occlusion_test_aabbs(
    const soc_hiz* hiz,
    const soc_frame_desc* frame,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    uint64_t* out_occluded_count
);

#endif
