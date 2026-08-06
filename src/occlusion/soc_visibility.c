#include "occlusion/soc_visibility.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
#include <arm64_neon.h>
#endif

#define SOC_AABB_CORNER_COUNT 8u
#define SOC_PROJECTION_SAFETY_EPSILON (32.0 * DBL_EPSILON)
#define SOC_TRANSFORM_ERROR_FACTOR (128.0 * DBL_EPSILON)
#define SOC_SMALL_ERROR_RATIO 0x1p-40
#define SOC_SMALL_ERROR_PROJECTION_MARGIN 0x1p-36

#define SOC_CLIP_MINIMUM_Z_BIT (UINT32_C(1) << 4u)
#define SOC_CLIP_MAXIMUM_Z_BIT (UINT32_C(1) << 5u)
#define SOC_CLIP_ALL_BITS \
    ((UINT32_C(1) << SOC_VISIBILITY_CLIP_PLANE_COUNT) - 1u)

#if defined(_MSC_VER)
#define SOC_FORCE_INLINE static __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_FORCE_INLINE static inline __attribute__((always_inline))
#else
#define SOC_FORCE_INLINE static inline
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
static double fused_multiply_add_double(
    double left,
    double right,
    double addend
)
{
#if defined(_MSC_VER) && !defined(__clang__)
    float64x2_t result = vdupq_n_f64(addend);

    result = vfmaq_f64(
        result,
        vdupq_n_f64(left),
        vdupq_n_f64(right)
    );
    return vgetq_lane_f64(result, 0);
#else
    return __builtin_fma(left, right, addend);
#endif
}
#endif

static double absolute_double(double value)
{
    return value < 0.0 ? -value : value;
}

static double maximum_double(double left, double right)
{
    return left > right ? left : right;
}

static double affine_component(
    double col0,
    double col1,
    double col2,
    double col3,
    double x,
    double y,
    double z
)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    double result = col1 * y;

    result = fused_multiply_add_double(col0, x, result);
    result = fused_multiply_add_double(col2, z, result);
    return result + col3;
#else
    return col0 * x + col1 * y + col2 * z + col3;
#endif
}

static double remap_negative_one_to_one_depth(double depth)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return fused_multiply_add_double(depth, 0.5, 0.5);
#else
    return depth * 0.5 + 0.5;
#endif
}

static soc_bool valid_aabb(const soc_aabb* bounds)
{
    if (bounds == NULL) {
        return SOC_FALSE;
    }

    return bounds->min.x <= bounds->max.x &&
        bounds->min.y <= bounds->max.y &&
        bounds->min.z <= bounds->max.z
        ? SOC_TRUE
        : SOC_FALSE;
}

static soc_visibility_clip_vertex transform_point(
    const soc_aabb_query_context* query,
    double x,
    double y,
    double z
)
{
    const soc_visibility_clip_vertex result = {
        affine_component(
            query->col0.x,
            query->col1.x,
            query->col2.x,
            query->col3.x,
            x,
            y,
            z
        ),
        affine_component(
            query->col0.y,
            query->col1.y,
            query->col2.y,
            query->col3.y,
            x,
            y,
            z
        ),
        affine_component(
            query->col0.z,
            query->col1.z,
            query->col2.z,
            query->col3.z,
            x,
            y,
            z
        ),
        affine_component(
            query->col0.w,
            query->col1.w,
            query->col2.w,
            query->col3.w,
            x,
            y,
            z
        ),
    };
    return result;
}

static soc_visibility_clip_vertex add_clip_vertices(
    const soc_visibility_clip_vertex* left,
    const soc_visibility_clip_vertex* right
)
{
    const soc_visibility_clip_vertex result = {
        left->x + right->x,
        left->y + right->y,
        left->z + right->z,
        left->w + right->w,
    };
    return result;
}

static double clip_row_scale(
    double col0,
    double col1,
    double col2,
    double col3
)
{
    return absolute_double(col0) + absolute_double(col1) +
        absolute_double(col2) + absolute_double(col3);
}

static soc_visibility_world_plane make_world_plane(
    double x,
    double y,
    double z,
    double d
)
{
    const soc_visibility_world_plane plane = {x, y, z, d};
    return plane;
}

