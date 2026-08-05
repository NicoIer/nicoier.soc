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

typedef soc_kernel_clip_vertex soc_clip_vertex;

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
    int64_t error_bound;
    int64_t inside_threshold;
    int64_t outside_threshold;
    double exact_start_x;
    double exact_start_y;
    double exact_end_x;
    double exact_end_y;
    soc_bool top_left;
    soc_bool exact_q8;
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
    soc_bool exact_coverage_only;
    soc_bool exact_q8;
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

/*
 * The expansion helpers and orient2d filter below are a compact adaptation
 * of Jonathan Richard Shewchuk's public-domain robust predicates.  The fast
 * determinant handles ordinary samples; exact expansion arithmetic is used
 * only when its sign is not reliable.
 * https://www.cs.cmu.edu/~quake/robust.html
 */
#define SOC_ROBUST_SPLITTER 134217729.0

static void robust_fast_two_sum(
    double left,
    double right,
    double* out_sum,
    double* out_error
)
{
    volatile double sum = left + right;
    const double right_virtual = sum - left;

    *out_sum = sum;
    *out_error = right - right_virtual;
}

static void robust_two_sum(
    double left,
    double right,
    double* out_sum,
    double* out_error
)
{
    volatile double sum = left + right;
    const double right_virtual = sum - left;
    const double left_virtual = sum - right_virtual;
    const double right_roundoff = right - right_virtual;
    const double left_roundoff = left - left_virtual;

    *out_sum = sum;
    *out_error = left_roundoff + right_roundoff;
}

static void robust_split(
    double value,
    double* out_high,
    double* out_low
)
{
    volatile double combined = SOC_ROBUST_SPLITTER * value;
    const double large = combined - value;
    const double high = combined - large;

    *out_high = high;
    *out_low = value - high;
}

static void robust_two_product(
    double left,
    double right,
    double out_product[2]
)
{
    double left_high;
    double left_low;
    double right_high;
    double right_low;
    double error1;
    double error2;
    double error3;
    volatile double product = left * right;

    robust_split(left, &left_high, &left_low);
    robust_split(right, &right_high, &right_low);
    error1 = product - left_high * right_high;
    error2 = error1 - left_low * right_high;
    error3 = error2 - left_high * right_low;
    out_product[0] = left_low * right_low - error3;
    out_product[1] = product;
}

static double robust_two_difference_tail(
    double left,
    double right,
    double difference
)
{
    const double right_virtual = left - difference;
    const double left_virtual = difference + right_virtual;
    const double right_roundoff = right_virtual - right;
    const double left_roundoff = left - left_virtual;

    return left_roundoff + right_roundoff;
}

static int robust_expansion_sum_zero_eliminate(
    int left_length,
    const double* left,
    int right_length,
    const double* right,
    double* out_sum
)
{
    int left_index = 0;
    int right_index = 0;
    int output_index = 0;
    double left_now = left[0];
    double right_now = right[0];
    double accumulator;

    if ((right_now > left_now) == (right_now > -left_now)) {
        accumulator = left_now;
        ++left_index;
    } else {
        accumulator = right_now;
        ++right_index;
    }

    if (left_index < left_length && right_index < right_length) {
        double next;
        double roundoff;

        left_now = left[left_index];
        right_now = right[right_index];
        if ((right_now > left_now) == (right_now > -left_now)) {
            robust_fast_two_sum(
                left_now,
                accumulator,
                &next,
                &roundoff
            );
            ++left_index;
        } else {
            robust_fast_two_sum(
                right_now,
                accumulator,
                &next,
                &roundoff
            );
            ++right_index;
        }
        accumulator = next;
        if (roundoff != 0.0) {
            out_sum[output_index++] = roundoff;
        }

        while (left_index < left_length &&
               right_index < right_length) {
            left_now = left[left_index];
            right_now = right[right_index];
            if ((right_now > left_now) ==
                (right_now > -left_now)) {
                robust_two_sum(
                    accumulator,
                    left_now,
                    &next,
                    &roundoff
                );
                ++left_index;
            } else {
                robust_two_sum(
                    accumulator,
                    right_now,
                    &next,
                    &roundoff
                );
                ++right_index;
            }
            accumulator = next;
            if (roundoff != 0.0) {
                out_sum[output_index++] = roundoff;
            }
        }
    }

    while (left_index < left_length) {
        double next;
        double roundoff;

        robust_two_sum(
            accumulator,
            left[left_index],
            &next,
            &roundoff
        );
        ++left_index;
        accumulator = next;
        if (roundoff != 0.0) {
            out_sum[output_index++] = roundoff;
        }
    }
    while (right_index < right_length) {
        double next;
        double roundoff;

        robust_two_sum(
            accumulator,
            right[right_index],
            &next,
            &roundoff
        );
        ++right_index;
        accumulator = next;
        if (roundoff != 0.0) {
            out_sum[output_index++] = roundoff;
        }
    }
    if (accumulator != 0.0 || output_index == 0) {
        out_sum[output_index++] = accumulator;
    }
    return output_index;
}

