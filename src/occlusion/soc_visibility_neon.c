#include "occlusion/soc_visibility.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#define SOC_CLIP_ALL_BITS \
    ((UINT32_C(1) << SOC_VISIBILITY_CLIP_PLANE_COUNT) - 1u)

#if defined(_MSC_VER)
#define SOC_NOINLINE __declspec(noinline)
#define SOC_FORCE_INLINE_NEON static __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_NOINLINE __attribute__((noinline))
#define SOC_FORCE_INLINE_NEON \
    static inline __attribute__((always_inline))
#else
#define SOC_NOINLINE
#define SOC_FORCE_INLINE_NEON static inline
#endif

static soc_bool valid_aabb_neon(const soc_aabb* bounds)
{
    return bounds->min.x <= bounds->max.x &&
        bounds->min.y <= bounds->max.y &&
        bounds->min.z <= bounds->max.z
            ? SOC_TRUE
            : SOC_FALSE;
}

static float clamp_float_neon(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static float32x4_t load_quad_f32(
    float lane0,
    float lane1,
    float lane2,
    float lane3
)
{
    float32x4_t result = vdupq_n_f32(lane0);

    result = vsetq_lane_f32(lane1, result, 1);
    result = vsetq_lane_f32(lane2, result, 2);
    return vsetq_lane_f32(lane3, result, 3);
}

static float get_quad_lane_f32(float32x4_t value, uint32_t lane)
{
    switch (lane) {
        case 0u:
            return vgetq_lane_f32(value, 0);
        case 1u:
            return vgetq_lane_f32(value, 1);
        case 2u:
            return vgetq_lane_f32(value, 2);
        default:
            return vgetq_lane_f32(value, 3);
    }
}

static float32x4_t ordered_min_quad_f32(
    float32x4_t accumulated,
    float32x4_t candidate
)
{
    return vbslq_f32(
        vcltq_f32(candidate, accumulated),
        candidate,
        accumulated
    );
}

static float32x4_t ordered_max_quad_f32(
    float32x4_t accumulated,
    float32x4_t candidate
)
{
    return vbslq_f32(
        vcgtq_f32(candidate, accumulated),
        candidate,
        accumulated
    );
}

static float32x4_t clamp_quad_f32(
    float32x4_t value,
    float minimum,
    float maximum
)
{
    const float32x4_t minimum_quad = vdupq_n_f32(minimum);
    const float32x4_t maximum_quad = vdupq_n_f32(maximum);

    value = vbslq_f32(vcltq_f32(value, minimum_quad), minimum_quad, value);
    return vbslq_f32(vcgtq_f32(value, maximum_quad), maximum_quad, value);
}

static float32x4_t select_extreme_quad_f32(
    const soc_visibility_world_plane* plane,
    float32x4_t minimum,
    float32x4_t maximum,
    uint32_t component,
    soc_bool select_maximum
)
{
    float coefficient;

    if (component == 0u) {
        coefficient = plane->x;
    } else if (component == 1u) {
        coefficient = plane->y;
    } else {
        coefficient = plane->z;
    }
    if (select_maximum == SOC_TRUE) {
        return coefficient >= 0.0f ? maximum : minimum;
    }
    return coefficient >= 0.0f ? minimum : maximum;
}

static float32x4_t plane_distance_quad_f32(
    const soc_visibility_world_plane* plane,
    float32x4_t minimum_x,
    float32x4_t maximum_x,
    float32x4_t minimum_y,
    float32x4_t maximum_y,
    float32x4_t minimum_z,
    float32x4_t maximum_z,
    soc_bool select_maximum
)
{
    const float32x4_t x = select_extreme_quad_f32(
        plane,
        minimum_x,
        maximum_x,
        0u,
        select_maximum
    );
    const float32x4_t y = select_extreme_quad_f32(
        plane,
        minimum_y,
        maximum_y,
        1u,
        select_maximum
    );
    const float32x4_t z = select_extreme_quad_f32(
        plane,
        minimum_z,
        maximum_z,
        2u,
        select_maximum
    );
    float32x4_t result = vmulq_n_f32(y, plane->y);

    result = vfmaq_n_f32(result, x, plane->x);
    result = vfmaq_n_f32(result, z, plane->z);
    return vaddq_f32(result, vdupq_n_f32(plane->d));
}

static float32x4_t transform_component_quad_f32(
    float col0,
    float col1,
    float col2,
    float col3,
    float32x4_t x,
    float32x4_t y,
    float32x4_t z
)
{
    float32x4_t result = vmulq_n_f32(y, col1);

    result = vfmaq_n_f32(result, x, col0);
    result = vfmaq_n_f32(result, z, col2);
    return vaddq_f32(result, vdupq_n_f32(col3));
}

static void build_corner_component_quad_f32(
    float32x4_t origin,
    float32x4_t axis_x,
    float32x4_t axis_y,
    float32x4_t axis_z,
    float32x4_t out_corners[8]
)
{
    out_corners[0] = origin;
    out_corners[1] = vaddq_f32(out_corners[0], axis_x);
    out_corners[2] = vaddq_f32(out_corners[0], axis_y);
    out_corners[3] = vaddq_f32(out_corners[1], axis_y);
    out_corners[4] = vaddq_f32(out_corners[0], axis_z);
    out_corners[5] = vaddq_f32(out_corners[1], axis_z);
    out_corners[6] = vaddq_f32(out_corners[2], axis_z);
    out_corners[7] = vaddq_f32(out_corners[3], axis_z);
}

static void project_aabb_quad_f32_neon(
    const soc_aabb_query_context* query,
    const soc_aabb bounds[4],
    soc_projected_aabb out_projected[4],
    soc_aabb_projection out_projection[4]
)
{
    static const soc_aabb zero_bounds = {
        .min = {0.0f, 0.0f, 0.0f},
        .max = {0.0f, 0.0f, 0.0f},
    };
    soc_aabb safe_bounds[4];
    soc_bool analytic[4] = {SOC_FALSE, SOC_FALSE, SOC_FALSE, SOC_FALSE};
    soc_bool valid[4];
    uint32_t all_outside_mask[4] = {
        SOC_CLIP_ALL_BITS,
        SOC_CLIP_ALL_BITS,
        SOC_CLIP_ALL_BITS,
        SOC_CLIP_ALL_BITS,
    };
    float32x4_t minimum_x;
    float32x4_t maximum_x;
    float32x4_t minimum_y;
    float32x4_t maximum_y;
    float32x4_t minimum_z;
    float32x4_t maximum_z;
    float32x4_t minimum_w;
    float32x4_t minimum_near;
    float32x4_t clip_x[8];
    float32x4_t clip_y[8];
    float32x4_t clip_z[8];
    float32x4_t clip_w[8];
    float32x4_t projected_minimum_x;
    float32x4_t projected_maximum_x;
    float32x4_t projected_minimum_y;
    float32x4_t projected_maximum_y;
    float32x4_t projected_nearest_depth;
    uint32_t lane;
    uint32_t plane;
    uint32_t corner;

    for (lane = 0u; lane < 4u; ++lane) {
        valid[lane] = valid_aabb_neon(&bounds[lane]);
        safe_bounds[lane] = valid[lane] == SOC_TRUE
            ? bounds[lane]
            : zero_bounds;
        out_projection[lane] = SOC_AABB_PROJECTION_UNKNOWN;
    }

    minimum_x = load_quad_f32(
        safe_bounds[0].min.x,
        safe_bounds[1].min.x,
        safe_bounds[2].min.x,
        safe_bounds[3].min.x
    );
    maximum_x = load_quad_f32(
        safe_bounds[0].max.x,
        safe_bounds[1].max.x,
        safe_bounds[2].max.x,
        safe_bounds[3].max.x
    );
    minimum_y = load_quad_f32(
        safe_bounds[0].min.y,
        safe_bounds[1].min.y,
        safe_bounds[2].min.y,
        safe_bounds[3].min.y
    );
    maximum_y = load_quad_f32(
        safe_bounds[0].max.y,
        safe_bounds[1].max.y,
        safe_bounds[2].max.y,
        safe_bounds[3].max.y
    );
    minimum_z = load_quad_f32(
        safe_bounds[0].min.z,
        safe_bounds[1].min.z,
        safe_bounds[2].min.z,
        safe_bounds[3].min.z
    );
    maximum_z = load_quad_f32(
        safe_bounds[0].max.z,
        safe_bounds[1].max.z,
        safe_bounds[2].max.z,
        safe_bounds[3].max.z
    );

    for (plane = 0u;
         plane < SOC_VISIBILITY_CLIP_PLANE_COUNT;
         ++plane) {
        const float32x4_t distance = plane_distance_quad_f32(
            &query->clip_planes[plane],
            minimum_x,
            maximum_x,
            minimum_y,
            maximum_y,
            minimum_z,
            maximum_z,
            SOC_TRUE
        );

        for (lane = 0u; lane < 4u; ++lane) {
            if (valid[lane] == SOC_TRUE &&
                get_quad_lane_f32(distance, lane) >= 0.0f) {
                all_outside_mask[lane] &= ~(UINT32_C(1) << plane);
            }
        }
    }

    minimum_w = plane_distance_quad_f32(
        &query->w_plane,
        minimum_x,
        maximum_x,
        minimum_y,
        maximum_y,
        minimum_z,
        maximum_z,
        SOC_FALSE
    );
    minimum_near = plane_distance_quad_f32(
        &query->clip_planes[SOC_VISIBILITY_NEAR_CLIP_PLANE_INDEX],
        minimum_x,
        maximum_x,
        minimum_y,
        maximum_y,
        minimum_z,
        maximum_z,
        SOC_FALSE
    );

    for (lane = 0u; lane < 4u; ++lane) {
        const float scalar_minimum_w = get_quad_lane_f32(minimum_w, lane);
        const float scalar_minimum_near = get_quad_lane_f32(
            minimum_near,
            lane
        );

        if (valid[lane] != SOC_TRUE) {
            continue;
        }
        if (scalar_minimum_w <= 0.0f) {
            out_projection[lane] = SOC_AABB_PROJECTION_UNKNOWN;
        } else if (scalar_minimum_near < 0.0f &&
            (all_outside_mask[lane] &
                SOC_VISIBILITY_NEAR_CLIP_PLANE_BIT) == 0u) {
            out_projection[lane] = SOC_AABB_PROJECTION_UNKNOWN;
        } else if (all_outside_mask[lane] != 0u) {
            out_projection[lane] = SOC_AABB_PROJECTION_OUTSIDE;
        } else {
            analytic[lane] = SOC_TRUE;
        }
    }

    if (analytic[0] != SOC_TRUE && analytic[1] != SOC_TRUE &&
        analytic[2] != SOC_TRUE && analytic[3] != SOC_TRUE) {
        return;
    }

    {
        const float32x4_t delta_x = vsubq_f32(maximum_x, minimum_x);
        const float32x4_t delta_y = vsubq_f32(maximum_y, minimum_y);
        const float32x4_t delta_z = vsubq_f32(maximum_z, minimum_z);

        build_corner_component_quad_f32(
            transform_component_quad_f32(
                query->col0.x,
                query->col1.x,
                query->col2.x,
                query->col3.x,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_n_f32(delta_x, query->col0.x),
            vmulq_n_f32(delta_y, query->col1.x),
            vmulq_n_f32(delta_z, query->col2.x),
            clip_x
        );
        build_corner_component_quad_f32(
            transform_component_quad_f32(
                query->col0.y,
                query->col1.y,
                query->col2.y,
                query->col3.y,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_n_f32(delta_x, query->col0.y),
            vmulq_n_f32(delta_y, query->col1.y),
            vmulq_n_f32(delta_z, query->col2.y),
            clip_y
        );
        build_corner_component_quad_f32(
            transform_component_quad_f32(
                query->col0.z,
                query->col1.z,
                query->col2.z,
                query->col3.z,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_n_f32(delta_x, query->col0.z),
            vmulq_n_f32(delta_y, query->col1.z),
            vmulq_n_f32(delta_z, query->col2.z),
            clip_z
        );
        build_corner_component_quad_f32(
            transform_component_quad_f32(
                query->col0.w,
                query->col1.w,
                query->col2.w,
                query->col3.w,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_n_f32(delta_x, query->col0.w),
            vmulq_n_f32(delta_y, query->col1.w),
            vmulq_n_f32(delta_z, query->col2.w),
            clip_w
        );
    }

    projected_minimum_x = vdupq_n_f32(FLT_MAX);
    projected_maximum_x = vdupq_n_f32(-FLT_MAX);
    projected_minimum_y = vdupq_n_f32(FLT_MAX);
    projected_maximum_y = vdupq_n_f32(-FLT_MAX);
    projected_nearest_depth = vdupq_n_f32(-FLT_MAX);

    for (corner = 0u; corner < 8u; ++corner) {
        float32x4_t inverse_w = vrecpeq_f32(clip_w[corner]);
        float32x4_t ndc_x;
        float32x4_t ndc_y;
        float32x4_t depth;

        inverse_w = vmulq_f32(
            inverse_w,
            vrecpsq_f32(clip_w[corner], inverse_w)
        );
        ndc_x = vmulq_f32(clip_x[corner], inverse_w);
        ndc_y = vmulq_f32(clip_y[corner], inverse_w);
        depth = vmulq_f32(clip_z[corner], inverse_w);

        if (query->clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = vfmaq_f32(
                vdupq_n_f32(0.5f),
                depth,
                vdupq_n_f32(0.5f)
            );
        }
        projected_minimum_x = ordered_min_quad_f32(
            projected_minimum_x,
            ndc_x
        );
        projected_maximum_x = ordered_max_quad_f32(
            projected_maximum_x,
            ndc_x
        );
        projected_minimum_y = ordered_min_quad_f32(
            projected_minimum_y,
            ndc_y
        );
        projected_maximum_y = ordered_max_quad_f32(
            projected_maximum_y,
            ndc_y
        );
        depth = clamp_quad_f32(depth, 0.0f, 1.0f);
        projected_nearest_depth = ordered_max_quad_f32(
            projected_nearest_depth,
            depth
        );
    }

    for (lane = 0u; lane < 4u; ++lane) {
        if (analytic[lane] != SOC_TRUE) {
            continue;
        }
        out_projected[lane].minimum_ndc_x = clamp_float_neon(
            get_quad_lane_f32(projected_minimum_x, lane),
            -1.0f,
            1.0f
        );
        out_projected[lane].maximum_ndc_x = clamp_float_neon(
            get_quad_lane_f32(projected_maximum_x, lane),
            -1.0f,
            1.0f
        );
        out_projected[lane].minimum_ndc_y = clamp_float_neon(
            get_quad_lane_f32(projected_minimum_y, lane),
            -1.0f,
            1.0f
        );
        out_projected[lane].maximum_ndc_y = clamp_float_neon(
            get_quad_lane_f32(projected_maximum_y, lane),
            -1.0f,
            1.0f
        );
        out_projected[lane].nearest_depth = clamp_float_neon(
            get_quad_lane_f32(projected_nearest_depth, lane),
            0.0f,
            1.0f
        );
        out_projection[lane] = SOC_AABB_PROJECTION_VALID;
    }
}

static soc_bool ranges_overlap(
    const void* left,
    size_t left_size,
    const void* right,
    size_t right_size
)
{
    const uintptr_t left_begin = (uintptr_t)left;
    const uintptr_t right_begin = (uintptr_t)right;
    uintptr_t left_end;
    uintptr_t right_end;

    if (left_size > UINTPTR_MAX - left_begin ||
        right_size > UINTPTR_MAX - right_begin) {
        return SOC_TRUE;
    }
    left_end = left_begin + left_size;
    right_end = right_begin + right_size;
    return left_begin < right_end && right_begin < left_end
        ? SOC_TRUE
        : SOC_FALSE;
}

static void accumulate_visibility(
    soc_visibility visibility,
    soc_occlusion_query_counts* counts
)
{
    if (visibility == SOC_VISIBILITY_UNKNOWN) {
        ++counts->unknown;
    } else if (visibility == SOC_VISIBILITY_OCCLUDED) {
        ++counts->occluded;
    } else {
        ++counts->visible;
    }
}

SOC_FORCE_INLINE_NEON void test_aabb_quad_layout_neon(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb bounds[4],
    soc_visibility out_visibility[4],
    soc_occlusion_query_counts* counts,
    soc_bool masked_layout
)
{
    soc_projected_aabb projected[4];
    soc_aabb_projection projection[4];
    uint32_t lane;

    project_aabb_quad_f32_neon(query, bounds, projected, projection);
    for (lane = 0u; lane < 4u; ++lane) {
        soc_visibility visibility;

        if (projection[lane] == SOC_AABB_PROJECTION_UNKNOWN) {
            visibility = SOC_VISIBILITY_UNKNOWN;
        } else if (projection[lane] == SOC_AABB_PROJECTION_OUTSIDE) {
            visibility = SOC_VISIBILITY_VISIBLE;
        } else if (masked_layout == SOC_TRUE) {
            visibility = soc_test_projected_aabb_masked_scalar(
                hiz,
                &projected[lane]
            );
        } else {
            visibility = soc_test_projected_aabb_dense_scalar(
                hiz,
                &projected[lane]
            );
        }
        out_visibility[lane] = visibility;
        accumulate_visibility(visibility, counts);
    }
}

static SOC_NOINLINE soc_result soc_occlusion_test_aabbs_quad_neon(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_occlusion_query_counts* out_counts
)
{
    soc_occlusion_query_counts counts = {0u, 0u, 0u};
    soc_result result;
    uint32_t index = 0u;

    result = soc_occlusion_validate_aabb_test(
        hiz,
        query,
        world_bounds,
        bounds_count,
        out_visibility,
        out_counts
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    if (ranges_overlap(
            world_bounds,
            (size_t)bounds_count * sizeof(*world_bounds),
            out_visibility,
            (size_t)bounds_count * sizeof(*out_visibility)
        ) == SOC_TRUE) {
        for (; index < bounds_count; ++index) {
            const soc_visibility visibility = soc_occlusion_test_aabb_scalar(
                hiz,
                query,
                &world_bounds[index]
            );

            out_visibility[index] = visibility;
            accumulate_visibility(visibility, &counts);
        }
        *out_counts = counts;
        return SOC_RESULT_OK;
    }

    if (hiz->masked == SOC_TRUE) {
        for (; index + 3u < bounds_count; index += 4u) {
            test_aabb_quad_layout_neon(
                hiz,
                query,
                &world_bounds[index],
                &out_visibility[index],
                &counts,
                SOC_TRUE
            );
        }
    } else {
        for (; index + 3u < bounds_count; index += 4u) {
            test_aabb_quad_layout_neon(
                hiz,
                query,
                &world_bounds[index],
                &out_visibility[index],
                &counts,
                SOC_FALSE
            );
        }
    }

    for (; index < bounds_count; ++index) {
        const soc_visibility visibility = soc_occlusion_test_aabb_scalar(
            hiz,
            query,
            &world_bounds[index]
        );

        out_visibility[index] = visibility;
        accumulate_visibility(visibility, &counts);
    }

    *out_counts = counts;
    return SOC_RESULT_OK;
}

soc_result soc_occlusion_test_aabbs_neon(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_occlusion_query_counts* out_counts
)
{
    if (bounds_count < 4u) {
        return soc_occlusion_test_aabbs(
            hiz,
            query,
            world_bounds,
            bounds_count,
            out_visibility,
            out_counts
        );
    }
    return soc_occlusion_test_aabbs_quad_neon(
        hiz,
        query,
        world_bounds,
        bounds_count,
        out_visibility,
        out_counts
    );
}

#endif