static double maximum_plane_distance(
    const soc_visibility_world_plane* plane,
    const soc_aabb* bounds
)
{
    return affine_component(
        plane->x,
        plane->y,
        plane->z,
        plane->d,
        plane->x >= 0.0 ? bounds->max.x : bounds->min.x,
        plane->y >= 0.0 ? bounds->max.y : bounds->min.y,
        plane->z >= 0.0 ? bounds->max.z : bounds->min.z
    );
}

static double minimum_plane_distance(
    const soc_visibility_world_plane* plane,
    const soc_aabb* bounds
)
{
    return affine_component(
        plane->x,
        plane->y,
        plane->z,
        plane->d,
        plane->x >= 0.0 ? bounds->min.x : bounds->max.x,
        plane->y >= 0.0 ? bounds->min.y : bounds->max.y,
        plane->z >= 0.0 ? bounds->min.z : bounds->max.z
    );
}

void soc_aabb_query_context_initialize(
    const soc_frame_desc* frame,
    soc_aabb_query_context* out_query
)
{
    soc_aabb_query_context query = {
        .col0 = {
            frame->clip_from_world.col0.x,
            frame->clip_from_world.col0.y,
            frame->clip_from_world.col0.z,
            frame->clip_from_world.col0.w,
        },
        .col1 = {
            frame->clip_from_world.col1.x,
            frame->clip_from_world.col1.y,
            frame->clip_from_world.col1.z,
            frame->clip_from_world.col1.w,
        },
        .col2 = {
            frame->clip_from_world.col2.x,
            frame->clip_from_world.col2.y,
            frame->clip_from_world.col2.z,
            frame->clip_from_world.col2.w,
        },
        .col3 = {
            frame->clip_from_world.col3.x,
            frame->clip_from_world.col3.y,
            frame->clip_from_world.col3.z,
            frame->clip_from_world.col3.w,
        },
        .clip_planes = {{0.0, 0.0, 0.0, 0.0}},
        .w_plane = {0.0, 0.0, 0.0, 0.0},
        .transform_error_scale = 0.0,
        .clip_depth_range = frame->clip_depth_range,
    };
    double maximum_row_scale = clip_row_scale(
        query.col0.x,
        query.col1.x,
        query.col2.x,
        query.col3.x
    );

    maximum_row_scale = maximum_double(
        maximum_row_scale,
        clip_row_scale(
            query.col0.y,
            query.col1.y,
            query.col2.y,
            query.col3.y
        )
    );
    maximum_row_scale = maximum_double(
        maximum_row_scale,
        clip_row_scale(
            query.col0.z,
            query.col1.z,
            query.col2.z,
            query.col3.z
        )
    );
    maximum_row_scale = maximum_double(
        maximum_row_scale,
        clip_row_scale(
            query.col0.w,
            query.col1.w,
            query.col2.w,
            query.col3.w
        )
    );
    query.transform_error_scale =
        maximum_row_scale * SOC_TRANSFORM_ERROR_FACTOR;

    /*
     * Convert the six homogeneous clip inequalities to world-space planes
     * once per batch. AABB extrema can then be selected analytically without
     * transforming all eight corners just to classify the box.
     */
    query.clip_planes[0] = make_world_plane(
        query.col0.x + query.col0.w,
        query.col1.x + query.col1.w,
        query.col2.x + query.col2.w,
        query.col3.x + query.col3.w
    );
    query.clip_planes[1] = make_world_plane(
        query.col0.w - query.col0.x,
        query.col1.w - query.col1.x,
        query.col2.w - query.col2.x,
        query.col3.w - query.col3.x
    );
    query.clip_planes[2] = make_world_plane(
        query.col0.y + query.col0.w,
        query.col1.y + query.col1.w,
        query.col2.y + query.col2.w,
        query.col3.y + query.col3.w
    );
    query.clip_planes[3] = make_world_plane(
        query.col0.w - query.col0.y,
        query.col1.w - query.col1.y,
        query.col2.w - query.col2.y,
        query.col3.w - query.col3.y
    );
    query.clip_planes[4] = frame->clip_depth_range ==
        SOC_CLIP_DEPTH_ZERO_TO_ONE
            ? make_world_plane(
                query.col0.z,
                query.col1.z,
                query.col2.z,
                query.col3.z
            )
            : make_world_plane(
                query.col0.z + query.col0.w,
                query.col1.z + query.col1.w,
                query.col2.z + query.col2.w,
                query.col3.z + query.col3.w
            );
    query.clip_planes[5] = make_world_plane(
        query.col0.w - query.col0.z,
        query.col1.w - query.col1.z,
        query.col2.w - query.col2.z,
        query.col3.w - query.col3.z
    );
    query.w_plane = make_world_plane(
        query.col0.w,
        query.col1.w,
        query.col2.w,
        query.col3.w
    );
    *out_query = query;
}