static int robust_product_difference(
    double positive_left,
    double positive_right,
    double negative_left,
    double negative_right,
    double out_difference[4]
)
{
    double positive[2];
    double negative[2];

    robust_two_product(positive_left, positive_right, positive);
    robust_two_product(negative_left, negative_right, negative);
    negative[0] = -negative[0];
    negative[1] = -negative[1];
    return robust_expansion_sum_zero_eliminate(
        2,
        positive,
        2,
        negative,
        out_difference
    );
}

static SOC_NOINLINE int exact_orient2d_sign(
    double point0_x,
    double point0_y,
    double point1_x,
    double point1_y,
    double point2_x,
    double point2_y
)
{
    const double point0_delta_x = point0_x - point2_x;
    const double point0_delta_y = point0_y - point2_y;
    const double point1_delta_x = point1_x - point2_x;
    const double point1_delta_y = point1_y - point2_y;
    const double point0_tail_x = robust_two_difference_tail(
        point0_x,
        point2_x,
        point0_delta_x
    );
    const double point0_tail_y = robust_two_difference_tail(
        point0_y,
        point2_y,
        point0_delta_y
    );
    const double point1_tail_x = robust_two_difference_tail(
        point1_x,
        point2_x,
        point1_delta_x
    );
    const double point1_tail_y = robust_two_difference_tail(
        point1_y,
        point2_y,
        point1_delta_y
    );
    double base[4];
    double correction[4];
    double partial0[8];
    double partial1[12];
    double determinant[16];
    const int base_length = robust_product_difference(
        point0_delta_x,
        point1_delta_y,
        point0_delta_y,
        point1_delta_x,
        base
    );
    int partial0_length;
    int partial1_length;
    int determinant_length;
    int correction_length;
    double most_significant;

    if (point0_tail_x == 0.0 && point0_tail_y == 0.0 &&
        point1_tail_x == 0.0 && point1_tail_y == 0.0) {
        most_significant = base[base_length - 1];
        return most_significant > 0.0
            ? 1
            : (most_significant < 0.0 ? -1 : 0);
    }

    correction_length = robust_product_difference(
        point0_tail_x,
        point1_delta_y,
        point0_tail_y,
        point1_delta_x,
        correction
    );
    partial0_length = robust_expansion_sum_zero_eliminate(
        base_length,
        base,
        correction_length,
        correction,
        partial0
    );
    correction_length = robust_product_difference(
        point0_delta_x,
        point1_tail_y,
        point0_delta_y,
        point1_tail_x,
        correction
    );
    partial1_length = robust_expansion_sum_zero_eliminate(
        partial0_length,
        partial0,
        correction_length,
        correction,
        partial1
    );
    correction_length = robust_product_difference(
        point0_tail_x,
        point1_tail_y,
        point0_tail_y,
        point1_tail_x,
        correction
    );
    determinant_length = robust_expansion_sum_zero_eliminate(
        partial1_length,
        partial1,
        correction_length,
        correction,
        determinant
    );
    most_significant = determinant[determinant_length - 1];

    return most_significant > 0.0
        ? 1
        : (most_significant < 0.0 ? -1 : 0);
}

