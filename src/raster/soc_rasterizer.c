#include "raster/soc_rasterizer.h"

#include "core/soc_mesh.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SOC_CLIP_PLANE_COUNT 6u
#define SOC_MAX_CLIPPED_VERTICES 12u
#define SOC_RASTER_SUBPIXEL_BITS 8u
#define SOC_RASTER_SUBPIXEL_SCALE \
    ((int64_t)1 << SOC_RASTER_SUBPIXEL_BITS)
#define SOC_RASTER_SUBPIXEL_HALF \
    (SOC_RASTER_SUBPIXEL_SCALE / 2)
#define SOC_RASTER_BLOCK_SIZE SOC_KERNEL_RASTER_BLOCK_SIZE
#define SOC_RASTER_DEPTH_GUARD_ULPS 1u
#define SOC_RASTER_FAST_CONDITION_LIMIT 8.0
#define SOC_RASTER_EVALUATION_ERROR_SCALE 256.0
#define SOC_RASTER_NUMERIC_ERROR_SCALE 128.0

#if defined(_MSC_VER)
#define SOC_NOINLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_NOINLINE __attribute__((noinline))
#else
#define SOC_NOINLINE
#endif

_Static_assert(
    FLT_RADIX == 2 && FLT_MANT_DIG == 24 && sizeof(float) == 4u,
    "soc rasterization requires IEEE-754 binary32"
);
_Static_assert(
    DBL_MANT_DIG == 53 && sizeof(double) == 8u,
    "soc rasterization requires IEEE-754 binary64"
);

typedef uint8_t soc_clip_outcode;

#define SOC_CLIP_OUTCODE_ALL \
    ((soc_clip_outcode)((1u << SOC_CLIP_PLANE_COUNT) - 1u))

typedef enum soc_clip_classification {
    SOC_CLIP_CLASSIFICATION_NONFINITE = 0,
    SOC_CLIP_CLASSIFICATION_ACCEPT,
    SOC_CLIP_CLASSIFICATION_REJECT,
    SOC_CLIP_CLASSIFICATION_PARTIAL,
} soc_clip_classification;

typedef struct soc_clip_vertex {
    double x;
    double y;
    double z;
    double w;
} soc_clip_vertex;

typedef struct soc_screen_vertex {
    double x;
    double y;
    double depth;
} soc_screen_vertex;

typedef struct soc_fixed_vertex {
    int64_t x;
    int64_t y;
} soc_fixed_vertex;

typedef struct soc_edge_equation {
    int64_t start_x;
    int64_t start_y;
    int64_t delta_x;
    int64_t delta_y;
    int64_t step_x;
    int64_t step_y;
    int64_t coverage_bias;
} soc_edge_equation;

typedef struct soc_raster_region {
    uint32_t minimum_x;
    uint32_t minimum_y;
    uint32_t end_x;
    uint32_t end_y;
} soc_raster_region;

typedef enum soc_raster_setup_result {
    SOC_RASTER_SETUP_REJECTED = 0,
    SOC_RASTER_SETUP_EMPTY,
    SOC_RASTER_SETUP_READY,
} soc_raster_setup_result;

typedef enum soc_raster_block_classification {
    SOC_RASTER_BLOCK_OUTSIDE = 0,
    SOC_RASTER_BLOCK_PARTIAL,
    SOC_RASTER_BLOCK_FULL,
} soc_raster_block_classification;

typedef struct soc_raster_triangle_setup {
    soc_edge_equation edges[3];
    soc_raster_region bounds;
    double depth_anchor_x;
    double depth_anchor_y;
    double depth_anchor;
    double depth_step_x;
    double depth_step_y;
    double depth_error_bound;
} soc_raster_triangle_setup;

typedef struct soc_raster_depth_plane {
    double anchor_x;
    double anchor_y;
    double anchor;
    double step_x;
    double step_y;
    double error_bound;
} soc_raster_depth_plane;

static soc_bool checked_size_multiply(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || (right != 0u && left > SIZE_MAX / right)) {
        return SOC_FALSE;
    }

    *out_result = left * right;
    return SOC_TRUE;
}

static soc_bool finite_double(double value)
{
    return value == value && value >= -DBL_MAX && value <= DBL_MAX
        ? SOC_TRUE
        : SOC_FALSE;
}

static soc_bool finite_clip_vertex(const soc_clip_vertex* vertex)
{
    return vertex != NULL &&
        finite_double(vertex->x) == SOC_TRUE &&
        finite_double(vertex->y) == SOC_TRUE &&
        finite_double(vertex->z) == SOC_TRUE &&
        finite_double(vertex->w) == SOC_TRUE
        ? SOC_TRUE
        : SOC_FALSE;
}

static soc_clip_vertex transform_vertex(
    const soc_mat4* matrix,
    const soc_clip_vertex* vertex
)
{
    const soc_clip_vertex result = {
        (double)matrix->col0.x * vertex->x +
            (double)matrix->col1.x * vertex->y +
            (double)matrix->col2.x * vertex->z +
            (double)matrix->col3.x * vertex->w,
        (double)matrix->col0.y * vertex->x +
            (double)matrix->col1.y * vertex->y +
            (double)matrix->col2.y * vertex->z +
            (double)matrix->col3.y * vertex->w,
        (double)matrix->col0.z * vertex->x +
            (double)matrix->col1.z * vertex->y +
            (double)matrix->col2.z * vertex->z +
            (double)matrix->col3.z * vertex->w,
        (double)matrix->col0.w * vertex->x +
            (double)matrix->col1.w * vertex->y +
            (double)matrix->col2.w * vertex->z +
            (double)matrix->col3.w * vertex->w,
    };
    return result;
}

static uint32_t read_mesh_index(
    const soc_mesh* mesh,
    uint32_t index
)
{
    const unsigned char* source = mesh->indices;

    if (mesh->index_type == SOC_INDEX_UINT16) {
        uint16_t value;
        memcpy(&value, source + (size_t)index * sizeof(value), sizeof(value));
        return value;
    }

    uint32_t value;
    memcpy(&value, source + (size_t)index * sizeof(value), sizeof(value));
    return value;
}

static double clip_plane_distance(
    const soc_clip_vertex* vertex,
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

static soc_clip_outcode compute_clip_outcode(
    const soc_clip_vertex* vertex,
    soc_clip_depth_range depth_range
)
{
    soc_clip_outcode outcode = 0u;
    uint32_t plane;

    for (plane = 0u; plane < SOC_CLIP_PLANE_COUNT; ++plane) {
        if (clip_plane_distance(vertex, plane, depth_range) < 0.0) {
            outcode = (soc_clip_outcode)(
                outcode | (soc_clip_outcode)(1u << plane)
            );
        }
    }
    return outcode;
}

static soc_clip_classification classify_clip_triangle(
    const soc_clip_vertex vertices[3],
    soc_clip_depth_range depth_range,
    soc_clip_outcode* out_active_planes
)
{
    soc_clip_outcode active_planes = 0u;
    soc_clip_outcode common_planes = SOC_CLIP_OUTCODE_ALL;
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        soc_clip_outcode outcode;

        if (finite_clip_vertex(&vertices[index]) != SOC_TRUE) {
            *out_active_planes = 0u;
            return SOC_CLIP_CLASSIFICATION_NONFINITE;
        }
        outcode = compute_clip_outcode(&vertices[index], depth_range);
        active_planes = (soc_clip_outcode)(active_planes | outcode);
        common_planes = (soc_clip_outcode)(common_planes & outcode);
    }

    *out_active_planes = active_planes;
    if (common_planes != 0u) {
        return SOC_CLIP_CLASSIFICATION_REJECT;
    }
    return active_planes == 0u
        ? SOC_CLIP_CLASSIFICATION_ACCEPT
        : SOC_CLIP_CLASSIFICATION_PARTIAL;
}

