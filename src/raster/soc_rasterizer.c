#include "raster/soc_rasterizer.h"

#include "core/soc_mesh.h"
#include "platform/soc_memory.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SOC_CLIP_PLANE_COUNT 6u
#define SOC_MAX_CLIPPED_VERTICES 12u
#define SOC_RASTER_SUBPIXEL_BITS 8u
#define SOC_RASTER_SUBPIXEL_SCALE \
    ((int64_t)1 << SOC_RASTER_SUBPIXEL_BITS)
#define SOC_RASTER_SUBPIXEL_HALF \
    (SOC_RASTER_SUBPIXEL_SCALE / 2)
#define SOC_RASTER_BLOCK_SIZE SOC_KERNEL_RASTER_BLOCK_SIZE
#define SOC_RASTER_UNTRACKED_TRIANGLE_SIZE UINT32_C(16)
#define SOC_RASTER_DEPTH_GUARD_ULPS 1u
#define SOC_RASTER_EVALUATION_ERROR_SCALE 256.0

#if defined(_MSC_VER)
#define SOC_NOINLINE __declspec(noinline)
#define SOC_MAYBE_UNUSED
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_NOINLINE __attribute__((noinline))
#define SOC_MAYBE_UNUSED __attribute__((unused))
#else
#define SOC_NOINLINE
#define SOC_MAYBE_UNUSED
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
    SOC_CLIP_CLASSIFICATION_ACCEPT = 0,
    SOC_CLIP_CLASSIFICATION_REJECT,
    SOC_CLIP_CLASSIFICATION_PARTIAL,
} soc_clip_classification;

typedef soc_kernel_clip_vertex soc_clip_vertex;

typedef struct soc_screen_vertex {
    double x;
    double y;
    double depth;
    int64_t fixed_x;
    int64_t fixed_y;
} soc_screen_vertex;

typedef struct soc_fixed_vertex {
    int64_t x;
    int64_t y;
} soc_fixed_vertex;

typedef soc_raster_prepared_edge soc_edge_equation;
typedef soc_raster_prepared_region soc_raster_region;

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

typedef soc_raster_prepared_triangle soc_raster_triangle_setup;

typedef struct soc_raster_depth_plane {
    double anchor_x;
    double anchor_y;
    double anchor;
    double step_x;
    double step_y;
    double error_bound;
} soc_raster_depth_plane;

typedef struct soc_raster_depth_block_candidate {
    float depth_origin;
    float depth_step_x;
    float depth_step_y;
    float nearest_depth;
    float farthest_depth;
    soc_bool is_constant;
} soc_raster_depth_block_candidate;

_Static_assert(
    sizeof(soc_raster_prepared_edge) == 24u,
    "prepared edge layout must remain 24 bytes"
);
_Static_assert(
    sizeof(soc_raster_prepared_triangle) == 128u,
    "prepared triangle layout must remain two cache lines"
);
_Static_assert(
    SOC_MAX_RASTER_DIMENSION <= (UINT32_C(1) << 20u),
    "compact fixed edges require raster dimensions at most 2^20"
);
_Static_assert(
    (SOC_MAX_RASTER_DIMENSION + SOC_RASTER_LOCK_TILE_SIZE - 1u) /
        SOC_RASTER_LOCK_TILE_SIZE <= UINT16_MAX,
    "prepared tile ranges must fit in uint16_t"
);

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

static soc_bool calculate_early_z_block_grid(
    uint32_t width,
    uint32_t height,
    uint32_t* out_column_count,
    uint32_t* out_row_count,
    size_t* out_block_count
)
{
    const uint32_t column_count =
        width / SOC_RASTER_BLOCK_SIZE +
        (width % SOC_RASTER_BLOCK_SIZE != 0u ? 1u : 0u);
    const uint32_t row_count =
        height / SOC_RASTER_BLOCK_SIZE +
        (height % SOC_RASTER_BLOCK_SIZE != 0u ? 1u : 0u);
    size_t block_count;

    if (out_column_count == NULL ||
        out_row_count == NULL ||
        out_block_count == NULL ||
        !checked_size_multiply(
            (size_t)column_count,
            (size_t)row_count,
            &block_count
        )) {
        return SOC_FALSE;
    }

    *out_column_count = column_count;
    *out_row_count = row_count;
    *out_block_count = block_count;
    return SOC_TRUE;
}

static void* allocate_early_z_storage(
    size_t block_count,
    size_t tile_count,
    float** out_farthest_depths,
    float** out_tile_farthest_depths
)
{
    const size_t farthest_bytes = block_count * sizeof(float);
    const size_t tile_offset =
        (farthest_bytes + 63u) & ~(size_t)63u;
    const size_t allocation_size =
        tile_offset + tile_count * sizeof(float);
    uint8_t* storage = soc_aligned_alloc(64u, allocation_size);

    if (storage != NULL) {
        *out_farthest_depths = (float*)storage;
        *out_tile_farthest_depths = (float*)(storage + tile_offset);
    }
    return storage;
}

static uint64_t* allocate_early_z_pending_masks(size_t block_count)
{
    return soc_aligned_alloc(64u, block_count * sizeof(uint64_t));
}

static soc_bool prepared_list_is_valid(
    const soc_raster_prepared_list* list
)
{
    if (list == NULL || list->count > list->capacity) {
        return SOC_FALSE;
    }
    if (list->capacity == 0u) {
        return list->data == NULL && list->count == 0u
            ? SOC_TRUE
            : SOC_FALSE;
    }
    return list->data != NULL ? SOC_TRUE : SOC_FALSE;
}