static double clamp_double(double value, double minimum, double maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

SOC_FORCE_INLINE soc_aabb_projection project_aabb_scalar_impl(
    const soc_aabb_query_context* query,
    const soc_aabb* bounds,
    soc_projected_aabb* out_projected
)
{
    soc_visibility_clip_vertex clip_corners[SOC_AABB_CORNER_COUNT];
    soc_visibility_clip_vertex axis_x;
    soc_visibility_clip_vertex axis_y;
    soc_visibility_clip_vertex axis_z;
    uint32_t all_outside_mask = SOC_CLIP_ALL_BITS;
    soc_bool has_nonpositive_w = SOC_FALSE;
    soc_bool has_corner_outside_near_clip_plane = SOC_FALSE;
    soc_bool use_direct_corners = SOC_FALSE;
    double bounds_scale = 1.0;
    double minimum_w = DBL_MAX;
    double minimum_near_clip_distance;
    double clip_error_margin;
    double projection_margin;
    uint32_t corner;
    uint32_t plane;

    if (valid_aabb(bounds) != SOC_TRUE || out_projected == NULL) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }

    bounds_scale = maximum_double(
        bounds_scale,
        absolute_double(bounds->min.x)
    );
    bounds_scale = maximum_double(
        bounds_scale,
        absolute_double(bounds->min.y)
    );
    bounds_scale = maximum_double(
        bounds_scale,
        absolute_double(bounds->min.z)
    );
    bounds_scale = maximum_double(
        bounds_scale,
        absolute_double(bounds->max.x)
    );
    bounds_scale = maximum_double(
        bounds_scale,
        absolute_double(bounds->max.y)
    );
    bounds_scale = maximum_double(
        bounds_scale,
        absolute_double(bounds->max.z)
    );
    clip_error_margin = query->transform_error_scale * bounds_scale;

    /*
     * The positive vertex is the exact affine maximum for each clip plane.
     * The scale-aware margin distinguishes confident classifications from
     * boundary cases, which take the direct-corner path below.
     */
    for (plane = 0u;
         plane < SOC_VISIBILITY_CLIP_PLANE_COUNT;
         ++plane) {
        const double maximum_distance = maximum_plane_distance(
            &query->clip_planes[plane],
            bounds
        );

        if (maximum_distance > clip_error_margin) {
            all_outside_mask &= ~(UINT32_C(1) << plane);
        } else if (maximum_distance >= -clip_error_margin) {
            use_direct_corners = SOC_TRUE;
        }
    }
    minimum_w = minimum_plane_distance(&query->w_plane, bounds);
    minimum_near_clip_distance = minimum_plane_distance(
        &query->clip_planes[SOC_VISIBILITY_NEAR_CLIP_PLANE_INDEX],
        bounds
    );
    if (minimum_w < -clip_error_margin) {
        has_nonpositive_w = SOC_TRUE;
    } else if (minimum_w <= clip_error_margin) {
        use_direct_corners = SOC_TRUE;
    }
    if (minimum_near_clip_distance < -clip_error_margin) {
        has_corner_outside_near_clip_plane = SOC_TRUE;
    } else if (minimum_near_clip_distance <= clip_error_margin) {
        use_direct_corners = SOC_TRUE;
    }

    /*
     * Preserve exact boundary behavior with the original corner-by-corner
     * classification. This path is entered only when an analytic extremum is
     * inside the scale-aware floating-point uncertainty band.
     */
    if (use_direct_corners == SOC_TRUE) {
        all_outside_mask = SOC_CLIP_ALL_BITS;
        has_nonpositive_w = SOC_FALSE;
        has_corner_outside_near_clip_plane = SOC_FALSE;
        minimum_w = DBL_MAX;

        for (corner = 0u; corner < SOC_AABB_CORNER_COUNT; ++corner) {
            const double x = (corner & 1u) != 0u
                ? bounds->max.x
                : bounds->min.x;
            const double y = (corner & 2u) != 0u
                ? bounds->max.y
                : bounds->min.y;
            const double z = (corner & 4u) != 0u
                ? bounds->max.z
                : bounds->min.z;
            soc_visibility_clip_vertex* clip = &clip_corners[corner];
            double minimum_z_distance;
            double maximum_z_distance;

            *clip = transform_point(query, x, y, z);
            minimum_z_distance =
                query->clip_depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
                    ? clip->z
                    : clip->z + clip->w;
            maximum_z_distance = clip->w - clip->z;

            if (clip->x + clip->w >= 0.0) {
                all_outside_mask &= ~(UINT32_C(1) << 0u);
            }
            if (clip->w - clip->x >= 0.0) {
                all_outside_mask &= ~(UINT32_C(1) << 1u);
            }
            if (clip->y + clip->w >= 0.0) {
                all_outside_mask &= ~(UINT32_C(1) << 2u);
            }
            if (clip->w - clip->y >= 0.0) {
                all_outside_mask &= ~(UINT32_C(1) << 3u);
            }
            if (minimum_z_distance >= 0.0) {
                all_outside_mask &= ~SOC_CLIP_MINIMUM_Z_BIT;
            }
            if (maximum_z_distance >= 0.0) {
                all_outside_mask &= ~SOC_CLIP_MAXIMUM_Z_BIT;
            }
            if (clip->w <= 0.0) {
                has_nonpositive_w = SOC_TRUE;
            }
            if (maximum_z_distance < 0.0) {
                has_corner_outside_near_clip_plane = SOC_TRUE;
            }
            minimum_w = clip->w < minimum_w ? clip->w : minimum_w;
        }
    }

    if (has_nonpositive_w == SOC_TRUE) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }
    if (has_corner_outside_near_clip_plane == SOC_TRUE &&
        (all_outside_mask & SOC_CLIP_MAXIMUM_Z_BIT) == 0u) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }
    if (all_outside_mask != 0u) {
        return SOC_AABB_PROJECTION_OUTSIDE;
    }

    if (use_direct_corners != SOC_TRUE) {
        clip_corners[0] = transform_point(
            query,
            bounds->min.x,
            bounds->min.y,
            bounds->min.z
        );
        axis_x.x = query->col0.x *
            ((double)bounds->max.x - bounds->min.x);
        axis_x.y = query->col0.y *
            ((double)bounds->max.x - bounds->min.x);
        axis_x.z = query->col0.z *
            ((double)bounds->max.x - bounds->min.x);
        axis_x.w = query->col0.w *
            ((double)bounds->max.x - bounds->min.x);
        axis_y.x = query->col1.x *
            ((double)bounds->max.y - bounds->min.y);
        axis_y.y = query->col1.y *
            ((double)bounds->max.y - bounds->min.y);
        axis_y.z = query->col1.z *
            ((double)bounds->max.y - bounds->min.y);
        axis_y.w = query->col1.w *
            ((double)bounds->max.y - bounds->min.y);
        axis_z.x = query->col2.x *
            ((double)bounds->max.z - bounds->min.z);
        axis_z.y = query->col2.y *
            ((double)bounds->max.z - bounds->min.z);
        axis_z.z = query->col2.z *
            ((double)bounds->max.z - bounds->min.z);
        axis_z.w = query->col2.w *
            ((double)bounds->max.z - bounds->min.z);

        /* One full transform plus three axis deltas reconstructs all corners. */
        clip_corners[1] = add_clip_vertices(&clip_corners[0], &axis_x);
        clip_corners[2] = add_clip_vertices(&clip_corners[0], &axis_y);
        clip_corners[3] = add_clip_vertices(&clip_corners[1], &axis_y);
        clip_corners[4] = add_clip_vertices(&clip_corners[0], &axis_z);
        clip_corners[5] = add_clip_vertices(&clip_corners[1], &axis_z);
        clip_corners[6] = add_clip_vertices(&clip_corners[2], &axis_z);
        clip_corners[7] = add_clip_vertices(&clip_corners[3], &axis_z);
    }

    out_projected->minimum_ndc_x = DBL_MAX;
    out_projected->maximum_ndc_x = -DBL_MAX;
    out_projected->minimum_ndc_y = DBL_MAX;
    out_projected->maximum_ndc_y = -DBL_MAX;
    out_projected->nearest_depth = -DBL_MAX;

    for (corner = 0u; corner < SOC_AABB_CORNER_COUNT; ++corner) {
        const soc_visibility_clip_vertex* clip = &clip_corners[corner];
        const double inverse_w = 1.0 / clip->w;
        const double ndc_x = clip->x * inverse_w;
        const double ndc_y = clip->y * inverse_w;
        double depth = clip->z * inverse_w;

        if (query->clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = remap_negative_one_to_one_depth(depth);
        }
        if (ndc_x < out_projected->minimum_ndc_x) {
            out_projected->minimum_ndc_x = ndc_x;
        }
        if (ndc_x > out_projected->maximum_ndc_x) {
            out_projected->maximum_ndc_x = ndc_x;
        }
        if (ndc_y < out_projected->minimum_ndc_y) {
            out_projected->minimum_ndc_y = ndc_y;
        }
        if (ndc_y > out_projected->maximum_ndc_y) {
            out_projected->maximum_ndc_y = ndc_y;
        }

        depth = clamp_double(depth, 0.0, 1.0);
        if (depth > out_projected->nearest_depth) {
            out_projected->nearest_depth = depth;
        }
    }

    /* Avoid another divide when the precomputed error ratio is tiny. */
    if (clip_error_margin <= minimum_w * SOC_SMALL_ERROR_RATIO) {
        projection_margin = SOC_PROJECTION_SAFETY_EPSILON +
            SOC_SMALL_ERROR_PROJECTION_MARGIN;
    } else {
        projection_margin = SOC_PROJECTION_SAFETY_EPSILON +
            8.0 * clip_error_margin /
                (minimum_w - clip_error_margin);
    }
    if (projection_margin < 0.0 ||
        projection_margin > 1.0) {
        projection_margin = 1.0;
    }

    out_projected->minimum_ndc_x = clamp_double(
        out_projected->minimum_ndc_x - projection_margin,
        -1.0,
        1.0
    );
    out_projected->maximum_ndc_x = clamp_double(
        out_projected->maximum_ndc_x + projection_margin,
        -1.0,
        1.0
    );
    out_projected->minimum_ndc_y = clamp_double(
        out_projected->minimum_ndc_y - projection_margin,
        -1.0,
        1.0
    );
    out_projected->maximum_ndc_y = clamp_double(
        out_projected->maximum_ndc_y + projection_margin,
        -1.0,
        1.0
    );
    out_projected->nearest_depth = clamp_double(
        out_projected->nearest_depth + projection_margin,
        0.0,
        1.0
    );
    return SOC_AABB_PROJECTION_VALID;
}