static soc_clip_vertex interpolate_clip_vertex(
    const soc_clip_vertex* start,
    const soc_clip_vertex* end,
    double amount
)
{
    const soc_clip_vertex result = {
        start->x + (end->x - start->x) * amount,
        start->y + (end->y - start->y) * amount,
        start->z + (end->z - start->z) * amount,
        start->w + (end->w - start->w) * amount,
    };
    return result;
}

static uint32_t clip_polygon_against_plane(
    const soc_clip_vertex* input,
    uint32_t input_count,
    soc_clip_vertex* output,
    uint32_t plane,
    soc_clip_depth_range depth_range
)
{
    soc_clip_vertex previous;
    double previous_distance;
    soc_bool previous_inside;
    uint32_t output_count = 0u;
    uint32_t index;

    if (input_count == 0u) {
        return 0u;
    }

    previous = input[input_count - 1u];
    previous_distance = clip_plane_distance(
        &previous,
        plane,
        depth_range
    );
    previous_inside = previous_distance >= 0.0 ? SOC_TRUE : SOC_FALSE;

    for (index = 0u; index < input_count; ++index) {
        const soc_clip_vertex current = input[index];
        const double current_distance = clip_plane_distance(
            &current,
            plane,
            depth_range
        );
        const soc_bool current_inside =
            current_distance >= 0.0 ? SOC_TRUE : SOC_FALSE;

        if (current_inside != previous_inside) {
            const double denominator =
                previous_distance - current_distance;
            double amount = previous_distance / denominator;

            if (amount < 0.0) {
                amount = 0.0;
            } else if (amount > 1.0) {
                amount = 1.0;
            }

            if (output_count >= SOC_MAX_CLIPPED_VERTICES) {
                return 0u;
            }
            output[output_count] = interpolate_clip_vertex(
                &previous,
                &current,
                amount
            );
            ++output_count;
        }

        if (current_inside == SOC_TRUE) {
            if (output_count >= SOC_MAX_CLIPPED_VERTICES) {
                return 0u;
            }
            output[output_count] = current;
            ++output_count;
        }

        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }

    return output_count;
}

static soc_bool polygon_inside_clip_plane(
    const soc_clip_vertex* vertices,
    uint32_t vertex_count,
    uint32_t plane,
    soc_clip_depth_range depth_range
)
{
    uint32_t index;

    for (index = 0u; index < vertex_count; ++index) {
        if (!(clip_plane_distance(
                &vertices[index],
                plane,
                depth_range
            ) >= 0.0)) {
            return SOC_FALSE;
        }
    }
    return SOC_TRUE;
}

static uint32_t clip_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex input_triangle[3],
    soc_clip_outcode active_planes,
    soc_clip_vertex output_polygon[SOC_MAX_CLIPPED_VERTICES]
)
{
    soc_clip_vertex buffer_a[SOC_MAX_CLIPPED_VERTICES];
    soc_clip_vertex buffer_b[SOC_MAX_CLIPPED_VERTICES];
    soc_clip_vertex* input = buffer_a;
    soc_clip_vertex* output = buffer_b;
    uint32_t vertex_count = 3u;
    uint32_t plane;
    uint32_t index;
    soc_bool polygon_changed = SOC_FALSE;

    for (index = 0u; index < 3u; ++index) {
        buffer_a[index] = input_triangle[index];
    }

    for (plane = 0u; plane < SOC_CLIP_PLANE_COUNT; ++plane) {
        const soc_clip_outcode plane_bit =
            (soc_clip_outcode)(1u << plane);
        soc_clip_vertex* swap;

        if ((active_planes & plane_bit) == 0u &&
            (polygon_changed != SOC_TRUE ||
                polygon_inside_clip_plane(
                    input,
                    vertex_count,
                    plane,
                    rasterizer->frame.clip_depth_range
                ) == SOC_TRUE)) {
            continue;
        }

        vertex_count = clip_polygon_against_plane(
            input,
            vertex_count,
            output,
            plane,
            rasterizer->frame.clip_depth_range
        );
        if (vertex_count == 0u) {
            return 0u;
        }

        swap = input;
        input = output;
        output = swap;
        polygon_changed = SOC_TRUE;
    }

    memcpy(
        output_polygon,
        input,
        (size_t)vertex_count * sizeof(*output_polygon)
    );
    return vertex_count;
}