soc_result soc_raster_prepared_list_reserve(
    soc_raster_prepared_list* list,
    size_t minimum_capacity
)
{
    soc_raster_prepared_triangle* allocation;
    size_t allocation_size;

    if (prepared_list_is_valid(list) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (minimum_capacity <= list->capacity) {
        return SOC_RESULT_OK;
    }
    if (!checked_size_multiply(
            minimum_capacity,
            sizeof(*allocation),
            &allocation_size
        )) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    allocation = soc_aligned_alloc(128u, allocation_size);
    if (allocation == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    if (list->count != 0u) {
        memcpy(
            allocation,
            list->data,
            list->count * sizeof(*allocation)
        );
    }
    soc_aligned_free(list->data);
    list->data = allocation;
    list->capacity = minimum_capacity;
    return SOC_RESULT_OK;
}

void soc_raster_prepared_list_shutdown(
    soc_raster_prepared_list* list
)
{
    if (list == NULL) {
        return;
    }

    soc_aligned_free(list->data);
    memset(list, 0, sizeof(*list));
}

static soc_result append_prepared_triangle(
    soc_raster_prepared_list* list,
    const soc_raster_prepared_triangle* prepared
)
{
    if (list->count == list->capacity) {
        size_t new_capacity;
        soc_result result;

        if (list->capacity == 0u) {
            new_capacity = 64u;
        } else if (list->capacity <= SIZE_MAX / 2u) {
            new_capacity = list->capacity * 2u;
        } else if (list->capacity < SIZE_MAX) {
            new_capacity = SIZE_MAX;
        } else {
            return SOC_RESULT_OUT_OF_MEMORY;
        }
        result = soc_raster_prepared_list_reserve(list, new_capacity);
        if (result != SOC_RESULT_OK) {
            return result;
        }
    }

    list->data[list->count] = *prepared;
    ++list->count;
    return SOC_RESULT_OK;
}

static SOC_MAYBE_UNUSED soc_bool calculate_tile_lock_grid(
    uint32_t width,
    uint32_t height,
    uint32_t* out_column_count,
    uint32_t* out_row_count,
    size_t* out_lock_count
)
{
    uint32_t column_count;
    uint32_t row_count;
    size_t lock_count;

    if (width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        out_column_count == NULL ||
        out_row_count == NULL ||
        out_lock_count == NULL) {
        return SOC_FALSE;
    }

    column_count = width / SOC_RASTER_LOCK_TILE_SIZE;
    if (width % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++column_count;
    }
    row_count = height / SOC_RASTER_LOCK_TILE_SIZE;
    if (height % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++row_count;
    }
    if (!checked_size_multiply(
            (size_t)column_count,
            (size_t)row_count,
            &lock_count
        )) {
        return SOC_FALSE;
    }

    *out_column_count = column_count;
    *out_row_count = row_count;
    *out_lock_count = lock_count;
    return SOC_TRUE;
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

static int64_t quantize_screen_coordinate(double coordinate)
{
    const double scaled =
        coordinate * (double)SOC_RASTER_SUBPIXEL_SCALE;
    return (int64_t)(scaled + 0.5);
}

static int64_t fixed_edge_value_at_pixel(
    const soc_edge_equation* edge,
    uint32_t pixel_x,
    uint32_t pixel_y
)
{
    return edge->sample_origin +
        edge->step_x * (int64_t)pixel_x +
        edge->step_y * (int64_t)pixel_y;
}

static soc_edge_equation make_edge_equation(
    const soc_fixed_vertex* start,
    const soc_fixed_vertex* end
)
{
    const int64_t delta_x = end->x - start->x;
    const int64_t delta_y = end->y - start->y;
    const soc_bool top_left = delta_y < 0 ||
            (delta_y == 0 && delta_x > 0)
        ? SOC_TRUE
        : SOC_FALSE;
    const int64_t bias = top_left == SOC_TRUE ? 0 : -1;
    const soc_edge_equation edge = {
        .sample_origin =
            delta_x * (SOC_RASTER_SUBPIXEL_HALF - start->y) -
            delta_y * (SOC_RASTER_SUBPIXEL_HALF - start->x) +
            bias,
        .step_x = -delta_y * SOC_RASTER_SUBPIXEL_SCALE,
        .step_y = delta_x * SOC_RASTER_SUBPIXEL_SCALE,
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

static void configure_snapped_depth_plane(
    const soc_screen_vertex screen[3],
    int64_t fixed_area,
    double extent_x,
    double extent_y,
    soc_raster_depth_plane* out_plane
)
{
    const double inverse_subpixel_scale =
        1.0 / (double)SOC_RASTER_SUBPIXEL_SCALE;
    const double delta_x10 =
        (double)(screen[1].fixed_x - screen[0].fixed_x) *
        inverse_subpixel_scale;
    const double delta_y10 =
        (double)(screen[1].fixed_y - screen[0].fixed_y) *
        inverse_subpixel_scale;
    const double delta_x20 =
        (double)(screen[2].fixed_x - screen[0].fixed_x) *
        inverse_subpixel_scale;
    const double delta_y20 =
        (double)(screen[2].fixed_y - screen[0].fixed_y) *
        inverse_subpixel_scale;
    const double delta_depth10 = screen[1].depth - screen[0].depth;
    const double delta_depth20 = screen[2].depth - screen[0].depth;
    const double fixed_area_scale =
        (double)SOC_RASTER_SUBPIXEL_SCALE *
        (double)SOC_RASTER_SUBPIXEL_SCALE;
    const double area = (double)fixed_area / fixed_area_scale;
    double evaluation_magnitude;

    {
        const double numerator_x_term0 = delta_depth10 * delta_y20;
        const double numerator_x_term1 = delta_depth20 * delta_y10;
        const double numerator_y_term0 = delta_x10 * delta_depth20;
        const double numerator_y_term1 = delta_x20 * delta_depth10;

        out_plane->step_x =
            (numerator_x_term0 - numerator_x_term1) / area;
        out_plane->step_y =
            (numerator_y_term0 - numerator_y_term1) / area;

        evaluation_magnitude =
            absolute_double(out_plane->anchor) +
            absolute_double(out_plane->step_x) * extent_x +
            absolute_double(out_plane->step_y) * extent_y + 1.0;
        out_plane->error_bound = next_double_up(
            evaluation_magnitude * DBL_EPSILON *
                SOC_RASTER_EVALUATION_ERROR_SCALE
        );
    }
}

static void store_prepared_depth_plane(
    soc_raster_triangle_setup* out_setup,
    const soc_raster_depth_plane* plane
)
{
    const double origin_offset_x =
        plane->step_x * (0.5 - plane->anchor_x);
    const double origin_offset_y =
        plane->step_y * (0.5 - plane->anchor_y);
    const double sample_origin =
        (plane->anchor + origin_offset_x) + origin_offset_y;
    const double origin_evaluation_magnitude =
        absolute_double(plane->anchor) +
        absolute_double(origin_offset_x) +
        absolute_double(origin_offset_y) +
        absolute_double(sample_origin) + 1.0;
    const double origin_error_bound = next_double_up(
        origin_evaluation_magnitude * DBL_EPSILON * 8.0
    );

    out_setup->depth_sample_origin = sample_origin;
    out_setup->depth_step_x = plane->step_x;
    out_setup->depth_step_y = plane->step_y;
    out_setup->depth_error_bound = next_double_up(
        plane->error_bound + origin_error_bound
    );
}

static void configure_depth_plane(
    const soc_screen_vertex screen[3],
    int64_t fixed_area,
    soc_raster_triangle_setup* out_setup
)
{
    soc_raster_depth_plane plane;
    const double anchor_x =
        (double)screen[0].fixed_x / (double)SOC_RASTER_SUBPIXEL_SCALE;
    const double anchor_y =
        (double)screen[0].fixed_y / (double)SOC_RASTER_SUBPIXEL_SCALE;
    const double minimum_offset_x = absolute_double(
        (double)out_setup->bounds.minimum_x + 0.5 - anchor_x
    );
    const double maximum_offset_x = absolute_double(
        (double)(out_setup->bounds.end_x - 1u) + 0.5 - anchor_x
    );
    const double minimum_offset_y = absolute_double(
        (double)out_setup->bounds.minimum_y + 0.5 - anchor_y
    );
    const double maximum_offset_y = absolute_double(
        (double)(out_setup->bounds.end_y - 1u) + 0.5 - anchor_y
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

    plane.anchor_x = anchor_x;
    plane.anchor_y = anchor_y;
    plane.anchor = screen[0].depth;
    configure_snapped_depth_plane(
        screen,
        fixed_area,
        extent_x,
        extent_y,
        &plane
    );
    store_prepared_depth_plane(out_setup, &plane);
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
    int64_t fixed_area;
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
        ndc_x = clamp_double(ndc_x, -1.0, 1.0);
        ndc_y = clamp_double(ndc_y, -1.0, 1.0);
        screen[index].x =
            (ndc_x * 0.5 + 0.5) * rasterizer->width;
        screen[index].y =
            (0.5 - ndc_y * 0.5) * rasterizer->height;
        screen[index].fixed_x = quantize_screen_coordinate(screen[index].x);
        screen[index].fixed_y = quantize_screen_coordinate(screen[index].y);
    }

    fixed_area =
        (screen[1].fixed_x - screen[0].fixed_x) *
            (screen[2].fixed_y - screen[0].fixed_y) -
        (screen[1].fixed_y - screen[0].fixed_y) *
            (screen[2].fixed_x - screen[0].fixed_x);
    if (fixed_area == 0) {
        return SOC_RASTER_SETUP_REJECTED;
    }
    if (two_sided != SOC_TRUE) {
        const soc_bool front_facing =
            rasterizer->frame.front_face == SOC_FRONT_FACE_CCW
                ? (fixed_area < 0 ? SOC_TRUE : SOC_FALSE)
                : (fixed_area > 0 ? SOC_TRUE : SOC_FALSE);

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
        screen[index].depth = clamp_double(depth, 0.0, 1.0);
    }
    if (fixed_area < 0) {
        swap_screen_vertices(&screen[1], &screen[2]);
    }
    return SOC_RASTER_SETUP_READY;
}

static void configure_shared_fan_depth_plane(
    const soc_rasterizer* rasterizer,
    const soc_screen_vertex plane_triangle[3],
    soc_raster_depth_plane* out_plane
)
{
    int64_t fixed_area;
    double extent_x;
    double extent_y;

    fixed_area =
        (plane_triangle[1].fixed_x - plane_triangle[0].fixed_x) *
            (plane_triangle[2].fixed_y - plane_triangle[0].fixed_y) -
        (plane_triangle[1].fixed_y - plane_triangle[0].fixed_y) *
            (plane_triangle[2].fixed_x - plane_triangle[0].fixed_x);
    out_plane->anchor_x =
        (double)plane_triangle[0].fixed_x /
        (double)SOC_RASTER_SUBPIXEL_SCALE;
    out_plane->anchor_y =
        (double)plane_triangle[0].fixed_y /
        (double)SOC_RASTER_SUBPIXEL_SCALE;
    out_plane->anchor = plane_triangle[0].depth;
    extent_x = next_double_up(
        absolute_double(0.5 - out_plane->anchor_x) >
            absolute_double(
                (double)rasterizer->width - 0.5 -
                out_plane->anchor_x
            )
            ? absolute_double(0.5 - out_plane->anchor_x)
            : absolute_double(
                (double)rasterizer->width - 0.5 -
                out_plane->anchor_x
            )
    );
    extent_y = next_double_up(
        absolute_double(0.5 - out_plane->anchor_y) >
            absolute_double(
                (double)rasterizer->height - 0.5 -
                out_plane->anchor_y
            )
            ? absolute_double(0.5 - out_plane->anchor_y)
            : absolute_double(
                (double)rasterizer->height - 0.5 -
                out_plane->anchor_y
            )
    );
    configure_snapped_depth_plane(
        plane_triangle,
        fixed_area,
        extent_x,
        extent_y,
        out_plane
    );
}

static soc_raster_setup_result setup_raster_triangle(
    const soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    const soc_raster_depth_plane* shared_depth_plane,
    soc_raster_triangle_setup* out_setup
)
{
    soc_fixed_vertex fixed[3];
    int64_t minimum_x;
    int64_t maximum_x;
    int64_t minimum_y;
    int64_t maximum_y;
    int64_t fixed_area;
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        fixed[index].x = screen[index].fixed_x;
        fixed[index].y = screen[index].fixed_y;
    }
    fixed_area =
        (fixed[1].x - fixed[0].x) *
            (fixed[2].y - fixed[0].y) -
        (fixed[1].y - fixed[0].y) *
            (fixed[2].x - fixed[0].x);
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

    out_setup->first_tile_column = (uint16_t)(
        out_setup->bounds.minimum_x / SOC_RASTER_LOCK_TILE_SIZE
    );
    out_setup->first_tile_row = (uint16_t)(
        out_setup->bounds.minimum_y / SOC_RASTER_LOCK_TILE_SIZE
    );
    out_setup->end_tile_column = (uint16_t)(
        (out_setup->bounds.end_x - 1u) /
            SOC_RASTER_LOCK_TILE_SIZE + 1u
    );
    out_setup->end_tile_row = (uint16_t)(
        (out_setup->bounds.end_y - 1u) /
            SOC_RASTER_LOCK_TILE_SIZE + 1u
    );

    out_setup->edges[0] = make_edge_equation(&fixed[1], &fixed[2]);
    out_setup->edges[1] = make_edge_equation(&fixed[2], &fixed[0]);
    out_setup->edges[2] = make_edge_equation(&fixed[0], &fixed[1]);

    if (shared_depth_plane != NULL) {
        store_prepared_depth_plane(out_setup, shared_depth_plane);
    } else if (screen[0].depth == screen[1].depth &&
        screen[0].depth == screen[2].depth) {
        out_setup->depth_sample_origin = screen[0].depth;
        out_setup->depth_step_x = 0.0;
        out_setup->depth_step_y = 0.0;
        out_setup->depth_error_bound =
            (absolute_double(out_setup->depth_sample_origin) + 1.0) *
            DBL_EPSILON * 64.0;
    } else {
        configure_depth_plane(
            screen,
            fixed_area,
            out_setup
        );
    }
    return SOC_RASTER_SETUP_READY;
}

static float make_conservative_depth(
    double depth,
    double depth_error_bound
)
{
    float candidate_depth;
    uint32_t candidate_bits;
    uint32_t guard;

    depth -= depth_error_bound;
    depth = clamp_double(depth, 0.0, 1.0);
    candidate_depth = (float)depth;
    memcpy(
        &candidate_bits,
        &candidate_depth,
        sizeof(candidate_bits)
    );
    if ((double)candidate_depth > depth && candidate_bits != 0u) {
        --candidate_bits;
    }
    for (guard = 0u;
         guard < SOC_RASTER_DEPTH_GUARD_ULPS && candidate_bits != 0u;
         ++guard) {
        --candidate_bits;
    }
    memcpy(
        &candidate_depth,
        &candidate_bits,
        sizeof(candidate_depth)
    );
    return candidate_depth;
}

static float make_far_biased_plane_depth(float depth)
{
    uint32_t bits;

    if (depth <= 0.0f) {
        depth = 0.0f;
    } else if (depth > 1.0f) {
        depth = 1.0f;
    }
    memcpy(&bits, &depth, sizeof(bits));
    bits = bits > SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
        ? bits - SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
        : 0u;
    memcpy(&depth, &bits, sizeof(depth));
    return depth;
}

static float make_depth_plane_block_origin(
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y
)
{
    double block_depth = setup->depth_sample_origin +
        setup->depth_step_x * (double)block_x +
        setup->depth_step_y * (double)block_y;

    block_depth -= setup->depth_error_bound;
    return (float)block_depth;
}

/*
 * Reproduce the depth kernel's f32 origin/step conversion, two-stage FMA and
 * far bias at all four rectangle corners.  Rounded FMA and the monotonic bias
 * keep the nearest corner conservative for every sample in the rectangle.
 */
static void configure_depth_block_candidate(
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    soc_raster_depth_block_candidate* out_candidate
)
{
    float x_offsets[2];
    float y_offsets[2];
    uint32_t corner_y;

    if (setup->depth_step_x == 0.0 && setup->depth_step_y == 0.0) {
        const double block_depth = setup->depth_sample_origin;

        out_candidate->depth_origin = make_conservative_depth(
            block_depth,
            setup->depth_error_bound
        );
        out_candidate->depth_step_x = 0.0f;
        out_candidate->depth_step_y = 0.0f;
        out_candidate->nearest_depth = out_candidate->depth_origin;
        out_candidate->farthest_depth = out_candidate->depth_origin;
        out_candidate->is_constant = SOC_TRUE;
        return;
    }

    out_candidate->depth_origin = make_depth_plane_block_origin(
        setup,
        block_x,
        block_y
    );
    out_candidate->depth_step_x = (float)setup->depth_step_x;
    out_candidate->depth_step_y = (float)setup->depth_step_y;
    out_candidate->nearest_depth = 0.0f;
    out_candidate->farthest_depth = 0.0f;
    out_candidate->is_constant = SOC_FALSE;
    x_offsets[0] = 0.0f;
    x_offsets[1] = (float)(block_width - 1u);
    y_offsets[0] = 0.0f;
    y_offsets[1] = (float)(block_height - 1u);

    for (corner_y = 0u; corner_y < 2u; ++corner_y) {
        const float row_depth = fmaf(
            out_candidate->depth_step_y,
            y_offsets[corner_y],
            out_candidate->depth_origin
        );
        uint32_t corner_x;

        for (corner_x = 0u; corner_x < 2u; ++corner_x) {
            const float candidate = make_far_biased_plane_depth(
                fmaf(
                    out_candidate->depth_step_x,
                    x_offsets[corner_x],
                    row_depth
                )
            );

            if (corner_x == 0u && corner_y == 0u) {
                out_candidate->nearest_depth = candidate;
                out_candidate->farthest_depth = candidate;
            } else {
                if (candidate > out_candidate->nearest_depth) {
                    out_candidate->nearest_depth = candidate;
                }
                if (candidate < out_candidate->farthest_depth) {
                    out_candidate->farthest_depth = candidate;
                }
            }
        }
    }
}

/*
 * Bound every 8x8 fine-kernel rebase covered by a coarse region.  Binary32
 * conversion and FMA are monotonic, so the four endpoint block origins plus
 * the complete [0,7] local-offset envelope contain every fine candidate.
 */
static void configure_coarse_depth_candidate(
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region,
    soc_raster_depth_block_candidate* out_candidate
)
{
    uint32_t last_block_x =
        (region->end_x - 1u) & ~(SOC_RASTER_BLOCK_SIZE - 1u);
    uint32_t last_block_y =
        (region->end_y - 1u) & ~(SOC_RASTER_BLOCK_SIZE - 1u);
    float minimum_origin;
    float maximum_origin;
    float origins[2];
    uint32_t origin_index;

    if (setup->depth_step_x == 0.0 && setup->depth_step_y == 0.0) {
        configure_depth_block_candidate(
            setup,
            region->minimum_x,
            region->minimum_y,
            1u,
            1u,
            out_candidate
        );
        return;
    }
    if (last_block_x < region->minimum_x) {
        last_block_x = region->minimum_x;
    }
    if (last_block_y < region->minimum_y) {
        last_block_y = region->minimum_y;
    }
    minimum_origin = make_depth_plane_block_origin(
        setup,
        region->minimum_x,
        region->minimum_y
    );
    maximum_origin = minimum_origin;
    {
        const uint32_t block_x[2] = {
            region->minimum_x,
            last_block_x,
        };
        const uint32_t block_y[2] = {
            region->minimum_y,
            last_block_y,
        };
        uint32_t y_index;

        for (y_index = 0u; y_index < 2u; ++y_index) {
            uint32_t x_index;

            for (x_index = 0u; x_index < 2u; ++x_index) {
                const float origin = make_depth_plane_block_origin(
                    setup,
                    block_x[x_index],
                    block_y[y_index]
                );

                if (origin < minimum_origin) {
                    minimum_origin = origin;
                }
                if (origin > maximum_origin) {
                    maximum_origin = origin;
                }
            }
        }
    }
    out_candidate->depth_origin = minimum_origin;
    out_candidate->depth_step_x = (float)setup->depth_step_x;
    out_candidate->depth_step_y = (float)setup->depth_step_y;
    out_candidate->is_constant = SOC_FALSE;
    origins[0] = minimum_origin;
    origins[1] = maximum_origin;
    for (origin_index = 0u; origin_index < 2u; ++origin_index) {
        uint32_t offset_y_index;

        for (offset_y_index = 0u;
             offset_y_index < 2u;
             ++offset_y_index) {
            const float offset_y = offset_y_index == 0u
                ? 0.0f
                : (float)(SOC_RASTER_BLOCK_SIZE - 1u);
            const float row_depth = fmaf(
                out_candidate->depth_step_y,
                offset_y,
                origins[origin_index]
            );
            uint32_t offset_x_index;

            for (offset_x_index = 0u;
                 offset_x_index < 2u;
                 ++offset_x_index) {
                const float offset_x = offset_x_index == 0u
                    ? 0.0f
                    : (float)(SOC_RASTER_BLOCK_SIZE - 1u);
                const float candidate = make_far_biased_plane_depth(
                    fmaf(
                        out_candidate->depth_step_x,
                        offset_x,
                        row_depth
                    )
                );

                if (origin_index == 0u && offset_y_index == 0u &&
                    offset_x_index == 0u) {
                    out_candidate->nearest_depth = candidate;
                    out_candidate->farthest_depth = candidate;
                } else {
                    if (candidate > out_candidate->nearest_depth) {
                        out_candidate->nearest_depth = candidate;
                    }
                    if (candidate < out_candidate->farthest_depth) {
                        out_candidate->farthest_depth = candidate;
                    }
                }
            }
        }
    }
}

static size_t find_early_z_block_index(
    const soc_rasterizer* rasterizer,
    uint32_t block_x,
    uint32_t block_y
)
{
    const uint32_t block_column = block_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_row = block_y / SOC_RASTER_BLOCK_SIZE;

    return (size_t)block_row * rasterizer->block_column_count +
        block_column;
}

static const float* find_small_early_z_farthest_depth(
    const soc_rasterizer* rasterizer,
    const soc_raster_region* region
)
{
    const uint32_t block_column =
        region->minimum_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_row =
        region->minimum_y / SOC_RASTER_BLOCK_SIZE;

    if ((region->end_x - 1u) / SOC_RASTER_BLOCK_SIZE != block_column ||
        (region->end_y - 1u) / SOC_RASTER_BLOCK_SIZE != block_row) {
        return NULL;
    }
    return &rasterizer->early_z_farthest_depths[
        (size_t)block_row * rasterizer->block_column_count + block_column
    ];
}

static soc_bool depth_block_is_early_z_rejected(
    const soc_raster_depth_block_candidate* candidate,
    float farthest_depth
)
{
    return candidate->nearest_depth <= farthest_depth
        ? SOC_TRUE
        : SOC_FALSE;
}

static void store_small_constant_depth(
    soc_rasterizer* rasterizer,
    uint32_t pixel_x,
    uint32_t pixel_y,
    float candidate_depth
)
{
    const size_t depth_index =
        (size_t)pixel_y * rasterizer->width + pixel_x;
    const float stored_depth = rasterizer->depth[depth_index];
    const soc_bool passes_depth = candidate_depth > stored_depth
        ? SOC_TRUE
        : SOC_FALSE;

    if (passes_depth == SOC_TRUE) {
        rasterizer->depth[depth_index] = candidate_depth;
    }
}

/* Small constant triangles do not amortize 8x8 state maintenance. */
static void rasterize_small_constant_triangle(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const soc_raster_region* region = &setup->bounds;
    const float candidate_depth = make_conservative_depth(
        setup->depth_sample_origin,
        setup->depth_error_bound
    );
    const float* early_z_farthest_depth =
        find_small_early_z_farthest_depth(rasterizer, region);
    int64_t row_edges[3];
    uint32_t edge_index;
    uint32_t pixel_y;

    if (early_z_farthest_depth != NULL &&
        candidate_depth <= *early_z_farthest_depth) {
        return;
    }
    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        row_edges[edge_index] = fixed_edge_value_at_pixel(
            &setup->edges[edge_index],
            region->minimum_x,
            region->minimum_y
        );
    }
    for (pixel_y = region->minimum_y; pixel_y < region->end_y; ++pixel_y) {
        int64_t edge0 = row_edges[0];
        int64_t edge1 = row_edges[1];
        int64_t edge2 = row_edges[2];
        uint32_t pixel_x;

        for (pixel_x = region->minimum_x;
             pixel_x < region->end_x;
             ++pixel_x) {
            if (edge0 >= 0 && edge1 >= 0 && edge2 >= 0) {
                store_small_constant_depth(
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
);

static uint64_t make_raster_block_mask(
    const soc_raster_triangle_setup* setup,
    const int64_t edge_values[3],
    uint32_t block_width,
    uint32_t block_height,
    soc_raster_block_classification classification
);

static void rasterize_depth_block(
    soc_rasterizer* rasterizer,
    const soc_raster_depth_block_candidate* candidate,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask
);

/* A <=8x8 varying triangle is cheaper as one rectangular kernel call. */
static void rasterize_small_plane_triangle_untracked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const soc_raster_region* region = &setup->bounds;
    const uint32_t block_width = region->end_x - region->minimum_x;
    const uint32_t block_height = region->end_y - region->minimum_y;
    int64_t edge_values[3];
    soc_raster_depth_block_candidate depth_candidate;
    uint64_t coverage_mask;
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        edge_values[edge_index] = fixed_edge_value_at_pixel(
            &setup->edges[edge_index],
            region->minimum_x,
            region->minimum_y
        );
    }
    depth_candidate.depth_origin = make_depth_plane_block_origin(
        setup,
        region->minimum_x,
        region->minimum_y
    );
    depth_candidate.depth_step_x = (float)setup->depth_step_x;
    depth_candidate.depth_step_y = (float)setup->depth_step_y;
    depth_candidate.is_constant = SOC_FALSE;
    coverage_mask = make_raster_block_mask(
        setup,
        edge_values,
        block_width,
        block_height,
        SOC_RASTER_BLOCK_PARTIAL
    );
    if (coverage_mask != 0u) {
        rasterize_depth_block(
            rasterizer,
            &depth_candidate,
            region->minimum_x,
            region->minimum_y,
            block_width,
            block_height,
            coverage_mask
        );
    }
}

static void rasterize_small_triangle_blocks_untracked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const soc_raster_region* region = &setup->bounds;
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);

    for (; aligned_y < region->end_y;
         aligned_y += SOC_RASTER_BLOCK_SIZE) {
        const uint32_t block_y = aligned_y < region->minimum_y
            ? region->minimum_y
            : aligned_y;
        const uint32_t block_end_y = aligned_y + SOC_RASTER_BLOCK_SIZE <
                region->end_y
            ? aligned_y + SOC_RASTER_BLOCK_SIZE
            : region->end_y;
        const uint32_t block_height = block_end_y - block_y;
        uint32_t aligned_x = region->minimum_x &
            ~(SOC_RASTER_BLOCK_SIZE - 1u);

        for (; aligned_x < region->end_x;
             aligned_x += SOC_RASTER_BLOCK_SIZE) {
            const uint32_t block_x = aligned_x < region->minimum_x
                ? region->minimum_x
                : aligned_x;
            const uint32_t block_end_x = aligned_x +
                    SOC_RASTER_BLOCK_SIZE < region->end_x
                ? aligned_x + SOC_RASTER_BLOCK_SIZE
                : region->end_x;
            const uint32_t block_width = block_end_x - block_x;
            int64_t edge_values[3];
            soc_raster_block_classification classification;
            soc_raster_depth_block_candidate depth_candidate;
            uint64_t coverage_mask;
            uint32_t edge_index;

            for (edge_index = 0u; edge_index < 3u; ++edge_index) {
                edge_values[edge_index] = fixed_edge_value_at_pixel(
                    &setup->edges[edge_index],
                    block_x,
                    block_y
                );
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
            if (setup->depth_step_x == 0.0 &&
                setup->depth_step_y == 0.0) {
                depth_candidate.depth_origin = make_conservative_depth(
                    setup->depth_sample_origin,
                    setup->depth_error_bound
                );
                depth_candidate.depth_step_x = 0.0f;
                depth_candidate.depth_step_y = 0.0f;
                depth_candidate.is_constant = SOC_TRUE;
            } else {
                depth_candidate.depth_origin =
                    make_depth_plane_block_origin(
                        setup,
                        block_x,
                        block_y
                    );
                depth_candidate.depth_step_x =
                    (float)setup->depth_step_x;
                depth_candidate.depth_step_y =
                    (float)setup->depth_step_y;
                depth_candidate.is_constant = SOC_FALSE;
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
                    &depth_candidate,
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

static uint64_t make_physical_block_mask(
    uint32_t block_width,
    uint32_t block_height
)
{
    if (block_width == SOC_RASTER_BLOCK_SIZE &&
        block_height == SOC_RASTER_BLOCK_SIZE) {
        return UINT64_MAX;
    }

    const uint64_t row_mask =
        (UINT64_C(1) << block_width) - UINT64_C(1);
    uint64_t mask = 0u;
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        mask |= row_mask << (row * SOC_RASTER_BLOCK_SIZE);
    }
    return mask;
}

static size_t find_target_early_z_block_index(
    const soc_raster_target* target,
    uint32_t block_x,
    uint32_t block_y
)
{
    const uint32_t first_block_column =
        target->origin_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t first_block_row =
        target->origin_y / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_column =
        block_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_row = block_y / SOC_RASTER_BLOCK_SIZE;

    return (size_t)(block_row - first_block_row) *
        target->early_z_column_count +
        (block_column - first_block_column);
}

static float scan_depth_block_summary(
    const float* depth,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height
)
{
    float summary = depth[0];
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        const float* depth_row = depth + (size_t)row * row_stride;
        uint32_t column = row == 0u ? 1u : 0u;

        for (; column < block_width; ++column) {
            const float candidate = depth_row[column];

            if (candidate < summary) {
                summary = candidate;
            }
        }
    }
    return summary == 0.0f ? 0.0f : summary;
}

static soc_bool update_early_z_after_store(
    float* farthest_depth,
    uint64_t* pending_mask_storage,
    const soc_raster_depth_block_candidate* candidate,
    uint64_t coverage_mask,
    const float* physical_depth,
    size_t row_stride,
    uint32_t physical_width,
    uint32_t physical_height
)
{
    const float untouched_depth = -1.0f;
    const float clear_depth = 0.0f;
    const float previous_farthest_depth = *farthest_depth;
    const soc_bool replaces_farthest =
        candidate->farthest_depth > previous_farthest_depth
            ? SOC_TRUE
            : SOC_FALSE;
    uint64_t pending_mask;
    uint64_t physical_mask;

    if (replaces_farthest != SOC_TRUE) {
        return SOC_FALSE;
    }
    physical_mask = make_physical_block_mask(
        physical_width,
        physical_height
    );
    if (coverage_mask == physical_mask) {
        *farthest_depth = candidate->farthest_depth;
        if (previous_farthest_depth == untouched_depth ||
            *pending_mask_storage != 0u) {
            *pending_mask_storage = 0u;
        }
        return SOC_TRUE;
    }
    pending_mask = (previous_farthest_depth == untouched_depth
            ? physical_mask
            : (*pending_mask_storage == 0u
                ? physical_mask
                : *pending_mask_storage)) &
        ~coverage_mask;
    if (pending_mask != 0u) {
        *pending_mask_storage = pending_mask;
        if (previous_farthest_depth == untouched_depth) {
            *farthest_depth = clear_depth;
        }
        return SOC_FALSE;
    }

    *farthest_depth = scan_depth_block_summary(
        physical_depth,
        row_stride,
        physical_width,
        physical_height
    );
    *pending_mask_storage = 0u;
    return SOC_TRUE;
}

static void rebuild_coarse_early_z(soc_rasterizer* rasterizer)
{
    const float clear_depth = 0.0f;
    const float untouched_depth = -1.0f;
    uint32_t tile_row;

    if (rasterizer->early_z_coarse_dirty != SOC_TRUE) {
        return;
    }
    rasterizer->early_z_ready_tile_count = 0u;
    for (tile_row = 0u;
         tile_row < rasterizer->early_z_tile_row_count;
         ++tile_row) {
        const uint32_t first_block_row = tile_row * 4u;
        const uint32_t block_row_count =
            rasterizer->block_row_count - first_block_row < 4u
                ? rasterizer->block_row_count - first_block_row
                : 4u;
        uint32_t tile_column;

        for (tile_column = 0u;
             tile_column < rasterizer->early_z_tile_column_count;
             ++tile_column) {
            const uint32_t first_block_column = tile_column * 4u;
            const uint32_t block_column_count =
                rasterizer->block_column_count - first_block_column < 4u
                    ? rasterizer->block_column_count - first_block_column
                    : 4u;
            const size_t tile_index =
                (size_t)tile_row *
                    rasterizer->early_z_tile_column_count +
                tile_column;
            float summary = rasterizer->early_z_farthest_depths[
                (size_t)first_block_row * rasterizer->block_column_count +
                first_block_column
            ];
            uint32_t row;

            for (row = 0u; row < block_row_count; ++row) {
                uint32_t column = row == 0u ? 1u : 0u;

                for (; column < block_column_count; ++column) {
                    const float candidate =
                        rasterizer->early_z_farthest_depths[
                        (size_t)(first_block_row + row) *
                            rasterizer->block_column_count +
                        first_block_column + column
                    ];

                    if (candidate < summary) {
                        summary = candidate;
                    }
                }
            }
            rasterizer->early_z_tile_farthest_depths[tile_index] =
                summary;
            if (summary != clear_depth && summary != untouched_depth) {
                ++rasterizer->early_z_ready_tile_count;
            }
        }
    }
    if (rasterizer->early_z_ready_tile_count ==
        rasterizer->early_z_tile_count) {
        size_t tile_index;
        float summary = rasterizer->early_z_tile_farthest_depths[0];

        for (tile_index = 1u;
             tile_index < rasterizer->early_z_tile_count;
             ++tile_index) {
            const float candidate =
                rasterizer->early_z_tile_farthest_depths[tile_index];

            if (candidate < summary) {
                summary = candidate;
            }
        }
        rasterizer->early_z_frame_farthest_depth = summary;
    }
    rasterizer->early_z_coarse_dirty = SOC_FALSE;
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
    const soc_raster_depth_block_candidate* candidate,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask
)
{
    if (candidate->is_constant == SOC_TRUE) {
        rasterizer->kernels->store_constant_depth_block_f32(
            rasterizer->depth +
                (size_t)block_y * rasterizer->width + block_x,
            rasterizer->width,
            block_width,
            block_height,
            coverage_mask,
            candidate->depth_origin
        );
        return;
    }

    rasterizer->kernels->store_depth_plane_block_f32(
        rasterizer->depth +
            (size_t)block_y * rasterizer->width + block_x,
        rasterizer->width,
        block_width,
        block_height,
        coverage_mask,
        candidate->depth_origin,
        candidate->depth_step_x,
        candidate->depth_step_y
    );
}

static void rasterize_triangle_blocks_fine(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);

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
            const uint32_t physical_end_x = aligned_end_x <
                    rasterizer->width
                ? aligned_end_x
                : rasterizer->width;
            const uint32_t physical_end_y = aligned_end_y <
                    rasterizer->height
                ? aligned_end_y
                : rasterizer->height;
            const uint32_t physical_width = physical_end_x - aligned_x;
            const uint32_t physical_height = physical_end_y - aligned_y;
            int64_t edge_values[3];
            soc_raster_block_classification classification;
            size_t early_z_index;
            soc_raster_depth_block_candidate depth_candidate;
            uint64_t coverage_mask;
            uint64_t cell_coverage_mask;
            uint32_t edge_index;

            for (edge_index = 0u; edge_index < 3u; ++edge_index) {
                edge_values[edge_index] = fixed_edge_value_at_pixel(
                    &setup->edges[edge_index],
                    block_x,
                    block_y
                );
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
            early_z_index = find_early_z_block_index(
                rasterizer,
                aligned_x,
                aligned_y
            );
            configure_depth_block_candidate(
                setup,
                block_x,
                block_y,
                block_width,
                block_height,
                &depth_candidate
            );
            if (depth_block_is_early_z_rejected(
                    &depth_candidate,
                    rasterizer->early_z_farthest_depths[early_z_index]
                ) == SOC_TRUE) {
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
                cell_coverage_mask = coverage_mask << (
                    (block_y - aligned_y) * SOC_RASTER_BLOCK_SIZE +
                    block_x - aligned_x
                );
                rasterize_depth_block(
                    rasterizer,
                    &depth_candidate,
                    block_x,
                    block_y,
                    block_width,
                    block_height,
                    coverage_mask
                );
                if (update_early_z_after_store(
                        &rasterizer->early_z_farthest_depths[
                            early_z_index
                        ],
                        &rasterizer->early_z_pending_masks[early_z_index],
                        &depth_candidate,
                        cell_coverage_mask,
                        rasterizer->depth +
                            (size_t)aligned_y * rasterizer->width +
                            aligned_x,
                        rasterizer->width,
                        physical_width,
                        physical_height
                    ) == SOC_TRUE) {
                    if (rasterizer->early_z_coarse_dirty != SOC_TRUE) {
                        rasterizer->early_z_coarse_dirty = SOC_TRUE;
                    }
                }
            }
        }
    }
}

static void rasterize_triangle_blocks(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    const float clear_depth = 0.0f;
    soc_raster_depth_block_candidate frame_candidate;
    uint32_t tile_y = region->minimum_y &
        ~(SOC_RASTER_LOCK_TILE_SIZE - 1u);

    if (rasterizer->early_z_pending_masks == NULL) {
        rasterizer->early_z_pending_masks =
            allocate_early_z_pending_masks(
                rasterizer->early_z_block_count
            );
    }
    if (rasterizer->early_z_ready_tile_count !=
        rasterizer->early_z_tile_count) {
        rasterize_triangle_blocks_fine(rasterizer, setup, region);
        return;
    }
    configure_coarse_depth_candidate(
        setup,
        region,
        &frame_candidate
    );
    if (frame_candidate.nearest_depth <=
        rasterizer->early_z_frame_farthest_depth) {
        return;
    }
    if (frame_candidate.farthest_depth >
        rasterizer->early_z_frame_farthest_depth) {
        rasterize_triangle_blocks_fine(rasterizer, setup, region);
        return;
    }

    for (; tile_y < region->end_y; tile_y += SOC_RASTER_LOCK_TILE_SIZE) {
        const uint32_t tile_end_y = tile_y + SOC_RASTER_LOCK_TILE_SIZE <
                region->end_y
            ? tile_y + SOC_RASTER_LOCK_TILE_SIZE
            : region->end_y;
        const uint32_t minimum_y = tile_y < region->minimum_y
            ? region->minimum_y
            : tile_y;
        uint32_t tile_x = region->minimum_x &
            ~(SOC_RASTER_LOCK_TILE_SIZE - 1u);

        for (; tile_x < region->end_x;
             tile_x += SOC_RASTER_LOCK_TILE_SIZE) {
            const uint32_t tile_end_x = tile_x +
                    SOC_RASTER_LOCK_TILE_SIZE < region->end_x
                ? tile_x + SOC_RASTER_LOCK_TILE_SIZE
                : region->end_x;
            const uint32_t minimum_x = tile_x < region->minimum_x
                ? region->minimum_x
                : tile_x;
            const float early_z_farthest_depth =
                rasterizer->early_z_tile_farthest_depths[
                    (size_t)(tile_y / SOC_RASTER_LOCK_TILE_SIZE) *
                        rasterizer->early_z_tile_column_count +
                    tile_x / SOC_RASTER_LOCK_TILE_SIZE
                ];
            soc_raster_region tile_region;

            tile_region.minimum_x = minimum_x;
            tile_region.minimum_y = minimum_y;
            tile_region.end_x = tile_end_x;
            tile_region.end_y = tile_end_y;
            if (early_z_farthest_depth != clear_depth) {
                soc_raster_depth_block_candidate candidate;

                configure_coarse_depth_candidate(
                    setup,
                    &tile_region,
                    &candidate
                );
                if (candidate.nearest_depth <= early_z_farthest_depth) {
                    continue;
                }
            }
            rasterize_triangle_blocks_fine(
                rasterizer,
                setup,
                &tile_region
            );
        }
    }
}

static void rasterize_depth_block_to_target(
    soc_rasterizer* rasterizer,
    const soc_raster_depth_block_candidate* candidate,
    const soc_raster_target* target,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask
)
{
    const size_t local_x = (size_t)(block_x - target->origin_x);
    const size_t local_y = (size_t)(block_y - target->origin_y);
    float* destination = target->depth +
        local_y * target->row_stride + local_x;

    if (candidate->is_constant == SOC_TRUE) {
        rasterizer->kernels->store_constant_depth_block_f32(
            destination,
            target->row_stride,
            block_width,
            block_height,
            coverage_mask,
            candidate->depth_origin
        );
        return;
    }

    rasterizer->kernels->store_depth_plane_block_f32(
        destination,
        target->row_stride,
        block_width,
        block_height,
        coverage_mask,
        candidate->depth_origin,
        candidate->depth_step_x,
        candidate->depth_step_y
    );
}

static void rasterize_triangle_blocks_to_target(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region,
    const soc_raster_target* target
)
{
    const uint32_t target_end_x = target->origin_x + target->width;
    const uint32_t target_end_y = target->origin_y + target->height;
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);

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
            const uint32_t physical_end_x = aligned_end_x < target_end_x
                ? aligned_end_x
                : target_end_x;
            const uint32_t physical_end_y = aligned_end_y < target_end_y
                ? aligned_end_y
                : target_end_y;
            const uint32_t physical_width = physical_end_x - aligned_x;
            const uint32_t physical_height = physical_end_y - aligned_y;
            int64_t edge_values[3];
            soc_raster_block_classification classification;
            size_t early_z_index;
            soc_raster_depth_block_candidate depth_candidate;
            uint64_t coverage_mask;
            uint64_t cell_coverage_mask;
            uint32_t edge_index;

            for (edge_index = 0u; edge_index < 3u; ++edge_index) {
                edge_values[edge_index] = fixed_edge_value_at_pixel(
                    &setup->edges[edge_index],
                    block_x,
                    block_y
                );
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
            early_z_index = find_target_early_z_block_index(
                target,
                aligned_x,
                aligned_y
            );
            configure_depth_block_candidate(
                setup,
                block_x,
                block_y,
                block_width,
                block_height,
                &depth_candidate
            );
            if (depth_block_is_early_z_rejected(
                    &depth_candidate,
                    target->early_z_farthest_depths[early_z_index]
                ) == SOC_TRUE) {
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
                cell_coverage_mask = coverage_mask << (
                    (block_y - aligned_y) * SOC_RASTER_BLOCK_SIZE +
                    block_x - aligned_x
                );
                rasterize_depth_block_to_target(
                    rasterizer,
                    &depth_candidate,
                    target,
                    block_x,
                    block_y,
                    block_width,
                    block_height,
                    coverage_mask
                );
                (void)update_early_z_after_store(
                    &target->early_z_farthest_depths[early_z_index],
                    &target->early_z_pending_masks[early_z_index],
                    &depth_candidate,
                    cell_coverage_mask,
                    target->depth +
                        (size_t)(aligned_y - target->origin_y) *
                            target->row_stride +
                        (aligned_x - target->origin_x),
                    target->row_stride,
                    physical_width,
                    physical_height
                );
            }
        }
    }
}

static void acquire_tile_lock(atomic_uint* lock)
{
    for (;;) {
        if (atomic_exchange_explicit(
                lock,
                1u,
                memory_order_acquire
            ) == 0u) {
            return;
        }
        while (atomic_load_explicit(lock, memory_order_relaxed) != 0u) {
        }
    }
}

static void release_tile_lock(atomic_uint* lock)
{
    atomic_store_explicit(lock, 0u, memory_order_release);
}

static soc_bool try_rasterize_single_tile_locked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    soc_raster_tile_locks* tile_locks = rasterizer->tile_locks;
    const uint32_t bounds_width =
        setup->bounds.end_x - setup->bounds.minimum_x;
    const uint32_t bounds_height =
        setup->bounds.end_y - setup->bounds.minimum_y;
    const uint32_t first_tile_x =
        setup->bounds.minimum_x / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t first_tile_y =
        setup->bounds.minimum_y / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t last_tile_x =
        (setup->bounds.end_x - 1u) / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t last_tile_y =
        (setup->bounds.end_y - 1u) / SOC_RASTER_LOCK_TILE_SIZE;
    size_t lock_index;
    atomic_uint* lock;

    if (first_tile_x != last_tile_x || first_tile_y != last_tile_y) {
        return SOC_FALSE;
    }

    lock_index =
        (size_t)first_tile_y * tile_locks->column_count + first_tile_x;
    lock = &tile_locks->locks[lock_index];
    acquire_tile_lock(lock);
    if (bounds_width <= SOC_RASTER_BLOCK_SIZE &&
        bounds_height <= SOC_RASTER_BLOCK_SIZE) {
        if (setup->depth_step_x == 0.0 && setup->depth_step_y == 0.0) {
            rasterize_small_constant_triangle(rasterizer, setup);
        } else {
            rasterize_small_plane_triangle_untracked(
                rasterizer,
                setup
            );
        }
    } else if (bounds_width <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE &&
        bounds_height <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE) {
        rasterize_small_triangle_blocks_untracked(rasterizer, setup);
    } else {
        rasterize_triangle_blocks(rasterizer, setup, &setup->bounds);
    }
    release_tile_lock(lock);
    return SOC_TRUE;
}

static void rasterize_triangle_tiles_locked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    soc_raster_tile_locks* tile_locks = rasterizer->tile_locks;
    const uint32_t first_tile_x =
        setup->bounds.minimum_x / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t first_tile_y =
        setup->bounds.minimum_y / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t end_tile_x =
        (setup->bounds.end_x - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u;
    const uint32_t end_tile_y =
        (setup->bounds.end_y - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u;
    uint32_t tile_y;

    for (tile_y = first_tile_y; tile_y < end_tile_y; ++tile_y) {
        const uint32_t tile_minimum_y =
            tile_y * SOC_RASTER_LOCK_TILE_SIZE;
        const uint32_t tile_end_y =
            tile_minimum_y + SOC_RASTER_LOCK_TILE_SIZE;
        uint32_t tile_x;

        for (tile_x = first_tile_x; tile_x < end_tile_x; ++tile_x) {
            const uint32_t tile_minimum_x =
                tile_x * SOC_RASTER_LOCK_TILE_SIZE;
            const uint32_t tile_end_x =
                tile_minimum_x + SOC_RASTER_LOCK_TILE_SIZE;
            const size_t lock_index =
                (size_t)tile_y * tile_locks->column_count + tile_x;
            soc_raster_region region;
            atomic_uint* lock = &tile_locks->locks[lock_index];

            region.minimum_x = setup->bounds.minimum_x > tile_minimum_x
                ? setup->bounds.minimum_x
                : tile_minimum_x;
            region.minimum_y = setup->bounds.minimum_y > tile_minimum_y
                ? setup->bounds.minimum_y
                : tile_minimum_y;
            region.end_x = setup->bounds.end_x < tile_end_x
                ? setup->bounds.end_x
                : tile_end_x;
            region.end_y = setup->bounds.end_y < tile_end_y
                ? setup->bounds.end_y
                : tile_end_y;

            acquire_tile_lock(lock);
            rasterize_triangle_blocks(rasterizer, setup, &region);
            release_tile_lock(lock);
        }
    }
}

static void rasterize_triangle_setup_unlocked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const uint32_t bounds_width =
        setup->bounds.end_x - setup->bounds.minimum_x;
    const uint32_t bounds_height =
        setup->bounds.end_y - setup->bounds.minimum_y;

    if (bounds_width <= SOC_RASTER_BLOCK_SIZE &&
        bounds_height <= SOC_RASTER_BLOCK_SIZE) {
        if (setup->depth_step_x == 0.0 && setup->depth_step_y == 0.0) {
            rasterize_small_constant_triangle(rasterizer, setup);
        } else {
            rasterize_small_plane_triangle_untracked(
                rasterizer,
                setup
            );
        }
        return;
    }
    if (bounds_width <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE &&
        bounds_height <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE) {
        rasterize_small_triangle_blocks_untracked(rasterizer, setup);
        return;
    }
    rasterize_triangle_blocks(rasterizer, setup, &setup->bounds);
}

static SOC_NOINLINE void rasterize_triangle_setup(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    if (rasterizer->tile_locks != NULL) {
        if (try_rasterize_single_tile_locked(
                rasterizer,
                setup
            ) == SOC_TRUE) {
            return;
        }
        rasterize_triangle_tiles_locked(rasterizer, setup);
        return;
    }

    rasterize_triangle_setup_unlocked(rasterizer, setup);
}

static soc_result process_screen_triangle(
    soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    const soc_raster_depth_plane* shared_depth_plane,
    soc_raster_prepared_list* prepared,
    soc_bool* out_rasterized
)
{
    soc_raster_triangle_setup setup;
    const soc_raster_setup_result setup_result = setup_raster_triangle(
        rasterizer,
        screen,
        shared_depth_plane,
        &setup
    );

    *out_rasterized = SOC_FALSE;
    if (setup_result == SOC_RASTER_SETUP_REJECTED) {
        return SOC_RESULT_OK;
    }
    *out_rasterized = SOC_TRUE;
    if (setup_result == SOC_RASTER_SETUP_EMPTY) {
        return SOC_RESULT_OK;
    }
    if (prepared != NULL) {
        return append_prepared_triangle(prepared, &setup);
    }

    rasterize_triangle_setup(rasterizer, &setup);
    return SOC_RESULT_OK;
}

static soc_result process_clip_triangle(
    soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided,
    soc_raster_prepared_list* prepared,
    soc_bool* out_rasterized
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

    *out_rasterized = SOC_FALSE;
    if (setup_result == SOC_RASTER_SETUP_REJECTED) {
        return SOC_RESULT_OK;
    }
    return process_screen_triangle(
        rasterizer,
        screen,
        NULL,
        prepared,
        out_rasterized
    );
}

soc_result soc_raster_tile_locks_initialize(
    soc_raster_tile_locks* tile_locks,
    uint32_t width,
    uint32_t height
)
{
    uint32_t column_count;
    uint32_t row_count;
    size_t lock_count;
    size_t lock_bytes;
    atomic_uint* locks;
    size_t lock_index;

    if (tile_locks == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    memset(tile_locks, 0, sizeof(*tile_locks));
    if (!calculate_tile_lock_grid(
            width,
            height,
            &column_count,
            &row_count,
            &lock_count
        ) ||
        !checked_size_multiply(
            lock_count,
            sizeof(*locks),
            &lock_bytes
        )) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    locks = malloc(lock_bytes);
    if (locks == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    for (lock_index = 0u; lock_index < lock_count; ++lock_index) {
        atomic_init(&locks[lock_index], 0u);
    }

    tile_locks->column_count = column_count;
    tile_locks->row_count = row_count;
    tile_locks->lock_count = lock_count;
    tile_locks->locks = locks;
    return SOC_RESULT_OK;
}

void soc_raster_tile_locks_shutdown(
    soc_raster_tile_locks* tile_locks
)
{
    if (tile_locks == NULL) {
        return;
    }

    free(tile_locks->locks);
    memset(tile_locks, 0, sizeof(*tile_locks));
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
    uint32_t block_column_count;
    uint32_t block_row_count;
    size_t early_z_block_count;
    uint32_t early_z_tile_column_count;
    uint32_t early_z_tile_row_count;
    size_t early_z_tile_count;
    void* early_z_storage;
    float* early_z_farthest_depths;
    float* early_z_tile_farthest_depths;

    if (rasterizer == NULL ||
        width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        depth == NULL ||
        kernels == NULL ||
        kernels->clear_f32 == NULL ||
        kernels->store_constant_depth_block_f32 == NULL ||
        kernels->store_depth_plane_block_f32 == NULL ||
        kernels->transform_triangle_f64 == NULL ||
        !checked_size_multiply(
            (size_t)width,
            (size_t)height,
            &required_element_count
        ) ||
        !calculate_early_z_block_grid(
            width,
            height,
            &block_column_count,
            &block_row_count,
            &early_z_block_count
        ) ||
        !calculate_tile_lock_grid(
            width,
            height,
            &early_z_tile_column_count,
            &early_z_tile_row_count,
            &early_z_tile_count
        ) ||
        depth_element_count < required_element_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    early_z_storage = allocate_early_z_storage(
        early_z_block_count,
        early_z_tile_count,
        &early_z_farthest_depths,
        &early_z_tile_farthest_depths
    );
    if (early_z_storage == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    memset(rasterizer, 0, sizeof(*rasterizer));
    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->depth_element_count = required_element_count;
    rasterizer->depth = depth;
    rasterizer->block_column_count = block_column_count;
    rasterizer->block_row_count = block_row_count;
    rasterizer->early_z_block_count = early_z_block_count;
    rasterizer->early_z_storage = early_z_storage;
    rasterizer->early_z_farthest_depths = early_z_farthest_depths;
    rasterizer->early_z_tile_column_count = early_z_tile_column_count;
    rasterizer->early_z_tile_row_count = early_z_tile_row_count;
    rasterizer->early_z_tile_count = early_z_tile_count;
    rasterizer->early_z_tile_farthest_depths =
        early_z_tile_farthest_depths;
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

    if (rasterizer->initialized == SOC_TRUE) {
        soc_aligned_free(rasterizer->early_z_storage);
        soc_aligned_free(rasterizer->early_z_pending_masks);
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
    uint32_t block_column_count;
    uint32_t block_row_count;
    size_t early_z_block_count;
    uint32_t early_z_tile_column_count;
    uint32_t early_z_tile_row_count;
    size_t early_z_tile_count;
    void* early_z_storage;
    float* early_z_farthest_depths;
    uint64_t* early_z_pending_masks;
    float* early_z_tile_farthest_depths;

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
        !calculate_early_z_block_grid(
            width,
            height,
            &block_column_count,
            &block_row_count,
            &early_z_block_count
        ) ||
        !calculate_tile_lock_grid(
            width,
            height,
            &early_z_tile_column_count,
            &early_z_tile_row_count,
            &early_z_tile_count
        ) ||
        depth_element_count < required_element_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    early_z_storage = rasterizer->early_z_storage;
    early_z_farthest_depths = rasterizer->early_z_farthest_depths;
    early_z_pending_masks = rasterizer->early_z_pending_masks;
    early_z_tile_farthest_depths =
        rasterizer->early_z_tile_farthest_depths;
    if (early_z_block_count != rasterizer->early_z_block_count ||
        early_z_tile_count != rasterizer->early_z_tile_count) {
        early_z_storage = allocate_early_z_storage(
            early_z_block_count,
            early_z_tile_count,
            &early_z_farthest_depths,
            &early_z_tile_farthest_depths
        );
        if (early_z_storage == NULL) {
            return SOC_RESULT_OUT_OF_MEMORY;
        }
        if (early_z_pending_masks != NULL &&
            early_z_block_count != rasterizer->early_z_block_count) {
            early_z_pending_masks = allocate_early_z_pending_masks(
                early_z_block_count
            );
            if (early_z_pending_masks == NULL) {
                soc_aligned_free(early_z_storage);
                return SOC_RESULT_OUT_OF_MEMORY;
            }
            soc_aligned_free(rasterizer->early_z_pending_masks);
        }
        soc_aligned_free(rasterizer->early_z_storage);
    }
    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->depth_element_count = required_element_count;
    rasterizer->depth = depth;
    rasterizer->block_column_count = block_column_count;
    rasterizer->block_row_count = block_row_count;
    rasterizer->early_z_block_count = early_z_block_count;
    rasterizer->early_z_storage = early_z_storage;
    rasterizer->early_z_farthest_depths = early_z_farthest_depths;
    rasterizer->early_z_pending_masks = early_z_pending_masks;
    rasterizer->early_z_tile_column_count = early_z_tile_column_count;
    rasterizer->early_z_tile_row_count = early_z_tile_row_count;
    rasterizer->early_z_tile_count = early_z_tile_count;
    rasterizer->early_z_tile_farthest_depths =
        early_z_tile_farthest_depths;
    rasterizer->tile_locks = NULL;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_configure_tile_locks(
    soc_rasterizer* rasterizer,
    soc_raster_tile_locks* tile_locks
)
{
    uint32_t expected_column_count;
    uint32_t expected_row_count;
    size_t expected_lock_count;

    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (tile_locks == NULL) {
        rasterizer->tile_locks = NULL;
        return SOC_RESULT_OK;
    }
    if (!calculate_tile_lock_grid(
            rasterizer->width,
            rasterizer->height,
            &expected_column_count,
            &expected_row_count,
            &expected_lock_count
        ) ||
        tile_locks->locks == NULL ||
        tile_locks->column_count != expected_column_count ||
        tile_locks->row_count != expected_row_count ||
        tile_locks->lock_count != expected_lock_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    rasterizer->tile_locks = tile_locks;
    return SOC_RESULT_OK;
}

static soc_result begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc,
    soc_bool clear_depth
)
{
    float initial_depth;
    float untouched_depth;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        rasterizer->depth == NULL ||
        desc == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    initial_depth = 0.0f;
    untouched_depth = -1.0f;
    rasterizer->frame = *desc;
    if (clear_depth == SOC_TRUE) {
        rasterizer->kernels->clear_f32(
            rasterizer->depth,
            rasterizer->depth_element_count,
            initial_depth
        );
    }
    rasterizer->kernels->clear_f32(
        rasterizer->early_z_farthest_depths,
        rasterizer->early_z_block_count,
        untouched_depth
    );
    rasterizer->early_z_ready_tile_count = 0u;
    rasterizer->early_z_frame_farthest_depth = initial_depth;
    rasterizer->early_z_coarse_dirty = SOC_FALSE;
    rasterizer->clipped_triangle_count = 0u;
    rasterizer->rasterized_triangle_count = 0u;
    rasterizer->frame_active = SOC_TRUE;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
)
{
    return begin_frame(rasterizer, desc, SOC_TRUE);
}

soc_result soc_rasterizer_begin_frame_no_clear(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
)
{
    return begin_frame(rasterizer, desc, SOC_FALSE);
}

static soc_result process_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count,
    soc_raster_prepared_list* prepared
)
{
    soc_kernel_mat4_f64 clip_from_world_f64;
    soc_kernel_mat4_f64 object_to_world_f64;
    soc_kernel_mat4_f64 clip_from_object_f64;
    uint32_t triangle;
    const uint32_t triangle_end = triangle_begin + triangle_count;
    const soc_bool two_sided =
        (mesh->flags & SOC_MESH_FLAG_TWO_SIDED) != 0u
            ? SOC_TRUE
            : SOC_FALSE;

    soc_kernel_mat4_f64_from_f32(
        &rasterizer->frame.clip_from_world,
        &clip_from_world_f64
    );
    soc_kernel_mat4_f64_from_f32(
        object_to_world,
        &object_to_world_f64
    );
    soc_kernel_mat4_f64_multiply(
        &clip_from_world_f64,
        &object_to_world_f64,
        &clip_from_object_f64
    );

    for (triangle = triangle_begin; triangle < triangle_end; ++triangle) {
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
            rasterizer->frame.clip_depth_range,
            clip_triangle_vertices,
            &clip_metadata
        );
        active_planes = clip_metadata.active_planes;
        if (clip_metadata.common_planes != 0u) {
            clip_classification = SOC_CLIP_CLASSIFICATION_REJECT;
        } else {
            clip_classification = active_planes == 0u
                ? SOC_CLIP_CLASSIFICATION_ACCEPT
                : SOC_CLIP_CLASSIFICATION_PARTIAL;
        }
        if (clip_classification == SOC_CLIP_CLASSIFICATION_REJECT) {
            ++rasterizer->clipped_triangle_count;
            continue;
        }

        if (clip_classification == SOC_CLIP_CLASSIFICATION_ACCEPT) {
            soc_bool was_rasterized;
            const soc_result result = process_clip_triangle(
                rasterizer,
                &clip_triangle_vertices[0],
                &clip_triangle_vertices[1],
                &clip_triangle_vertices[2],
                two_sided,
                prepared,
                &was_rasterized
            );

            if (result != SOC_RESULT_OK) {
                return result;
            }
            if (was_rasterized == SOC_TRUE) {
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
                    two_sided,
                    screen
                );

            if (setup_result == SOC_RASTER_SETUP_REJECTED) {
                continue;
            }
            if (clipped_vertex_count > 3u &&
                shared_depth_plane == NULL) {
                configure_shared_fan_depth_plane(
                    rasterizer,
                    screen,
                    &fan_depth_plane
                );
                shared_depth_plane = &fan_depth_plane;
            }
            {
                soc_bool was_rasterized;
                const soc_result result = process_screen_triangle(
                    rasterizer,
                    screen,
                    shared_depth_plane,
                    prepared,
                    &was_rasterized
                );

                if (result != SOC_RESULT_OK) {
                    return result;
                }
                if (was_rasterized == SOC_TRUE) {
                    ++rasterizer->rasterized_triangle_count;
                }
            }
        }
    }

    return SOC_RESULT_OK;
}

static soc_bool occluder_triangle_range_is_valid(
    const soc_mesh* mesh,
    uint32_t triangle_begin,
    uint32_t triangle_count
)
{
    const uint32_t mesh_triangle_count = mesh != NULL
        ? mesh->index_count / 3u
        : 0u;

    return triangle_count != 0u &&
        triangle_begin < mesh_triangle_count &&
        triangle_count <= mesh_triangle_count - triangle_begin
        ? SOC_TRUE
        : SOC_FALSE;
}

soc_result soc_rasterizer_submit_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count
)
{
    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        mesh == NULL ||
        mesh->positions_xyz == NULL ||
        mesh->indices == NULL ||
        object_to_world == NULL ||
        occluder_triangle_range_is_valid(
            mesh,
            triangle_begin,
            triangle_count
        ) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    return process_occluder_triangles(
        rasterizer,
        mesh,
        object_to_world,
        triangle_begin,
        triangle_count,
        NULL
    );
}

soc_result soc_rasterizer_prepare_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count,
    soc_raster_prepared_list* prepared
)
{
    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (mesh == NULL ||
        mesh->positions_xyz == NULL ||
        mesh->indices == NULL ||
        object_to_world == NULL ||
        prepared_list_is_valid(prepared) != SOC_TRUE ||
        occluder_triangle_range_is_valid(
            mesh,
            triangle_begin,
            triangle_count
        ) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    return process_occluder_triangles(
        rasterizer,
        mesh,
        object_to_world,
        triangle_begin,
        triangle_count,
        prepared
    );
}

soc_result soc_rasterizer_rasterize_prepared_triangles(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    size_t prepared_count
)
{
    size_t index;

    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (prepared_count != 0u && prepared == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (index = 0u; index < prepared_count; ++index) {
        rasterize_triangle_setup(rasterizer, &prepared[index]);
    }
    return SOC_RESULT_OK;
}

void soc_rasterizer_rasterize_prepared_region_unchecked(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region
)
{
    soc_raster_region intersection;

    intersection.minimum_x =
        prepared->bounds.minimum_x > region->minimum_x
            ? prepared->bounds.minimum_x
            : region->minimum_x;
    intersection.minimum_y =
        prepared->bounds.minimum_y > region->minimum_y
            ? prepared->bounds.minimum_y
            : region->minimum_y;
    intersection.end_x = prepared->bounds.end_x < region->end_x
        ? prepared->bounds.end_x
        : region->end_x;
    intersection.end_y = prepared->bounds.end_y < region->end_y
        ? prepared->bounds.end_y
        : region->end_y;

    if (intersection.minimum_x >= intersection.end_x ||
        intersection.minimum_y >= intersection.end_y) {
        return;
    }
    if (intersection.minimum_x == prepared->bounds.minimum_x &&
        intersection.minimum_y == prepared->bounds.minimum_y &&
        intersection.end_x == prepared->bounds.end_x &&
        intersection.end_y == prepared->bounds.end_y) {
        rasterize_triangle_setup_unlocked(rasterizer, prepared);
        return;
    }

    rasterize_triangle_blocks(rasterizer, prepared, &intersection);
}

soc_result soc_rasterizer_rasterize_prepared_region(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region
)
{
    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (prepared == NULL ||
        region == NULL ||
        region->minimum_x > region->end_x ||
        region->minimum_y > region->end_y ||
        prepared->bounds.minimum_x >= prepared->bounds.end_x ||
        prepared->bounds.minimum_y >= prepared->bounds.end_y ||
        prepared->bounds.end_x > rasterizer->width ||
        prepared->bounds.end_y > rasterizer->height) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    soc_rasterizer_rasterize_prepared_region_unchecked(
        rasterizer,
        prepared,
        region
    );
    return SOC_RESULT_OK;
}

static soc_bool raster_target_is_valid(
    const soc_raster_target* target
)
{
    size_t last_row_offset;
    size_t required_elements;

    if (target == NULL ||
        target->depth == NULL ||
        target->width == 0u ||
        target->height == 0u ||
        target->row_stride < (size_t)target->width ||
        target->width > UINT32_MAX - target->origin_x ||
        target->height > UINT32_MAX - target->origin_y ||
        !checked_size_multiply(
            (size_t)(target->height - 1u),
            target->row_stride,
            &last_row_offset
        ) ||
        (size_t)target->width > SIZE_MAX - last_row_offset) {
        return SOC_FALSE;
    }

    required_elements = last_row_offset + (size_t)target->width;
    if (target->element_count < required_elements) {
        return SOC_FALSE;
    }
    return SOC_TRUE;
}

void soc_raster_target_reset_early_z_unchecked(
    soc_raster_target* target
)
{
    size_t block_index;

    for (block_index = 0u;
         block_index < target->early_z_block_count;
         ++block_index) {
        target->early_z_farthest_depths[block_index] = -1.0f;
    }
}

void soc_rasterizer_rasterize_prepared_region_to_target_unchecked(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region,
    const soc_raster_target* target
)
{
    soc_raster_region intersection;

    intersection.minimum_x =
        prepared->bounds.minimum_x > region->minimum_x
            ? prepared->bounds.minimum_x
            : region->minimum_x;
    intersection.minimum_y =
        prepared->bounds.minimum_y > region->minimum_y
            ? prepared->bounds.minimum_y
            : region->minimum_y;
    intersection.end_x = prepared->bounds.end_x < region->end_x
        ? prepared->bounds.end_x
        : region->end_x;
    intersection.end_y = prepared->bounds.end_y < region->end_y
        ? prepared->bounds.end_y
        : region->end_y;
    if (intersection.minimum_x >= intersection.end_x ||
        intersection.minimum_y >= intersection.end_y) {
        return;
    }

    rasterize_triangle_blocks_to_target(
        rasterizer,
        prepared,
        &intersection,
        target
    );
}

soc_result soc_rasterizer_rasterize_prepared_region_to_target(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region,
    const soc_raster_target* target
)
{
    soc_raster_region framebuffer_region;
    uint32_t target_end_x;
    uint32_t target_end_y;

    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (prepared == NULL ||
        region == NULL ||
        region->minimum_x > region->end_x ||
        region->minimum_y > region->end_y ||
        prepared->bounds.minimum_x >= prepared->bounds.end_x ||
        prepared->bounds.minimum_y >= prepared->bounds.end_y ||
        prepared->bounds.end_x > rasterizer->width ||
        prepared->bounds.end_y > rasterizer->height ||
        raster_target_is_valid(target) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    framebuffer_region.minimum_x = region->minimum_x;
    framebuffer_region.minimum_y = region->minimum_y;
    framebuffer_region.end_x = region->end_x < rasterizer->width
        ? region->end_x
        : rasterizer->width;
    framebuffer_region.end_y = region->end_y < rasterizer->height
        ? region->end_y
        : rasterizer->height;
    if (framebuffer_region.minimum_x >= framebuffer_region.end_x ||
        framebuffer_region.minimum_y >= framebuffer_region.end_y) {
        return SOC_RESULT_OK;
    }

    target_end_x = target->origin_x + target->width;
    target_end_y = target->origin_y + target->height;
    if (target->origin_x > framebuffer_region.minimum_x ||
        target->origin_y > framebuffer_region.minimum_y ||
        target_end_x < framebuffer_region.end_x ||
        target_end_y < framebuffer_region.end_y) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    soc_rasterizer_rasterize_prepared_region_to_target_unchecked(
        rasterizer,
        prepared,
        &framebuffer_region,
        target
    );
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
    const uint32_t triangle_count = mesh != NULL
        ? mesh->index_count / 3u
        : 0u;

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
    if (rasterizer->early_z_ready_tile_count !=
        rasterizer->early_z_tile_count) {
        rebuild_coarse_early_z(rasterizer);
    }

    for (instance = 0u; instance < instance_count; ++instance) {
        const soc_result result =
            soc_rasterizer_submit_occluder_triangles(
                rasterizer,
                mesh,
                &object_to_world[instance],
                0u,
                triangle_count
            );

        if (result != SOC_RESULT_OK) {
            return result;
        }
        if (instance + 1u < instance_count &&
            rasterizer->early_z_ready_tile_count !=
                rasterizer->early_z_tile_count) {
            rebuild_coarse_early_z(rasterizer);
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
