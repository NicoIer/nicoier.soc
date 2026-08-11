#include "occlusion/soc_visibility.h"

#if defined(SOC_BUILD_AARCH32_NEON_FMA)

#if !defined(__arm__) || !defined(__ARM_NEON) || \
    !defined(__ARM_FEATURE_FMA)
#error "SOC_BUILD_AARCH32_NEON_FMA requires ARMv7 NEON with VFPv4 FMA"
#endif

#include <arm_neon.h>

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#define SOC_CLIP_ALL_BITS \
    ((UINT32_C(1) << SOC_VISIBILITY_CLIP_PLANE_COUNT) - 1u)

#if defined(__clang__) || defined(__GNUC__)
#define SOC_NOINLINE __attribute__((noinline))
#define SOC_FORCE_INLINE_NEON \
    static inline __attribute__((always_inline))
#else
#define SOC_NOINLINE
#define SOC_FORCE_INLINE_NEON static inline
#endif

SOC_FORCE_INLINE_NEON soc_bool valid_aabb_neon(
    const soc_aabb* bounds
)
{
    return bounds->min.x <= bounds->max.x &&
        bounds->min.y <= bounds->max.y &&
        bounds->min.z <= bounds->max.z
            ? SOC_TRUE
            : SOC_FALSE;
}

SOC_FORCE_INLINE_NEON float clamp_float_neon(
    float value,
    float minimum,
    float maximum
)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

