#include "occlusion/soc_visibility.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#define SOC_VISIBILITY_CLIP_PLANE_COUNT 6u
#define SOC_AABB_CORNER_COUNT 8u

typedef struct soc_visibility_clip_vertex {
    double x;
    double y;
    double z;
    double w;
} soc_visibility_clip_vertex;

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
    const soc_mat4* matrix,
    double x,
    double y,
    double z
)
{
    const soc_visibility_clip_vertex result = {
        (double)matrix->col0.x * x +
            (double)matrix->col1.x * y +
            (double)matrix->col2.x * z +
            (double)matrix->col3.x,
        (double)matrix->col0.y * x +
            (double)matrix->col1.y * y +
            (double)matrix->col2.y * z +
            (double)matrix->col3.y,
        (double)matrix->col0.z * x +
            (double)matrix->col1.z * y +
            (double)matrix->col2.z * z +
            (double)matrix->col3.z,
        (double)matrix->col0.w * x +
            (double)matrix->col1.w * y +
            (double)matrix->col2.w * z +
            (double)matrix->col3.w,
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

static double clip_plane_distance(
    const soc_visibility_clip_vertex* vertex,
    uint32_t plane,
    soc_clip_depth_range depth_range
)
{
    switch (plane) {
        case 0u:
            return vertex->x + vertex->w;
        case 1u:
            return vertex->w - vertex->x;
        case 2u:
            return vertex->y + vertex->w;
        case 3u:
            return vertex->w - vertex->y;
        case 4u:
            return depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
                ? vertex->z
                : vertex->z + vertex->w;
        default:
            return vertex->w - vertex->z;
    }
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
    const soc_frame_desc* frame,
    const soc_aabb* bounds,
    soc_projected_aabb* out_projected
)
{
    soc_visibility_clip_vertex clip_corners[SOC_AABB_CORNER_COUNT];
    const uint32_t near_plane =
        frame->depth_direction == SOC_DEPTH_REVERSED ? 5u : 4u;
    soc_bool all_outside[SOC_VISIBILITY_CLIP_PLANE_COUNT] = {
        SOC_TRUE,
        SOC_TRUE,
        SOC_TRUE,
        SOC_TRUE,
        SOC_TRUE,
        SOC_TRUE,
    };
    soc_bool has_nonpositive_w = SOC_FALSE;
    soc_bool has_near_plane_outside_corner = SOC_FALSE;
    uint32_t corner;
    uint32_t plane;

    if (valid_aabb(bounds) != SOC_TRUE || out_projected == NULL) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }

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

        *clip = transform_point(&frame->clip_from_world, x, y, z);
        if (finite_clip_vertex(clip) != SOC_TRUE) {
            return SOC_AABB_PROJECTION_UNKNOWN;
        }

        for (plane = 0u;
             plane < SOC_VISIBILITY_CLIP_PLANE_COUNT;
             ++plane) {
            const double distance = clip_plane_distance(
                clip,
                plane,
                frame->clip_depth_range
            );

            if (distance >= 0.0) {
                all_outside[plane] = SOC_FALSE;
            }
        }

        if (clip->w <= 0.0) {
            has_nonpositive_w = SOC_TRUE;
        }
        if (clip_plane_distance(
                clip,
                near_plane,
                frame->clip_depth_range
            ) < 0.0) {
            has_near_plane_outside_corner = SOC_TRUE;
        }
    }

    if (has_nonpositive_w == SOC_TRUE) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }
    if (has_near_plane_outside_corner == SOC_TRUE &&
        all_outside[near_plane] != SOC_TRUE) {
        return SOC_AABB_PROJECTION_UNKNOWN;
    }
    for (plane = 0u;
         plane < SOC_VISIBILITY_CLIP_PLANE_COUNT;
         ++plane) {
        if (all_outside[plane] == SOC_TRUE) {
            return SOC_AABB_PROJECTION_OUTSIDE;
        }
    }

    out_projected->minimum_ndc_x = DBL_MAX;
    out_projected->maximum_ndc_x = -DBL_MAX;
    out_projected->minimum_ndc_y = DBL_MAX;
    out_projected->maximum_ndc_y = -DBL_MAX;
    out_projected->nearest_depth =
        frame->depth_direction == SOC_DEPTH_REVERSED
            ? -DBL_MAX
            : DBL_MAX;

    for (corner = 0u; corner < SOC_AABB_CORNER_COUNT; ++corner) {
        const soc_visibility_clip_vertex* clip = &clip_corners[corner];
        const double ndc_x = clip->x / clip->w;
        const double ndc_y = clip->y / clip->w;
        double depth = clip->z / clip->w;

        if (frame->clip_depth_range ==
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
        if (frame->depth_direction == SOC_DEPTH_REVERSED) {
            if (depth > out_projected->nearest_depth) {
                out_projected->nearest_depth = depth;
            }
        } else if (depth < out_projected->nearest_depth) {
            out_projected->nearest_depth = depth;
        }
    }

    out_projected->minimum_ndc_x = clamp_double(
        out_projected->minimum_ndc_x,
        -1.0,
        1.0
    );
    out_projected->maximum_ndc_x = clamp_double(
        out_projected->maximum_ndc_x,
        -1.0,
        1.0
    );
    out_projected->minimum_ndc_y = clamp_double(
        out_projected->minimum_ndc_y,
        -1.0,
        1.0
    );
    out_projected->maximum_ndc_y = clamp_double(
        out_projected->maximum_ndc_y,
        -1.0,
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
    const soc_frame_desc* frame,
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
                frame->depth_direction == SOC_DEPTH_REVERSED
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
    const soc_frame_desc* frame,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    uint64_t* out_occluded_count
)
{
    uint64_t occluded_count = 0u;
    uint32_t index;

    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        hiz->level_count == 0u ||
        hiz->levels[0].width == 0u ||
        hiz->levels[0].height == 0u ||
        frame == NULL ||
        out_occluded_count == NULL ||
        (frame->clip_depth_range != SOC_CLIP_DEPTH_ZERO_TO_ONE &&
            frame->clip_depth_range !=
                SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) ||
        (frame->depth_direction != SOC_DEPTH_FORWARD &&
            frame->depth_direction != SOC_DEPTH_REVERSED)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *out_occluded_count = 0u;
    if (bounds_count == 0u) {
        return SOC_RESULT_OK;
    }
    if (world_bounds == NULL || out_visibility == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (index = 0u; index < bounds_count; ++index) {
        soc_projected_aabb projected;
        const soc_aabb_projection projection = project_aabb(
            frame,
            &world_bounds[index],
            &projected
        );

        if (projection == SOC_AABB_PROJECTION_UNKNOWN) {
            out_visibility[index] = SOC_VISIBILITY_UNKNOWN;
        } else if (projection == SOC_AABB_PROJECTION_OUTSIDE) {
            out_visibility[index] = SOC_VISIBILITY_VISIBLE;
        } else {
            out_visibility[index] = test_projected_aabb(
                hiz,
                frame,
                &projected
            );
            if (out_visibility[index] == SOC_VISIBILITY_OCCLUDED) {
                ++occluded_count;
            }
        }
    }

    *out_occluded_count = occluded_count;
    return SOC_RESULT_OK;
}