soc_aabb_projection soc_project_aabb_scalar(
    const soc_aabb_query_context* query,
    const soc_aabb* bounds,
    soc_projected_aabb* out_projected
)
{
    return project_aabb_scalar_impl(query, bounds, out_projected);
}

static void projected_pixel_bounds(
    const soc_projected_aabb* projected,
    uint32_t width,
    uint32_t height,
    uint32_t* out_minimum_x,
    uint32_t* out_maximum_x,
    uint32_t* out_minimum_y,
    uint32_t* out_maximum_y
)
{
    const double minimum_x =
        (projected->minimum_ndc_x * 0.5 + 0.5) * width;
    const double maximum_x =
        (projected->maximum_ndc_x * 0.5 + 0.5) * width;
    const double minimum_y =
        (0.5 - projected->maximum_ndc_y * 0.5) * height;
    const double maximum_y =
        (0.5 - projected->minimum_ndc_y * 0.5) * height;

    *out_minimum_x = minimum_x >= width
        ? width - 1u
        : (uint32_t)minimum_x;
    *out_maximum_x = maximum_x >= width
        ? width - 1u
        : (uint32_t)maximum_x;
    *out_minimum_y = minimum_y >= height
        ? height - 1u
        : (uint32_t)minimum_y;
    *out_maximum_y = maximum_y >= height
        ? height - 1u
        : (uint32_t)maximum_y;
}

