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
#define SOC_PROJECTION_SAFETY_EPSILON (32.0 * DBL_EPSILON)
#define SOC_SMALL_ERROR_RATIO 0x1p-40
#define SOC_SMALL_ERROR_PROJECTION_MARGIN 0x1p-36

#if defined(_MSC_VER)
#define SOC_NOINLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_NOINLINE __attribute__((noinline))
#else
#define SOC_NOINLINE
#endif

static soc_bool finite_double_neon(double value)
{
    return value == value && value >= -DBL_MAX && value <= DBL_MAX
        ? SOC_TRUE
        : SOC_FALSE;
}

static soc_bool valid_aabb_neon(const soc_aabb* bounds)
{
    return finite_double_neon(bounds->min.x) == SOC_TRUE &&
        finite_double_neon(bounds->min.y) == SOC_TRUE &&
        finite_double_neon(bounds->min.z) == SOC_TRUE &&
        finite_double_neon(bounds->max.x) == SOC_TRUE &&
        finite_double_neon(bounds->max.y) == SOC_TRUE &&
        finite_double_neon(bounds->max.z) == SOC_TRUE &&
        bounds->min.x <= bounds->max.x &&
        bounds->min.y <= bounds->max.y &&
        bounds->min.z <= bounds->max.z
            ? SOC_TRUE
            : SOC_FALSE;
}