static double edge_function(
    const soc_screen_vertex* start,
    const soc_screen_vertex* end,
    double point_x,
    double point_y
)
{
    return (end->x - start->x) * (point_y - start->y) -
        (end->y - start->y) * (point_x - start->x);
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

static void swap_screen_vertices(
    soc_screen_vertex* left,
    soc_screen_vertex* right
)
{
    const soc_screen_vertex temporary = *left;
    *left = *right;
    *right = temporary;
}

static int64_t quantize_screen_coordinate(
    double coordinate,
    soc_bool* out_exact
)
{
    const double scaled =
        coordinate * (double)SOC_RASTER_SUBPIXEL_SCALE;
    const int64_t quantized = (int64_t)(scaled + 0.5);

    *out_exact = scaled == (double)quantized ? SOC_TRUE : SOC_FALSE;
    return quantized;
}

static int64_t fixed_edge_value(
    const soc_edge_equation* edge,
    int64_t point_x,
    int64_t point_y
)
{
    return edge->delta_x * (point_y - edge->start_y) -
        edge->delta_y * (point_x - edge->start_x);
}

static soc_edge_equation make_edge_equation(
    const soc_fixed_vertex* start,
    const soc_fixed_vertex* end
)
{
    const int64_t delta_x = end->x - start->x;
    const int64_t delta_y = end->y - start->y;
    const soc_edge_equation edge = {
        .start_x = start->x,
        .start_y = start->y,
        .delta_x = delta_x,
        .delta_y = delta_y,
        .step_x = -delta_y * SOC_RASTER_SUBPIXEL_SCALE,
        .step_y = delta_x * SOC_RASTER_SUBPIXEL_SCALE,
        .coverage_bias = 0,
    };
    return edge;
}

static double absolute_double(double value)
{
    return value < 0.0 ? -value : value;
}

static double next_double_up(double value)
{
    uint64_t bits;

    if (value == 0.0) {
        bits = UINT64_C(1);
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    memcpy(&bits, &value, sizeof(bits));
    if (value > 0.0) {
        ++bits;
    } else {
        --bits;
    }
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int64_t compute_edge_coverage_bias(
    const soc_edge_equation* fixed_edge,
    const soc_screen_vertex* exact_start,
    const soc_screen_vertex* exact_end,
    soc_bool endpoints_exact,
    int64_t position_error_bound
)
{
    const double exact_delta_x = exact_end->x - exact_start->x;
    const double exact_delta_y = exact_end->y - exact_start->y;
    const soc_bool top_left =
        exact_delta_y < 0.0 ||
            (exact_delta_y == 0.0 && exact_delta_x > 0.0)
            ? SOC_TRUE
            : SOC_FALSE;

    if (endpoints_exact == SOC_TRUE) {
        return top_left == SOC_TRUE ? 0 : -1;
    }

    {
        /*
         * Nearest Q8 snapping moves each endpoint component by at most half
         * a fixed-point unit.  Bound the resulting affine edge error over the
         * complete bbox and subtract it once here.  Covered samples are then
         * a subset of the unsnapped triangle without any per-sample fallback.
         */
        const int64_t absolute_delta_x = fixed_edge->delta_x < 0
            ? -fixed_edge->delta_x
            : fixed_edge->delta_x;
        const int64_t absolute_delta_y = fixed_edge->delta_y < 0
            ? -fixed_edge->delta_y
            : fixed_edge->delta_y;
        int64_t inward_bias = position_error_bound +
            (absolute_delta_x + absolute_delta_y + 1) /
                2 +
            2;

        if (top_left != SOC_TRUE) {
            ++inward_bias;
        }
        return -inward_bias;
    }
}

static void configure_constant_far_depth_plane(
    const soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    soc_raster_triangle_setup* out_setup
)
{
    double farthest_depth = screen[0].depth;
    uint32_t index;

    for (index = 1u; index < 3u; ++index) {
        if (rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED) {
            if (screen[index].depth < farthest_depth) {
                farthest_depth = screen[index].depth;
            }
        } else if (screen[index].depth > farthest_depth) {
            farthest_depth = screen[index].depth;
        }
    }
    out_setup->depth_anchor_x = screen[0].x;
    out_setup->depth_anchor_y = screen[0].y;
    out_setup->depth_anchor = farthest_depth;
    out_setup->depth_step_x = 0.0;
    out_setup->depth_step_y = 0.0;
    out_setup->depth_error_bound =
        (absolute_double(farthest_depth) + 1.0) *
        DBL_EPSILON * 64.0;
}

static soc_bool try_configure_fast_depth_plane(
    const soc_screen_vertex screen[3],
    int64_t fixed_area,
    soc_bool all_fixed_exact,
    double extent_x,
    double extent_y,
    soc_raster_triangle_setup* out_setup
)
{
    const double delta_x10 = screen[1].x - screen[0].x;
    const double delta_y10 = screen[1].y - screen[0].y;
    const double delta_x20 = screen[2].x - screen[0].x;
    const double delta_y20 = screen[2].y - screen[0].y;
    const double delta_depth10 = screen[1].depth - screen[0].depth;
    const double delta_depth20 = screen[2].depth - screen[0].depth;
    const double area_term0 = delta_x10 * delta_y20;
    const double area_term1 = delta_y10 * delta_x20;
    double area;
    double evaluation_magnitude;

    if (all_fixed_exact == SOC_TRUE) {
        const double fixed_area_scale =
            (double)SOC_RASTER_SUBPIXEL_SCALE *
            (double)SOC_RASTER_SUBPIXEL_SCALE;

        area = (double)fixed_area / fixed_area_scale;
    } else {
        area = area_term0 - area_term1;
    }
    /*
     * A determinant is only used when cancellation is bounded: the sum of
     * its two product magnitudes may be at most eight times the positive
     * result.  Exact-Q8 inputs then use a 256-epsilon evaluation guard.
     * Non-Q8 inputs additionally account for determinant/numerator error over
     * the complete bbox.  The guard also covers block anchoring plus the seven
     * column-local additions.  Ill-conditioned triangles keep the same
     * coverage and use their farthest vertex depth instead.
     */
    if (area <= 0.0 ||
        absolute_double(area_term0) + absolute_double(area_term1) >
            area * SOC_RASTER_FAST_CONDITION_LIMIT) {
        return SOC_FALSE;
    }

    {
        const double numerator_x_term0 = delta_depth10 * delta_y20;
        const double numerator_x_term1 = delta_depth20 * delta_y10;
        const double numerator_y_term0 = delta_x10 * delta_depth20;
        const double numerator_y_term1 = delta_x20 * delta_depth10;

        out_setup->depth_step_x =
            (numerator_x_term0 - numerator_x_term1) / area;
        out_setup->depth_step_y =
            (numerator_y_term0 - numerator_y_term1) / area;
        if (finite_double(out_setup->depth_step_x) != SOC_TRUE ||
            finite_double(out_setup->depth_step_y) != SOC_TRUE) {
            return SOC_FALSE;
        }

        evaluation_magnitude =
            absolute_double(out_setup->depth_anchor) +
            absolute_double(out_setup->depth_step_x) * extent_x +
            absolute_double(out_setup->depth_step_y) * extent_y + 1.0;
        if (all_fixed_exact == SOC_TRUE) {
            out_setup->depth_error_bound = next_double_up(
                evaluation_magnitude * DBL_EPSILON *
                    SOC_RASTER_EVALUATION_ERROR_SCALE
            );
        } else {
            const double area_error =
                (absolute_double(area_term0) +
                    absolute_double(area_term1)) *
                    DBL_EPSILON * SOC_RASTER_NUMERIC_ERROR_SCALE +
                DBL_MIN * SOC_RASTER_NUMERIC_ERROR_SCALE;
            const double area_lower_bound = area - area_error;
            const double numerator_x_error =
                (absolute_double(numerator_x_term0) +
                    absolute_double(numerator_x_term1)) *
                    DBL_EPSILON * SOC_RASTER_NUMERIC_ERROR_SCALE +
                DBL_MIN * SOC_RASTER_NUMERIC_ERROR_SCALE;
            const double numerator_y_error =
                (absolute_double(numerator_y_term0) +
                    absolute_double(numerator_y_term1)) *
                    DBL_EPSILON * SOC_RASTER_NUMERIC_ERROR_SCALE +
                DBL_MIN * SOC_RASTER_NUMERIC_ERROR_SCALE;
            double depth_minimum = screen[0].depth;
            double depth_maximum = screen[0].depth;
            double step_x_error;
            double step_y_error;
            double coefficient_error;
            uint32_t index;

            if (area_lower_bound <= 0.0) {
                return SOC_FALSE;
            }
            step_x_error =
                (numerator_x_error +
                    absolute_double(out_setup->depth_step_x) *
                        area_error) /
                    area_lower_bound +
                absolute_double(out_setup->depth_step_x) *
                    DBL_EPSILON * 4.0;
            step_y_error =
                (numerator_y_error +
                    absolute_double(out_setup->depth_step_y) *
                        area_error) /
                    area_lower_bound +
                absolute_double(out_setup->depth_step_y) *
                    DBL_EPSILON * 4.0;
            coefficient_error =
                step_x_error * extent_x +
                step_y_error * extent_y;
            out_setup->depth_error_bound = next_double_up(
                coefficient_error +
                evaluation_magnitude * DBL_EPSILON *
                    SOC_RASTER_EVALUATION_ERROR_SCALE
            );
            for (index = 1u; index < 3u; ++index) {
                if (screen[index].depth < depth_minimum) {
                    depth_minimum = screen[index].depth;
                }
                if (screen[index].depth > depth_maximum) {
                    depth_maximum = screen[index].depth;
                }
            }
            if (out_setup->depth_error_bound >=
                depth_maximum - depth_minimum) {
                return SOC_FALSE;
            }
        }
    }
    return finite_double(out_setup->depth_error_bound);
}

static void configure_depth_plane(
    const soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    int64_t fixed_area,
    soc_bool all_fixed_exact,
    soc_raster_triangle_setup* out_setup
)
{
    const double minimum_offset_x = absolute_double(
        (double)out_setup->bounds.minimum_x + 0.5 - screen[0].x
    );
    const double maximum_offset_x = absolute_double(
        (double)(out_setup->bounds.end_x - 1u) + 0.5 - screen[0].x
    );
    const double minimum_offset_y = absolute_double(
        (double)out_setup->bounds.minimum_y + 0.5 - screen[0].y
    );
    const double maximum_offset_y = absolute_double(
        (double)(out_setup->bounds.end_y - 1u) + 0.5 - screen[0].y
    );
    const double extent_x = next_double_up(
        minimum_offset_x > maximum_offset_x
            ? minimum_offset_x
            : maximum_offset_x
    );
    const double extent_y = next_double_up(
        minimum_offset_y > maximum_offset_y
            ? minimum_offset_y
            : maximum_offset_y
    );

    out_setup->depth_anchor_x = screen[0].x;
    out_setup->depth_anchor_y = screen[0].y;
    out_setup->depth_anchor = screen[0].depth;
    if (try_configure_fast_depth_plane(
            screen,
            fixed_area,
            all_fixed_exact,
            extent_x,
            extent_y,
            out_setup
        ) != SOC_TRUE) {
        configure_constant_far_depth_plane(rasterizer, screen, out_setup);
    }
}

static soc_raster_setup_result prepare_screen_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided,
    soc_screen_vertex screen[3]
)
{
    const soc_clip_vertex* clip_vertices[3] = {clip0, clip1, clip2};
    double screen_area;
    uint32_t index;

    /* Project only x/y until the cheap degeneracy and facing tests pass. */
    for (index = 0u; index < 3u; ++index) {
        double ndc_x;
        double ndc_y;

        if (clip_vertices[index]->w <= 0.0) {
            return SOC_RASTER_SETUP_REJECTED;
        }
        ndc_x = clip_vertices[index]->x / clip_vertices[index]->w;
        ndc_y = clip_vertices[index]->y / clip_vertices[index]->w;
        if (finite_double(ndc_x) != SOC_TRUE ||
            finite_double(ndc_y) != SOC_TRUE) {
            return SOC_RASTER_SETUP_REJECTED;
        }
        ndc_x = clamp_double(ndc_x, -1.0, 1.0);
        ndc_y = clamp_double(ndc_y, -1.0, 1.0);
        screen[index].x =
            (ndc_x * 0.5 + 0.5) * rasterizer->width;
        screen[index].y =
            (0.5 - ndc_y * 0.5) * rasterizer->height;
    }

    screen_area = edge_function(
        &screen[0],
        &screen[1],
        screen[2].x,
        screen[2].y
    );
    if (screen_area == 0.0) {
        return SOC_RASTER_SETUP_REJECTED;
    }
    if (two_sided != SOC_TRUE) {
        const soc_bool front_facing =
            rasterizer->frame.front_face == SOC_FRONT_FACE_CCW
                ? (screen_area < 0.0 ? SOC_TRUE : SOC_FALSE)
                : (screen_area > 0.0 ? SOC_TRUE : SOC_FALSE);

        if (front_facing != SOC_TRUE) {
            return SOC_RASTER_SETUP_REJECTED;
        }
    }

    for (index = 0u; index < 3u; ++index) {
        double depth = clip_vertices[index]->z / clip_vertices[index]->w;

        if (rasterizer->frame.clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = depth * 0.5 + 0.5;
        }
        if (finite_double(depth) != SOC_TRUE) {
            return SOC_RASTER_SETUP_REJECTED;
        }
        screen[index].depth = clamp_double(depth, 0.0, 1.0);
    }
    if (screen_area < 0.0) {
        swap_screen_vertices(&screen[1], &screen[2]);
    }
    return SOC_RASTER_SETUP_READY;
}

static soc_bool configure_shared_fan_depth_plane(
    const soc_rasterizer* rasterizer,
    const soc_screen_vertex plane_triangle[3],
    soc_raster_depth_plane* out_plane
)
{
    soc_raster_triangle_setup plane_setup;
    double extent_x;
    double extent_y;

    memset(&plane_setup, 0, sizeof(plane_setup));
    plane_setup.depth_anchor_x = plane_triangle[0].x;
    plane_setup.depth_anchor_y = plane_triangle[0].y;
    plane_setup.depth_anchor = plane_triangle[0].depth;
    extent_x = next_double_up(
        absolute_double(0.5 - plane_setup.depth_anchor_x) >
            absolute_double(
                (double)rasterizer->width - 0.5 -
                plane_setup.depth_anchor_x
            )
            ? absolute_double(0.5 - plane_setup.depth_anchor_x)
            : absolute_double(
                (double)rasterizer->width - 0.5 -
                plane_setup.depth_anchor_x
            )
    );
    extent_y = next_double_up(
        absolute_double(0.5 - plane_setup.depth_anchor_y) >
            absolute_double(
                (double)rasterizer->height - 0.5 -
                plane_setup.depth_anchor_y
            )
            ? absolute_double(0.5 - plane_setup.depth_anchor_y)
            : absolute_double(
                (double)rasterizer->height - 0.5 -
                plane_setup.depth_anchor_y
            )
    );
    if (try_configure_fast_depth_plane(
            plane_triangle,
            0,
            SOC_FALSE,
            extent_x,
            extent_y,
            &plane_setup
        ) != SOC_TRUE) {
        return SOC_FALSE;
    }

    out_plane->anchor_x = plane_setup.depth_anchor_x;
    out_plane->anchor_y = plane_setup.depth_anchor_y;
    out_plane->anchor = plane_setup.depth_anchor;
    out_plane->step_x = plane_setup.depth_step_x;
    out_plane->step_y = plane_setup.depth_step_y;
    out_plane->error_bound = plane_setup.depth_error_bound;
    return SOC_TRUE;
}

static soc_raster_setup_result setup_raster_triangle(
    const soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    const soc_raster_depth_plane* shared_depth_plane,
    soc_raster_triangle_setup* out_setup
)
{
    soc_fixed_vertex fixed[3];
    soc_bool fixed_exact[3];
    int64_t minimum_x;
    int64_t maximum_x;
    int64_t minimum_y;
    int64_t maximum_y;
    int64_t fixed_area;
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        soc_bool exact_x;
        soc_bool exact_y;

        fixed[index].x = quantize_screen_coordinate(
            screen[index].x,
            &exact_x
        );
        fixed[index].y = quantize_screen_coordinate(
            screen[index].y,
            &exact_y
        );
        fixed_exact[index] = exact_x == SOC_TRUE && exact_y == SOC_TRUE
            ? SOC_TRUE
            : SOC_FALSE;
    }
    {
        const soc_edge_equation area_edge = make_edge_equation(
            &fixed[0],
            &fixed[1]
        );
        fixed_area = fixed_edge_value(
            &area_edge,
            fixed[2].x,
            fixed[2].y
        );
    }
    if (fixed_area <= 0) {
        return SOC_RASTER_SETUP_REJECTED;
    }

    minimum_x = fixed[0].x;
    maximum_x = fixed[0].x;
    minimum_y = fixed[0].y;
    maximum_y = fixed[0].y;
    for (index = 1u; index < 3u; ++index) {
        if (fixed[index].x < minimum_x) {
            minimum_x = fixed[index].x;
        }
        if (fixed[index].x > maximum_x) {
            maximum_x = fixed[index].x;
        }
        if (fixed[index].y < minimum_y) {
            minimum_y = fixed[index].y;
        }
        if (fixed[index].y > maximum_y) {
            maximum_y = fixed[index].y;
        }
    }

    if (maximum_x < SOC_RASTER_SUBPIXEL_HALF ||
        maximum_y < SOC_RASTER_SUBPIXEL_HALF) {
        return SOC_RASTER_SETUP_EMPTY;
    }

    out_setup->bounds.minimum_x = minimum_x <= SOC_RASTER_SUBPIXEL_HALF
        ? 0u
        : (uint32_t)(
            (minimum_x - SOC_RASTER_SUBPIXEL_HALF +
                SOC_RASTER_SUBPIXEL_SCALE - 1) /
            SOC_RASTER_SUBPIXEL_SCALE
        );
    out_setup->bounds.minimum_y = minimum_y <= SOC_RASTER_SUBPIXEL_HALF
        ? 0u
        : (uint32_t)(
            (minimum_y - SOC_RASTER_SUBPIXEL_HALF +
                SOC_RASTER_SUBPIXEL_SCALE - 1) /
            SOC_RASTER_SUBPIXEL_SCALE
        );
    out_setup->bounds.end_x = (uint32_t)(
        (maximum_x - SOC_RASTER_SUBPIXEL_HALF) /
            SOC_RASTER_SUBPIXEL_SCALE +
        1
    );
    out_setup->bounds.end_y = (uint32_t)(
        (maximum_y - SOC_RASTER_SUBPIXEL_HALF) /
            SOC_RASTER_SUBPIXEL_SCALE +
        1
    );
    if (out_setup->bounds.end_x > rasterizer->width) {
        out_setup->bounds.end_x = rasterizer->width;
    }
    if (out_setup->bounds.end_y > rasterizer->height) {
        out_setup->bounds.end_y = rasterizer->height;
    }
    if (out_setup->bounds.minimum_x >= out_setup->bounds.end_x ||
        out_setup->bounds.minimum_y >= out_setup->bounds.end_y) {
        return SOC_RASTER_SETUP_EMPTY;
    }

    {
        const int64_t position_error_bound =
            ((int64_t)(out_setup->bounds.end_x -
                    out_setup->bounds.minimum_x) +
                (int64_t)(out_setup->bounds.end_y -
                    out_setup->bounds.minimum_y) +
                2) *
            SOC_RASTER_SUBPIXEL_SCALE;

        out_setup->edges[0] = make_edge_equation(&fixed[1], &fixed[2]);
        out_setup->edges[1] = make_edge_equation(&fixed[2], &fixed[0]);
        out_setup->edges[2] = make_edge_equation(&fixed[0], &fixed[1]);
        out_setup->edges[0].coverage_bias = compute_edge_coverage_bias(
            &out_setup->edges[0],
            &screen[1],
            &screen[2],
            fixed_exact[1] == SOC_TRUE && fixed_exact[2] == SOC_TRUE
                ? SOC_TRUE
                : SOC_FALSE,
            position_error_bound
        );
        out_setup->edges[1].coverage_bias = compute_edge_coverage_bias(
            &out_setup->edges[1],
            &screen[2],
            &screen[0],
            fixed_exact[2] == SOC_TRUE && fixed_exact[0] == SOC_TRUE
                ? SOC_TRUE
                : SOC_FALSE,
            position_error_bound
        );
        out_setup->edges[2].coverage_bias = compute_edge_coverage_bias(
            &out_setup->edges[2],
            &screen[0],
            &screen[1],
            fixed_exact[0] == SOC_TRUE && fixed_exact[1] == SOC_TRUE
                ? SOC_TRUE
                : SOC_FALSE,
            position_error_bound
        );
    }

    if (shared_depth_plane != NULL) {
        out_setup->depth_anchor_x = shared_depth_plane->anchor_x;
        out_setup->depth_anchor_y = shared_depth_plane->anchor_y;
        out_setup->depth_anchor = shared_depth_plane->anchor;
        out_setup->depth_step_x = shared_depth_plane->step_x;
        out_setup->depth_step_y = shared_depth_plane->step_y;
        out_setup->depth_error_bound = shared_depth_plane->error_bound;
    } else if (screen[0].depth == screen[1].depth &&
        screen[0].depth == screen[2].depth) {
        out_setup->depth_anchor_x = screen[0].x;
        out_setup->depth_anchor_y = screen[0].y;
        out_setup->depth_anchor = screen[0].depth;
        out_setup->depth_step_x = 0.0;
        out_setup->depth_step_y = 0.0;
        out_setup->depth_error_bound =
            (absolute_double(out_setup->depth_anchor) + 1.0) *
            DBL_EPSILON * 64.0;
    } else {
        configure_depth_plane(
            rasterizer,
            screen,
            fixed_area,
            fixed_exact[0] == SOC_TRUE &&
                fixed_exact[1] == SOC_TRUE &&
                fixed_exact[2] == SOC_TRUE
                ? SOC_TRUE
                : SOC_FALSE,
            out_setup
        );
    }
    return SOC_RASTER_SETUP_READY;
}

static float make_conservative_depth(
    const soc_rasterizer* rasterizer,
    double depth,
    double depth_error_bound
)
{
    float candidate_depth;
    uint32_t candidate_bits;
    uint32_t guard;

    if (finite_double(depth) != SOC_TRUE) {
        return rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
            ? 0.0f
            : 1.0f;
    }

    depth = rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
        ? depth - depth_error_bound
        : depth + depth_error_bound;
    depth = clamp_double(depth, 0.0, 1.0);
    candidate_depth = (float)depth;
    memcpy(
        &candidate_bits,
        &candidate_depth,
        sizeof(candidate_bits)
    );
    if (rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED) {
        if ((double)candidate_depth > depth && candidate_bits != 0u) {
            --candidate_bits;
        }
        for (guard = 0u;
             guard < SOC_RASTER_DEPTH_GUARD_ULPS &&
                candidate_bits != 0u;
             ++guard) {
            --candidate_bits;
        }
    } else {
        const uint32_t one_bits = UINT32_C(0x3f800000);

        if ((double)candidate_depth < depth &&
            candidate_bits < one_bits) {
            ++candidate_bits;
        }
        for (guard = 0u;
             guard < SOC_RASTER_DEPTH_GUARD_ULPS &&
                candidate_bits < one_bits;
             ++guard) {
            ++candidate_bits;
        }
    }
    memcpy(
        &candidate_depth,
        &candidate_bits,
        sizeof(candidate_depth)
    );
    return candidate_depth;
}

static void store_depth_candidate(
    soc_rasterizer* rasterizer,
    uint32_t pixel_x,
    uint32_t pixel_y,
    float candidate_depth
)
{
    const size_t depth_index =
        (size_t)pixel_y * rasterizer->width + pixel_x;
    const float stored_depth = rasterizer->depth[depth_index];
    const soc_bool passes_depth =
        rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
            ? (candidate_depth > stored_depth ? SOC_TRUE : SOC_FALSE)
            : (candidate_depth < stored_depth ? SOC_TRUE : SOC_FALSE);

    if (passes_depth == SOC_TRUE) {
        rasterizer->depth[depth_index] = candidate_depth;
    }
}

static void rasterize_depth_sample(
    soc_rasterizer* rasterizer,
    uint32_t pixel_x,
    uint32_t pixel_y,
    double depth,
    double depth_error_bound
)
{
    size_t depth_index;
    float candidate_depth;
    float stored_depth;
    soc_bool passes_depth;
    uint32_t candidate_bits;
    uint32_t guard;

    if (finite_double(depth) != SOC_TRUE) {
        return;
    }

    depth = rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
        ? depth - depth_error_bound
        : depth + depth_error_bound;
    depth = clamp_double(depth, 0.0, 1.0);
    candidate_depth = (float)depth;
    memcpy(
        &candidate_bits,
        &candidate_depth,
        sizeof(candidate_bits)
    );
    if (rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED) {
        if ((double)candidate_depth > depth && candidate_bits != 0u) {
            --candidate_bits;
        }
        for (guard = 0u;
             guard < SOC_RASTER_DEPTH_GUARD_ULPS &&
                candidate_bits != 0u;
             ++guard) {
            --candidate_bits;
        }
    } else {
        const uint32_t one_bits = UINT32_C(0x3f800000);

        if ((double)candidate_depth < depth &&
            candidate_bits < one_bits) {
            ++candidate_bits;
        }
        for (guard = 0u;
             guard < SOC_RASTER_DEPTH_GUARD_ULPS &&
                candidate_bits < one_bits;
             ++guard) {
            ++candidate_bits;
        }
    }
    memcpy(
        &candidate_depth,
        &candidate_bits,
        sizeof(candidate_depth)
    );

    depth_index = (size_t)pixel_y * rasterizer->width + pixel_x;
    stored_depth = rasterizer->depth[depth_index];
    passes_depth = rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
        ? (candidate_depth > stored_depth ? SOC_TRUE : SOC_FALSE)
        : (candidate_depth < stored_depth ? SOC_TRUE : SOC_FALSE);
    if (passes_depth == SOC_TRUE) {
        rasterizer->depth[depth_index] = candidate_depth;
    }
}

static void rasterize_small_constant_triangle(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const soc_raster_region* region = &setup->bounds;
    const int64_t sample_x =
        (int64_t)region->minimum_x * SOC_RASTER_SUBPIXEL_SCALE +
        SOC_RASTER_SUBPIXEL_HALF;
    const int64_t sample_y =
        (int64_t)region->minimum_y * SOC_RASTER_SUBPIXEL_SCALE +
        SOC_RASTER_SUBPIXEL_HALF;
    const float candidate_depth = make_conservative_depth(
        rasterizer,
        setup->depth_anchor,
        setup->depth_error_bound
    );
    int64_t row_edges[3];
    uint32_t edge_index;
    uint32_t pixel_y;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        row_edges[edge_index] = fixed_edge_value(
            &setup->edges[edge_index],
            sample_x,
            sample_y
        ) + setup->edges[edge_index].coverage_bias;
    }

    for (pixel_y = region->minimum_y;
         pixel_y < region->end_y;
         ++pixel_y) {
        int64_t edge0 = row_edges[0];
        int64_t edge1 = row_edges[1];
        int64_t edge2 = row_edges[2];
        uint32_t pixel_x;

        for (pixel_x = region->minimum_x;
             pixel_x < region->end_x;
             ++pixel_x) {
            if (edge0 >= 0 && edge1 >= 0 && edge2 >= 0) {
                store_depth_candidate(
                    rasterizer,
                    pixel_x,
                    pixel_y,
                    candidate_depth
                );
            }
            edge0 += setup->edges[0].step_x;
            edge1 += setup->edges[1].step_x;
            edge2 += setup->edges[2].step_x;
        }
        row_edges[0] += setup->edges[0].step_y;
        row_edges[1] += setup->edges[1].step_y;
        row_edges[2] += setup->edges[2].step_y;
    }
}