static uint32_t select_hiz_level(
    const soc_hiz* hiz,
    uint32_t* minimum_x,
    uint32_t* maximum_x,
    uint32_t* minimum_y,
    uint32_t* maximum_y
)
{
    uint32_t level = 0u;

    while (level + 1u < hiz->level_count &&
        (*maximum_x - *minimum_x > 1u ||
            *maximum_y - *minimum_y > 1u)) {
        *minimum_x /= 2u;
        *maximum_x /= 2u;
        *minimum_y /= 2u;
        *maximum_y /= 2u;
        ++level;
    }

    return level;
}

SOC_FORCE_INLINE soc_visibility test_projected_aabb_scalar_impl(
    const soc_hiz* hiz,
    const soc_projected_aabb* projected
)
{
    const float nearest_depth = (float)projected->nearest_depth;
    uint32_t minimum_x;
    uint32_t maximum_x;
    uint32_t minimum_y;
    uint32_t maximum_y;
    uint32_t level;
    const soc_hiz_level* metadata;
    const float* depth;
    uint32_t y;

    projected_pixel_bounds(
        projected,
        hiz->levels[0].width,
        hiz->levels[0].height,
        &minimum_x,
        &maximum_x,
        &minimum_y,
        &maximum_y
    );
    level = select_hiz_level(
        hiz,
        &minimum_x,
        &maximum_x,
        &minimum_y,
        &maximum_y
    );
    metadata = &hiz->levels[level];
    depth = hiz->data + metadata->offset;

    for (y = minimum_y; y <= maximum_y; ++y) {
        uint32_t x;

        for (x = minimum_x; x <= maximum_x; ++x) {
            const float stored_depth =
                depth[(size_t)y * metadata->width + x];

            if (!(nearest_depth < stored_depth)) {
                return SOC_VISIBILITY_VISIBLE;
            }
        }
    }

    return SOC_VISIBILITY_OCCLUDED;
}

