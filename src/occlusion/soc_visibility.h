#ifndef SOC_VISIBILITY_H_INCLUDED
#define SOC_VISIBILITY_H_INCLUDED

#include <soc/soc.h>

#include "occlusion/soc_hiz.h"

#define SOC_VISIBILITY_CLIP_PLANE_COUNT 6u
#define SOC_VISIBILITY_NEAR_CLIP_PLANE_INDEX 5u
#define SOC_VISIBILITY_NEAR_CLIP_PLANE_BIT \
    (UINT32_C(1) << SOC_VISIBILITY_NEAR_CLIP_PLANE_INDEX)

typedef struct soc_visibility_clip_vertex {
    float x;
    float y;
    float z;
    float w;
} soc_visibility_clip_vertex;

typedef struct soc_visibility_world_plane {
    float x;
    float y;
    float z;
    float d;
} soc_visibility_world_plane;

typedef struct soc_aabb_query_context {
    soc_visibility_clip_vertex col0;
    soc_visibility_clip_vertex col1;
    soc_visibility_clip_vertex col2;
    soc_visibility_clip_vertex col3;
    soc_visibility_world_plane clip_planes[
        SOC_VISIBILITY_CLIP_PLANE_COUNT
    ];
    soc_visibility_world_plane w_plane;
    soc_clip_depth_range clip_depth_range;
} soc_aabb_query_context;

typedef struct soc_projected_aabb {
    float minimum_ndc_x;
    float maximum_ndc_x;
    float minimum_ndc_y;
    float maximum_ndc_y;
    float nearest_depth;
} soc_projected_aabb;

typedef enum soc_aabb_projection {
    SOC_AABB_PROJECTION_UNKNOWN = 0,
    SOC_AABB_PROJECTION_OUTSIDE,
    SOC_AABB_PROJECTION_VALID,
} soc_aabb_projection;

typedef struct soc_occlusion_query_counts {
    uint64_t visible;
    uint64_t occluded;
    uint64_t unknown;
} soc_occlusion_query_counts;

void soc_aabb_query_context_initialize(
    const soc_frame_desc* frame,
    soc_aabb_query_context* out_query
);

soc_result soc_occlusion_validate_aabb_test(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_occlusion_query_counts* out_counts
);

soc_aabb_projection soc_project_aabb_scalar(
    const soc_aabb_query_context* query,
    const soc_aabb* bounds,
    soc_projected_aabb* out_projected
);

soc_visibility soc_test_projected_aabb_scalar(
    const soc_hiz* hiz,
    const soc_projected_aabb* projected
);

soc_visibility soc_test_projected_aabb_dense_scalar(
    const soc_hiz* hiz,
    const soc_projected_aabb* projected
);

soc_visibility soc_test_projected_aabb_masked_scalar(
    const soc_hiz* hiz,
    const soc_projected_aabb* projected
);

soc_visibility soc_occlusion_test_aabb_scalar(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* bounds
);

soc_result soc_occlusion_test_aabbs(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_occlusion_query_counts* out_counts
);

#if defined(__aarch64__) || defined(_M_ARM64) || \
    defined(SOC_BUILD_AARCH32_NEON_FMA)
soc_result soc_occlusion_test_aabbs_neon(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_occlusion_query_counts* out_counts
);
#endif

#endif