static soc_raster_block_classification classify_raster_block(
    const soc_raster_triangle_setup* setup,
    const int64_t edge_values[3],
    uint32_t block_width,
    uint32_t block_height
)
{
    soc_bool fully_covered = SOC_TRUE;
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        const soc_edge_equation* edge = &setup->edges[edge_index];
        const int64_t extent_x =
            edge->step_x * (int64_t)(block_width - 1u);
        const int64_t extent_y =
            edge->step_y * (int64_t)(block_height - 1u);
        int64_t minimum = edge_values[edge_index];
        int64_t maximum = edge_values[edge_index];

        if (extent_x < 0) {
            minimum += extent_x;
        } else {
            maximum += extent_x;
        }
        if (extent_y < 0) {
            minimum += extent_y;
        } else {
            maximum += extent_y;
        }

        if (maximum < 0) {
            return SOC_RASTER_BLOCK_OUTSIDE;
        }
        if (minimum < 0) {
            fully_covered = SOC_FALSE;
        }
    }

    return fully_covered == SOC_TRUE
        ? SOC_RASTER_BLOCK_FULL
        : SOC_RASTER_BLOCK_PARTIAL;
}

static uint64_t make_raster_block_mask(
    const soc_raster_triangle_setup* setup,
    const int64_t edge_values[3],
    uint32_t block_width,
    uint32_t block_height,
    soc_raster_block_classification classification
)
{
    uint64_t coverage_mask = 0u;
    uint32_t row;

    if (classification == SOC_RASTER_BLOCK_FULL) {
        const uint64_t row_mask =
            (UINT64_C(1) << block_width) - UINT64_C(1);

        for (row = 0u; row < block_height; ++row) {
            coverage_mask |= row_mask << (row * SOC_RASTER_BLOCK_SIZE);
        }
        return coverage_mask;
    }

    for (row = 0u; row < block_height; ++row) {
        int64_t edge0 = edge_values[0] +
            setup->edges[0].step_y * (int64_t)row;
        int64_t edge1 = edge_values[1] +
            setup->edges[1].step_y * (int64_t)row;
        int64_t edge2 = edge_values[2] +
            setup->edges[2].step_y * (int64_t)row;
        uint32_t column;

        for (column = 0u; column < block_width; ++column) {
            if (edge0 >= 0 && edge1 >= 0 && edge2 >= 0) {
                coverage_mask |= UINT64_C(1) <<
                    (row * SOC_RASTER_BLOCK_SIZE + column);
            }
            edge0 += setup->edges[0].step_x;
            edge1 += setup->edges[1].step_x;
            edge2 += setup->edges[2].step_x;
        }
    }
    return coverage_mask;
}