static inline int robust_orient2d_sign(
    double point0_x,
    double point0_y,
    double point1_x,
    double point1_y,
    double point2_x,
    double point2_y
)
{
    const double left =
        (point0_x - point2_x) * (point1_y - point2_y);
    const double right =
        (point0_y - point2_y) * (point1_x - point2_x);
    const double determinant = left - right;
    double determinant_sum;

    if (left > 0.0) {
        if (right <= 0.0) {
            return determinant > 0.0 ? 1 : -1;
        }
        determinant_sum = left + right;
    } else if (left < 0.0) {
        if (right >= 0.0) {
            return determinant < 0.0 ? -1 : 1;
        }
        determinant_sum = -left - right;
    } else {
        return right > 0.0 ? -1 : (right < 0.0 ? 1 : 0);
    }

    {
        const double epsilon = DBL_EPSILON * 0.5;
        const double error_bound =
            (3.0 + 16.0 * epsilon) * epsilon * determinant_sum;

        if (determinant >= error_bound) {
            return 1;
        }
        if (-determinant >= error_bound) {
            return -1;
        }
    }
    return exact_orient2d_sign(
        point0_x,
        point0_y,
        point1_x,
        point1_y,
        point2_x,
        point2_y
    );
}

#undef SOC_ROBUST_SPLITTER

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
        .error_bound = 0,
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