SOC_FORCE_INLINE_NEON float32x4_t load_quad_f32(
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

SOC_FORCE_INLINE_NEON uint32x4_t load_quad_u32(
    uint32_t lane0,
    uint32_t lane1,
    uint32_t lane2,
    uint32_t lane3
)
{
    uint32x4_t result = vdupq_n_u32(lane0);

    result = vsetq_lane_u32(lane1, result, 1);
    result = vsetq_lane_u32(lane2, result, 2);
    return vsetq_lane_u32(lane3, result, 3);
}

SOC_FORCE_INLINE_NEON float get_quad_lane_f32(
    float32x4_t value,
    uint32_t lane
)
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

SOC_FORCE_INLINE_NEON float32x4_t ordered_min_quad_f32(
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

SOC_FORCE_INLINE_NEON float32x4_t ordered_max_quad_f32(
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

SOC_FORCE_INLINE_NEON float32x4_t clamp_quad_f32(
    float32x4_t value,
    float minimum,
    float maximum
)
{
    const float32x4_t minimum_quad = vdupq_n_f32(minimum);
    const float32x4_t maximum_quad = vdupq_n_f32(maximum);

    value = vbslq_f32(
        vcltq_f32(value, minimum_quad),
        minimum_quad,
        value
    );
    return vbslq_f32(
        vcgtq_f32(value, maximum_quad),
        maximum_quad,
        value
    );
}

SOC_FORCE_INLINE_NEON float32x4_t select_extreme_quad_f32(
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

SOC_FORCE_INLINE_NEON float32x4_t plane_distance_quad_f32(
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

SOC_FORCE_INLINE_NEON float32x4_t transform_component_quad_f32(
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

/*
 * Traverse the eight corners in Gray-code order:
 *   000, 001, 011, 010, 110, 111, 101, 100.
 * Only one world axis changes per step, so four current clip components are
 * enough. This avoids materializing clip_[xyzw][8] on AArch32's small SIMD
 * register file and stack.
 */
static SOC_NOINLINE void project_aabb_quad_f32_neon(
    const soc_aabb_query_context* query,
    const soc_aabb bounds[4],
    soc_projected_aabb out_projected[4],
    soc_aabb_projection out_projection[4]
)
{
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
    uint32x4_t valid_mask;
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
        out_projection[lane] = SOC_AABB_PROJECTION_UNKNOWN;
    }

    valid_mask = load_quad_u32(
        valid[0] == SOC_TRUE ? UINT32_MAX : 0u,
        valid[1] == SOC_TRUE ? UINT32_MAX : 0u,
        valid[2] == SOC_TRUE ? UINT32_MAX : 0u,
        valid[3] == SOC_TRUE ? UINT32_MAX : 0u
    );
    minimum_x = vbslq_f32(
        valid_mask,
        load_quad_f32(
            bounds[0].min.x,
            bounds[1].min.x,
            bounds[2].min.x,
            bounds[3].min.x
        ),
        vdupq_n_f32(0.0f)
    );
    maximum_x = vbslq_f32(
        valid_mask,
        load_quad_f32(
            bounds[0].max.x,
            bounds[1].max.x,
            bounds[2].max.x,
            bounds[3].max.x
        ),
        vdupq_n_f32(0.0f)
    );
    minimum_y = vbslq_f32(
        valid_mask,
        load_quad_f32(
            bounds[0].min.y,
            bounds[1].min.y,
            bounds[2].min.y,
            bounds[3].min.y
        ),
        vdupq_n_f32(0.0f)
    );
    maximum_y = vbslq_f32(
        valid_mask,
        load_quad_f32(
            bounds[0].max.y,
            bounds[1].max.y,
            bounds[2].max.y,
            bounds[3].max.y
        ),
        vdupq_n_f32(0.0f)
    );
    minimum_z = vbslq_f32(
        valid_mask,
        load_quad_f32(
            bounds[0].min.z,
            bounds[1].min.z,
            bounds[2].min.z,
            bounds[3].min.z
        ),
        vdupq_n_f32(0.0f)
    );
    maximum_z = vbslq_f32(
        valid_mask,
        load_quad_f32(
            bounds[0].max.z,
            bounds[1].max.z,
            bounds[2].max.z,
            bounds[3].max.z
        ),
        vdupq_n_f32(0.0f)
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
                all_outside_mask[lane] &=
                    ~(UINT32_C(1) << plane);
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
        const float scalar_minimum_w =
            get_quad_lane_f32(minimum_w, lane);
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
        const float32x4_t delta_x =
            vsubq_f32(maximum_x, minimum_x);
        const float32x4_t delta_y =
            vsubq_f32(maximum_y, minimum_y);
        const float32x4_t delta_z =
            vsubq_f32(maximum_z, minimum_z);
        float32x4_t clip_x = transform_component_quad_f32(
            query->col0.x,
            query->col1.x,
            query->col2.x,
            query->col3.x,
            minimum_x,
            minimum_y,
            minimum_z
        );
        float32x4_t clip_y = transform_component_quad_f32(
            query->col0.y,
            query->col1.y,
            query->col2.y,
            query->col3.y,
            minimum_x,
            minimum_y,
            minimum_z
        );
        float32x4_t clip_z = transform_component_quad_f32(
            query->col0.z,
            query->col1.z,
            query->col2.z,
            query->col3.z,
            minimum_x,
            minimum_y,
            minimum_z
        );
        float32x4_t clip_w = transform_component_quad_f32(
            query->col0.w,
            query->col1.w,
            query->col2.w,
            query->col3.w,
            minimum_x,
            minimum_y,
            minimum_z
        );

        projected_minimum_x = vdupq_n_f32(FLT_MAX);
        projected_maximum_x = vdupq_n_f32(-FLT_MAX);
        projected_minimum_y = vdupq_n_f32(FLT_MAX);
        projected_maximum_y = vdupq_n_f32(-FLT_MAX);
        projected_nearest_depth = vdupq_n_f32(-FLT_MAX);

        for (corner = 0u; corner < 8u; ++corner) {
            float32x4_t inverse_w;
            float32x4_t ndc_x;
            float32x4_t ndc_y;
            float32x4_t depth;

            if (corner != 0u) {
                const soc_visibility_clip_vertex* column;
                float32x4_t delta;
                float coefficient_x;
                float coefficient_y;
                float coefficient_z;
                float coefficient_w;

                if (corner == 4u) {
                    column = &query->col2;
                    delta = delta_z;
                } else if (corner == 2u || corner == 6u) {
                    column = &query->col1;
                    delta = delta_y;
                } else {
                    column = &query->col0;
                    delta = delta_x;
                }

                coefficient_x = column->x;
                coefficient_y = column->y;
                coefficient_z = column->z;
                coefficient_w = column->w;
                if (corner == 3u || corner >= 6u) {
                    coefficient_x = -coefficient_x;
                    coefficient_y = -coefficient_y;
                    coefficient_z = -coefficient_z;
                    coefficient_w = -coefficient_w;
                }

                clip_x = vfmaq_n_f32(
                    clip_x,
                    delta,
                    coefficient_x
                );
                clip_y = vfmaq_n_f32(
                    clip_y,
                    delta,
                    coefficient_y
                );
                clip_z = vfmaq_n_f32(
                    clip_z,
                    delta,
                    coefficient_z
                );
                clip_w = vfmaq_n_f32(
                    clip_w,
                    delta,
                    coefficient_w
                );
            }

            inverse_w = vrecpeq_f32(clip_w);
            inverse_w = vmulq_f32(
                inverse_w,
                vrecpsq_f32(clip_w, inverse_w)
            );
            ndc_x = vmulq_f32(clip_x, inverse_w);
            ndc_y = vmulq_f32(clip_y, inverse_w);
            depth = vmulq_f32(clip_z, inverse_w);

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

SOC_FORCE_INLINE_NEON soc_bool ranges_overlap(
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

SOC_FORCE_INLINE_NEON void accumulate_visibility(
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

    project_aabb_quad_f32_neon(
        query,
        bounds,
        projected,
        projection
    );
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
            const soc_visibility visibility =
                soc_occlusion_test_aabb_scalar(
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
        const soc_visibility visibility =
            soc_occlusion_test_aabb_scalar(
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