static void rasterize_depth_block(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask
)
{
    const double block_depth = setup->depth_anchor +
        setup->depth_step_x *
            ((double)block_x + 0.5 - setup->depth_anchor_x) +
        setup->depth_step_y *
            ((double)block_y + 0.5 - setup->depth_anchor_y);
    uint32_t row;

    if (setup->depth_step_x == 0.0 && setup->depth_step_y == 0.0) {
        const float candidate_depth = make_conservative_depth(
            rasterizer,
            block_depth,
            setup->depth_error_bound
        );

        rasterizer->kernels->store_constant_depth_block_f32(
            rasterizer->depth + (size_t)block_y * rasterizer->width +
                block_x,
            rasterizer->width,
            block_width,
            block_height,
            coverage_mask,
            candidate_depth,
            rasterizer->frame.depth_direction
        );
        return;
    }

    for (row = 0u; row < block_height; ++row) {
        double depth = block_depth + setup->depth_step_y * (double)row;
        uint32_t column;

        for (column = 0u; column < block_width; ++column) {
            const uint32_t bit =
                row * SOC_RASTER_BLOCK_SIZE + column;

            if ((coverage_mask & (UINT64_C(1) << bit)) != 0u) {
                rasterize_depth_sample(
                    rasterizer,
                    block_x + column,
                    block_y + row,
                    depth,
                    setup->depth_error_bound
                );
            }
            depth += setup->depth_step_x;
        }
    }
}

