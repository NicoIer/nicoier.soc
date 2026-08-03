#include "occlusion/soc_visibility.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#define SOC_AABB_CORNER_COUNT 8u
#define SOC_PROJECTION_SAFETY_EPSILON (32.0 * DBL_EPSILON)
#define SOC_TRANSFORM_ERROR_FACTOR (128.0 * DBL_EPSILON)
#define SOC_SMALL_ERROR_RATIO 0x1p-40
#define SOC_SMALL_ERROR_PROJECTION_MARGIN 0x1p-36

#define SOC_CLIP_MINIMUM_Z_BIT (UINT32_C(1) << 4u)
#define SOC_CLIP_MAXIMUM_Z_BIT (UINT32_C(1) << 5u)
#define SOC_CLIP_ALL_BITS \
    ((UINT32_C(1) << SOC_VISIBILITY_CLIP_PLANE_COUNT) - 1u)

typedef struct soc_projected_aabb {
    double minimum_ndc_x;
    double maximum_ndc_x;
    double minimum_ndc_y;
    double maximum_ndc_y;
    double nearest_depth;
} soc_projected_aabb;

typedef enum soc_aabb_projection {
    SOC_AABB_PROJECTION_UNKNOWN = 0,
    SOC_AABB_PROJECTION_OUTSIDE,
    SOC_AABB_PROJECTION_VALID,
} soc_aabb_projection;

static soc_bool finite_double(double value)
{
    return value == value && value >= -DBL_MAX && value <= DBL_MAX
        ? SOC_TRUE
        : SOC_FALSE;
}

static double absolute_double(double value)
{
    return value < 0.0 ? -value : value;
}

static double maximum_double(double left, double right)
{
    return left > right ? left : right;
}

static soc_bool valid_aabb(const soc_aabb* bounds)
{
    if (bounds == NULL ||
        finite_double(bounds->min.x) != SOC_TRUE ||
        finite_double(bounds->min.y) != SOC_TRUE ||
        finite_double(bounds->min.z) != SOC_TRUE ||
        finite_double(bounds->max.x) != SOC_TRUE ||
        finite_double(bounds->max.y) != SOC_TRUE ||
        finite_double(bounds->max.z) != SOC_TRUE) {
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
        query->col0.x * x + query->col1.x * y +
            query->col2.x * z + query->col3.x,
        query->col0.y * x + query->col1.y * y +
            query->col2.y * z + query->col3.y,
        query->col0.z * x + query->col1.z * y +
            query->col2.z * z + query->col3.z,
        query->col0.w * x + query->col1.w * y +
            query->col2.w * z + query->col3.w,
    };
    return result;
}

static soc_bool finite_clip_vertex(
    const soc_visibility_clip_vertex* vertex
)
{
    return vertex != NULL &&
        finite_double(vertex->x) == SOC_TRUE &&
        finite_double(vertex->y) == SOC_TRUE &&
        finite_double(vertex->z) == SOC_TRUE &&
        finite_double(vertex->w) == SOC_TRUE
        ? SOC_TRUE
        : SOC_FALSE;
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
    return plane->x * (plane->x >= 0.0 ? bounds->max.x : bounds->min.x) +
        plane->y * (plane->y >= 0.0 ? bounds->max.y : bounds->min.y) +
        plane->z * (plane->z >= 0.0 ? bounds->max.z : bounds->min.z) +
        plane->d;
}