static double clamp_double_neon(
    double value,
    double minimum,
    double maximum
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

static float64x2_t load_pair_f64(double lane0, double lane1)
{
    float64x2_t result = vdupq_n_f64(lane0);

    return vsetq_lane_f64(lane1, result, 1);
}

static double get_pair_lane_f64(float64x2_t value, uint32_t lane)
{
    return lane == 0u
        ? vgetq_lane_f64(value, 0)
        : vgetq_lane_f64(value, 1);
}

static float64x2_t ordered_min_pair_f64(
    float64x2_t accumulated,
    float64x2_t candidate
)
{
    return vbslq_f64(
        vcltq_f64(candidate, accumulated),
        candidate,
        accumulated
    );
}

static float64x2_t ordered_max_pair_f64(
    float64x2_t accumulated,
    float64x2_t candidate
)
{
    return vbslq_f64(
        vcgtq_f64(candidate, accumulated),
        candidate,
        accumulated
    );
}

static float64x2_t clamp_pair_f64(
    float64x2_t value,
    double minimum,
    double maximum
)
{
    const float64x2_t minimum_pair = vdupq_n_f64(minimum);
    const float64x2_t maximum_pair = vdupq_n_f64(maximum);

    value = vbslq_f64(vcltq_f64(value, minimum_pair), minimum_pair, value);
    return vbslq_f64(vcgtq_f64(value, maximum_pair), maximum_pair, value);
}

static float64x2_t select_extreme_pair_f64(
    const soc_visibility_world_plane* plane,
    float64x2_t minimum,
    float64x2_t maximum,
    uint32_t component,
    soc_bool select_maximum
)
{
    double coefficient;

    if (component == 0u) {
        coefficient = plane->x;
    } else if (component == 1u) {
        coefficient = plane->y;
    } else {
        coefficient = plane->z;
    }
    if (select_maximum == SOC_TRUE) {
        return coefficient >= 0.0 ? maximum : minimum;
    }
    return coefficient >= 0.0 ? minimum : maximum;
}

static float64x2_t plane_distance_pair_f64(
    const soc_visibility_world_plane* plane,
    float64x2_t minimum_x,
    float64x2_t maximum_x,
    float64x2_t minimum_y,
    float64x2_t maximum_y,
    float64x2_t minimum_z,
    float64x2_t maximum_z,
    soc_bool select_maximum
)
{
    const float64x2_t x = select_extreme_pair_f64(
        plane,
        minimum_x,
        maximum_x,
        0u,
        select_maximum
    );
    const float64x2_t y = select_extreme_pair_f64(
        plane,
        minimum_y,
        maximum_y,
        1u,
        select_maximum
    );
    const float64x2_t z = select_extreme_pair_f64(
        plane,
        minimum_z,
        maximum_z,
        2u,
        select_maximum
    );
    float64x2_t result = vmulq_f64(vdupq_n_f64(plane->y), y);

    /* Match the normalized Scalar tree: y*y, then fused x*x and z*z. */
    result = vfmaq_f64(result, vdupq_n_f64(plane->x), x);
    result = vfmaq_f64(result, vdupq_n_f64(plane->z), z);
    return vaddq_f64(result, vdupq_n_f64(plane->d));
}

static float64x2_t transform_component_pair_f64(
    double col0,
    double col1,
    double col2,
    double col3,
    float64x2_t x,
    float64x2_t y,
    float64x2_t z
)
{
    float64x2_t result = vmulq_f64(vdupq_n_f64(col1), y);

    /* Match affine_component's explicit fused operation tree. */
    result = vfmaq_f64(result, vdupq_n_f64(col0), x);
    result = vfmaq_f64(result, vdupq_n_f64(col2), z);
    return vaddq_f64(result, vdupq_n_f64(col3));
}

static void build_corner_component_pair_f64(
    float64x2_t origin,
    float64x2_t axis_x,
    float64x2_t axis_y,
    float64x2_t axis_z,
    float64x2_t out_corners[8]
)
{
    out_corners[0] = origin;
    out_corners[1] = vaddq_f64(out_corners[0], axis_x);
    out_corners[2] = vaddq_f64(out_corners[0], axis_y);
    out_corners[3] = vaddq_f64(out_corners[1], axis_y);
    out_corners[4] = vaddq_f64(out_corners[0], axis_z);
    out_corners[5] = vaddq_f64(out_corners[1], axis_z);
    out_corners[6] = vaddq_f64(out_corners[2], axis_z);
    out_corners[7] = vaddq_f64(out_corners[3], axis_z);
}

static float64x2_t duplicate_active_lane_f64(
    float64x2_t value,
    const soc_bool active[2]
)
{
    if (active[0] == SOC_TRUE && active[1] != SOC_TRUE) {
        return vdupq_n_f64(vgetq_lane_f64(value, 0));
    }
    if (active[1] == SOC_TRUE && active[0] != SOC_TRUE) {
        return vdupq_n_f64(vgetq_lane_f64(value, 1));
    }
    return value;
}

static double projection_margin_f64(
    double clip_error_margin,
    double minimum_w
)
{
    double projection_margin;

    if (clip_error_margin <= minimum_w * SOC_SMALL_ERROR_RATIO) {
        projection_margin = SOC_PROJECTION_SAFETY_EPSILON +
            SOC_SMALL_ERROR_PROJECTION_MARGIN;
    } else {
        projection_margin = SOC_PROJECTION_SAFETY_EPSILON +
            8.0 * clip_error_margin /
                (minimum_w - clip_error_margin);
    }
    if (finite_double_neon(projection_margin) != SOC_TRUE ||
        projection_margin < 0.0 ||
        projection_margin > 1.0) {
        projection_margin = 1.0;
    }
    return projection_margin;
}

static void project_aabb_pair_f64_neon(
    const soc_aabb_query_context* query,
    const soc_aabb bounds[2],
    soc_projected_aabb out_projected[2],
    soc_aabb_projection out_projection[2]
)
{
    static const soc_aabb zero_bounds = {
        .min = {0.0f, 0.0f, 0.0f},
        .max = {0.0f, 0.0f, 0.0f},
    };
    soc_aabb safe_bounds[2];
    soc_bool analytic[2] = {SOC_FALSE, SOC_FALSE};
    soc_bool valid[2];
    soc_bool use_scalar[2] = {SOC_FALSE, SOC_FALSE};
    uint32_t all_outside_mask[2] = {
        SOC_CLIP_ALL_BITS,
        SOC_CLIP_ALL_BITS,
    };
    float64x2_t minimum_x;
    float64x2_t maximum_x;
    float64x2_t minimum_y;
    float64x2_t maximum_y;
    float64x2_t minimum_z;
    float64x2_t maximum_z;
    float64x2_t bounds_scale;
    float64x2_t clip_error_margin;
    float64x2_t minimum_w;
    float64x2_t minimum_near;
    float64x2_t clip_x[8];
    float64x2_t clip_y[8];
    float64x2_t clip_z[8];
    float64x2_t clip_w[8];
    float64x2_t projected_minimum_x;
    float64x2_t projected_maximum_x;
    float64x2_t projected_minimum_y;
    float64x2_t projected_maximum_y;
    float64x2_t projected_nearest_depth;
    soc_bool projected_finite[2] = {SOC_TRUE, SOC_TRUE};
    uint32_t lane;
    uint32_t plane;
    uint32_t corner;

    for (lane = 0u; lane < 2u; ++lane) {
        valid[lane] = valid_aabb_neon(&bounds[lane]);
        safe_bounds[lane] = valid[lane] == SOC_TRUE
            ? bounds[lane]
            : zero_bounds;
        out_projection[lane] = SOC_AABB_PROJECTION_UNKNOWN;
        if (valid[lane] != SOC_TRUE) {
            out_projection[lane] = soc_project_aabb_scalar(
                query,
                &bounds[lane],
                &out_projected[lane]
            );
        }
    }

    minimum_x = load_pair_f64(
        safe_bounds[0].min.x,
        safe_bounds[1].min.x
    );
    maximum_x = load_pair_f64(
        safe_bounds[0].max.x,
        safe_bounds[1].max.x
    );
    minimum_y = load_pair_f64(
        safe_bounds[0].min.y,
        safe_bounds[1].min.y
    );
    maximum_y = load_pair_f64(
        safe_bounds[0].max.y,
        safe_bounds[1].max.y
    );
    minimum_z = load_pair_f64(
        safe_bounds[0].min.z,
        safe_bounds[1].min.z
    );
    maximum_z = load_pair_f64(
        safe_bounds[0].max.z,
        safe_bounds[1].max.z
    );

    bounds_scale = vdupq_n_f64(1.0);
    bounds_scale = ordered_max_pair_f64(bounds_scale, vabsq_f64(minimum_x));
    bounds_scale = ordered_max_pair_f64(bounds_scale, vabsq_f64(minimum_y));
    bounds_scale = ordered_max_pair_f64(bounds_scale, vabsq_f64(minimum_z));
    bounds_scale = ordered_max_pair_f64(bounds_scale, vabsq_f64(maximum_x));
    bounds_scale = ordered_max_pair_f64(bounds_scale, vabsq_f64(maximum_y));
    bounds_scale = ordered_max_pair_f64(bounds_scale, vabsq_f64(maximum_z));
    clip_error_margin = vmulq_f64(
        vdupq_n_f64(query->transform_error_scale),
        bounds_scale
    );

    for (plane = 0u;
         plane < SOC_VISIBILITY_CLIP_PLANE_COUNT;
         ++plane) {
        const float64x2_t distance = plane_distance_pair_f64(
            &query->clip_planes[plane],
            minimum_x,
            maximum_x,
            minimum_y,
            maximum_y,
            minimum_z,
            maximum_z,
            SOC_TRUE
        );

        for (lane = 0u; lane < 2u; ++lane) {
            const double scalar_distance = get_pair_lane_f64(distance, lane);
            const double scalar_margin = get_pair_lane_f64(
                clip_error_margin,
                lane
            );

            if (valid[lane] != SOC_TRUE) {
                continue;
            }
            if (finite_double_neon(scalar_distance) != SOC_TRUE ||
                finite_double_neon(scalar_margin) != SOC_TRUE) {
                use_scalar[lane] = SOC_TRUE;
            } else if (scalar_distance > scalar_margin) {
                all_outside_mask[lane] &= ~(UINT32_C(1) << plane);
            } else if (scalar_distance >= -scalar_margin) {
                use_scalar[lane] = SOC_TRUE;
            }
        }
    }

    minimum_w = plane_distance_pair_f64(
        &query->w_plane,
        minimum_x,
        maximum_x,
        minimum_y,
        maximum_y,
        minimum_z,
        maximum_z,
        SOC_FALSE
    );
    minimum_near = plane_distance_pair_f64(
        &query->clip_planes[query->near_clip_plane_index],
        minimum_x,
        maximum_x,
        minimum_y,
        maximum_y,
        minimum_z,
        maximum_z,
        SOC_FALSE
    );

    for (lane = 0u; lane < 2u; ++lane) {
        const double scalar_margin = get_pair_lane_f64(
            clip_error_margin,
            lane
        );
        const double scalar_minimum_w = get_pair_lane_f64(minimum_w, lane);
        const double scalar_minimum_near = get_pair_lane_f64(
            minimum_near,
            lane
        );
        soc_bool has_nonpositive_w = SOC_FALSE;
        soc_bool has_corner_outside_near = SOC_FALSE;

        if (valid[lane] != SOC_TRUE) {
            continue;
        }
        if (finite_double_neon(scalar_minimum_w) != SOC_TRUE ||
            finite_double_neon(scalar_minimum_near) != SOC_TRUE) {
            use_scalar[lane] = SOC_TRUE;
        } else {
            if (scalar_minimum_w < -scalar_margin) {
                has_nonpositive_w = SOC_TRUE;
            } else if (scalar_minimum_w <= scalar_margin) {
                use_scalar[lane] = SOC_TRUE;
            }
            if (scalar_minimum_near < -scalar_margin) {
                has_corner_outside_near = SOC_TRUE;
            } else if (scalar_minimum_near <= scalar_margin) {
                use_scalar[lane] = SOC_TRUE;
            }
        }

        if (use_scalar[lane] == SOC_TRUE) {
            out_projection[lane] = soc_project_aabb_scalar(
                query,
                &bounds[lane],
                &out_projected[lane]
            );
        } else if (has_nonpositive_w == SOC_TRUE) {
            out_projection[lane] = SOC_AABB_PROJECTION_UNKNOWN;
        } else if (has_corner_outside_near == SOC_TRUE &&
            (all_outside_mask[lane] & query->near_clip_plane_bit) == 0u) {
            out_projection[lane] = SOC_AABB_PROJECTION_UNKNOWN;
        } else if (all_outside_mask[lane] != 0u) {
            out_projection[lane] = SOC_AABB_PROJECTION_OUTSIDE;
        } else {
            analytic[lane] = SOC_TRUE;
        }
    }

    if (analytic[0] != SOC_TRUE && analytic[1] != SOC_TRUE) {
        return;
    }

    minimum_x = duplicate_active_lane_f64(minimum_x, analytic);
    maximum_x = duplicate_active_lane_f64(maximum_x, analytic);
    minimum_y = duplicate_active_lane_f64(minimum_y, analytic);
    maximum_y = duplicate_active_lane_f64(maximum_y, analytic);
    minimum_z = duplicate_active_lane_f64(minimum_z, analytic);
    maximum_z = duplicate_active_lane_f64(maximum_z, analytic);
    clip_error_margin = duplicate_active_lane_f64(
        clip_error_margin,
        analytic
    );
    minimum_w = duplicate_active_lane_f64(minimum_w, analytic);

    {
        const float64x2_t delta_x = vsubq_f64(maximum_x, minimum_x);
        const float64x2_t delta_y = vsubq_f64(maximum_y, minimum_y);
        const float64x2_t delta_z = vsubq_f64(maximum_z, minimum_z);

        build_corner_component_pair_f64(
            transform_component_pair_f64(
                query->col0.x,
                query->col1.x,
                query->col2.x,
                query->col3.x,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_f64(vdupq_n_f64(query->col0.x), delta_x),
            vmulq_f64(vdupq_n_f64(query->col1.x), delta_y),
            vmulq_f64(vdupq_n_f64(query->col2.x), delta_z),
            clip_x
        );
        build_corner_component_pair_f64(
            transform_component_pair_f64(
                query->col0.y,
                query->col1.y,
                query->col2.y,
                query->col3.y,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_f64(vdupq_n_f64(query->col0.y), delta_x),
            vmulq_f64(vdupq_n_f64(query->col1.y), delta_y),
            vmulq_f64(vdupq_n_f64(query->col2.y), delta_z),
            clip_y
        );
        build_corner_component_pair_f64(
            transform_component_pair_f64(
                query->col0.z,
                query->col1.z,
                query->col2.z,
                query->col3.z,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_f64(vdupq_n_f64(query->col0.z), delta_x),
            vmulq_f64(vdupq_n_f64(query->col1.z), delta_y),
            vmulq_f64(vdupq_n_f64(query->col2.z), delta_z),
            clip_z
        );
        build_corner_component_pair_f64(
            transform_component_pair_f64(
                query->col0.w,
                query->col1.w,
                query->col2.w,
                query->col3.w,
                minimum_x,
                minimum_y,
                minimum_z
            ),
            vmulq_f64(vdupq_n_f64(query->col0.w), delta_x),
            vmulq_f64(vdupq_n_f64(query->col1.w), delta_y),
            vmulq_f64(vdupq_n_f64(query->col2.w), delta_z),
            clip_w
        );
    }

    projected_minimum_x = vdupq_n_f64(DBL_MAX);
    projected_maximum_x = vdupq_n_f64(-DBL_MAX);
    projected_minimum_y = vdupq_n_f64(DBL_MAX);
    projected_maximum_y = vdupq_n_f64(-DBL_MAX);
    projected_nearest_depth = query->depth_direction == SOC_DEPTH_REVERSED
        ? vdupq_n_f64(-DBL_MAX)
        : vdupq_n_f64(DBL_MAX);

    for (corner = 0u; corner < 8u; ++corner) {
        const float64x2_t inverse_w = vdivq_f64(
            vdupq_n_f64(1.0),
            clip_w[corner]
        );
        const float64x2_t ndc_x = vmulq_f64(
            clip_x[corner],
            inverse_w
        );
        const float64x2_t ndc_y = vmulq_f64(
            clip_y[corner],
            inverse_w
        );
        float64x2_t depth = vmulq_f64(clip_z[corner], inverse_w);

        if (query->clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = vfmaq_f64(
                vdupq_n_f64(0.5),
                depth,
                vdupq_n_f64(0.5)
            );
        }
        for (lane = 0u; lane < 2u; ++lane) {
            if (analytic[lane] == SOC_TRUE &&
                (finite_double_neon(get_pair_lane_f64(ndc_x, lane)) !=
                        SOC_TRUE ||
                    finite_double_neon(get_pair_lane_f64(ndc_y, lane)) !=
                        SOC_TRUE ||
                    finite_double_neon(get_pair_lane_f64(depth, lane)) !=
                        SOC_TRUE)) {
                projected_finite[lane] = SOC_FALSE;
            }
        }

        projected_minimum_x = ordered_min_pair_f64(
            projected_minimum_x,
            ndc_x
        );
        projected_maximum_x = ordered_max_pair_f64(
            projected_maximum_x,
            ndc_x
        );
        projected_minimum_y = ordered_min_pair_f64(
            projected_minimum_y,
            ndc_y
        );
        projected_maximum_y = ordered_max_pair_f64(
            projected_maximum_y,
            ndc_y
        );
        depth = clamp_pair_f64(depth, 0.0, 1.0);
        projected_nearest_depth =
            query->depth_direction == SOC_DEPTH_REVERSED
                ? ordered_max_pair_f64(projected_nearest_depth, depth)
                : ordered_min_pair_f64(projected_nearest_depth, depth);
    }

    for (lane = 0u; lane < 2u; ++lane) {
        double projection_margin;

        if (analytic[lane] != SOC_TRUE) {
            continue;
        }
        if (projected_finite[lane] != SOC_TRUE) {
            out_projection[lane] = soc_project_aabb_scalar(
                query,
                &bounds[lane],
                &out_projected[lane]
            );
            continue;
        }
        projection_margin = projection_margin_f64(
            get_pair_lane_f64(clip_error_margin, lane),
            get_pair_lane_f64(minimum_w, lane)
        );
        out_projected[lane].minimum_ndc_x = clamp_double_neon(
            get_pair_lane_f64(projected_minimum_x, lane) -
                projection_margin,
            -1.0,
            1.0
        );
        out_projected[lane].maximum_ndc_x = clamp_double_neon(
            get_pair_lane_f64(projected_maximum_x, lane) +
                projection_margin,
            -1.0,
            1.0
        );
        out_projected[lane].minimum_ndc_y = clamp_double_neon(
            get_pair_lane_f64(projected_minimum_y, lane) -
                projection_margin,
            -1.0,
            1.0
        );
        out_projected[lane].maximum_ndc_y = clamp_double_neon(
            get_pair_lane_f64(projected_maximum_y, lane) +
                projection_margin,
            -1.0,
            1.0
        );
        out_projected[lane].nearest_depth = clamp_double_neon(
            query->depth_direction == SOC_DEPTH_REVERSED
                ? get_pair_lane_f64(projected_nearest_depth, lane) +
                    projection_margin
                : get_pair_lane_f64(projected_nearest_depth, lane) -
                    projection_margin,
            0.0,
            1.0
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

static SOC_NOINLINE soc_result soc_occlusion_test_aabbs_pair_neon(
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

    if (query->all_finite != SOC_TRUE ||
        ranges_overlap(
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

    for (; index + 1u < bounds_count; index += 2u) {
        soc_projected_aabb projected[2];
        soc_aabb_projection projection[2];
        uint32_t lane;

        project_aabb_pair_f64_neon(
            query,
            &world_bounds[index],
            projected,
            projection
        );
        for (lane = 0u; lane < 2u; ++lane) {
            soc_visibility visibility;

            if (projection[lane] == SOC_AABB_PROJECTION_UNKNOWN) {
                visibility = SOC_VISIBILITY_UNKNOWN;
            } else if (projection[lane] == SOC_AABB_PROJECTION_OUTSIDE) {
                visibility = SOC_VISIBILITY_VISIBLE;
            } else {
                visibility = soc_test_projected_aabb_scalar(
                    hiz,
                    query,
                    &projected[lane]
                );
            }
            out_visibility[index + lane] = visibility;
            accumulate_visibility(visibility, &counts);
        }
    }

    if (index < bounds_count) {
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
    /* Preserve the single-query path as a direct Scalar tail call. */
    if (bounds_count < 2u) {
        return soc_occlusion_test_aabbs(
            hiz,
            query,
            world_bounds,
            bounds_count,
            out_visibility,
            out_counts
        );
    }
    return soc_occlusion_test_aabbs_pair_neon(
        hiz,
        query,
        world_bounds,
        bounds_count,
        out_visibility,
        out_counts
    );
}

#endif