static void rasterize_triangle_blocks(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    const uint32_t bounds_width = region->end_x - region->minimum_x;
    const uint32_t bounds_height = region->end_y - region->minimum_y;
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);

    if (bounds_width <= SOC_RASTER_BLOCK_SIZE &&
        bounds_height <= SOC_RASTER_BLOCK_SIZE) {
        const int64_t sample_x =
            (int64_t)region->minimum_x *
                SOC_RASTER_SUBPIXEL_SCALE +
            SOC_RASTER_SUBPIXEL_HALF;
        const int64_t sample_y =
            (int64_t)region->minimum_y *
                SOC_RASTER_SUBPIXEL_SCALE +
            SOC_RASTER_SUBPIXEL_HALF;
        int64_t edge_values[3];
        uint64_t coverage_mask;
        uint32_t edge_index;

        for (edge_index = 0u; edge_index < 3u; ++edge_index) {
            edge_values[edge_index] = fixed_edge_value(
                &setup->edges[edge_index],
                sample_x,
                sample_y
            ) + setup->edges[edge_index].coverage_bias;
        }
        coverage_mask = make_raster_block_mask(
            setup,
            edge_values,
            bounds_width,
            bounds_height,
            SOC_RASTER_BLOCK_PARTIAL
        );
        if (coverage_mask != 0u) {
            rasterize_depth_block(
                rasterizer,
                setup,
                region->minimum_x,
                region->minimum_y,
                bounds_width,
                bounds_height,
                coverage_mask
            );
        }
        return;
    }

    for (; aligned_y < region->end_y;
         aligned_y += SOC_RASTER_BLOCK_SIZE) {
        const uint32_t block_y = aligned_y < region->minimum_y
            ? region->minimum_y
            : aligned_y;
        const uint32_t aligned_end_y =
            aligned_y + SOC_RASTER_BLOCK_SIZE;
        const uint32_t block_end_y = aligned_end_y < region->end_y
            ? aligned_end_y
            : region->end_y;
        const uint32_t block_height = block_end_y - block_y;
        uint32_t aligned_x = region->minimum_x &
            ~(SOC_RASTER_BLOCK_SIZE - 1u);

        for (; aligned_x < region->end_x;
             aligned_x += SOC_RASTER_BLOCK_SIZE) {
            const uint32_t block_x = aligned_x < region->minimum_x
                ? region->minimum_x
                : aligned_x;
            const uint32_t aligned_end_x =
                aligned_x + SOC_RASTER_BLOCK_SIZE;
            const uint32_t block_end_x = aligned_end_x < region->end_x
                ? aligned_end_x
                : region->end_x;
            const uint32_t block_width = block_end_x - block_x;
            const int64_t sample_x =
                (int64_t)block_x * SOC_RASTER_SUBPIXEL_SCALE +
                SOC_RASTER_SUBPIXEL_HALF;
            const int64_t sample_y =
                (int64_t)block_y * SOC_RASTER_SUBPIXEL_SCALE +
                SOC_RASTER_SUBPIXEL_HALF;
            int64_t edge_values[3];
            soc_raster_block_classification classification;
            uint64_t coverage_mask;
            uint32_t edge_index;

            for (edge_index = 0u; edge_index < 3u; ++edge_index) {
                edge_values[edge_index] = fixed_edge_value(
                    &setup->edges[edge_index],
                    sample_x,
                    sample_y
                ) + setup->edges[edge_index].coverage_bias;
            }
            classification = classify_raster_block(
                setup,
                edge_values,
                block_width,
                block_height
            );
            if (classification == SOC_RASTER_BLOCK_OUTSIDE) {
                continue;
            }
            coverage_mask = make_raster_block_mask(
                setup,
                edge_values,
                block_width,
                block_height,
                classification
            );
            if (coverage_mask != 0u) {
                rasterize_depth_block(
                    rasterizer,
                    setup,
                    block_x,
                    block_y,
                    block_width,
                    block_height,
                    coverage_mask
                );
            }
        }
    }
}