static double minimum_plane_distance(
    const soc_visibility_world_plane* plane,
    const soc_aabb* bounds
)
{
    return plane->x * (plane->x >= 0.0 ? bounds->min.x : bounds->max.x) +
        plane->y * (plane->y >= 0.0 ? bounds->min.y : bounds->max.y) +
        plane->z * (plane->z >= 0.0 ? bounds->min.z : bounds->max.z) +
        plane->d;
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
        .near_clip_plane_index =
            frame->depth_direction == SOC_DEPTH_REVERSED
            ? 5u
            : 4u,
        .near_clip_plane_bit =
            frame->depth_direction == SOC_DEPTH_REVERSED
            ? SOC_CLIP_MAXIMUM_Z_BIT
            : SOC_CLIP_MINIMUM_Z_BIT,
        .clip_depth_range = frame->clip_depth_range,
        .depth_direction = frame->depth_direction,
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

static soc_aabb_projection project_aabb(
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
    if (finite_double(clip_error_margin) != SOC_TRUE) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }

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

        if (finite_double(maximum_distance) != SOC_TRUE) {
            return SOC_AABB_PROJECTION_UNKNOWN;
        }
        if (maximum_distance > clip_error_margin) {
            all_outside_mask &= ~(UINT32_C(1) << plane);
        } else if (maximum_distance >= -clip_error_margin) {
            use_direct_corners = SOC_TRUE;
        }
    }
    minimum_w = minimum_plane_distance(&query->w_plane, bounds);
    minimum_near_clip_distance = minimum_plane_distance(
        &query->clip_planes[query->near_clip_plane_index],
        bounds
    );
    if (finite_double(minimum_w) != SOC_TRUE ||
        finite_double(minimum_near_clip_distance) != SOC_TRUE) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }
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
            double near_clip_distance;

            *clip = transform_point(query, x, y, z);
            if (finite_clip_vertex(clip) != SOC_TRUE) {
                return SOC_AABB_PROJECTION_UNKNOWN;
            }
            minimum_z_distance =
                query->clip_depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
                    ? clip->z
                    : clip->z + clip->w;
            maximum_z_distance = clip->w - clip->z;
            near_clip_distance =
                query->near_clip_plane_bit == SOC_CLIP_MAXIMUM_Z_BIT
                    ? maximum_z_distance
                    : minimum_z_distance;

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
            if (near_clip_distance < 0.0) {
                has_corner_outside_near_clip_plane = SOC_TRUE;
            }
            minimum_w = clip->w < minimum_w ? clip->w : minimum_w;
        }
    }

    if (has_nonpositive_w == SOC_TRUE) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }
    if (has_corner_outside_near_clip_plane == SOC_TRUE &&
        (all_outside_mask & query->near_clip_plane_bit) == 0u) {
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

    for (corner = 0u; corner < SOC_AABB_CORNER_COUNT; ++corner) {
        if (finite_clip_vertex(&clip_corners[corner]) != SOC_TRUE) {
            return SOC_AABB_PROJECTION_UNKNOWN;
        }
    }

    out_projected->minimum_ndc_x = DBL_MAX;
    out_projected->maximum_ndc_x = -DBL_MAX;
    out_projected->minimum_ndc_y = DBL_MAX;
    out_projected->maximum_ndc_y = -DBL_MAX;
    out_projected->nearest_depth =
        query->depth_direction == SOC_DEPTH_REVERSED
            ? -DBL_MAX
            : DBL_MAX;

    for (corner = 0u; corner < SOC_AABB_CORNER_COUNT; ++corner) {
        const soc_visibility_clip_vertex* clip = &clip_corners[corner];
        const double inverse_w = 1.0 / clip->w;
        const double ndc_x = clip->x * inverse_w;
        const double ndc_y = clip->y * inverse_w;
        double depth = clip->z * inverse_w;

        if (query->clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = depth * 0.5 + 0.5;
        }
        if (finite_double(ndc_x) != SOC_TRUE ||
            finite_double(ndc_y) != SOC_TRUE ||
            finite_double(depth) != SOC_TRUE) {
            return SOC_AABB_PROJECTION_UNKNOWN;
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
        if (query->depth_direction == SOC_DEPTH_REVERSED) {
            if (depth > out_projected->nearest_depth) {
                out_projected->nearest_depth = depth;
            }
        } else if (depth < out_projected->nearest_depth) {
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
    if (finite_double(projection_margin) != SOC_TRUE ||
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
        query->depth_direction == SOC_DEPTH_REVERSED
            ? out_projected->nearest_depth + projection_margin
            : out_projected->nearest_depth - projection_margin,
        0.0,
        1.0
    );
    return SOC_AABB_PROJECTION_VALID;
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

static soc_visibility test_projected_aabb(
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
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
            const soc_bool occluded =
                query->depth_direction == SOC_DEPTH_REVERSED
                    ? (nearest_depth < stored_depth
                        ? SOC_TRUE
                        : SOC_FALSE)
                    : (nearest_depth > stored_depth
                        ? SOC_TRUE
                        : SOC_FALSE);

            if (occluded != SOC_TRUE) {
                return SOC_VISIBILITY_VISIBLE;
            }
        }
    }

    return SOC_VISIBILITY_OCCLUDED;
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
    uint32_t index;

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
                SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) ||
        (query->depth_direction != SOC_DEPTH_FORWARD &&
            query->depth_direction != SOC_DEPTH_REVERSED)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *out_counts = counts;
    if (bounds_count == 0u) {
        return SOC_RESULT_OK;
    }
    if (world_bounds == NULL || out_visibility == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (index = 0u; index < bounds_count; ++index) {
        soc_projected_aabb projected;
        const soc_aabb_projection projection = project_aabb(
            query,
            &world_bounds[index],
            &projected
        );

        if (projection == SOC_AABB_PROJECTION_UNKNOWN) {
            out_visibility[index] = SOC_VISIBILITY_UNKNOWN;
            ++counts.unknown;
        } else if (projection == SOC_AABB_PROJECTION_OUTSIDE) {
            out_visibility[index] = SOC_VISIBILITY_VISIBLE;
            ++counts.visible;
        } else {
            out_visibility[index] = test_projected_aabb(
                hiz,
                query,
                &projected
            );
            if (out_visibility[index] == SOC_VISIBILITY_OCCLUDED) {
                ++counts.occluded;
            } else {
                ++counts.visible;
            }
        }
    }

    *out_counts = counts;
    return SOC_RESULT_OK;
}