static void configure_edge_coverage(
    soc_edge_equation* fixed_edge,
    const soc_screen_vertex* exact_start,
    const soc_screen_vertex* exact_end,
    soc_bool endpoints_exact,
    int64_t position_error_bound
)
{
    fixed_edge->exact_start_x = exact_start->x;
    fixed_edge->exact_start_y = exact_start->y;
    fixed_edge->exact_end_x = exact_end->x;
    fixed_edge->exact_end_y = exact_end->y;
    fixed_edge->top_left =
        exact_end->y < exact_start->y ||
            (exact_end->y == exact_start->y &&
                exact_end->x > exact_start->x)
            ? SOC_TRUE
            : SOC_FALSE;
    fixed_edge->exact_q8 = endpoints_exact;

    if (endpoints_exact == SOC_TRUE) {
        fixed_edge->error_bound = 0;
        fixed_edge->inside_threshold =
            fixed_edge->top_left == SOC_TRUE ? 0 : 1;
        fixed_edge->outside_threshold = fixed_edge->inside_threshold;
        return;
    }

    {
        /*
         * Nearest Q8 snapping moves each endpoint component by at most half
         * a fixed-point unit.  This symmetric bound encloses the difference
         * between the raw fixed edge and the unsnapped edge over the complete
         * bbox.  Samples inside the uncertainty interval use exact orient2d.
         */
        const int64_t absolute_delta_x = fixed_edge->delta_x < 0
            ? -fixed_edge->delta_x
            : fixed_edge->delta_x;
        const int64_t absolute_delta_y = fixed_edge->delta_y < 0
            ? -fixed_edge->delta_y
            : fixed_edge->delta_y;
        fixed_edge->error_bound = position_error_bound +
            (absolute_delta_x + absolute_delta_y + 1) /
                2 +
            2;
        fixed_edge->inside_threshold = fixed_edge->error_bound +
            (fixed_edge->top_left == SOC_TRUE ? 0 : 1);
        fixed_edge->outside_threshold = -fixed_edge->error_bound;
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
    int screen_orientation;
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

    screen_orientation = robust_orient2d_sign(
        screen[0].x,
        screen[0].y,
        screen[1].x,
        screen[1].y,
        screen[2].x,
        screen[2].y
    );
    if (screen_orientation == 0) {
        return SOC_RASTER_SETUP_REJECTED;
    }
    if (two_sided != SOC_TRUE) {
        const soc_bool front_facing =
            rasterizer->frame.front_face == SOC_FRONT_FACE_CCW
                ? (screen_orientation < 0 ? SOC_TRUE : SOC_FALSE)
                : (screen_orientation > 0 ? SOC_TRUE : SOC_FALSE);

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
    if (screen_orientation < 0) {
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
    double exact_minimum_x;
    double exact_maximum_x;
    double exact_minimum_y;
    double exact_maximum_y;
    soc_bool all_fixed_exact;
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
    all_fixed_exact = fixed_exact[0] == SOC_TRUE &&
        fixed_exact[1] == SOC_TRUE &&
        fixed_exact[2] == SOC_TRUE
        ? SOC_TRUE
        : SOC_FALSE;
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
    out_setup->exact_coverage_only = fixed_area <= 0
        ? SOC_TRUE
        : SOC_FALSE;
    out_setup->exact_q8 = all_fixed_exact;

    minimum_x = fixed[0].x;
    maximum_x = fixed[0].x;
    minimum_y = fixed[0].y;
    maximum_y = fixed[0].y;
    exact_minimum_x = screen[0].x;
    exact_maximum_x = screen[0].x;
    exact_minimum_y = screen[0].y;
    exact_maximum_y = screen[0].y;
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
        if (screen[index].x < exact_minimum_x) {
            exact_minimum_x = screen[index].x;
        }
        if (screen[index].x > exact_maximum_x) {
            exact_maximum_x = screen[index].x;
        }
        if (screen[index].y < exact_minimum_y) {
            exact_minimum_y = screen[index].y;
        }
        if (screen[index].y > exact_maximum_y) {
            exact_maximum_y = screen[index].y;
        }
    }

    if ((all_fixed_exact == SOC_TRUE &&
            (maximum_x < SOC_RASTER_SUBPIXEL_HALF ||
                maximum_y < SOC_RASTER_SUBPIXEL_HALF)) ||
        (all_fixed_exact != SOC_TRUE &&
            (exact_maximum_x < 0.5 || exact_maximum_y < 0.5))) {
        return SOC_RASTER_SETUP_EMPTY;
    }

    if (all_fixed_exact == SOC_TRUE) {
        out_setup->bounds.minimum_x =
            minimum_x <= SOC_RASTER_SUBPIXEL_HALF
            ? 0u
            : (uint32_t)(
                (minimum_x - SOC_RASTER_SUBPIXEL_HALF +
                    SOC_RASTER_SUBPIXEL_SCALE - 1) /
                SOC_RASTER_SUBPIXEL_SCALE
            );
        out_setup->bounds.minimum_y =
            minimum_y <= SOC_RASTER_SUBPIXEL_HALF
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
    } else {
        const double minimum_sample_x = exact_minimum_x - 0.5;
        const double minimum_sample_y = exact_minimum_y - 0.5;

        out_setup->bounds.minimum_x = minimum_sample_x <= 0.0
            ? 0u
            : (uint32_t)minimum_sample_x;
        out_setup->bounds.minimum_y = minimum_sample_y <= 0.0
            ? 0u
            : (uint32_t)minimum_sample_y;
        if ((double)out_setup->bounds.minimum_x < minimum_sample_x) {
            ++out_setup->bounds.minimum_x;
        }
        if ((double)out_setup->bounds.minimum_y < minimum_sample_y) {
            ++out_setup->bounds.minimum_y;
        }
        out_setup->bounds.end_x =
            (uint32_t)(exact_maximum_x - 0.5) + 1u;
        out_setup->bounds.end_y =
            (uint32_t)(exact_maximum_y - 0.5) + 1u;
    }
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
        configure_edge_coverage(
            &out_setup->edges[0],
            &screen[1],
            &screen[2],
            fixed_exact[1] == SOC_TRUE && fixed_exact[2] == SOC_TRUE
                ? SOC_TRUE
                : SOC_FALSE,
            position_error_bound
        );
        configure_edge_coverage(
            &out_setup->edges[1],
            &screen[2],
            &screen[0],
            fixed_exact[2] == SOC_TRUE && fixed_exact[0] == SOC_TRUE
                ? SOC_TRUE
                : SOC_FALSE,
            position_error_bound
        );
        configure_edge_coverage(
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
            all_fixed_exact,
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

static soc_bool robust_triangle_contains_sample(
    const soc_raster_triangle_setup* setup,
    double sample_x,
    double sample_y
)
{
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        const soc_edge_equation* edge = &setup->edges[edge_index];
        const int orientation = robust_orient2d_sign(
            edge->exact_start_x,
            edge->exact_start_y,
            edge->exact_end_x,
            edge->exact_end_y,
            sample_x,
            sample_y
        );

        if (orientation < 0 ||
            (orientation == 0 && edge->top_left != SOC_TRUE)) {
            return SOC_FALSE;
        }
    }
    return SOC_TRUE;
}

static inline soc_bool robust_edge_contains_sample(
    const soc_edge_equation* edge,
    double sample_x,
    double sample_y
)
{
    const int orientation = robust_orient2d_sign(
        edge->exact_start_x,
        edge->exact_start_y,
        edge->exact_end_x,
        edge->exact_end_y,
        sample_x,
        sample_y
    );

    return orientation > 0 ||
        (orientation == 0 && edge->top_left == SOC_TRUE)
        ? SOC_TRUE
        : SOC_FALSE;
}

static inline soc_bool raster_triangle_contains_sample(
    const soc_raster_triangle_setup* setup,
    const int64_t edge_values[3],
    uint32_t pixel_x,
    uint32_t pixel_y
)
{
    const soc_edge_equation* edge0 = &setup->edges[0];
    const soc_edge_equation* edge1 = &setup->edges[1];
    const soc_edge_equation* edge2 = &setup->edges[2];

    if (setup->exact_coverage_only == SOC_TRUE) {
        return robust_triangle_contains_sample(
            setup,
            (double)pixel_x + 0.5,
            (double)pixel_y + 0.5
        );
    }
    if (edge_values[0] >= edge0->inside_threshold &&
        edge_values[1] >= edge1->inside_threshold &&
        edge_values[2] >= edge2->inside_threshold) {
        return SOC_TRUE;
    }
    if (edge_values[0] < edge0->outside_threshold ||
        edge_values[1] < edge1->outside_threshold ||
        edge_values[2] < edge2->outside_threshold) {
        return SOC_FALSE;
    }
    {
        const double sample_x = (double)pixel_x + 0.5;
        const double sample_y = (double)pixel_y + 0.5;

        if (edge_values[0] < edge0->inside_threshold &&
            robust_edge_contains_sample(
                edge0,
                sample_x,
                sample_y
            ) != SOC_TRUE) {
            return SOC_FALSE;
        }
        if (edge_values[1] < edge1->inside_threshold &&
            robust_edge_contains_sample(
                edge1,
                sample_x,
                sample_y
            ) != SOC_TRUE) {
            return SOC_FALSE;
        }
        if (edge_values[2] < edge2->inside_threshold &&
            robust_edge_contains_sample(
                edge2,
                sample_x,
                sample_y
            ) != SOC_TRUE) {
            return SOC_FALSE;
        }
    }
    return SOC_TRUE;
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
        );
        if (setup->exact_q8 == SOC_TRUE &&
            setup->edges[edge_index].top_left != SOC_TRUE) {
            --row_edges[edge_index];
        }
    }

    if (setup->exact_q8 == SOC_TRUE) {
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
        return;
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
            const int64_t edge_values[3] = {edge0, edge1, edge2};

            if (raster_triangle_contains_sample(
                    setup,
                    edge_values,
                    pixel_x,
                    pixel_y
                ) == SOC_TRUE) {
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

    if (setup->exact_coverage_only == SOC_TRUE) {
        return SOC_RASTER_BLOCK_PARTIAL;
    }

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

        if (setup->exact_q8 == SOC_TRUE) {
            if (maximum < 0) {
                return SOC_RASTER_BLOCK_OUTSIDE;
            }
            if (minimum < 0) {
                fully_covered = SOC_FALSE;
            }
        } else {
            if (maximum < edge->outside_threshold) {
                return SOC_RASTER_BLOCK_OUTSIDE;
            }
            if (minimum < edge->inside_threshold) {
                fully_covered = SOC_FALSE;
            }
        }
    }

    return fully_covered == SOC_TRUE
        ? SOC_RASTER_BLOCK_FULL
        : SOC_RASTER_BLOCK_PARTIAL;
}

static uint64_t make_raster_block_mask(
    const soc_raster_triangle_setup* setup,
    const int64_t edge_values[3],
    uint32_t block_x,
    uint32_t block_y,
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

    if (setup->exact_q8 == SOC_TRUE) {
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

    for (row = 0u; row < block_height; ++row) {
        int64_t edge0 = edge_values[0] +
            setup->edges[0].step_y * (int64_t)row;
        int64_t edge1 = edge_values[1] +
            setup->edges[1].step_y * (int64_t)row;
        int64_t edge2 = edge_values[2] +
            setup->edges[2].step_y * (int64_t)row;
        uint32_t column;

        for (column = 0u; column < block_width; ++column) {
            const int64_t pixel_edges[3] = {edge0, edge1, edge2};

            if (raster_triangle_contains_sample(
                    setup,
                    pixel_edges,
                    block_x + column,
                    block_y + row
                ) == SOC_TRUE) {
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
            );
            if (setup->exact_q8 == SOC_TRUE &&
                setup->edges[edge_index].top_left != SOC_TRUE) {
                --edge_values[edge_index];
            }
        }
        coverage_mask = make_raster_block_mask(
            setup,
            edge_values,
            region->minimum_x,
            region->minimum_y,
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
                );
                if (setup->exact_q8 == SOC_TRUE &&
                    setup->edges[edge_index].top_left != SOC_TRUE) {
                    --edge_values[edge_index];
                }
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
                block_x,
                block_y,
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
        kernels->transform_triangle_f64 == NULL ||
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
    soc_kernel_mat4_f64 clip_from_world_f64;
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

    soc_kernel_mat4_f64_from_f32(
        &rasterizer->frame.clip_from_world,
        &clip_from_world_f64
    );

    for (instance = 0u; instance < instance_count; ++instance) {
        const soc_mat4* instance_transform = &object_to_world[instance];
        soc_kernel_mat4_f64 object_to_world_f64;
        soc_kernel_mat4_f64 clip_from_object_f64;
        const uint32_t triangle_count = mesh->index_count / 3u;
        uint32_t triangle;

        soc_kernel_mat4_f64_from_f32(
            instance_transform,
            &object_to_world_f64
        );
        soc_kernel_mat4_f64_multiply(
            &clip_from_world_f64,
            &object_to_world_f64,
            &clip_from_object_f64
        );

        for (triangle = 0u; triangle < triangle_count; ++triangle) {
            soc_clip_vertex clip_triangle_vertices[3];
            soc_clip_vertex clipped_polygon[SOC_MAX_CLIPPED_VERTICES];
            soc_raster_depth_plane fan_depth_plane;
            const soc_raster_depth_plane* shared_depth_plane = NULL;
            soc_clip_outcode active_planes;
            soc_clip_classification clip_classification;
            soc_kernel_clip_metadata clip_metadata;
            uint32_t clipped_vertex_count;
            uint32_t fan_index;
            const uint32_t mesh_index0 = read_mesh_index(
                mesh,
                triangle * 3u
            );
            const uint32_t mesh_index1 = read_mesh_index(
                mesh,
                triangle * 3u + 1u
            );
            const uint32_t mesh_index2 = read_mesh_index(
                mesh,
                triangle * 3u + 2u
            );

            rasterizer->kernels->transform_triangle_f64(
                &clip_from_object_f64,
                mesh->positions_xyz + (size_t)mesh_index0 * 3u,
                mesh->positions_xyz + (size_t)mesh_index1 * 3u,
                mesh->positions_xyz + (size_t)mesh_index2 * 3u,
                mesh->positions_all_finite,
                rasterizer->frame.clip_depth_range,
                clip_triangle_vertices,
                &clip_metadata
            );
            active_planes = clip_metadata.active_planes;
            if (clip_metadata.all_finite != SOC_TRUE) {
                clip_classification = SOC_CLIP_CLASSIFICATION_NONFINITE;
            } else if (clip_metadata.common_planes != 0u) {
                clip_classification = SOC_CLIP_CLASSIFICATION_REJECT;
            } else {
                clip_classification = active_planes == 0u
                    ? SOC_CLIP_CLASSIFICATION_ACCEPT
                    : SOC_CLIP_CLASSIFICATION_PARTIAL;
            }
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