static SOC_NOINLINE soc_bool rasterize_prepared_triangle(
    soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    const soc_raster_depth_plane* shared_depth_plane
)
{
    soc_raster_triangle_setup setup;
    const soc_raster_setup_result setup_result = setup_raster_triangle(
        rasterizer,
        screen,
        shared_depth_plane,
        &setup
    );

    if (setup_result == SOC_RASTER_SETUP_REJECTED) {
        return SOC_FALSE;
    }
    if (setup_result == SOC_RASTER_SETUP_EMPTY) {
        return SOC_TRUE;
    }

    if (setup.bounds.end_x - setup.bounds.minimum_x <=
            SOC_RASTER_BLOCK_SIZE &&
        setup.bounds.end_y - setup.bounds.minimum_y <=
            SOC_RASTER_BLOCK_SIZE &&
        setup.depth_step_x == 0.0 && setup.depth_step_y == 0.0) {
        rasterize_small_constant_triangle(rasterizer, &setup);
        return SOC_TRUE;
    }

    rasterize_triangle_blocks(rasterizer, &setup, &setup.bounds);
    return SOC_TRUE;
}

static soc_bool rasterize_triangle(
    soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided
)
{
    soc_screen_vertex screen[3];
    const soc_raster_setup_result setup_result = prepare_screen_triangle(
        rasterizer,
        clip0,
        clip1,
        clip2,
        two_sided,
        screen
    );

    if (setup_result == SOC_RASTER_SETUP_REJECTED) {
        return SOC_FALSE;
    }
    return rasterize_prepared_triangle(rasterizer, screen, NULL);
}

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count,
    const soc_kernel_table* kernels
)
{
    size_t required_element_count;

    if (rasterizer == NULL ||
        width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        depth == NULL ||
        kernels == NULL ||
        kernels->clear_f32 == NULL ||
        kernels->store_constant_depth_block_f32 == NULL ||
        !checked_size_multiply(
            (size_t)width,
            (size_t)height,
            &required_element_count
        ) ||
        depth_element_count < required_element_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    memset(rasterizer, 0, sizeof(*rasterizer));
    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->depth_element_count = required_element_count;
    rasterizer->depth = depth;
    rasterizer->kernels = kernels;
    rasterizer->clipped_triangle_count = 0u;
    rasterizer->rasterized_triangle_count = 0u;
    rasterizer->initialized = SOC_TRUE;
    rasterizer->frame_active = SOC_FALSE;
    memset(&rasterizer->frame, 0, sizeof(rasterizer->frame));
    return SOC_RESULT_OK;
}

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL) {
        return;
    }

    memset(rasterizer, 0, sizeof(*rasterizer));
}