soc_visibility soc_test_projected_aabb_scalar(
    const soc_hiz* hiz,
    const soc_projected_aabb* projected
)
{
    return test_projected_aabb_scalar_impl(hiz, projected);
}

soc_result soc_occlusion_validate_aabb_test(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_occlusion_query_counts* out_counts
)
{
    soc_occlusion_query_counts counts = {0u, 0u, 0u};

    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        hiz->level_count == 0u ||
        hiz->levels[0].width == 0u ||
        hiz->levels[0].height == 0u ||
        query == NULL ||
        out_counts == NULL ||
        (size_t)bounds_count > SIZE_MAX / sizeof(*world_bounds) ||
        (query->clip_depth_range != SOC_CLIP_DEPTH_ZERO_TO_ONE &&
            query->clip_depth_range !=
                SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *out_counts = counts;
    if (bounds_count == 0u) {
        return SOC_RESULT_OK;
    }
    if (world_bounds == NULL || out_visibility == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    return SOC_RESULT_OK;
}

SOC_FORCE_INLINE soc_visibility occlusion_test_aabb_scalar_impl(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* bounds
)
{
    soc_projected_aabb projected;
    const soc_aabb_projection projection = project_aabb_scalar_impl(
        query,
        bounds,
        &projected
    );

    if (projection == SOC_AABB_PROJECTION_UNKNOWN) {
        return SOC_VISIBILITY_UNKNOWN;
    }
    if (projection == SOC_AABB_PROJECTION_OUTSIDE) {
        return SOC_VISIBILITY_VISIBLE;
    }
    return test_projected_aabb_scalar_impl(hiz, &projected);
}

soc_visibility soc_occlusion_test_aabb_scalar(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* bounds
)
{
    return occlusion_test_aabb_scalar_impl(hiz, query, bounds);
}

soc_result soc_occlusion_test_aabbs(
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
    uint32_t index;

    result = soc_occlusion_validate_aabb_test(
        hiz,
        query,
        world_bounds,
        bounds_count,
        out_visibility,
        out_counts
    );
    if (result != SOC_RESULT_OK || bounds_count == 0u) {
        return result;
    }

    for (index = 0u; index < bounds_count; ++index) {
        const soc_visibility visibility = occlusion_test_aabb_scalar_impl(
            hiz,
            query,
            &world_bounds[index]
        );

        out_visibility[index] = visibility;
        if (visibility == SOC_VISIBILITY_UNKNOWN) {
            ++counts.unknown;
        } else if (visibility == SOC_VISIBILITY_OCCLUDED) {
            ++counts.occluded;
        } else {
            ++counts.visible;
        }
    }

    *out_counts = counts;
    return SOC_RESULT_OK;
}
