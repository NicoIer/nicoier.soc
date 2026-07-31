#include "raster/soc_rasterizer.h"

#include "core/soc_mesh.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SOC_CLIP_PLANE_COUNT 6u
#define SOC_MAX_CLIPPED_VERTICES 12u

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

static soc_result allocate_depth_buffer(
    uint32_t width,
    uint32_t height,
    float** out_depth,
    size_t* out_element_count
)
{
    float* depth;
    size_t element_count;
    size_t byte_count;

    if (out_depth == NULL ||
        out_element_count == NULL ||
        width == 0u ||
        height == 0u ||
        !checked_size_multiply(
            (size_t)width,
            (size_t)height,
            &element_count
        ) ||
        !checked_size_multiply(
            element_count,
            sizeof(float),
            &byte_count
        )) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    depth = malloc(byte_count);
    if (depth == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    *out_depth = depth;
    *out_element_count = element_count;
    return SOC_RESULT_OK;
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
    soc_clip_depth_range depth_range,
    soc_bool* out_was_clipped
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
        } else {
            *out_was_clipped = SOC_TRUE;
        }

        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }

    return output_count;
}

static uint32_t clip_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex input_triangle[3],
    soc_clip_vertex output_polygon[SOC_MAX_CLIPPED_VERTICES],
    soc_bool* out_was_clipped
)
{
    soc_clip_vertex buffer_a[SOC_MAX_CLIPPED_VERTICES];
    soc_clip_vertex buffer_b[SOC_MAX_CLIPPED_VERTICES];
    soc_clip_vertex* input = buffer_a;
    soc_clip_vertex* output = buffer_b;
    uint32_t vertex_count = 3u;
    uint32_t plane;
    uint32_t index;

    *out_was_clipped = SOC_FALSE;
    for (index = 0u; index < 3u; ++index) {
        if (finite_clip_vertex(&input_triangle[index]) != SOC_TRUE) {
            *out_was_clipped = SOC_TRUE;
            return 0u;
        }
        buffer_a[index] = input_triangle[index];
    }

    for (plane = 0u; plane < SOC_CLIP_PLANE_COUNT; ++plane) {
        soc_clip_vertex* swap;

        vertex_count = clip_polygon_against_plane(
            input,
            vertex_count,
            output,
            plane,
            rasterizer->frame.clip_depth_range,
            out_was_clipped
        );
        if (vertex_count == 0u) {
            return 0u;
        }

        swap = input;
        input = output;
        output = swap;
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

static soc_bool is_top_left_edge(
    const soc_screen_vertex* start,
    const soc_screen_vertex* end
)
{
    const double delta_x = end->x - start->x;
    const double delta_y = end->y - start->y;

    return delta_y < 0.0 || (delta_y == 0.0 && delta_x > 0.0)
        ? SOC_TRUE
        : SOC_FALSE;
}

static soc_bool edge_contains_sample(
    double edge_value,
    soc_bool top_left
)
{
    return edge_value > 0.0 ||
        (edge_value == 0.0 && top_left == SOC_TRUE)
        ? SOC_TRUE
        : SOC_FALSE;
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

static soc_bool rasterize_triangle(
    soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided
)
{
    const soc_clip_vertex* clip_vertices[3] = {clip0, clip1, clip2};
    soc_screen_vertex ndc[3];
    soc_screen_vertex screen[3];
    double ndc_area;
    double screen_area;
    double minimum_x;
    double maximum_x;
    double minimum_y;
    double maximum_y;
    uint32_t minimum_pixel_x;
    uint32_t maximum_pixel_x;
    uint32_t minimum_pixel_y;
    uint32_t maximum_pixel_y;
    soc_bool edge0_top_left;
    soc_bool edge1_top_left;
    soc_bool edge2_top_left;
    uint32_t index;
    uint32_t pixel_y;

    for (index = 0u; index < 3u; ++index) {
        double depth;

        if (clip_vertices[index]->w <= 0.0) {
            return SOC_FALSE;
        }

        ndc[index].x = clip_vertices[index]->x / clip_vertices[index]->w;
        ndc[index].y = clip_vertices[index]->y / clip_vertices[index]->w;
        depth = clip_vertices[index]->z / clip_vertices[index]->w;
        if (rasterizer->frame.clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = depth * 0.5 + 0.5;
        }

        if (finite_double(ndc[index].x) != SOC_TRUE ||
            finite_double(ndc[index].y) != SOC_TRUE ||
            finite_double(depth) != SOC_TRUE) {
            return SOC_FALSE;
        }

        ndc[index].x = clamp_double(ndc[index].x, -1.0, 1.0);
        ndc[index].y = clamp_double(ndc[index].y, -1.0, 1.0);
        ndc[index].depth = clamp_double(depth, 0.0, 1.0);
    }

    ndc_area = edge_function(
        &ndc[0],
        &ndc[1],
        ndc[2].x,
        ndc[2].y
    );
    if (ndc_area == 0.0) {
        return SOC_FALSE;
    }

    if (two_sided != SOC_TRUE) {
        const soc_bool front_facing =
            rasterizer->frame.front_face == SOC_FRONT_FACE_CCW
                ? (ndc_area > 0.0 ? SOC_TRUE : SOC_FALSE)
                : (ndc_area < 0.0 ? SOC_TRUE : SOC_FALSE);
        if (front_facing != SOC_TRUE) {
            return SOC_FALSE;
        }
    }

    for (index = 0u; index < 3u; ++index) {
        screen[index].x =
            (ndc[index].x * 0.5 + 0.5) * rasterizer->width;
        screen[index].y =
            (0.5 - ndc[index].y * 0.5) * rasterizer->height;
        screen[index].depth = ndc[index].depth;
    }

    screen_area = edge_function(
        &screen[0],
        &screen[1],
        screen[2].x,
        screen[2].y
    );
    if (screen_area == 0.0) {
        return SOC_FALSE;
    }
    if (screen_area < 0.0) {
        swap_screen_vertices(&screen[1], &screen[2]);
        screen_area = -screen_area;
    }

    minimum_x = screen[0].x;
    maximum_x = screen[0].x;
    minimum_y = screen[0].y;
    maximum_y = screen[0].y;
    for (index = 1u; index < 3u; ++index) {
        if (screen[index].x < minimum_x) {
            minimum_x = screen[index].x;
        }
        if (screen[index].x > maximum_x) {
            maximum_x = screen[index].x;
        }
        if (screen[index].y < minimum_y) {
            minimum_y = screen[index].y;
        }
        if (screen[index].y > maximum_y) {
            maximum_y = screen[index].y;
        }
    }

    if (maximum_x < 0.0 ||
        maximum_y < 0.0 ||
        minimum_x > (double)rasterizer->width ||
        minimum_y > (double)rasterizer->height) {
        return SOC_TRUE;
    }

    minimum_x = clamp_double(
        minimum_x,
        0.0,
        (double)(rasterizer->width - 1u)
    );
    maximum_x = clamp_double(
        maximum_x,
        0.0,
        (double)(rasterizer->width - 1u)
    );
    minimum_y = clamp_double(
        minimum_y,
        0.0,
        (double)(rasterizer->height - 1u)
    );
    maximum_y = clamp_double(
        maximum_y,
        0.0,
        (double)(rasterizer->height - 1u)
    );

    minimum_pixel_x = (uint32_t)minimum_x;
    maximum_pixel_x = (uint32_t)maximum_x;
    minimum_pixel_y = (uint32_t)minimum_y;
    maximum_pixel_y = (uint32_t)maximum_y;

    edge0_top_left = is_top_left_edge(&screen[1], &screen[2]);
    edge1_top_left = is_top_left_edge(&screen[2], &screen[0]);
    edge2_top_left = is_top_left_edge(&screen[0], &screen[1]);

    for (pixel_y = minimum_pixel_y;
         pixel_y <= maximum_pixel_y;
         ++pixel_y) {
        uint32_t pixel_x;

        for (pixel_x = minimum_pixel_x;
             pixel_x <= maximum_pixel_x;
             ++pixel_x) {
            const double sample_x = (double)pixel_x + 0.5;
            const double sample_y = (double)pixel_y + 0.5;
            const double edge0 = edge_function(
                &screen[1],
                &screen[2],
                sample_x,
                sample_y
            );
            const double edge1 = edge_function(
                &screen[2],
                &screen[0],
                sample_x,
                sample_y
            );
            const double edge2 = edge_function(
                &screen[0],
                &screen[1],
                sample_x,
                sample_y
            );
            double depth;
            size_t depth_index;
            float stored_depth;
            soc_bool passes_depth;

            if (edge_contains_sample(edge0, edge0_top_left) != SOC_TRUE ||
                edge_contains_sample(edge1, edge1_top_left) != SOC_TRUE ||
                edge_contains_sample(edge2, edge2_top_left) != SOC_TRUE) {
                continue;
            }

            depth = (
                edge0 * screen[0].depth +
                edge1 * screen[1].depth +
                edge2 * screen[2].depth
            ) / screen_area;
            if (finite_double(depth) != SOC_TRUE) {
                continue;
            }
            depth = clamp_double(depth, 0.0, 1.0);
            depth_index = (size_t)pixel_y * rasterizer->width + pixel_x;
            stored_depth = rasterizer->depth[depth_index];
            passes_depth =
                rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
                    ? (depth > stored_depth ? SOC_TRUE : SOC_FALSE)
                    : (depth < stored_depth ? SOC_TRUE : SOC_FALSE);
            if (passes_depth == SOC_TRUE) {
                rasterizer->depth[depth_index] = (float)depth;
            }
        }
    }

    return SOC_TRUE;
}

static uint32_t halve_ceil(uint32_t value)
{
    return value / 2u + value % 2u;
}

static uint32_t calculate_hiz_level_count(uint32_t width, uint32_t height)
{
    uint32_t level_count = 1u;

    while (width > 1u || height > 1u) {
        width = halve_ceil(width);
        height = halve_ceil(height);
        ++level_count;
    }

    return level_count;
}

static void calculate_hiz_level_dimensions(
    const soc_rasterizer* rasterizer,
    uint32_t level,
    uint32_t* out_width,
    uint32_t* out_height
)
{
    uint32_t width = rasterizer->width;
    uint32_t height = rasterizer->height;
    uint32_t current_level;

    for (current_level = 0u; current_level < level; ++current_level) {
        width = halve_ceil(width);
        height = halve_ceil(height);
    }

    *out_width = width;
    *out_height = height;
}

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
)
{
    float* depth;
    size_t depth_element_count;
    soc_result result;

    if (rasterizer == NULL || width == 0u || height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    result = allocate_depth_buffer(
        width,
        height,
        &depth,
        &depth_element_count
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->hiz_level_count = calculate_hiz_level_count(width, height);
    rasterizer->depth_element_count = depth_element_count;
    rasterizer->depth = depth;
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

    free(rasterizer->depth);
    memset(rasterizer, 0, sizeof(*rasterizer));
}

soc_result soc_rasterizer_resize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
)
{
    float* depth;
    size_t depth_element_count;
    soc_result result;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        width == 0u ||
        height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    if (rasterizer->width == width && rasterizer->height == height) {
        return SOC_RESULT_OK;
    }

    result = allocate_depth_buffer(
        width,
        height,
        &depth,
        &depth_element_count
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    free(rasterizer->depth);
    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->hiz_level_count = calculate_hiz_level_count(width, height);
    rasterizer->depth_element_count = depth_element_count;
    rasterizer->depth = depth;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
)
{
    float clear_depth;
    size_t index;

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
    for (index = 0u; index < rasterizer->depth_element_count; ++index) {
        rasterizer->depth[index] = clear_depth;
    }
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
    uint32_t instance;

    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        mesh == NULL ||
        mesh->positions_xyz == NULL ||
        mesh->indices == NULL ||
        object_to_world == NULL ||
        instance_count == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (instance = 0u; instance < instance_count; ++instance) {
        const soc_mat4* instance_transform = &object_to_world[instance];
        const uint32_t triangle_count = mesh->index_count / 3u;
        uint32_t triangle;

        for (triangle = 0u; triangle < triangle_count; ++triangle) {
            soc_clip_vertex clip_triangle_vertices[3];
            soc_clip_vertex clipped_polygon[SOC_MAX_CLIPPED_VERTICES];
            soc_bool was_clipped;
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

            clipped_vertex_count = clip_triangle(
                rasterizer,
                clip_triangle_vertices,
                clipped_polygon,
                &was_clipped
            );
            if (was_clipped == SOC_TRUE) {
                ++rasterizer->clipped_triangle_count;
            }
            if (clipped_vertex_count < 3u) {
                continue;
            }

            for (fan_index = 1u;
                 fan_index + 1u < clipped_vertex_count;
                 ++fan_index) {
                if (rasterize_triangle(
                        rasterizer,
                        &clipped_polygon[0],
                        &clipped_polygon[fan_index],
                        &clipped_polygon[fan_index + 1u],
                        (mesh->flags & SOC_MESH_FLAG_TWO_SIDED) != 0u
                            ? SOC_TRUE
                            : SOC_FALSE
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

    /* Framework only: the Hi-Z hierarchy will be built here. */
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_test_aabbs(
    soc_rasterizer* rasterizer,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility
)
{
    uint32_t index;

    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        world_bounds == NULL ||
        out_visibility == NULL ||
        bounds_count == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    /*
     * Framework only: UNKNOWN is fail-open and prevents false occlusion until
     * projection and Hi-Z testing are implemented.
     */
    for (index = 0u; index < bounds_count; ++index) {
        out_visibility[index] = SOC_VISIBILITY_UNKNOWN;
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

soc_result soc_rasterizer_query_hiz_level(
    const soc_rasterizer* rasterizer,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
)
{
    uint32_t width;
    uint32_t height;
    uint64_t required_count;
    uint64_t index;
    float clear_depth;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        rasterizer->frame_active != SOC_TRUE ||
        out_info == NULL ||
        out_info->struct_size < SOC_HIZ_LEVEL_INFO_SIZE_V1) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (level >= rasterizer->hiz_level_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    calculate_hiz_level_dimensions(rasterizer, level, &width, &height);
    required_count = (uint64_t)width * height;

    out_info->level = level;
    out_info->width = width;
    out_info->height = height;
    out_info->required_element_count = required_count;

    if (out_depth == NULL) {
        return out_depth_count == 0u
            ? SOC_RESULT_OK
            : SOC_RESULT_INVALID_ARGUMENT;
    }
    if (out_depth_count < required_count) {
        return SOC_RESULT_BUFFER_TOO_SMALL;
    }

    if (level == 0u) {
        memcpy(
            out_depth,
            rasterizer->depth,
            rasterizer->depth_element_count * sizeof(float)
        );
    } else {
        clear_depth = rasterizer->frame.depth_direction == SOC_DEPTH_REVERSED
            ? 0.0f
            : 1.0f;
        for (index = 0u; index < required_count; ++index) {
            out_depth[index] = clear_depth;
        }
    }

    return SOC_RESULT_OK;
}