soc_result soc_rasterizer_resize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count
)
{
    size_t required_element_count;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        depth == NULL ||
        !checked_size_multiply(
            (size_t)width,
            (size_t)height,
            &required_element_count
        ) ||
        depth_element_count < required_element_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->depth_element_count = required_element_count;
    rasterizer->depth = depth;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
)
{
    float clear_depth;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        rasterizer->depth == NULL ||
        desc == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    rasterizer->frame = *desc;
    clear_depth = desc->depth_direction == SOC_DEPTH_REVERSED
        ? 0.0f
        : 1.0f;
    rasterizer->kernels->clear_f32(
        rasterizer->depth,
        rasterizer->depth_element_count,
        clear_depth
    );
    rasterizer->clipped_triangle_count = 0u;
    rasterizer->rasterized_triangle_count = 0u;
    rasterizer->frame_active = SOC_TRUE;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_submit_occluders(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
)
{
    size_t transform_byte_count;
    uint32_t instance;

    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        mesh == NULL ||
        mesh->positions_xyz == NULL ||
        mesh->indices == NULL ||
        object_to_world == NULL ||
        instance_count == 0u ||
        !checked_size_multiply(
            (size_t)instance_count,
            sizeof(*object_to_world),
            &transform_byte_count
        )) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (instance = 0u; instance < instance_count; ++instance) {
        const soc_mat4* instance_transform = &object_to_world[instance];
        const uint32_t triangle_count = mesh->index_count / 3u;
        uint32_t triangle;

        for (triangle = 0u; triangle < triangle_count; ++triangle) {
            soc_clip_vertex clip_triangle_vertices[3];
            soc_clip_vertex clipped_polygon[SOC_MAX_CLIPPED_VERTICES];
            soc_raster_depth_plane fan_depth_plane;
            const soc_raster_depth_plane* shared_depth_plane = NULL;
            soc_clip_outcode active_planes;
            soc_clip_classification clip_classification;
            uint32_t clipped_vertex_count;
            uint32_t corner;
            uint32_t fan_index;

            for (corner = 0u; corner < 3u; ++corner) {
                const uint32_t mesh_index = read_mesh_index(
                    mesh,
                    triangle * 3u + corner
                );
                const size_t position_index = (size_t)mesh_index * 3u;
                const soc_clip_vertex object_position = {
                    mesh->positions_xyz[position_index],
                    mesh->positions_xyz[position_index + 1u],
                    mesh->positions_xyz[position_index + 2u],
                    1.0,
                };
                const soc_clip_vertex world_position = transform_vertex(
                    instance_transform,
                    &object_position
                );

                clip_triangle_vertices[corner] = transform_vertex(
                    &rasterizer->frame.clip_from_world,
                    &world_position
                );
            }

            clip_classification = classify_clip_triangle(
                clip_triangle_vertices,
                rasterizer->frame.clip_depth_range,
                &active_planes
            );
            if (clip_classification == SOC_CLIP_CLASSIFICATION_NONFINITE ||
                clip_classification == SOC_CLIP_CLASSIFICATION_REJECT) {
                ++rasterizer->clipped_triangle_count;
                continue;
            }

            if (clip_classification == SOC_CLIP_CLASSIFICATION_ACCEPT) {
                if (rasterize_triangle(
                        rasterizer,
                        &clip_triangle_vertices[0],
                        &clip_triangle_vertices[1],
                        &clip_triangle_vertices[2],
                        (mesh->flags & SOC_MESH_FLAG_TWO_SIDED) != 0u
                            ? SOC_TRUE
                            : SOC_FALSE
                    ) == SOC_TRUE) {
                    ++rasterizer->rasterized_triangle_count;
                }
                continue;
            }

            ++rasterizer->clipped_triangle_count;
            clipped_vertex_count = clip_triangle(
                rasterizer,
                clip_triangle_vertices,
                active_planes,
                clipped_polygon
            );
            if (clipped_vertex_count < 3u) {
                continue;
            }

            for (fan_index = 1u;
                 fan_index + 1u < clipped_vertex_count;
                 ++fan_index) {
                soc_screen_vertex screen[3];
                const soc_raster_setup_result setup_result =
                    prepare_screen_triangle(
                        rasterizer,
                        &clipped_polygon[0],
                        &clipped_polygon[fan_index],
                        &clipped_polygon[fan_index + 1u],
                        (mesh->flags & SOC_MESH_FLAG_TWO_SIDED) != 0u
                            ? SOC_TRUE
                            : SOC_FALSE,
                        screen
                    );

                if (setup_result == SOC_RASTER_SETUP_REJECTED) {
                    continue;
                }
                if (clipped_vertex_count > 3u &&
                    shared_depth_plane == NULL &&
                    configure_shared_fan_depth_plane(
                        rasterizer,
                        screen,
                        &fan_depth_plane
                    ) == SOC_TRUE) {
                    shared_depth_plane = &fan_depth_plane;
                }
                if (rasterize_prepared_triangle(
                        rasterizer,
                        screen,
                        shared_depth_plane
                    ) == SOC_TRUE) {
                    ++rasterizer->rasterized_triangle_count;
                }
            }
        }
    }

    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_finish_occluders(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL || rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_end_frame(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL || rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    rasterizer->frame_active = SOC_FALSE;
    return SOC_RESULT_OK;
}
