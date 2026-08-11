#include "core/soc_mesh.h"
#include "platform/soc_thread_pool.h"
#include "raster/soc_rasterizer.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define CANARY_WORD_COUNT 8u
#define MAX_CANARY_PIXEL_COUNT (19u * 17u)

static float absolute_float(float value)
{
    return value < 0.0f ? -value : value;
}

static int float_buffers_match_f32(
    const float* left,
    const float* right,
    size_t count,
    float tolerance
)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (left[index] == right[index]) {
            continue;
        }
        if (!(absolute_float(left[index] - right[index]) <= tolerance)) {
            return 0;
        }
    }
    return 1;
}

static int depth_buffers_match_f32(
    const float* left,
    const float* right,
    size_t count,
    float tolerance
)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (!(left[index] >= 0.0f && left[index] <= 1.0f) ||
            !(right[index] >= 0.0f && right[index] <= 1.0f) ||
            ((left[index] == 0.0f) != (right[index] == 0.0f)) ||
            absolute_float(left[index] - right[index]) > tolerance) {
            return 0;
        }
    }
    return 1;
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf( \
                stderr, \
                "%s:%d: check failed: %s\n", \
                __FILE__, \
                __LINE__, \
                #condition \
            ); \
            return 1; \
        } \
    } while (0)

typedef struct raster_capture {
    uint32_t width;
    uint32_t height;
    size_t pixel_count;
    uint64_t clipped_triangle_count;
    uint64_t rasterized_triangle_count;
    float* depth;
} raster_capture;

typedef struct screen_vertex {
    double x;
    double y;
    float depth;
} screen_vertex;

typedef struct locked_raster_state {
    soc_rasterizer* rasterizers;
    const soc_mesh* meshes;
    soc_mat4 object_to_world;
    uint32_t submission_count;
    soc_result results[2];
} locked_raster_state;

static size_t counted_depth_block_store_calls;

static void count_store_constant_depth_block_f32(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float candidate_depth
)
{
    ++counted_depth_block_store_calls;
    soc_kernel_store_constant_depth_block_f32_scalar(
        destination,
        row_stride,
        block_width,
        block_height,
        coverage_mask,
        candidate_depth
    );
}

static void count_store_depth_plane_block_f32(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float depth_origin,
    float depth_step_x,
    float depth_step_y
)
{
    ++counted_depth_block_store_calls;
    soc_kernel_store_depth_plane_block_f32_scalar(
        destination,
        row_stride,
        block_width,
        block_height,
        coverage_mask,
        depth_origin,
        depth_step_x,
        depth_step_y
    );
}

static soc_kernel_table make_counting_scalar_kernel_table(void)
{
    soc_kernel_table kernels = *soc_kernel_table_scalar();

    kernels.store_constant_depth_block_f32 =
        count_store_constant_depth_block_f32;
    kernels.store_depth_plane_block_f32 =
        count_store_depth_plane_block_f32;
    return kernels;
}

static void submit_locked_meshes(
    void* user_data,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    locked_raster_state* state = user_data;
    uint32_t submission;
    soc_result result = SOC_RESULT_OK;

    if (worker_count != 2u || worker_index >= 2u) {
        return;
    }
    for (submission = 0u;
         submission < state->submission_count && result == SOC_RESULT_OK;
         ++submission) {
        result = soc_rasterizer_submit_occluders(
            &state->rasterizers[worker_index],
            &state->meshes[worker_index],
            &state->object_to_world,
            1u
        );
    }
    if (result == SOC_RESULT_OK) {
        result = soc_rasterizer_finish_occluders(
            &state->rasterizers[worker_index]
        );
    }
    state->results[worker_index] = result;
}

static soc_mat4 identity_matrix(void)
{
    const soc_mat4 identity = {
        .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
        .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
        .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
        .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    return identity;
}

static soc_frame_desc make_frame_desc(
    soc_clip_depth_range clip_depth_range
)
{
    const soc_frame_desc frame = {
        .struct_size = sizeof(frame),
        .clip_from_world = {
            .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
            .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
        },
        .clip_depth_range = clip_depth_range,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    return frame;
}

static soc_mesh make_mesh(
    float* positions,
    uint32_t vertex_count,
    uint32_t* indices,
    uint32_t index_count
)
{
    soc_mesh mesh;

    memset(&mesh, 0, sizeof(mesh));
    mesh.flags = SOC_MESH_FLAG_TWO_SIDED;
    mesh.vertex_count = vertex_count;
    mesh.index_count = index_count;
    mesh.index_type = SOC_INDEX_UINT32;
    mesh.positions_xyz = positions;
    mesh.indices = indices;
    return mesh;
}

static void write_screen_vertex(
    float* positions,
    uint32_t vertex_index,
    uint32_t width,
    uint32_t height,
    const screen_vertex* vertex,
    soc_clip_depth_range clip_depth_range
)
{
    const size_t offset = (size_t)vertex_index * 3u;

    positions[offset] = (float)(
        vertex->x * 2.0 / (double)width - 1.0
    );
    positions[offset + 1u] = (float)(
        1.0 - vertex->y * 2.0 / (double)height
    );
    positions[offset + 2u] =
        clip_depth_range == SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
            ? vertex->depth * 2.0f - 1.0f
            : vertex->depth;
}

static void write_triangle(
    float positions[9],
    uint32_t width,
    uint32_t height,
    const screen_vertex vertices[3],
    soc_clip_depth_range clip_depth_range
)
{
    uint32_t vertex;

    for (vertex = 0u; vertex < 3u; ++vertex) {
        write_screen_vertex(
            positions,
            vertex,
            width,
            height,
            &vertices[vertex],
            clip_depth_range
        );
    }
}

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float float_from_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int run_mesh_sequence(
    const soc_mesh* const* meshes,
    uint32_t mesh_count,
    const soc_frame_desc* frame,
    uint32_t width,
    uint32_t height,
    raster_capture* out_capture
)
{
    const size_t pixel_count = (size_t)width * height;
    const soc_mat4 identity = identity_matrix();
    soc_rasterizer rasterizer;
    soc_result result;
    uint32_t mesh_index;

    memset(out_capture, 0, sizeof(*out_capture));
    out_capture->depth = malloc(pixel_count * sizeof(*out_capture->depth));
    if (out_capture->depth == NULL) {
        return 1;
    }

    result = soc_rasterizer_initialize(
        &rasterizer,
        width,
        height,
        out_capture->depth,
        pixel_count,
        soc_kernel_table_scalar()
    );
    if (result == SOC_RESULT_OK) {
        result = soc_rasterizer_begin_frame(&rasterizer, frame);
    }
    for (mesh_index = 0u;
         result == SOC_RESULT_OK && mesh_index < mesh_count;
         ++mesh_index) {
        result = soc_rasterizer_submit_occluders(
            &rasterizer,
            meshes[mesh_index],
            &identity,
            1u
        );
    }
    if (result == SOC_RESULT_OK) {
        result = soc_rasterizer_finish_occluders(&rasterizer);
    }
    if (result == SOC_RESULT_OK) {
        out_capture->width = width;
        out_capture->height = height;
        out_capture->pixel_count = pixel_count;
        out_capture->clipped_triangle_count =
            rasterizer.clipped_triangle_count;
        out_capture->rasterized_triangle_count =
            rasterizer.rasterized_triangle_count;
        result = soc_rasterizer_end_frame(&rasterizer);
    }
    soc_rasterizer_shutdown(&rasterizer);

    if (result != SOC_RESULT_OK) {
        free(out_capture->depth);
        memset(out_capture, 0, sizeof(*out_capture));
        return 1;
    }
    return 0;
}

static int run_prepared_mesh_sequence(
    const soc_mesh* const* meshes,
    uint32_t mesh_count,
    const soc_frame_desc* frame,
    uint32_t width,
    uint32_t height,
    raster_capture* out_capture,
    size_t* out_prepared_count
)
{
    const size_t pixel_count = (size_t)width * height;
    const soc_mat4 identity = identity_matrix();
    soc_raster_prepared_list prepared = {0};
    soc_rasterizer rasterizer;
    soc_result result;
    uint64_t clipped_before_replay = 0u;
    uint64_t rasterized_before_replay = 0u;
    uint32_t mesh_index;

    memset(&rasterizer, 0, sizeof(rasterizer));
    memset(out_capture, 0, sizeof(*out_capture));
    *out_prepared_count = 0u;
    out_capture->depth = malloc(pixel_count * sizeof(*out_capture->depth));
    if (out_capture->depth == NULL) {
        return 1;
    }

    result = soc_rasterizer_initialize(
        &rasterizer,
        width,
        height,
        out_capture->depth,
        pixel_count,
        soc_kernel_table_scalar()
    );
    if (result == SOC_RESULT_OK) {
        result = soc_rasterizer_begin_frame(&rasterizer, frame);
    }
    for (mesh_index = 0u;
         result == SOC_RESULT_OK && mesh_index < mesh_count;
         ++mesh_index) {
        result = soc_rasterizer_prepare_occluder_triangles(
            &rasterizer,
            meshes[mesh_index],
            &identity,
            0u,
            meshes[mesh_index]->index_count / 3u,
            &prepared
        );
    }
    if (result == SOC_RESULT_OK) {
        clipped_before_replay = rasterizer.clipped_triangle_count;
        rasterized_before_replay = rasterizer.rasterized_triangle_count;
        result = soc_rasterizer_rasterize_prepared_triangles(
            &rasterizer,
            prepared.data,
            prepared.count
        );
    }
    if (result == SOC_RESULT_OK &&
        (rasterizer.clipped_triangle_count != clipped_before_replay ||
         rasterizer.rasterized_triangle_count !=
            rasterized_before_replay)) {
        result = SOC_RESULT_INTERNAL_ERROR;
    }
    if (result == SOC_RESULT_OK) {
        result = soc_rasterizer_finish_occluders(&rasterizer);
    }
    if (result == SOC_RESULT_OK) {
        out_capture->width = width;
        out_capture->height = height;
        out_capture->pixel_count = pixel_count;
        out_capture->clipped_triangle_count = clipped_before_replay;
        out_capture->rasterized_triangle_count = rasterized_before_replay;
        *out_prepared_count = prepared.count;
        result = soc_rasterizer_end_frame(&rasterizer);
    }

    soc_raster_prepared_list_shutdown(&prepared);
    soc_rasterizer_shutdown(&rasterizer);
    if (result != SOC_RESULT_OK) {
        free(out_capture->depth);
        memset(out_capture, 0, sizeof(*out_capture));
        *out_prepared_count = 0u;
        return 1;
    }
    return 0;
}

static int run_one_mesh(
    const soc_mesh* mesh,
    const soc_frame_desc* frame,
    uint32_t width,
    uint32_t height,
    raster_capture* out_capture
)
{
    const soc_mesh* meshes[] = {mesh};

    return run_mesh_sequence(
        meshes,
        1u,
        frame,
        width,
        height,
        out_capture
    );
}

static void release_capture(raster_capture* capture)
{
    free(capture->depth);
    memset(capture, 0, sizeof(*capture));
}

static uint64_t capture_coverage_mask_8x8(
    const raster_capture* capture
)
{
    const uint32_t clear_bits = float_bits(0.0f);
    uint64_t mask = 0u;
    uint32_t pixel;

    for (pixel = 0u; pixel < 64u; ++pixel) {
        if (float_bits(capture->depth[pixel]) != clear_bits) {
            mask |= UINT64_C(1) << pixel;
        }
    }
    return mask;
}

static uint32_t random_u32(uint32_t* state)
{
    uint32_t value = *state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static double random_unit(uint32_t* state)
{
    return (double)(random_u32(state) >> 8u) *
        (1.0 / 16777216.0);
}

static int compact_edge_matches_reference(
    int64_t start_x,
    int64_t start_y,
    int64_t end_x,
    int64_t end_y,
    uint32_t pixel_x,
    uint32_t pixel_y
)
{
    const int64_t subpixel_scale = INT64_C(256);
    const int64_t subpixel_half = INT64_C(128);
    const int64_t delta_x = end_x - start_x;
    const int64_t delta_y = end_y - start_y;
    const int64_t sample_x =
        (int64_t)pixel_x * subpixel_scale + subpixel_half;
    const int64_t sample_y =
        (int64_t)pixel_y * subpixel_scale + subpixel_half;
    const int64_t bias = delta_y < 0 ||
            (delta_y == 0 && delta_x > 0)
        ? 0
        : -1;
    const int64_t reference =
        delta_x * (sample_y - start_y) -
        delta_y * (sample_x - start_x) + bias;
    const soc_raster_prepared_edge compact = {
        .sample_origin =
            delta_x * (subpixel_half - start_y) -
            delta_y * (subpixel_half - start_x) + bias,
        .step_x = -delta_y * subpixel_scale,
        .step_y = delta_x * subpixel_scale,
    };
    const int64_t actual = compact.sample_origin +
        compact.step_x * (int64_t)pixel_x +
        compact.step_y * (int64_t)pixel_y;

    CHECK(actual == reference);
    return 0;
}

static int test_compact_prepared_edges_are_integer_exact(void)
{
    static const uint32_t boundary_pixels[][2] = {
        {0u, 0u},
        {SOC_MAX_RASTER_DIMENSION - 1u, 0u},
        {0u, SOC_MAX_RASTER_DIMENSION - 1u},
        {
            SOC_MAX_RASTER_DIMENSION - 1u,
            SOC_MAX_RASTER_DIMENSION - 1u,
        },
        {
            SOC_MAX_RASTER_DIMENSION / 2u,
            SOC_MAX_RASTER_DIMENSION / 2u,
        },
    };
    const int64_t maximum_fixed =
        (int64_t)SOC_MAX_RASTER_DIMENSION * INT64_C(256);
    const int64_t boundary_edges[][4] = {
        {0, 0, maximum_fixed, maximum_fixed},
        {maximum_fixed, 0, 0, maximum_fixed},
        {0, maximum_fixed, maximum_fixed, 0},
        {maximum_fixed, maximum_fixed, 0, 0},
        {0, 0, maximum_fixed, 0},
        {maximum_fixed, maximum_fixed, maximum_fixed, 0},
    };
    uint32_t random_state = UINT32_C(0x91e10da5);
    size_t edge_index;

    _Static_assert(
        sizeof(soc_raster_prepared_edge) == 24u,
        "unexpected prepared edge size"
    );
    _Static_assert(
        sizeof(soc_raster_prepared_triangle) == 108u ||
            sizeof(soc_raster_prepared_triangle) == 112u,
        "unexpected prepared triangle size"
    );
    for (edge_index = 0u;
         edge_index < ARRAY_COUNT(boundary_edges);
         ++edge_index) {
        size_t pixel_index;

        for (pixel_index = 0u;
             pixel_index < ARRAY_COUNT(boundary_pixels);
             ++pixel_index) {
            CHECK(compact_edge_matches_reference(
                boundary_edges[edge_index][0],
                boundary_edges[edge_index][1],
                boundary_edges[edge_index][2],
                boundary_edges[edge_index][3],
                boundary_pixels[pixel_index][0],
                boundary_pixels[pixel_index][1]
            ) == 0);
        }
    }

    for (edge_index = 0u; edge_index < 16384u; ++edge_index) {
        const uint32_t fixed_modulus =
            (uint32_t)maximum_fixed + 1u;
        const int64_t start_x =
            (int64_t)(random_u32(&random_state) % fixed_modulus);
        const int64_t start_y =
            (int64_t)(random_u32(&random_state) % fixed_modulus);
        const int64_t end_x =
            (int64_t)(random_u32(&random_state) % fixed_modulus);
        const int64_t end_y =
            (int64_t)(random_u32(&random_state) % fixed_modulus);
        const uint32_t pixel_x =
            random_u32(&random_state) % SOC_MAX_RASTER_DIMENSION;
        const uint32_t pixel_y =
            random_u32(&random_state) % SOC_MAX_RASTER_DIMENSION;

        CHECK(compact_edge_matches_reference(
            start_x,
            start_y,
            end_x,
            end_y,
            pixel_x,
            pixel_y
        ) == 0);
    }
    return 0;
}

static double snap_screen_coordinate_q8(double coordinate)
{
    return (double)(int64_t)(coordinate * 256.0 + 0.5) / 256.0;
}

static double continuous_edge_value(
    const screen_vertex* start,
    const screen_vertex* end,
    double point_x,
    double point_y
)
{
    return (end->x - start->x) * (point_y - start->y) -
        (end->y - start->y) * (point_x - start->x);
}

static int continuous_edge_contains_sample(
    const screen_vertex* start,
    const screen_vertex* end,
    double point_x,
    double point_y
)
{
    const double value = continuous_edge_value(
        start,
        end,
        point_x,
        point_y
    );
    const double delta_x = end->x - start->x;
    const double delta_y = end->y - start->y;
    const int top_left = delta_y < 0.0 ||
        (delta_y == 0.0 && delta_x > 0.0);

    return value > 0.0 || (value == 0.0 && top_left);
}

static void reconstruct_screen_triangle(
    const float positions[9],
    uint32_t width,
    uint32_t height,
    screen_vertex out_vertices[3]
)
{
    uint32_t vertex;

    for (vertex = 0u; vertex < 3u; ++vertex) {
        const size_t offset = (size_t)vertex * 3u;

        out_vertices[vertex].x =
            ((double)positions[offset] * 0.5 + 0.5) *
                (double)width;
        out_vertices[vertex].y =
            (0.5 - (double)positions[offset + 1u] * 0.5) *
                (double)height;
        out_vertices[vertex].depth = positions[offset + 2u];
    }
    if (continuous_edge_value(
            &out_vertices[0],
            &out_vertices[1],
            out_vertices[2].x,
            out_vertices[2].y
        ) < 0.0) {
        const screen_vertex temporary = out_vertices[1];

        out_vertices[1] = out_vertices[2];
        out_vertices[2] = temporary;
    }
}

static int test_q8_snapped_coverage_and_depth_match_f32_math(void)
{
    enum {
        WIDTH = 31,
        HEIGHT = 23,
        TRIANGLE_COUNT = 64,
    };
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const uint32_t clear_bits = float_bits(0.0f);
    uint32_t indices[] = {0u, 1u, 2u};
    uint32_t random_state = UINT32_C(0x7b1d5a39);
    uint32_t accepted_triangle_count = 0u;
    uint32_t attempt_count = 0u;
    uint32_t total_covered_count = 0u;
    uint32_t non_lattice_coordinate_count = 0u;

    while (accepted_triangle_count < TRIANGLE_COUNT &&
           attempt_count < 2048u) {
        screen_vertex generated[3];
        screen_vertex reconstructed[3];
        float positions[9];
        soc_mesh mesh;
        raster_capture capture;
        double area;
        uint32_t vertex;
        uint32_t y;

        ++attempt_count;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            generated[vertex].x = 1.0 +
                random_unit(&random_state) * ((double)WIDTH - 2.0);
            generated[vertex].y = 1.0 +
                random_unit(&random_state) * ((double)HEIGHT - 2.0);
            generated[vertex].depth = (float)(
                0.1 + random_unit(&random_state) * 0.8
            );
        }
        area = continuous_edge_value(
            &generated[0],
            &generated[1],
            generated[2].x,
            generated[2].y
        );
        if (area > -8.0 && area < 8.0) {
            continue;
        }

        write_triangle(
            positions,
            WIDTH,
            HEIGHT,
            generated,
            SOC_CLIP_DEPTH_ZERO_TO_ONE
        );
        reconstruct_screen_triangle(
            positions,
            WIDTH,
            HEIGHT,
            reconstructed
        );
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const double scaled_x = reconstructed[vertex].x * 256.0;
            const double scaled_y = reconstructed[vertex].y * 256.0;
            const uint64_t rounded_x = (uint64_t)(scaled_x + 0.5);
            const uint64_t rounded_y = (uint64_t)(scaled_y + 0.5);

            if (scaled_x != (double)rounded_x) {
                ++non_lattice_coordinate_count;
            }
            if (scaled_y != (double)rounded_y) {
                ++non_lattice_coordinate_count;
            }
            reconstructed[vertex].x = snap_screen_coordinate_q8(
                reconstructed[vertex].x
            );
            reconstructed[vertex].y = snap_screen_coordinate_q8(
                reconstructed[vertex].y
            );
        }

        mesh = make_mesh(positions, 3u, indices, 3u);
        CHECK(run_one_mesh(
            &mesh,
            &frame,
            WIDTH,
            HEIGHT,
            &capture
        ) == 0);
        CHECK(capture.clipped_triangle_count == 0u);
        CHECK(capture.rasterized_triangle_count == 1u);

        for (y = 0u; y < HEIGHT; ++y) {
            uint32_t x;

            for (x = 0u; x < WIDTH; ++x) {
                const size_t pixel = (size_t)y * WIDTH + x;
                const float stored_depth = capture.depth[pixel];
                const int covered =
                    float_bits(stored_depth) != clear_bits;
                int reference_covered = 1;
                uint32_t edge;

                for (edge = 0u; edge < 3u; ++edge) {
                    if (!continuous_edge_contains_sample(
                            &reconstructed[(edge + 1u) % 3u],
                            &reconstructed[(edge + 2u) % 3u],
                            (double)x + 0.5,
                            (double)y + 0.5
                        )) {
                        reference_covered = 0;
                        break;
                    }
                }
                CHECK(covered == reference_covered);
                if (!covered) {
                    continue;
                }
                {
                    const double point_x = (double)x + 0.5;
                    const double point_y = (double)y + 0.5;
                    const double triangle_area = continuous_edge_value(
                        &reconstructed[0],
                        &reconstructed[1],
                        reconstructed[2].x,
                        reconstructed[2].y
                    );
                    const double weight0 = continuous_edge_value(
                        &reconstructed[1],
                        &reconstructed[2],
                        point_x,
                        point_y
                    );
                    const double weight1 = continuous_edge_value(
                        &reconstructed[2],
                        &reconstructed[0],
                        point_x,
                        point_y
                    );
                    const double weight2 = continuous_edge_value(
                        &reconstructed[0],
                        &reconstructed[1],
                        point_x,
                        point_y
                    );
                    const float expected_depth = (
                        (float)weight0 * reconstructed[0].depth +
                        (float)weight1 * reconstructed[1].depth +
                        (float)weight2 * reconstructed[2].depth
                    ) / (float)triangle_area;

                    CHECK(stored_depth >= 0.0f);
                    CHECK(stored_depth <= 1.0f);
                    CHECK(absolute_float(
                        stored_depth - expected_depth
                    ) <= 0x1p-10f);
                }
                ++total_covered_count;
            }
        }
        release_capture(&capture);
        ++accepted_triangle_count;
    }

    CHECK(accepted_triangle_count == TRIANGLE_COUNT);
    CHECK(total_covered_count > 1024u);
    CHECK(non_lattice_coordinate_count > TRIANGLE_COUNT * 3u);
    return 0;
}

/*
 * This literal mask is a compact contract for Q8 setup, the top-left edge
 * convention, and row-major 8x8 block masks.  The vertices deliberately use
 * quarter-pixel coordinates, which are represented exactly by Q8.
 */
static int test_fixed_top_left_coverage_mask(void)
{
    static const screen_vertex vertices[3] = {
        {1.25, 1.25, 0.375f},
        {6.75, 1.25, 0.375f},
        {6.75, 6.25, 0.375f},
    };
    static const uint64_t expected_mask =
        UINT64_C(0x0000406070787c00);
    uint32_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_mesh mesh;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    raster_capture capture;
    uint64_t mask;

    write_triangle(
        positions,
        8u,
        8u,
        vertices,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    mesh = make_mesh(positions, 3u, indices, 3u);
    CHECK(run_one_mesh(&mesh, &frame, 8u, 8u, &capture) == 0);
    CHECK(capture.clipped_triangle_count == 0u);
    CHECK(capture.rasterized_triangle_count == 1u);

    mask = capture_coverage_mask_8x8(&capture);
    release_capture(&capture);
    CHECK(mask == expected_mask);
    return 0;
}

static int test_shared_edge_masks_are_a_partition(void)
{
    static const screen_vertex first_vertices[3] = {
        {1.25, 1.25, 0.375f},
        {6.75, 1.25, 0.375f},
        {6.75, 6.25, 0.375f},
    };
    static const screen_vertex second_vertices[3] = {
        {1.25, 1.25, 0.375f},
        {6.75, 6.25, 0.375f},
        {1.25, 6.25, 0.375f},
    };
    static const uint64_t expected_union =
        UINT64_C(0x00007e7e7e7e7e00);
    uint32_t indices[] = {0u, 1u, 2u};
    float first_positions[9];
    float second_positions[9];
    soc_mesh first_mesh;
    soc_mesh second_mesh;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    raster_capture first_capture;
    raster_capture second_capture;
    uint64_t first_mask;
    uint64_t second_mask;

    write_triangle(
        first_positions,
        8u,
        8u,
        first_vertices,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    write_triangle(
        second_positions,
        8u,
        8u,
        second_vertices,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    first_mesh = make_mesh(first_positions, 3u, indices, 3u);
    second_mesh = make_mesh(second_positions, 3u, indices, 3u);

    CHECK(run_one_mesh(&first_mesh, &frame, 8u, 8u, &first_capture) == 0);
    CHECK(run_one_mesh(
        &second_mesh,
        &frame,
        8u,
        8u,
        &second_capture
    ) == 0);
    first_mask = capture_coverage_mask_8x8(&first_capture);
    second_mask = capture_coverage_mask_8x8(&second_capture);
    release_capture(&first_capture);
    release_capture(&second_capture);

    CHECK((first_mask & second_mask) == 0u);
    CHECK((first_mask | second_mask) == expected_union);
    return 0;
}

static int capture_pixel_is_covered(
    const raster_capture* capture,
    size_t pixel
)
{
    return float_bits(capture->depth[pixel]) !=
        float_bits(0.0f);
}

static int check_non_lattice_shared_quad(
    uint32_t width,
    uint32_t height,
    double minimum,
    double maximum,
    int opposite_diagonal,
    int varying_depth
)
{
    screen_vertex vertices[4] = {
        {minimum, minimum, 0.25f},
        {maximum, minimum, varying_depth ? 0.35f : 0.25f},
        {maximum, maximum, varying_depth ? 0.45f : 0.25f},
        {minimum, maximum, varying_depth ? 0.55f : 0.25f},
    };
    uint32_t first_indices[3];
    uint32_t second_indices[3];
    uint32_t first_then_second[6];
    uint32_t second_then_first[6];
    float positions[12];
    soc_mesh first_mesh;
    soc_mesh second_mesh;
    soc_mesh combined_first_mesh;
    soc_mesh combined_second_mesh;
    const soc_mesh* split_first_order[2];
    const soc_mesh* split_second_order[2];
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    raster_capture first_capture;
    raster_capture second_capture;
    raster_capture combined_first_capture;
    raster_capture combined_second_capture;
    raster_capture split_first_capture;
    raster_capture split_second_capture;
    uint32_t vertex;
    uint32_t y;

    if (opposite_diagonal) {
        const uint32_t first[3] = {0u, 1u, 3u};
        const uint32_t second[3] = {1u, 2u, 3u};

        memcpy(first_indices, first, sizeof(first_indices));
        memcpy(second_indices, second, sizeof(second_indices));
    } else {
        const uint32_t first[3] = {0u, 1u, 2u};
        const uint32_t second[3] = {0u, 2u, 3u};

        memcpy(first_indices, first, sizeof(first_indices));
        memcpy(second_indices, second, sizeof(second_indices));
    }
    memcpy(first_then_second, first_indices, sizeof(first_indices));
    memcpy(
        first_then_second + ARRAY_COUNT(first_indices),
        second_indices,
        sizeof(second_indices)
    );
    memcpy(second_then_first, second_indices, sizeof(second_indices));
    memcpy(
        second_then_first + ARRAY_COUNT(second_indices),
        first_indices,
        sizeof(first_indices)
    );

    for (vertex = 0u; vertex < ARRAY_COUNT(vertices); ++vertex) {
        write_screen_vertex(
            positions,
            vertex,
            width,
            height,
            &vertices[vertex],
            SOC_CLIP_DEPTH_ZERO_TO_ONE
        );
    }
    first_mesh = make_mesh(positions, 4u, first_indices, 3u);
    second_mesh = make_mesh(positions, 4u, second_indices, 3u);
    combined_first_mesh = make_mesh(
        positions,
        4u,
        first_then_second,
        6u
    );
    combined_second_mesh = make_mesh(
        positions,
        4u,
        second_then_first,
        6u
    );
    split_first_order[0] = &first_mesh;
    split_first_order[1] = &second_mesh;
    split_second_order[0] = &second_mesh;
    split_second_order[1] = &first_mesh;

    CHECK(run_one_mesh(
        &first_mesh,
        &frame,
        width,
        height,
        &first_capture
    ) == 0);
    CHECK(run_one_mesh(
        &second_mesh,
        &frame,
        width,
        height,
        &second_capture
    ) == 0);
    CHECK(run_one_mesh(
        &combined_first_mesh,
        &frame,
        width,
        height,
        &combined_first_capture
    ) == 0);
    CHECK(run_one_mesh(
        &combined_second_mesh,
        &frame,
        width,
        height,
        &combined_second_capture
    ) == 0);
    CHECK(run_mesh_sequence(
        split_first_order,
        2u,
        &frame,
        width,
        height,
        &split_first_capture
    ) == 0);
    CHECK(run_mesh_sequence(
        split_second_order,
        2u,
        &frame,
        width,
        height,
        &split_second_capture
    ) == 0);

    CHECK(first_capture.rasterized_triangle_count == 1u);
    CHECK(second_capture.rasterized_triangle_count == 1u);
    CHECK(combined_first_capture.rasterized_triangle_count == 2u);
    CHECK(combined_second_capture.rasterized_triangle_count == 2u);
    CHECK(split_first_capture.rasterized_triangle_count == 2u);
    CHECK(split_second_capture.rasterized_triangle_count == 2u);
    CHECK(depth_buffers_match_f32(
        combined_first_capture.depth,
        combined_second_capture.depth,
        combined_first_capture.pixel_count,
        0x1p-10f
    ));
    CHECK(depth_buffers_match_f32(
        split_first_capture.depth,
        split_second_capture.depth,
        split_first_capture.pixel_count,
        0x1p-10f
    ));

    for (y = 0u; y < height; ++y) {
        uint32_t x;

        for (x = 0u; x < width; ++x) {
            const size_t pixel = (size_t)y * width + x;
            const double sample_x = (double)x + 0.5;
            const double sample_y = (double)y + 0.5;
            const int expected = sample_x > minimum &&
                sample_x < maximum &&
                sample_y > minimum &&
                sample_y < maximum;
            const int first = capture_pixel_is_covered(
                &first_capture,
                pixel
            );
            const int second = capture_pixel_is_covered(
                &second_capture,
                pixel
            );

            /* Tile-local F32 edge values may round away the one-unit
             * top-left bias on a large non-lattice shared edge. Either
             * triangle may own that boundary sample, and duplicate ownership
             * is harmless; the functional contract here is a crack-free
             * combined quad in either submission order. */
            CHECK((first || second) == expected);
            CHECK(capture_pixel_is_covered(
                &combined_first_capture,
                pixel
            ) == expected);
            CHECK(capture_pixel_is_covered(
                &combined_second_capture,
                pixel
            ) == expected);
            CHECK(capture_pixel_is_covered(
                &split_first_capture,
                pixel
            ) == expected);
            CHECK(capture_pixel_is_covered(
                &split_second_capture,
                pixel
            ) == expected);
        }
    }

    release_capture(&first_capture);
    release_capture(&second_capture);
    release_capture(&combined_first_capture);
    release_capture(&combined_second_capture);
    release_capture(&split_first_capture);
    release_capture(&split_second_capture);
    return 0;
}

static int test_non_lattice_shared_edges_have_no_cracks(void)
{
    CHECK(check_non_lattice_shared_quad(
        32u,
        32u,
        2.1,
        29.1,
        0,
        0
    ) == 0);
    CHECK(check_non_lattice_shared_quad(
        32u,
        32u,
        2.1,
        28.9,
        1,
        0
    ) == 0);
    CHECK(check_non_lattice_shared_quad(
        8u,
        8u,
        1.1,
        6.9,
        0,
        0
    ) == 0);
    CHECK(check_non_lattice_shared_quad(
        8u,
        8u,
        1.1,
        6.9,
        0,
        1
    ) == 0);
    return 0;
}

static int check_q8_collapsed_triangle_is_discarded(void)
{
    enum {
        WIDTH = 16,
        HEIGHT = 16,
    };
    static const screen_vertex vertices[3] = {
        {3.5, 1.0, 0.375f},
        {3.499, 14.0, 0.375f},
        {3.501, 14.0, 0.375f},
    };
    uint32_t indices[] = {0u, 1u, 2u};
    float positions[9];
    screen_vertex reconstructed[3];
    soc_mesh mesh;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    raster_capture capture;
    int64_t fixed_x[3];
    int64_t fixed_y[3];
    int64_t fixed_area;
    uint32_t vertex;
    size_t pixel;

    write_triangle(
        positions,
        WIDTH,
        HEIGHT,
        vertices,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    reconstruct_screen_triangle(
        positions,
        WIDTH,
        HEIGHT,
        reconstructed
    );
    for (vertex = 0u; vertex < 3u; ++vertex) {
        fixed_x[vertex] = (int64_t)(
            reconstructed[vertex].x * 256.0 + 0.5
        );
        fixed_y[vertex] = (int64_t)(
            reconstructed[vertex].y * 256.0 + 0.5
        );
    }
    fixed_area = (fixed_x[1] - fixed_x[0]) *
            (fixed_y[2] - fixed_y[0]) -
        (fixed_y[1] - fixed_y[0]) *
            (fixed_x[2] - fixed_x[0]);
    CHECK(fixed_area == 0);

    mesh = make_mesh(positions, 3u, indices, 3u);
    CHECK(run_one_mesh(
        &mesh,
        &frame,
        WIDTH,
        HEIGHT,
        &capture
    ) == 0);
    CHECK(capture.rasterized_triangle_count == 0u);
    for (pixel = 0u; pixel < (size_t)WIDTH * HEIGHT; ++pixel) {
        CHECK(!capture_pixel_is_covered(
            &capture,
            pixel
        ));
    }
    release_capture(&capture);
    return 0;
}

static int test_q8_collapsed_triangle_is_discarded(void)
{
    CHECK(check_q8_collapsed_triangle_is_discarded() == 0);
    return 0;
}

static double ndc_edge_value(
    const float positions[9],
    uint32_t start,
    uint32_t end,
    double point_x,
    double point_y
)
{
    const double start_x = positions[(size_t)start * 3u];
    const double start_y = positions[(size_t)start * 3u + 1u];
    const double end_x = positions[(size_t)end * 3u];
    const double end_y = positions[(size_t)end * 3u + 1u];

    return (end_x - start_x) * (point_y - start_y) -
        (end_y - start_y) * (point_x - start_x);
}

static int check_clipped_fan_coverage(
    const float source_positions[9],
    uint64_t expected_rasterized_triangles
)
{
    enum {
        WIDTH = 32,
        HEIGHT = 32,
    };
    uint32_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_mesh mesh;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    raster_capture capture;
    const double area = ndc_edge_value(
        source_positions,
        0u,
        1u,
        source_positions[6],
        source_positions[7]
    );
    uint32_t checked_count = 0u;
    uint32_t covered_count = 0u;
    uint32_t y;

    memcpy(positions, source_positions, sizeof(positions));
    mesh = make_mesh(positions, 3u, indices, 3u);
    CHECK(run_one_mesh(
        &mesh,
        &frame,
        WIDTH,
        HEIGHT,
        &capture
    ) == 0);
    CHECK(capture.clipped_triangle_count == 1u);
    CHECK(capture.rasterized_triangle_count ==
        expected_rasterized_triangles);
    CHECK(area != 0.0);

    for (y = 0u; y < HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < WIDTH; ++x) {
            const size_t pixel = (size_t)y * WIDTH + x;
            const double ndc_x =
                ((double)x + 0.5) * 2.0 / WIDTH - 1.0;
            const double ndc_y =
                1.0 - ((double)y + 0.5) * 2.0 / HEIGHT;
            const double edge0 = ndc_edge_value(
                positions,
                0u,
                1u,
                ndc_x,
                ndc_y
            );
            const double edge1 = ndc_edge_value(
                positions,
                1u,
                2u,
                ndc_x,
                ndc_y
            );
            const double edge2 = ndc_edge_value(
                positions,
                2u,
                0u,
                ndc_x,
                ndc_y
            );
            int expected;

            /* Q8 endpoint snapping can move an exterior polygon edge by a
             * fraction of a pixel. Samples in that narrow boundary band are
             * intentionally governed by snapped fixed-point geometry. */
            if ((edge0 < 0.0 ? -edge0 : edge0) <= 0.001 ||
                (edge1 < 0.0 ? -edge1 : edge1) <= 0.001 ||
                (edge2 < 0.0 ? -edge2 : edge2) <= 0.001) {
                continue;
            }
            expected = area > 0.0
                ? (edge0 > 0.0 && edge1 > 0.0 && edge2 > 0.0)
                : (edge0 < 0.0 && edge1 < 0.0 && edge2 < 0.0);
            CHECK(capture_pixel_is_covered(
                &capture,
                pixel
            ) == expected);
            ++checked_count;
            if (expected) {
                ++covered_count;
            }
        }
    }
    release_capture(&capture);
    CHECK(checked_count > 900u);
    CHECK(covered_count > 64u);
    return 0;
}

static int test_clipped_polygon_fans_have_no_cracks(void)
{
    static const float clipped_quad[9] = {
        -1.5f, -0.5f, 0.375f,
         0.8f, -0.8f, 0.375f,
         0.8f,  0.8f, 0.375f,
    };
    static const float clipped_pentagon[9] = {
         0.8f, -0.8f, 0.375f,
         1.2f,  0.8f, 0.375f,
        -1.8f,  2.0f, 0.375f,
    };

    CHECK(check_clipped_fan_coverage(
        clipped_quad,
        2u
    ) == 0);
    CHECK(check_clipped_fan_coverage(
        clipped_pentagon,
        3u
    ) == 0);
    return 0;
}

static int check_varying_depth_matches_f32_math(void)
{
    static const screen_vertex vertices[3] = {
        {1.0, 1.0, 0.125f},
        {13.0, 1.0, 0.625f},
        {1.0, 13.0, 0.875f},
    };
    uint32_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_mesh mesh;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const uint32_t clear_bits = float_bits(0.0f);
    raster_capture capture;
    uint32_t covered_count = 0u;
    uint32_t y;

    write_triangle(
        positions,
        16u,
        16u,
        vertices,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    mesh = make_mesh(positions, 3u, indices, 3u);
    CHECK(run_one_mesh(&mesh, &frame, 16u, 16u, &capture) == 0);

    for (y = 0u; y < 16u; ++y) {
        uint32_t x;

        for (x = 0u; x < 16u; ++x) {
            const float stored = capture.depth[(size_t)y * 16u + x];
            const uint32_t stored_bits = float_bits(stored);
            float expected;

            if (stored_bits == clear_bits) {
                continue;
            }

            expected = 0.125f +
                0.5f * ((float)x + 0.5f - 1.0f) / 12.0f +
                0.75f * ((float)y + 0.5f - 1.0f) / 12.0f;
            CHECK(stored >= 0.0f);
            CHECK(stored <= 1.0f);
            CHECK(absolute_float(stored - expected) <= 0x1p-11f);
            ++covered_count;
        }
    }

    release_capture(&capture);
    CHECK(covered_count > 32u);
    return 0;
}

static int test_varying_depth_matches_f32_math(void)
{
    CHECK(check_varying_depth_matches_f32_math() == 0);
    return 0;
}

static int check_depth_range_and_submission_order(
    soc_clip_depth_range clip_depth_range
)
{
    static const double triangle_xy[6] = {
        1.0, 1.0,
        15.0, 1.0,
        1.0, 15.0,
    };
    const float winning_depth = 0.75f;
    const float losing_depth = 0.25f;
    const uint32_t clear_bits = float_bits(0.0f);
    uint32_t indices[] = {0u, 1u, 2u};
    float winning_positions[9];
    float losing_positions[9];
    screen_vertex winning_vertices[3];
    screen_vertex losing_vertices[3];
    soc_mesh winning_mesh;
    soc_mesh losing_mesh;
    const soc_mesh* losing_then_winning[2];
    const soc_mesh* winning_then_losing[2];
    const soc_frame_desc frame = make_frame_desc(clip_depth_range);
    raster_capture first_capture;
    raster_capture second_capture;
    uint32_t covered_count = 0u;
    uint32_t vertex;
    size_t pixel;

    for (vertex = 0u; vertex < 3u; ++vertex) {
        winning_vertices[vertex].x = triangle_xy[vertex * 2u];
        winning_vertices[vertex].y = triangle_xy[vertex * 2u + 1u];
        winning_vertices[vertex].depth = winning_depth;
        losing_vertices[vertex].x = winning_vertices[vertex].x;
        losing_vertices[vertex].y = winning_vertices[vertex].y;
        losing_vertices[vertex].depth = losing_depth;
    }
    write_triangle(
        winning_positions,
        16u,
        16u,
        winning_vertices,
        clip_depth_range
    );
    write_triangle(
        losing_positions,
        16u,
        16u,
        losing_vertices,
        clip_depth_range
    );
    winning_mesh = make_mesh(winning_positions, 3u, indices, 3u);
    losing_mesh = make_mesh(losing_positions, 3u, indices, 3u);
    losing_then_winning[0] = &losing_mesh;
    losing_then_winning[1] = &winning_mesh;
    winning_then_losing[0] = &winning_mesh;
    winning_then_losing[1] = &losing_mesh;

    CHECK(run_mesh_sequence(
        losing_then_winning,
        2u,
        &frame,
        16u,
        16u,
        &first_capture
    ) == 0);
    CHECK(run_mesh_sequence(
        winning_then_losing,
        2u,
        &frame,
        16u,
        16u,
        &second_capture
    ) == 0);
    CHECK(first_capture.rasterized_triangle_count == 2u);
    CHECK(second_capture.rasterized_triangle_count == 2u);

    for (pixel = 0u; pixel < first_capture.pixel_count; ++pixel) {
        const float first_depth = first_capture.depth[pixel];
        const float second_depth = second_capture.depth[pixel];

        CHECK(absolute_float(first_depth - second_depth) <= 0x1p-20f);
        if (float_bits(first_depth) != clear_bits) {
            CHECK(first_depth >= 0.0f);
            CHECK(first_depth <= 1.0f);
            CHECK(absolute_float(
                first_depth - winning_depth
            ) <= 0x1p-12f);
            ++covered_count;
        }
    }
    release_capture(&first_capture);
    release_capture(&second_capture);
    CHECK(covered_count > 64u);
    return 0;
}

static int test_depth_ranges_and_submission_orders(void)
{
    static const soc_clip_depth_range clip_depth_ranges[] = {
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
    };
    size_t range_index;

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        CHECK(check_depth_range_and_submission_order(
            clip_depth_ranges[range_index]
        ) == 0);
    }
    return 0;
}

static int check_canary_dimensions(
    uint32_t width,
    uint32_t height
)
{
    static const uint32_t canary_bits = UINT32_C(0x7fc12345);
    const size_t pixel_count = (size_t)width * height;
    const float clear_depth = 0.0f;
    const uint32_t clear_bits = float_bits(clear_depth);
    const float source_depth = 0.5f;
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    float storage[
        CANARY_WORD_COUNT + MAX_CANARY_PIXEL_COUNT + CANARY_WORD_COUNT
    ];
    float positions[] = {
        -1.0f, -1.0f, 0.5f,
         3.0f, -1.0f, 0.5f,
        -1.0f,  3.0f, 0.5f,
    };
    uint32_t indices[] = {0u, 1u, 2u};
    soc_mesh mesh = make_mesh(positions, 3u, indices, 3u);
    soc_rasterizer rasterizer;
    size_t storage_index;
    uint32_t changed_count = 0u;

    CHECK(pixel_count <= MAX_CANARY_PIXEL_COUNT);
    for (storage_index = 0u;
         storage_index < ARRAY_COUNT(storage);
         ++storage_index) {
        storage[storage_index] = float_from_bits(canary_bits);
    }

    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        width,
        height,
        storage + CANARY_WORD_COUNT,
        pixel_count,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_submit_occluders(
        &rasterizer,
        &mesh,
        &identity,
        1u
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_finish_occluders(&rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);

    for (storage_index = 0u;
         storage_index < CANARY_WORD_COUNT;
         ++storage_index) {
        CHECK(float_bits(storage[storage_index]) == canary_bits);
        CHECK(float_bits(
            storage[CANARY_WORD_COUNT + pixel_count + storage_index]
        ) == canary_bits);
    }
    for (storage_index = 0u;
         storage_index < pixel_count;
         ++storage_index) {
        const uint32_t bits = float_bits(
            storage[CANARY_WORD_COUNT + storage_index]
        );

        if (bits != clear_bits) {
            const float stored_depth =
                storage[CANARY_WORD_COUNT + storage_index];

            CHECK(stored_depth >= 0.0f);
            CHECK(stored_depth <= 1.0f);
            CHECK(absolute_float(
                stored_depth - source_depth
            ) <= 0x1p-12f);
            ++changed_count;
        }
    }
    CHECK(changed_count > 0u);
    return 0;
}

static int test_odd_block_tails_and_narrow_canaries(void)
{
    static const uint32_t dimensions[][2] = {
        {19u, 13u},
        {9u, 15u},
        {1u, 17u},
        {17u, 1u},
    };
    size_t dimension_index;

    for (dimension_index = 0u;
         dimension_index < ARRAY_COUNT(dimensions);
         ++dimension_index) {
        CHECK(check_canary_dimensions(
            dimensions[dimension_index][0],
            dimensions[dimension_index][1]
        ) == 0);
    }
    return 0;
}

static int test_triangle_range_submission_matches_full_mesh(void)
{
    enum {
        WIDTH = 32,
        HEIGHT = 24,
    };
    const size_t pixel_count = (size_t)WIDTH * HEIGHT;
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    float positions[] = {
        -0.9f, -0.8f, 0.8f,
        -0.1f, -0.8f, 0.8f,
        -0.5f,  0.1f, 0.8f,

        -1.4f, -0.6f, 0.6f,
        -0.2f, -0.6f, 0.6f,
        -0.6f,  0.8f, 0.6f,

         1.2f, -0.5f, 0.4f,
         1.5f,  0.0f, 0.4f,
         1.2f,  0.5f, 0.4f,

         0.1f, -0.8f, 0.2f,
         0.9f, -0.8f, 0.2f,
         0.5f,  0.2f, 0.2f,
    };
    uint32_t indices[] = {
        0u, 1u, 2u,
        3u, 4u, 5u,
        6u, 7u, 8u,
        9u, 10u, 11u,
    };
    soc_mesh mesh = make_mesh(
        positions,
        12u,
        indices,
        (uint32_t)ARRAY_COUNT(indices)
    );
    raster_capture full_capture;
    raster_capture range_capture;
    soc_rasterizer rasterizer;

    CHECK(run_one_mesh(
        &mesh,
        &frame,
        WIDTH,
        HEIGHT,
        &full_capture
    ) == 0);

    memset(&range_capture, 0, sizeof(range_capture));
    range_capture.depth = malloc(
        pixel_count * sizeof(*range_capture.depth)
    );
    CHECK(range_capture.depth != NULL);
    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        WIDTH,
        HEIGHT,
        range_capture.depth,
        pixel_count,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        1u
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        1u,
        2u
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        3u,
        1u
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_finish_occluders(&rasterizer) == SOC_RESULT_OK);
    range_capture.width = WIDTH;
    range_capture.height = HEIGHT;
    range_capture.pixel_count = pixel_count;
    range_capture.clipped_triangle_count =
        rasterizer.clipped_triangle_count;
    range_capture.rasterized_triangle_count =
        rasterizer.rasterized_triangle_count;
    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);

    CHECK(full_capture.clipped_triangle_count == 2u);
    CHECK(full_capture.rasterized_triangle_count == 4u);
    CHECK(range_capture.clipped_triangle_count ==
        full_capture.clipped_triangle_count);
    CHECK(range_capture.rasterized_triangle_count ==
        full_capture.rasterized_triangle_count);
    CHECK(depth_buffers_match_f32(
        range_capture.depth,
        full_capture.depth,
        pixel_count,
        0x1p-10f
    ));

    release_capture(&range_capture);
    release_capture(&full_capture);
    return 0;
}

static int test_triangle_range_submission_validates_bounds(void)
{
    enum {
        WIDTH = 8,
        HEIGHT = 8,
        PIXEL_COUNT = WIDTH * HEIGHT,
    };
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    float depth[PIXEL_COUNT];
    float positions[] = {
        -0.9f, -0.9f, 0.7f,
         0.0f, -0.9f, 0.7f,
        -0.5f,  0.0f, 0.7f,
         0.0f,  0.0f, 0.3f,
         0.9f,  0.0f, 0.3f,
         0.5f,  0.9f, 0.3f,
    };
    uint32_t indices[] = {0u, 1u, 2u, 3u, 4u, 5u};
    soc_mesh mesh = make_mesh(
        positions,
        6u,
        indices,
        (uint32_t)ARRAY_COUNT(indices)
    );
    soc_mesh invalid_mesh;
    soc_rasterizer rasterizer;
    uint32_t pixel;

    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        WIDTH,
        HEIGHT,
        depth,
        PIXEL_COUNT,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);

    CHECK(soc_rasterizer_submit_occluder_triangles(
        NULL,
        &mesh,
        &identity,
        0u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        NULL,
        &identity,
        0u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        NULL,
        0u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);

    invalid_mesh = mesh;
    invalid_mesh.positions_xyz = NULL;
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &invalid_mesh,
        &identity,
        0u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    invalid_mesh = mesh;
    invalid_mesh.indices = NULL;
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &invalid_mesh,
        &identity,
        0u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);

    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        0u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        2u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        1u,
        2u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        1u,
        UINT32_MAX
    ) == SOC_RESULT_INVALID_ARGUMENT);

    CHECK(rasterizer.clipped_triangle_count == 0u);
    CHECK(rasterizer.rasterized_triangle_count == 0u);
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        CHECK(float_bits(depth[pixel]) == float_bits(0.0f));
    }

    CHECK(soc_rasterizer_submit_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        1u,
        1u
    ) == SOC_RESULT_OK);
    CHECK(rasterizer.rasterized_triangle_count == 1u);
    CHECK(soc_rasterizer_finish_occluders(&rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);
    return 0;
}

static int check_prepared_replay_matches_immediate(void)
{
    enum {
        WIDTH = 32,
        HEIGHT = 32,
    };
    static const screen_vertex empty_triangle[3] = {
        {1.10, 1.10, 0.45f},
        {1.30, 1.10, 0.45f},
        {1.10, 1.30, 0.45f},
    };
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    float positions[36] = {
        /* One ordinary, varying-depth triangle. */
        -0.7f, -0.7f, 0.25f,
         0.4f, -0.6f, 0.65f,
        -0.3f,  0.6f, 0.40f,

        /* Clips to a pentagon and exercises the shared fan depth plane. */
         0.8f, -0.8f, 0.20f,
         1.2f,  0.8f, 0.70f,
        -1.8f,  2.0f, 0.40f,

        /* Trivially rejected by the right clip plane. */
         1.2f, -0.4f, 0.30f,
         1.6f,  0.0f, 0.30f,
         1.2f,  0.4f, 0.30f,
    };
    uint32_t indices[] = {
        0u, 1u, 2u,
        3u, 4u, 5u,
        6u, 7u, 8u,
        9u, 10u, 11u,
    };
    soc_mesh mesh;
    const soc_mesh* meshes[1];
    raster_capture immediate;
    raster_capture replayed;
    size_t prepared_count;

    write_triangle(
        positions + 27u,
        WIDTH,
        HEIGHT,
        empty_triangle,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    mesh = make_mesh(
        positions,
        12u,
        indices,
        (uint32_t)ARRAY_COUNT(indices)
    );
    meshes[0] = &mesh;

    CHECK(run_mesh_sequence(
        meshes,
        1u,
        &frame,
        WIDTH,
        HEIGHT,
        &immediate
    ) == 0);
    CHECK(run_prepared_mesh_sequence(
        meshes,
        1u,
        &frame,
        WIDTH,
        HEIGHT,
        &replayed,
        &prepared_count
    ) == 0);

    CHECK(immediate.clipped_triangle_count == 2u);
    CHECK(immediate.rasterized_triangle_count == 5u);
    CHECK(prepared_count == 4u);
    CHECK(replayed.clipped_triangle_count ==
        immediate.clipped_triangle_count);
    CHECK(replayed.rasterized_triangle_count ==
        immediate.rasterized_triangle_count);
    CHECK(depth_buffers_match_f32(
        replayed.depth,
        immediate.depth,
        immediate.pixel_count,
        0x1p-10f
    ));

    release_capture(&replayed);
    release_capture(&immediate);
    return 0;
}

static int test_prepared_replay_matches_immediate(void)
{
    CHECK(check_prepared_replay_matches_immediate() == 0);
    return 0;
}

static int test_prepared_list_and_invalid_state_semantics(void)
{
    enum {
        WIDTH = 8,
        HEIGHT = 8,
        PIXEL_COUNT = WIDTH * HEIGHT,
    };
    const size_t impossible_capacity =
        SIZE_MAX / sizeof(soc_raster_prepared_triangle) + 1u;
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    float depth[PIXEL_COUNT];
    float positions[] = {
        -0.8f, -0.8f, 0.4f,
         0.8f, -0.8f, 0.4f,
         0.0f,  0.8f, 0.4f,
    };
    uint32_t indices[] = {0u, 1u, 2u};
    soc_mesh mesh = make_mesh(
        positions,
        3u,
        indices,
        (uint32_t)ARRAY_COUNT(indices)
    );
    soc_raster_prepared_list list = {0};
    soc_raster_prepared_list invalid_list = {0};
    soc_raster_prepared_triangle* allocation;
    size_t capacity;
    soc_rasterizer rasterizer;
    uint64_t clipped_count;
    uint64_t rasterized_count;

    _Static_assert(
        sizeof(soc_raster_prepared_edge) == 24u,
        "unexpected prepared edge size"
    );
    _Static_assert(
        sizeof(soc_raster_prepared_triangle) == 108u ||
            sizeof(soc_raster_prepared_triangle) == 112u,
        "unexpected prepared triangle size"
    );
    CHECK(soc_raster_prepared_list_reserve(NULL, 1u) ==
        SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_raster_prepared_list_reserve(&list, 2u) == SOC_RESULT_OK);
    CHECK(list.data != NULL);
    CHECK(((uintptr_t)list.data & 127u) == 0u);
    CHECK(list.capacity == 2u);
    list.data[0].bounds.minimum_x = 17u;
    list.count = 1u;
    CHECK(soc_raster_prepared_list_reserve(&list, 5u) == SOC_RESULT_OK);
    CHECK(list.capacity == 5u);
    CHECK(((uintptr_t)list.data & 127u) == 0u);
    CHECK(list.count == 1u);
    CHECK(list.data[0].bounds.minimum_x == 17u);

    allocation = list.data;
    capacity = list.capacity;
    CHECK(soc_raster_prepared_list_reserve(
        &list,
        impossible_capacity
    ) == SOC_RESULT_OUT_OF_MEMORY);
    CHECK(list.data == allocation);
    CHECK(list.capacity == capacity);
    CHECK(list.count == 1u);
    CHECK(list.data[0].bounds.minimum_x == 17u);
    soc_raster_prepared_list_shutdown(&list);
    CHECK(list.data == NULL);
    CHECK(list.count == 0u);
    CHECK(list.capacity == 0u);
    soc_raster_prepared_list_shutdown(NULL);

    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        WIDTH,
        HEIGHT,
        depth,
        PIXEL_COUNT,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        1u,
        &list
    ) == SOC_RESULT_INVALID_STATE);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        NULL,
        0u
    ) == SOC_RESULT_INVALID_STATE);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);

    invalid_list.count = 1u;
    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        1u,
        &invalid_list
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        0u,
        &list
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        NULL,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        NULL,
        0u
    ) == SOC_RESULT_OK);

    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        1u,
        &list
    ) == SOC_RESULT_OK);
    CHECK(list.count == 1u);
    clipped_count = rasterizer.clipped_triangle_count;
    rasterized_count = rasterizer.rasterized_triangle_count;
    CHECK(clipped_count == 0u);
    CHECK(rasterized_count == 1u);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        list.data,
        list.count
    ) == SOC_RESULT_OK);
    CHECK(rasterizer.clipped_triangle_count == clipped_count);
    CHECK(rasterizer.rasterized_triangle_count == rasterized_count);
    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);

    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &rasterizer,
        &mesh,
        &identity,
        0u,
        1u,
        &list
    ) == SOC_RESULT_INVALID_STATE);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        list.data,
        list.count
    ) == SOC_RESULT_INVALID_STATE);

    soc_raster_prepared_list_shutdown(&list);
    soc_rasterizer_shutdown(&rasterizer);
    return 0;
}

static int check_prepared_region_replay(void)
{
    enum {
        WIDTH = 67,
        HEIGHT = 65,
    };
    static const screen_vertex triangle[3] = {
        {1.0, 1.0, 0.35f},
        {66.8, 64.9, 0.35f},
        {1.0, 64.9, 0.35f},
    };
    const size_t pixel_count = (size_t)WIDTH * HEIGHT;
    const size_t depth_bytes = pixel_count * sizeof(float);
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const uint32_t clear_bits = float_bits(0.0f);
    uint32_t indices[] = {0u, 1u, 2u};
    float positions[9];
    float* full_depth = malloc(depth_bytes);
    float* region_depth = malloc(depth_bytes);
    soc_mesh mesh;
    soc_raster_prepared_list prepared = {0};
    soc_rasterizer full_rasterizer;
    soc_rasterizer region_rasterizer;
    soc_raster_prepared_region region;
    soc_raster_prepared_region invalid_region;
    soc_raster_prepared_triangle invalid_prepared;
    uint64_t clipped_count;
    uint64_t rasterized_count;
    size_t pixel;
    uint32_t tail_covered = 0u;
    uint32_t fragment_covered = 0u;
    uint32_t tile_y;

    CHECK(full_depth != NULL);
    CHECK(region_depth != NULL);
    write_triangle(
        positions,
        WIDTH,
        HEIGHT,
        triangle,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    mesh = make_mesh(positions, 3u, indices, 3u);

    CHECK(soc_rasterizer_initialize(
        &full_rasterizer,
        WIDTH,
        HEIGHT,
        full_depth,
        pixel_count,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &full_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &full_rasterizer,
        &mesh,
        &identity,
        0u,
        1u,
        &prepared
    ) == SOC_RESULT_OK);
    CHECK(prepared.count == 1u);
    CHECK(prepared.data[0].bounds.end_x == WIDTH);
    CHECK(prepared.data[0].bounds.end_y == HEIGHT);
    CHECK(prepared.data[0].first_tile_column == 0u);
    CHECK(prepared.data[0].first_tile_row == 0u);
    CHECK(prepared.data[0].end_tile_column == 3u);
    CHECK(prepared.data[0].end_tile_row == 3u);
    clipped_count = full_rasterizer.clipped_triangle_count;
    rasterized_count = full_rasterizer.rasterized_triangle_count;
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &full_rasterizer,
        prepared.data,
        prepared.count
    ) == SOC_RESULT_OK);
    CHECK(full_rasterizer.clipped_triangle_count == clipped_count);
    CHECK(full_rasterizer.rasterized_triangle_count == rasterized_count);

    CHECK(soc_rasterizer_initialize(
        &region_rasterizer,
        WIDTH,
        HEIGHT,
        region_depth,
        pixel_count,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    region = prepared.data[0].bounds;
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &prepared.data[0],
        &region
    ) == SOC_RESULT_INVALID_STATE);
    CHECK(soc_rasterizer_begin_frame(
        &region_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_rasterize_prepared_region(
        NULL,
        &prepared.data[0],
        &region
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        NULL,
        &region
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &prepared.data[0],
        NULL
    ) == SOC_RESULT_INVALID_ARGUMENT);

    for (tile_y = 0u; tile_y < HEIGHT; tile_y += 32u) {
        uint32_t tile_x;

        for (tile_x = 0u; tile_x < WIDTH; tile_x += 32u) {
            region.minimum_x = tile_x;
            region.minimum_y = tile_y;
            /* Deliberately leave tail ends beyond the framebuffer. */
            region.end_x = tile_x + 32u;
            region.end_y = tile_y + 32u;
            CHECK(soc_rasterizer_rasterize_prepared_region(
                &region_rasterizer,
                &prepared.data[0],
                &region
            ) == SOC_RESULT_OK);
        }
    }
    CHECK(region_rasterizer.clipped_triangle_count == 0u);
    CHECK(region_rasterizer.rasterized_triangle_count == 0u);
    CHECK(depth_buffers_match_f32(
        region_depth,
        full_depth,
        pixel_count,
        0x1p-10f
    ));
    for (pixel = (size_t)64u * WIDTH + 64u;
         pixel < pixel_count;
         ++pixel) {
        if (float_bits(region_depth[pixel]) != clear_bits) {
            ++tail_covered;
        }
    }
    CHECK(tail_covered > 0u);

    CHECK(soc_rasterizer_end_frame(&region_rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &region_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    for (tile_y = 0u; tile_y < HEIGHT; tile_y += 32u) {
        uint32_t tile_x;

        for (tile_x = 0u; tile_x < WIDTH; tile_x += 32u) {
            region.minimum_x = tile_x;
            region.minimum_y = tile_y;
            region.end_x = tile_x + 32u < WIDTH
                ? tile_x + 32u
                : WIDTH;
            region.end_y = tile_y + 32u < HEIGHT
                ? tile_y + 32u
                : HEIGHT;
            soc_rasterizer_rasterize_prepared_region_unchecked(
                &region_rasterizer,
                &prepared.data[0],
                &region
            );
        }
    }
    CHECK(region_rasterizer.clipped_triangle_count == 0u);
    CHECK(region_rasterizer.rasterized_triangle_count == 0u);
    CHECK(depth_buffers_match_f32(
        region_depth,
        full_depth,
        pixel_count,
        0x1p-10f
    ));

    invalid_region = prepared.data[0].bounds;
    invalid_region.minimum_x = invalid_region.end_x + 1u;
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &prepared.data[0],
        &invalid_region
    ) == SOC_RESULT_INVALID_ARGUMENT);
    invalid_prepared = prepared.data[0];
    invalid_prepared.bounds.end_x = WIDTH + 1u;
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &invalid_prepared,
        &region
    ) == SOC_RESULT_INVALID_ARGUMENT);
    region.minimum_x = 5u;
    region.minimum_y = 3u;
    region.end_x = 5u;
    region.end_y = 17u;
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &prepared.data[0],
        &region
    ) == SOC_RESULT_OK);
    CHECK(depth_buffers_match_f32(
        region_depth,
        full_depth,
        pixel_count,
        0x1p-10f
    ));

    CHECK(soc_rasterizer_end_frame(&region_rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &region_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    region.minimum_x = 0u;
    region.minimum_y = 0u;
    region.end_x = 32u;
    region.end_y = 32u;
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &prepared.data[0],
        &region
    ) == SOC_RESULT_OK);
    for (pixel = 0u; pixel < pixel_count; ++pixel) {
        const uint32_t x = (uint32_t)(pixel % WIDTH);
        const uint32_t y = (uint32_t)(pixel / WIDTH);

        if (x < 32u && y < 32u) {
            if (float_bits(region_depth[pixel]) != clear_bits) {
                ++fragment_covered;
            }
        } else {
            CHECK(float_bits(region_depth[pixel]) == clear_bits);
        }
    }
    CHECK(fragment_covered > 0u);
    CHECK(region_rasterizer.clipped_triangle_count == 0u);
    CHECK(region_rasterizer.rasterized_triangle_count == 0u);

    CHECK(soc_rasterizer_end_frame(&region_rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &region_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    region = prepared.data[0].bounds;
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &prepared.data[0],
        &region
    ) == SOC_RESULT_OK);
    CHECK(depth_buffers_match_f32(
        region_depth,
        full_depth,
        pixel_count,
        0x1p-10f
    ));
    CHECK(soc_rasterizer_end_frame(&region_rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_rasterize_prepared_region(
        &region_rasterizer,
        &prepared.data[0],
        &region
    ) == SOC_RESULT_INVALID_STATE);

    CHECK(soc_rasterizer_end_frame(&full_rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&region_rasterizer);
    soc_rasterizer_shutdown(&full_rasterizer);
    soc_raster_prepared_list_shutdown(&prepared);
    free(region_depth);
    free(full_depth);
    return 0;
}

static int test_prepared_region_replay(void)
{
    CHECK(check_prepared_region_replay() == 0);
    return 0;
}

static int test_masked_prepared_region_replay(void)
{
    enum {
        WIDTH = 67,
        HEIGHT = 65,
        SUBTILE_COLUMN_COUNT = (WIDTH + 7) / 8,
        SUBTILE_ROW_COUNT = (HEIGHT + 3) / 4,
        SUBTILE_COUNT = SUBTILE_COLUMN_COUNT * SUBTILE_ROW_COUNT,
    };
    static const screen_vertex triangles[3][3] = {
        {
            {1.0, 1.0, 0.34f},
            {66.8, 64.9, 0.34f},
            {1.0, 64.9, 0.34f},
        },
        {
            {66.8, 1.0, 0.79f},
            {66.8, 64.9, 0.79f},
            {1.0, 1.0, 0.79f},
        },
        {
            {0.5, 33.2, 0.23f},
            {66.6, 30.8, 0.88f},
            {33.1, 64.9, 0.51f},
        },
    };
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    uint32_t indices[] = {
        0u, 1u, 2u,
        3u, 4u, 5u,
        6u, 7u, 8u,
    };
    float positions[27];
    float full_z0[SUBTILE_COUNT];
    float full_z1[SUBTILE_COUNT];
    uint32_t full_masks[SUBTILE_COUNT];
    float region_z0[SUBTILE_COUNT];
    float region_z1[SUBTILE_COUNT];
    uint32_t region_masks[SUBTILE_COUNT];
    soc_mesh mesh;
    soc_raster_prepared_list prepared = {0};
    soc_rasterizer full_rasterizer;
    soc_rasterizer region_rasterizer;
    uint32_t triangle;
    uint32_t tile_y;

    for (triangle = 0u; triangle < 3u; ++triangle) {
        write_triangle(
            positions + (size_t)triangle * 9u,
            WIDTH,
            HEIGHT,
            triangles[triangle],
            SOC_CLIP_DEPTH_ZERO_TO_ONE
        );
    }
    mesh = make_mesh(
        positions,
        9u,
        indices,
        (uint32_t)ARRAY_COUNT(indices)
    );

    CHECK(soc_rasterizer_initialize_masked(
        &full_rasterizer,
        WIDTH,
        HEIGHT,
        full_z0,
        full_z1,
        full_masks,
        SUBTILE_COLUMN_COUNT,
        SUBTILE_ROW_COUNT,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &full_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &full_rasterizer,
        &mesh,
        &identity,
        0u,
        3u,
        &prepared
    ) == SOC_RESULT_OK);
    CHECK(prepared.count == 3u);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &full_rasterizer,
        prepared.data,
        prepared.count
    ) == SOC_RESULT_OK);

    CHECK(soc_rasterizer_initialize_masked(
        &region_rasterizer,
        WIDTH,
        HEIGHT,
        region_z0,
        region_z1,
        region_masks,
        SUBTILE_COLUMN_COUNT,
        SUBTILE_ROW_COUNT,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &region_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    region_rasterizer.masked_reference_count = SIZE_MAX;

    for (tile_y = 0u; tile_y < HEIGHT; tile_y += 32u) {
        uint32_t tile_x;

        for (tile_x = 0u; tile_x < WIDTH; tile_x += 32u) {
            const soc_raster_prepared_region region = {
                .minimum_x = tile_x,
                .minimum_y = tile_y,
                .end_x = tile_x + 32u < WIDTH
                    ? tile_x + 32u
                    : WIDTH,
                .end_y = tile_y + 32u < HEIGHT
                    ? tile_y + 32u
                    : HEIGHT,
            };
            size_t prepared_index;

            for (prepared_index = 0u;
                 prepared_index < prepared.count;
                 ++prepared_index) {
                soc_rasterizer_rasterize_prepared_region_unchecked(
                    &region_rasterizer,
                    &prepared.data[prepared_index],
                    &region
                );
            }
        }
    }

    CHECK(float_buffers_match_f32(
        full_z0,
        region_z0,
        ARRAY_COUNT(full_z0),
        0x1p-10f
    ));
    CHECK(float_buffers_match_f32(
        full_z1,
        region_z1,
        ARRAY_COUNT(full_z1),
        0x1p-10f
    ));
    CHECK(memcmp(full_masks, region_masks, sizeof(full_masks)) == 0);
    CHECK(full_z0[SUBTILE_COUNT - 1u] >= 0.0f ||
        full_masks[SUBTILE_COUNT - 1u] != 0u);

    CHECK(soc_rasterizer_end_frame(&region_rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_end_frame(&full_rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&region_rasterizer);
    soc_rasterizer_shutdown(&full_rasterizer);
    soc_raster_prepared_list_shutdown(&prepared);
    return 0;
}

static int test_masked_equal_depth_does_not_create_working_layer(void)
{
    enum {
        WIDTH = 8,
        HEIGHT = 4,
    };
    static const screen_vertex triangles[3][3] = {
        {
            {0.0, 0.0, 0.625f},
            {8.0, 0.0, 0.625f},
            {8.0, 4.0, 0.625f},
        },
        {
            {0.0, 0.0, 0.625f},
            {8.0, 4.0, 0.625f},
            {0.0, 4.0, 0.625f},
        },
        {
            {0.0, 0.0, 0.625f},
            {4.0, 0.0, 0.625f},
            {0.0, 2.0, 0.625f},
        },
    };
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    uint32_t indices[] = {
        0u, 1u, 2u,
        3u, 4u, 5u,
        6u, 7u, 8u,
    };
    float positions[27];
    float summary_z0[1];
    float summary_z1[1];
    uint32_t summary_mask[1];
    float no_summary_z0[1];
    float no_summary_z1[1];
    uint32_t no_summary_mask[1];
    float established_z0;
    soc_mesh mesh;
    soc_raster_prepared_list prepared = {0};
    soc_rasterizer summary_rasterizer;
    soc_rasterizer no_summary_rasterizer;
    uint32_t triangle;

    for (triangle = 0u; triangle < 3u; ++triangle) {
        write_triangle(
            positions + (size_t)triangle * 9u,
            WIDTH,
            HEIGHT,
            triangles[triangle],
            SOC_CLIP_DEPTH_ZERO_TO_ONE
        );
    }
    mesh = make_mesh(
        positions,
        9u,
        indices,
        (uint32_t)ARRAY_COUNT(indices)
    );

    CHECK(soc_rasterizer_initialize_masked(
        &summary_rasterizer,
        WIDTH,
        HEIGHT,
        summary_z0,
        summary_z1,
        summary_mask,
        1u,
        1u,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_initialize_masked(
        &no_summary_rasterizer,
        WIDTH,
        HEIGHT,
        no_summary_z0,
        no_summary_z1,
        no_summary_mask,
        1u,
        1u,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &summary_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &no_summary_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    no_summary_rasterizer.masked_reference_count = SIZE_MAX;
    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &summary_rasterizer,
        &mesh,
        &identity,
        0u,
        3u,
        &prepared
    ) == SOC_RESULT_OK);
    CHECK(prepared.count == 3u);
    CHECK(prepared.data[0].depth_step_x == 0.0);
    CHECK(prepared.data[0].depth_step_y == 0.0);
    CHECK(prepared.data[1].depth_sample_origin ==
        prepared.data[0].depth_sample_origin);
    CHECK(prepared.data[2].depth_sample_origin ==
        prepared.data[0].depth_sample_origin);

    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &summary_rasterizer,
        prepared.data,
        2u
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &no_summary_rasterizer,
        prepared.data,
        2u
    ) == SOC_RESULT_OK);
    CHECK(summary_z0[0] >= 0.0f);
    CHECK(float_bits(summary_z1[0]) == float_bits(FLT_MAX));
    CHECK(summary_mask[0] == 0u);
    CHECK(float_buffers_match_f32(
        summary_z0,
        no_summary_z0,
        ARRAY_COUNT(summary_z0),
        0x1p-10f
    ));
    CHECK(float_buffers_match_f32(
        summary_z1,
        no_summary_z1,
        ARRAY_COUNT(summary_z1),
        0x1p-10f
    ));
    CHECK(memcmp(summary_mask, no_summary_mask, sizeof(summary_mask)) == 0);
    established_z0 = summary_z0[0];

    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &summary_rasterizer,
        &prepared.data[2],
        1u
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &no_summary_rasterizer,
        &prepared.data[2],
        1u
    ) == SOC_RESULT_OK);
    CHECK(float_bits(summary_z0[0]) == float_bits(established_z0));
    CHECK(float_bits(no_summary_z0[0]) == float_bits(established_z0));
    CHECK(float_bits(summary_z1[0]) == float_bits(FLT_MAX));
    CHECK(float_bits(no_summary_z1[0]) == float_bits(FLT_MAX));
    CHECK(summary_mask[0] == 0u);
    CHECK(no_summary_mask[0] == 0u);
    CHECK(float_buffers_match_f32(
        summary_z0,
        no_summary_z0,
        ARRAY_COUNT(summary_z0),
        0x1p-10f
    ));
    CHECK(float_buffers_match_f32(
        summary_z1,
        no_summary_z1,
        ARRAY_COUNT(summary_z1),
        0x1p-10f
    ));
    CHECK(memcmp(summary_mask, no_summary_mask, sizeof(summary_mask)) == 0);

    CHECK(soc_rasterizer_end_frame(
        &no_summary_rasterizer
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_end_frame(&summary_rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&no_summary_rasterizer);
    soc_rasterizer_shutdown(&summary_rasterizer);
    soc_raster_prepared_list_shutdown(&prepared);
    return 0;
}

static int check_prepared_target_replay(void)
{
    enum {
        WIDTH = 67,
        HEIGHT = 65,
        LOCAL_WIDTH = 32,
        LOCAL_HEIGHT = 32,
        LOCAL_STRIDE = 35,
        LOCAL_ELEMENT_COUNT = LOCAL_STRIDE * LOCAL_HEIGHT,
        LOCAL_REQUIRED_COUNT =
            (LOCAL_HEIGHT - 1) * LOCAL_STRIDE + LOCAL_WIDTH,
        LOCAL_STORAGE_COUNT =
            LOCAL_ELEMENT_COUNT + CANARY_WORD_COUNT * 2u,
        LOCAL_EARLY_Z_COLUMN_COUNT =
            LOCAL_WIDTH / SOC_KERNEL_RASTER_BLOCK_SIZE,
        LOCAL_EARLY_Z_ROW_COUNT =
            LOCAL_HEIGHT / SOC_KERNEL_RASTER_BLOCK_SIZE,
        LOCAL_EARLY_Z_BLOCK_COUNT =
            LOCAL_EARLY_Z_COLUMN_COUNT * LOCAL_EARLY_Z_ROW_COUNT,
    };
    static const screen_vertex constant_triangle[3] = {
        {1.0, 1.0, 0.62f},
        {66.8, 64.9, 0.62f},
        {1.0, 64.9, 0.62f},
    };
    static const screen_vertex varying_triangle[3] = {
        {1.0, 1.0, 0.18f},
        {66.8, 64.9, 0.82f},
        {1.0, 64.9, 0.41f},
    };
    const size_t pixel_count = (size_t)WIDTH * HEIGHT;
    const size_t depth_bytes = pixel_count * sizeof(float);
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const float clear_depth = 0.0f;
    const uint32_t canary_bits = UINT32_C(0x7fc12345);
    const float canary = float_from_bits(canary_bits);
    uint32_t indices[] = {0u, 1u, 2u, 3u, 4u, 5u};
    float positions[18];
    float* full_depth = malloc(depth_bytes);
    float* assembled_depth = malloc(depth_bytes);
    float local_storage[LOCAL_STORAGE_COUNT];
    float* local_depth = local_storage + CANARY_WORD_COUNT;
    soc_mesh mesh;
    soc_raster_prepared_list prepared = {0};
    soc_rasterizer full_rasterizer;
    soc_rasterizer target_rasterizer;
    soc_raster_prepared_region region;
    soc_raster_target target;
    soc_raster_target invalid_target;
    float local_early_z_farthest_depths[LOCAL_EARLY_Z_BLOCK_COUNT];
    uint64_t local_early_z_pending_masks[LOCAL_EARLY_Z_BLOCK_COUNT];
    size_t local_index;
    uint32_t tile_y;

    CHECK(full_depth != NULL);
    CHECK(assembled_depth != NULL);
    write_triangle(
        positions,
        WIDTH,
        HEIGHT,
        constant_triangle,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    write_triangle(
        positions + 9u,
        WIDTH,
        HEIGHT,
        varying_triangle,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    mesh = make_mesh(
        positions,
        6u,
        indices,
        (uint32_t)ARRAY_COUNT(indices)
    );

    CHECK(soc_rasterizer_initialize(
        &full_rasterizer,
        WIDTH,
        HEIGHT,
        full_depth,
        pixel_count,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &full_rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_prepare_occluder_triangles(
        &full_rasterizer,
        &mesh,
        &identity,
        0u,
        2u,
        &prepared
    ) == SOC_RESULT_OK);
    CHECK(prepared.count == 2u);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &full_rasterizer,
        prepared.data,
        prepared.count
    ) == SOC_RESULT_OK);

    CHECK(soc_rasterizer_initialize(
        &target_rasterizer,
        WIDTH,
        HEIGHT,
        assembled_depth,
        pixel_count,
        soc_kernel_table_scalar()
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(
        &target_rasterizer,
        &frame
    ) == SOC_RESULT_OK);

    memset(&target, 0, sizeof(target));
    target.depth = local_depth;
    target.row_stride = LOCAL_STRIDE;
    target.element_count = LOCAL_ELEMENT_COUNT;
    target.width = LOCAL_WIDTH;
    target.height = LOCAL_HEIGHT;
    target.early_z_farthest_depths = local_early_z_farthest_depths;
    target.early_z_pending_masks = local_early_z_pending_masks;
    target.early_z_block_count = LOCAL_EARLY_Z_BLOCK_COUNT;
    target.early_z_column_count = LOCAL_EARLY_Z_COLUMN_COUNT;
    for (tile_y = 0u; tile_y < HEIGHT; tile_y += LOCAL_HEIGHT) {
        uint32_t tile_x;

        for (tile_x = 0u; tile_x < WIDTH; tile_x += LOCAL_WIDTH) {
            uint32_t local_y;
            size_t prepared_index;

            for (local_index = 0u;
                 local_index < LOCAL_STORAGE_COUNT;
                 ++local_index) {
                local_storage[local_index] = canary;
            }
            for (local_y = 0u; local_y < LOCAL_HEIGHT; ++local_y) {
                uint32_t local_x;

                for (local_x = 0u; local_x < LOCAL_WIDTH; ++local_x) {
                    local_depth[(size_t)local_y * LOCAL_STRIDE + local_x] =
                        clear_depth;
                }
            }

            region.minimum_x = tile_x;
            region.minimum_y = tile_y;
            region.end_x = tile_x + LOCAL_WIDTH < WIDTH
                ? tile_x + LOCAL_WIDTH
                : WIDTH;
            region.end_y = tile_y + LOCAL_HEIGHT < HEIGHT
                ? tile_y + LOCAL_HEIGHT
                : HEIGHT;
            target.origin_x = tile_x;
            target.origin_y = tile_y;
            soc_raster_target_reset_early_z_unchecked(&target);
            for (prepared_index = 0u;
                 prepared_index < prepared.count;
                 ++prepared_index) {
                CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
                    &target_rasterizer,
                    &prepared.data[prepared_index],
                    &region,
                    &target
                ) == SOC_RESULT_OK);
            }

            for (local_index = 0u;
                 local_index < CANARY_WORD_COUNT;
                 ++local_index) {
                CHECK(float_bits(local_storage[local_index]) == canary_bits);
                CHECK(float_bits(local_storage[
                    CANARY_WORD_COUNT + LOCAL_ELEMENT_COUNT + local_index
                ]) == canary_bits);
            }
            for (local_y = 0u; local_y < LOCAL_HEIGHT; ++local_y) {
                uint32_t padding_x;

                for (padding_x = LOCAL_WIDTH;
                     padding_x < LOCAL_STRIDE;
                     ++padding_x) {
                    CHECK(float_bits(local_depth[
                        (size_t)local_y * LOCAL_STRIDE + padding_x
                    ]) == canary_bits);
                }
            }
            for (local_y = 0u;
                 local_y < region.end_y - region.minimum_y;
                 ++local_y) {
                uint32_t local_x;

                for (local_x = 0u;
                     local_x < region.end_x - region.minimum_x;
                     ++local_x) {
                    assembled_depth[
                        (size_t)(region.minimum_y + local_y) * WIDTH +
                            region.minimum_x + local_x
                    ] = local_depth[
                        (size_t)local_y * LOCAL_STRIDE + local_x
                    ];
                }
            }
        }
    }

    CHECK(depth_buffers_match_f32(
        assembled_depth,
        full_depth,
        pixel_count,
        0x1p-10f
    ));
    CHECK(target_rasterizer.clipped_triangle_count == 0u);
    CHECK(target_rasterizer.rasterized_triangle_count == 0u);

    for (tile_y = 0u; tile_y < HEIGHT; tile_y += LOCAL_HEIGHT) {
        uint32_t tile_x;

        for (tile_x = 0u; tile_x < WIDTH; tile_x += LOCAL_WIDTH) {
            uint32_t local_y;
            size_t prepared_index;

            for (local_index = 0u;
                 local_index < LOCAL_STORAGE_COUNT;
                 ++local_index) {
                local_storage[local_index] = canary;
            }
            for (local_y = 0u; local_y < LOCAL_HEIGHT; ++local_y) {
                uint32_t local_x;

                for (local_x = 0u; local_x < LOCAL_WIDTH; ++local_x) {
                    local_depth[(size_t)local_y * LOCAL_STRIDE + local_x] =
                        clear_depth;
                }
            }

            region.minimum_x = tile_x;
            region.minimum_y = tile_y;
            region.end_x = tile_x + LOCAL_WIDTH < WIDTH
                ? tile_x + LOCAL_WIDTH
                : WIDTH;
            region.end_y = tile_y + LOCAL_HEIGHT < HEIGHT
                ? tile_y + LOCAL_HEIGHT
                : HEIGHT;
            target.origin_x = tile_x;
            target.origin_y = tile_y;
            soc_raster_target_reset_early_z_unchecked(&target);
            for (prepared_index = 0u;
                 prepared_index < prepared.count;
                 ++prepared_index) {
                soc_rasterizer_rasterize_prepared_region_to_target_unchecked(
                    &target_rasterizer,
                    &prepared.data[prepared_index],
                    &region,
                    &target
                );
            }

            for (local_index = 0u;
                 local_index < CANARY_WORD_COUNT;
                 ++local_index) {
                CHECK(float_bits(local_storage[local_index]) == canary_bits);
                CHECK(float_bits(local_storage[
                    CANARY_WORD_COUNT + LOCAL_ELEMENT_COUNT + local_index
                ]) == canary_bits);
            }
            for (local_y = 0u; local_y < LOCAL_HEIGHT; ++local_y) {
                uint32_t padding_x;

                for (padding_x = LOCAL_WIDTH;
                     padding_x < LOCAL_STRIDE;
                     ++padding_x) {
                    CHECK(float_bits(local_depth[
                        (size_t)local_y * LOCAL_STRIDE + padding_x
                    ]) == canary_bits);
                }
            }
            for (local_y = 0u;
                 local_y < region.end_y - region.minimum_y;
                 ++local_y) {
                uint32_t local_x;

                for (local_x = 0u;
                     local_x < region.end_x - region.minimum_x;
                     ++local_x) {
                    assembled_depth[
                        (size_t)(region.minimum_y + local_y) * WIDTH +
                            region.minimum_x + local_x
                    ] = local_depth[
                        (size_t)local_y * LOCAL_STRIDE + local_x
                    ];
                }
            }
        }
    }

    CHECK(depth_buffers_match_f32(
        assembled_depth,
        full_depth,
        pixel_count,
        0x1p-10f
    ));
    CHECK(target_rasterizer.clipped_triangle_count == 0u);
    CHECK(target_rasterizer.rasterized_triangle_count == 0u);

    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        NULL,
        &prepared.data[0],
        &region,
        &target
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        NULL
    ) == SOC_RESULT_INVALID_ARGUMENT);

    invalid_target = target;
    invalid_target.depth = NULL;
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        &invalid_target
    ) == SOC_RESULT_INVALID_ARGUMENT);
    invalid_target = target;
    invalid_target.row_stride = LOCAL_WIDTH - 1u;
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        &invalid_target
    ) == SOC_RESULT_INVALID_ARGUMENT);
    invalid_target = target;
    invalid_target.element_count = LOCAL_REQUIRED_COUNT - 1u;
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        &invalid_target
    ) == SOC_RESULT_INVALID_ARGUMENT);
    invalid_target = target;
    invalid_target.origin_x = region.minimum_x + 1u;
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        &invalid_target
    ) == SOC_RESULT_INVALID_ARGUMENT);
    invalid_target = target;
    invalid_target.origin_x = UINT32_MAX - 15u;
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        &invalid_target
    ) == SOC_RESULT_INVALID_ARGUMENT);
    invalid_target = target;
    invalid_target.origin_x = 0u;
    invalid_target.origin_y = 0u;
    invalid_target.width = 1u;
    invalid_target.height = 3u;
    invalid_target.row_stride = SIZE_MAX / 2u + 1u;
    invalid_target.element_count = SIZE_MAX;
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        &invalid_target
    ) == SOC_RESULT_INVALID_ARGUMENT);

    CHECK(depth_buffers_match_f32(
        assembled_depth,
        full_depth,
        pixel_count,
        0x1p-10f
    ));
    CHECK(soc_rasterizer_end_frame(&target_rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &target_rasterizer,
        &prepared.data[0],
        &region,
        &target
    ) == SOC_RESULT_INVALID_STATE);
    CHECK(soc_rasterizer_end_frame(&full_rasterizer) == SOC_RESULT_OK);

    soc_rasterizer_shutdown(&target_rasterizer);
    soc_rasterizer_shutdown(&full_rasterizer);
    soc_raster_prepared_list_shutdown(&prepared);
    free(assembled_depth);
    free(full_depth);
    return 0;
}

static int test_prepared_target_replay(void)
{
    CHECK(check_prepared_target_replay() == 0);
    return 0;
}

static soc_raster_prepared_triangle make_full_early_z_prepared_triangle(
    float depth
)
{
    soc_raster_prepared_triangle prepared;
    uint32_t edge;

    memset(&prepared, 0, sizeof(prepared));
    for (edge = 0u; edge < 3u; ++edge) {
        prepared.edges[edge].sample_origin = 1;
    }
    prepared.bounds.end_x = 16u;
    prepared.bounds.end_y = 16u;
    prepared.depth_sample_origin = depth;
    prepared.end_tile_column = 1u;
    prepared.end_tile_row = 1u;
    return prepared;
}

static soc_raster_prepared_triangle make_sized_full_early_z_prepared_triangle(
    uint32_t width,
    uint32_t height,
    float depth
)
{
    soc_raster_prepared_triangle prepared =
        make_full_early_z_prepared_triangle(depth);

    prepared.bounds.end_x = width;
    prepared.bounds.end_y = height;
    prepared.end_tile_column = (uint16_t)(
        (width - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u
    );
    prepared.end_tile_row = (uint16_t)(
        (height - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u
    );
    return prepared;
}

static soc_raster_prepared_triangle make_full_early_z_target_triangle(
    uint32_t origin_x,
    uint32_t origin_y,
    uint32_t width,
    uint32_t height,
    float depth
)
{
    soc_raster_prepared_triangle prepared =
        make_sized_full_early_z_prepared_triangle(width, height, depth);

    prepared.bounds.minimum_x = origin_x;
    prepared.bounds.minimum_y = origin_y;
    prepared.bounds.end_x = origin_x + width;
    prepared.bounds.end_y = origin_y + height;
    prepared.first_tile_column =
        (uint16_t)(origin_x / SOC_RASTER_LOCK_TILE_SIZE);
    prepared.first_tile_row =
        (uint16_t)(origin_y / SOC_RASTER_LOCK_TILE_SIZE);
    prepared.end_tile_column = (uint16_t)(
        (prepared.bounds.end_x - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u
    );
    prepared.end_tile_row = (uint16_t)(
        (prepared.bounds.end_y - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u
    );
    return prepared;
}

static int check_prepared_target_block_early_z(void)
{
    enum {
        FRAMEBUFFER_WIDTH = 24,
        FRAMEBUFFER_HEIGHT = 24,
        FRAMEBUFFER_PIXEL_COUNT =
            FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT,
        TARGET_ORIGIN_X = 8,
        TARGET_ORIGIN_Y = 8,
        TARGET_WIDTH = 9,
        TARGET_HEIGHT = 9,
        TARGET_ROW_STRIDE = 12,
        TARGET_ELEMENT_COUNT = TARGET_ROW_STRIDE * TARGET_HEIGHT,
        TARGET_STORAGE_COUNT =
            CANARY_WORD_COUNT + TARGET_ELEMENT_COUNT + CANARY_WORD_COUNT,
        TARGET_BLOCK_COLUMN_COUNT = 2,
        TARGET_BLOCK_ROW_COUNT = 2,
        TARGET_BLOCK_COUNT =
            TARGET_BLOCK_COLUMN_COUNT * TARGET_BLOCK_ROW_COUNT,
        EARLY_Z_STORAGE_COUNT =
            CANARY_WORD_COUNT + TARGET_BLOCK_COUNT + CANARY_WORD_COUNT,
    };
    const uint32_t canary_bits = UINT32_C(0x7fc12345);
    const float canary = float_from_bits(canary_bits);
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const float clear_depth = 0.0f;
    const float untouched_depth = -1.0f;
    const float seed_depth = 0.75f;
    const float farther_depth = 0.25f;
    const float nearer_depth = 0.875f;
    const soc_raster_prepared_triangle seed =
        make_full_early_z_target_triangle(
            TARGET_ORIGIN_X,
            TARGET_ORIGIN_Y,
            TARGET_WIDTH,
            TARGET_HEIGHT,
            seed_depth
        );
    const soc_raster_prepared_triangle farther =
        make_full_early_z_target_triangle(
            TARGET_ORIGIN_X,
            TARGET_ORIGIN_Y,
            TARGET_WIDTH,
            TARGET_HEIGHT,
            farther_depth
        );
    const soc_raster_prepared_triangle nearer =
        make_full_early_z_target_triangle(
            TARGET_ORIGIN_X,
            TARGET_ORIGIN_Y,
            TARGET_WIDTH,
            TARGET_HEIGHT,
            nearer_depth
        );
    const soc_raster_prepared_region region = {
        TARGET_ORIGIN_X,
        TARGET_ORIGIN_Y,
        TARGET_ORIGIN_X + TARGET_WIDTH,
        TARGET_ORIGIN_Y + TARGET_HEIGHT,
    };
    soc_kernel_table kernels = make_counting_scalar_kernel_table();
    soc_rasterizer rasterizer;
    soc_raster_target target;
    float framebuffer_depth[FRAMEBUFFER_PIXEL_COUNT];
    float target_storage[TARGET_STORAGE_COUNT];
    float seed_snapshot[TARGET_STORAGE_COUNT];
    float farthest_depth_storage[EARLY_Z_STORAGE_COUNT];
    uint64_t pending_mask_storage[EARLY_Z_STORAGE_COUNT];
    float* target_depth = target_storage + CANARY_WORD_COUNT;
    float* early_z_farthest_depths =
        farthest_depth_storage + CANARY_WORD_COUNT;
    uint64_t* early_z_pending_masks =
        pending_mask_storage + CANARY_WORD_COUNT;
    size_t store_calls_before;
    size_t index;
    uint32_t y;

    for (index = 0u; index < TARGET_STORAGE_COUNT; ++index) {
        target_storage[index] = canary;
    }
    for (y = 0u; y < TARGET_HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < TARGET_WIDTH; ++x) {
            target_depth[(size_t)y * TARGET_ROW_STRIDE + x] = clear_depth;
        }
    }
    for (index = 0u; index < EARLY_Z_STORAGE_COUNT; ++index) {
        farthest_depth_storage[index] = canary;
        pending_mask_storage[index] = UINT64_C(0xa5a5a5a5a5a5a5a5);
    }

    memset(&target, 0, sizeof(target));
    target.depth = target_depth;
    target.row_stride = TARGET_ROW_STRIDE;
    target.element_count = TARGET_ELEMENT_COUNT;
    target.origin_x = TARGET_ORIGIN_X;
    target.origin_y = TARGET_ORIGIN_Y;
    target.width = TARGET_WIDTH;
    target.height = TARGET_HEIGHT;
    target.early_z_farthest_depths = early_z_farthest_depths;
    target.early_z_pending_masks = early_z_pending_masks;
    target.early_z_block_count = TARGET_BLOCK_COUNT;
    target.early_z_column_count = TARGET_BLOCK_COLUMN_COUNT;
    soc_raster_target_reset_early_z_unchecked(&target);
    for (index = 0u; index < TARGET_BLOCK_COUNT; ++index) {
        CHECK(float_bits(early_z_farthest_depths[index]) ==
            float_bits(untouched_depth));
    }

    counted_depth_block_store_calls = 0u;
    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        FRAMEBUFFER_WIDTH,
        FRAMEBUFFER_HEIGHT,
        framebuffer_depth,
        FRAMEBUFFER_PIXEL_COUNT,
        &kernels
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);

    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &rasterizer,
        &seed,
        &region,
        &target
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == TARGET_BLOCK_COUNT);
    for (y = 0u; y < TARGET_HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < TARGET_WIDTH; ++x) {
            CHECK(float_bits(target_depth[
                (size_t)y * TARGET_ROW_STRIDE + x
            ]) == float_bits(target_depth[0]));
            CHECK(float_bits(target_depth[
                (size_t)y * TARGET_ROW_STRIDE + x
            ]) != float_bits(clear_depth));
        }
    }
    for (index = 0u; index < TARGET_BLOCK_COUNT; ++index) {
        CHECK(early_z_pending_masks[index] == 0u);
        CHECK(float_bits(early_z_farthest_depths[index]) !=
            float_bits(untouched_depth));
        CHECK(early_z_farthest_depths[index] <= target_depth[0]);
    }
    memcpy(seed_snapshot, target_storage, sizeof(seed_snapshot));

    store_calls_before = counted_depth_block_store_calls;
    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &rasterizer,
        &seed,
        &region,
        &target
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == store_calls_before);
    CHECK(memcmp(target_storage, seed_snapshot, sizeof(seed_snapshot)) == 0);

    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &rasterizer,
        &farther,
        &region,
        &target
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == store_calls_before);
    CHECK(memcmp(target_storage, seed_snapshot, sizeof(seed_snapshot)) == 0);

    CHECK(soc_rasterizer_rasterize_prepared_region_to_target(
        &rasterizer,
        &nearer,
        &region,
        &target
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls ==
        store_calls_before + TARGET_BLOCK_COUNT);
    for (y = 0u; y < TARGET_HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < TARGET_WIDTH; ++x) {
            const size_t local_index =
                (size_t)y * TARGET_ROW_STRIDE + x;

            CHECK(target_depth[local_index] > seed_snapshot[
                CANARY_WORD_COUNT + local_index
            ]);
            CHECK(float_bits(target_depth[local_index]) ==
                float_bits(target_depth[0]));
        }
    }
    for (index = 0u; index < TARGET_BLOCK_COUNT; ++index) {
        CHECK(early_z_pending_masks[index] == 0u);
        CHECK(early_z_farthest_depths[index] <= target_depth[0]);
    }
    for (index = 0u; index < CANARY_WORD_COUNT; ++index) {
        CHECK(float_bits(target_storage[index]) == canary_bits);
        CHECK(float_bits(target_storage[
            CANARY_WORD_COUNT + TARGET_ELEMENT_COUNT + index
        ]) == canary_bits);
        CHECK(float_bits(farthest_depth_storage[index]) == canary_bits);
        CHECK(pending_mask_storage[index] ==
            UINT64_C(0xa5a5a5a5a5a5a5a5));
        CHECK(float_bits(farthest_depth_storage[
            CANARY_WORD_COUNT + TARGET_BLOCK_COUNT + index
        ]) == canary_bits);
        CHECK(pending_mask_storage[
            CANARY_WORD_COUNT + TARGET_BLOCK_COUNT + index
        ] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    }
    for (y = 0u; y < TARGET_HEIGHT; ++y) {
        uint32_t x;

        for (x = TARGET_WIDTH; x < TARGET_ROW_STRIDE; ++x) {
            CHECK(float_bits(target_depth[
                (size_t)y * TARGET_ROW_STRIDE + x
            ]) == canary_bits);
        }
    }

    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);
    return 0;
}

static int test_prepared_target_block_early_z(void)
{
    CHECK(check_prepared_target_block_early_z() == 0);
    return 0;
}

static soc_raster_prepared_triangle make_full_early_z_prepared_plane(
    uint32_t width,
    uint32_t height,
    float depth_origin,
    float depth_step_x,
    float depth_step_y
)
{
    soc_raster_prepared_triangle prepared =
        make_sized_full_early_z_prepared_triangle(
            width,
            height,
            depth_origin
        );

    prepared.depth_step_x = depth_step_x;
    prepared.depth_step_y = depth_step_y;
    return prepared;
}

static int check_prepared_plane_early_z_behavior(void)
{
    enum {
        WIDTH = 24,
        HEIGHT = 24,
        PIXEL_COUNT = WIDTH * HEIGHT,
        BLOCK_COUNT = 9,
    };
    const float stored_depth = 0.5f;
    const float behind_origin = 0.40f;
    const float nearer_origin = 0.60f;
    const float depth_step = 0.001f;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const soc_raster_prepared_triangle seed =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            stored_depth
        );
    soc_kernel_table kernels = make_counting_scalar_kernel_table();
    soc_rasterizer rasterizer;
    soc_raster_prepared_triangle behind_candidate;
    soc_raster_prepared_triangle nearer_candidate;
    float depth[PIXEL_COUNT];
    size_t store_calls_before;
    uint32_t y;

    counted_depth_block_store_calls = 0u;
    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        WIDTH,
        HEIGHT,
        depth,
        PIXEL_COUNT,
        &kernels
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &seed,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == BLOCK_COUNT);

    behind_candidate = make_full_early_z_prepared_plane(
        WIDTH,
        HEIGHT,
        behind_origin,
        depth_step,
        depth_step
    );
    nearer_candidate = make_full_early_z_prepared_plane(
        WIDTH,
        HEIGHT,
        nearer_origin,
        -depth_step,
        -depth_step
    );

    store_calls_before = counted_depth_block_store_calls;
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &behind_candidate,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == store_calls_before);
    for (y = 0u; y < HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < WIDTH; ++x) {
            CHECK(absolute_float(
                depth[(size_t)y * WIDTH + x] - stored_depth
            ) <= 0x1p-20f);
        }
    }

    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &nearer_candidate,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls ==
        store_calls_before + BLOCK_COUNT);
    for (y = 0u; y < HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < WIDTH; ++x) {
            const float actual = depth[(size_t)y * WIDTH + x];
            const float expected = nearer_origin -
                depth_step * (float)x - depth_step * (float)y;

            CHECK(actual >= 0.0f);
            CHECK(actual <= 1.0f);
            CHECK(actual > stored_depth);
            CHECK(absolute_float(actual - expected) <= 0x1p-18f);
        }
    }

    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);
    return 0;
}

static int test_prepared_plane_early_z_behavior(void)
{
    CHECK(check_prepared_plane_early_z_behavior() == 0);
    return 0;
}

static int check_coarse_rebase_crosses_early_z_boundary(void)
{
    enum {
        WIDTH = 32,
        HEIGHT = 32,
        PIXEL_COUNT = WIDTH * HEIGHT,
    };
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const float depth_origin = 0.43f;
    const float depth_step_x = 0.01f;
    const float summary = 0.5f;
    const soc_raster_prepared_region probe_region = {
        7u, 0u, 9u, 1u,
    };
    soc_kernel_table kernels = make_counting_scalar_kernel_table();
    soc_rasterizer rasterizer;
    soc_raster_prepared_triangle prepared;
    const soc_raster_prepared_triangle allocation_seed =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            0.5f
        );
    float depth[PIXEL_COUNT];
    size_t index;
    uint32_t edge;

    memset(&prepared, 0, sizeof(prepared));
    for (edge = 0u; edge < 3u; ++edge) {
        prepared.edges[edge].sample_origin = 1;
    }
    prepared.bounds.minimum_x = 0u;
    prepared.bounds.minimum_y = 0u;
    prepared.bounds.end_x = WIDTH;
    prepared.bounds.end_y = HEIGHT;
    prepared.depth_sample_origin = depth_origin;
    prepared.depth_step_x = depth_step_x;
    prepared.depth_step_y = 0.0f;
    prepared.first_tile_column = 0u;
    prepared.first_tile_row = 0u;
    prepared.end_tile_column = 1u;
    prepared.end_tile_row = 1u;

    counted_depth_block_store_calls = 0u;
    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        WIDTH,
        HEIGHT,
        depth,
        PIXEL_COUNT,
        &kernels
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);
    CHECK(rasterizer.early_z_block_count == 16u);
    CHECK(rasterizer.early_z_tile_count == 1u);
    CHECK(rasterizer.early_z_pending_masks == NULL);
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &allocation_seed,
        1u
    ) == SOC_RESULT_OK);
    CHECK(rasterizer.early_z_pending_masks != NULL);
    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);
    counted_depth_block_store_calls = 0u;
    for (index = 0u; index < PIXEL_COUNT; ++index) {
        depth[index] = summary;
    }
    for (index = 0u;
         index < rasterizer.early_z_block_count;
         ++index) {
        rasterizer.early_z_farthest_depths[index] = summary;
        rasterizer.early_z_pending_masks[index] = 0u;
    }
    rasterizer.early_z_tile_farthest_depths[0] = summary;
    rasterizer.early_z_frame_farthest_depth = summary;
    rasterizer.early_z_ready_tile_count = rasterizer.early_z_tile_count;

    CHECK(soc_rasterizer_rasterize_prepared_region(
        &rasterizer,
        &prepared,
        &probe_region
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls != 0u);
    CHECK(absolute_float(depth[7] - summary) <= 0x1p-20f);
    CHECK(depth[8] > summary);
    CHECK(depth[8] >= 0.0f);
    CHECK(depth[8] <= 1.0f);
    CHECK(absolute_float(
        depth[8] - (depth_origin + 8.0f * depth_step_x)
    ) <= 0x1p-18f);

    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);
    return 0;
}

static int test_coarse_rebase_crosses_early_z_boundary(void)
{
    CHECK(check_coarse_rebase_crosses_early_z_boundary() == 0);
    return 0;
}

static int check_block_early_z_state_lifecycle(void)
{
    enum {
        WIDTH = 24,
        HEIGHT = 24,
        PIXEL_COUNT = WIDTH * HEIGHT,
        BLOCK_COUNT = 9,
    };
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const float clear_depth = 0.0f;
    const float first_depth = 0.75f;
    const float farther_depth = 0.25f;
    const float closer_depth = 0.875f;
    const float no_clear_stored_depth = 0.05f;
    const float no_clear_candidate_depth = 0.125f;
    const soc_raster_prepared_triangle first =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            first_depth
        );
    const soc_raster_prepared_triangle farther =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            farther_depth
        );
    const soc_raster_prepared_triangle closer =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            closer_depth
        );
    const soc_raster_prepared_triangle no_clear_candidate =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            no_clear_candidate_depth
        );
    soc_kernel_table kernels = make_counting_scalar_kernel_table();
    soc_rasterizer rasterizer;
    float depth[PIXEL_COUNT];
    float first_result[PIXEL_COUNT];
    size_t store_calls_before;
    size_t pixel;

    counted_depth_block_store_calls = 0u;
    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        WIDTH,
        HEIGHT,
        depth,
        PIXEL_COUNT,
        &kernels
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);

    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &first,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == BLOCK_COUNT);
    memcpy(first_result, depth, sizeof(first_result));
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        CHECK(float_bits(depth[pixel]) == float_bits(depth[0]));
        CHECK(float_bits(depth[pixel]) != float_bits(clear_depth));
    }

    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &first,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == BLOCK_COUNT);
    CHECK(memcmp(depth, first_result, sizeof(depth)) == 0);

    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &farther,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == BLOCK_COUNT);
    CHECK(memcmp(depth, first_result, sizeof(depth)) == 0);

    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &closer,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == BLOCK_COUNT * 2u);
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        CHECK(float_bits(depth[pixel]) == float_bits(depth[0]));
        CHECK(depth[pixel] > first_result[pixel]);
    }

    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        CHECK(float_bits(depth[pixel]) == float_bits(clear_depth));
    }
    store_calls_before = counted_depth_block_store_calls;
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &farther,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls ==
        store_calls_before + BLOCK_COUNT);
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        CHECK(float_bits(depth[pixel]) != float_bits(clear_depth));
    }

    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        depth[pixel] = no_clear_stored_depth;
    }
    CHECK(soc_rasterizer_begin_frame_no_clear(
        &rasterizer,
        &frame
    ) == SOC_RESULT_OK);
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        CHECK(float_bits(depth[pixel]) ==
            float_bits(no_clear_stored_depth));
    }
    store_calls_before = counted_depth_block_store_calls;
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &no_clear_candidate,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls ==
        store_calls_before + BLOCK_COUNT);
    for (pixel = 0u; pixel < PIXEL_COUNT; ++pixel) {
        CHECK(float_bits(depth[pixel]) == float_bits(depth[0]));
        CHECK(depth[pixel] > no_clear_stored_depth);
    }

    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);
    return 0;
}

static int test_block_early_z_state_lifecycle(void)
{
    CHECK(check_block_early_z_state_lifecycle() == 0);
    return 0;
}

static int check_partial_block_pending_union(void)
{
    enum {
        WIDTH = 8,
        HEIGHT = 8,
        PIXEL_COUNT = WIDTH * HEIGHT,
    };
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const float clear_depth = 0.0f;
    const float untouched_depth = -1.0f;
    const float near_depth = 0.75f;
    const float farther_depth = 0.25f;
    const soc_raster_prepared_triangle near =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            near_depth
        );
    const soc_raster_prepared_triangle farther =
        make_sized_full_early_z_prepared_triangle(
            WIDTH,
            HEIGHT,
            farther_depth
        );
    const soc_raster_prepared_region left_region = {
        0u, 0u, WIDTH / 2u, HEIGHT,
    };
    const soc_raster_prepared_region right_region = {
        WIDTH / 2u, 0u, WIDTH, HEIGHT,
    };
    soc_kernel_table kernels = make_counting_scalar_kernel_table();
    soc_rasterizer rasterizer;
    float depth[PIXEL_COUNT];
    float filled_depth[PIXEL_COUNT];
    size_t store_calls_before;
    uint32_t y;

    counted_depth_block_store_calls = 0u;
    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        WIDTH,
        HEIGHT,
        depth,
        PIXEL_COUNT,
        &kernels
    ) == SOC_RESULT_OK);
    CHECK(soc_rasterizer_begin_frame(&rasterizer, &frame) == SOC_RESULT_OK);
    CHECK(rasterizer.early_z_block_count == 1u);
    CHECK(float_bits(rasterizer.early_z_farthest_depths[0]) ==
        float_bits(untouched_depth));

    CHECK(soc_rasterizer_rasterize_prepared_region(
        &rasterizer,
        &near,
        &left_region
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == 1u);
    CHECK(float_bits(rasterizer.early_z_farthest_depths[0]) ==
        float_bits(clear_depth));
    CHECK(rasterizer.early_z_pending_masks[0] != 0u);
    for (y = 0u; y < HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < WIDTH; ++x) {
            const float stored = depth[(size_t)y * WIDTH + x];

            if (x < WIDTH / 2u) {
                CHECK(float_bits(stored) != float_bits(clear_depth));
            } else {
                CHECK(float_bits(stored) == float_bits(clear_depth));
            }
        }
    }

    CHECK(soc_rasterizer_rasterize_prepared_region(
        &rasterizer,
        &near,
        &right_region
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == 2u);
    CHECK(rasterizer.early_z_pending_masks[0] == 0u);
    CHECK(float_bits(rasterizer.early_z_farthest_depths[0]) !=
        float_bits(clear_depth));
    for (y = 0u; y < HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < WIDTH; ++x) {
            CHECK(float_bits(depth[(size_t)y * WIDTH + x]) ==
                float_bits(depth[0]));
        }
    }
    CHECK(rasterizer.early_z_farthest_depths[0] <= depth[0]);
    memcpy(filled_depth, depth, sizeof(filled_depth));

    store_calls_before = counted_depth_block_store_calls;
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        &rasterizer,
        &farther,
        1u
    ) == SOC_RESULT_OK);
    CHECK(counted_depth_block_store_calls == store_calls_before);
    CHECK(memcmp(depth, filled_depth, sizeof(depth)) == 0);

    CHECK(soc_rasterizer_end_frame(&rasterizer) == SOC_RESULT_OK);
    soc_rasterizer_shutdown(&rasterizer);
    return 0;
}

static int test_partial_block_pending_union(void)
{
    CHECK(check_partial_block_pending_union() == 0);
    return 0;
}

static int rasterize_resized_early_z_surface(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* frame,
    uint32_t width,
    uint32_t height,
    uint32_t expected_block_columns,
    uint32_t expected_block_rows,
    float source_depth,
    float* storage,
    size_t storage_count
)
{
    static const uint32_t canary_bits = UINT32_C(0x7fc12345);
    const float canary = float_from_bits(canary_bits);
    const float clear_depth = 0.0f;
    const float untouched_depth = -1.0f;
    const size_t pixel_count = (size_t)width * height;
    const size_t expected_block_count =
        (size_t)expected_block_columns * expected_block_rows;
    const soc_raster_prepared_triangle prepared =
        make_sized_full_early_z_prepared_triangle(
            width,
            height,
            source_depth
        );
    float* depth = storage + CANARY_WORD_COUNT;
    size_t store_calls_before;
    size_t index;

    CHECK(storage_count ==
        CANARY_WORD_COUNT + pixel_count + CANARY_WORD_COUNT);
    for (index = 0u; index < storage_count; ++index) {
        storage[index] = canary;
    }

    CHECK(rasterizer->frame_active == SOC_FALSE);
    CHECK(soc_rasterizer_resize(
        rasterizer,
        width,
        height,
        depth,
        pixel_count
    ) == SOC_RESULT_OK);
    CHECK(rasterizer->width == width);
    CHECK(rasterizer->height == height);
    CHECK(rasterizer->depth == depth);
    CHECK(rasterizer->depth_element_count == pixel_count);
    CHECK(rasterizer->block_column_count == expected_block_columns);
    CHECK(rasterizer->block_row_count == expected_block_rows);
    CHECK(rasterizer->early_z_block_count == expected_block_count);
    CHECK(rasterizer->early_z_storage != NULL);
    CHECK(rasterizer->early_z_farthest_depths != NULL);

    CHECK(soc_rasterizer_begin_frame(rasterizer, frame) == SOC_RESULT_OK);
    for (index = 0u; index < pixel_count; ++index) {
        CHECK(float_bits(depth[index]) == float_bits(clear_depth));
    }
    for (index = 0u; index < expected_block_count; ++index) {
        CHECK(float_bits(rasterizer->early_z_farthest_depths[index]) ==
            float_bits(untouched_depth));
    }

    store_calls_before = counted_depth_block_store_calls;
    CHECK(soc_rasterizer_rasterize_prepared_triangles(
        rasterizer,
        &prepared,
        1u
    ) == SOC_RESULT_OK);
    CHECK(rasterizer->early_z_pending_masks != NULL);
    CHECK(counted_depth_block_store_calls ==
        store_calls_before + expected_block_count);
    for (index = 0u; index < pixel_count; ++index) {
        CHECK(float_bits(depth[index]) == float_bits(depth[0]));
        CHECK(float_bits(depth[index]) != float_bits(clear_depth));
    }
    for (index = 0u; index < expected_block_count; ++index) {
        CHECK(rasterizer->early_z_pending_masks[index] == 0u);
        CHECK(rasterizer->early_z_farthest_depths[index] <= depth[0]);
    }
    for (index = 0u; index < CANARY_WORD_COUNT; ++index) {
        CHECK(float_bits(storage[index]) == canary_bits);
        CHECK(float_bits(storage[
            CANARY_WORD_COUNT + pixel_count + index
        ]) == canary_bits);
    }

    CHECK(soc_rasterizer_end_frame(rasterizer) == SOC_RESULT_OK);
    return 0;
}

static int check_rasterizer_resize_early_z_lifecycle(void)
{
    enum {
        INITIAL_WIDTH = 16,
        INITIAL_HEIGHT = 16,
        INITIAL_PIXEL_COUNT = INITIAL_WIDTH * INITIAL_HEIGHT,
        FIRST_WIDTH = 9,
        FIRST_HEIGHT = 17,
        FIRST_PIXEL_COUNT = FIRST_WIDTH * FIRST_HEIGHT,
        FIRST_STORAGE_COUNT =
            CANARY_WORD_COUNT + FIRST_PIXEL_COUNT + CANARY_WORD_COUNT,
        SECOND_WIDTH = 17,
        SECOND_HEIGHT = 1,
        SECOND_PIXEL_COUNT = SECOND_WIDTH * SECOND_HEIGHT,
        SECOND_STORAGE_COUNT =
            CANARY_WORD_COUNT + SECOND_PIXEL_COUNT + CANARY_WORD_COUNT,
    };
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const float source_depth = 0.65f;
    soc_kernel_table kernels = make_counting_scalar_kernel_table();
    soc_rasterizer rasterizer;
    float initial_depth[INITIAL_PIXEL_COUNT];
    float first_storage[FIRST_STORAGE_COUNT];
    float second_storage[SECOND_STORAGE_COUNT];

    counted_depth_block_store_calls = 0u;
    CHECK(soc_rasterizer_initialize(
        &rasterizer,
        INITIAL_WIDTH,
        INITIAL_HEIGHT,
        initial_depth,
        INITIAL_PIXEL_COUNT,
        &kernels
    ) == SOC_RESULT_OK);
    CHECK(rasterizer.block_column_count == 2u);
    CHECK(rasterizer.block_row_count == 2u);
    CHECK(rasterizer.early_z_block_count == 4u);
    CHECK(rasterizer.early_z_storage != NULL);
    CHECK(rasterizer.early_z_farthest_depths != NULL);
    CHECK(rasterizer.early_z_pending_masks == NULL);

    CHECK(rasterize_resized_early_z_surface(
        &rasterizer,
        &frame,
        FIRST_WIDTH,
        FIRST_HEIGHT,
        2u,
        3u,
        source_depth,
        first_storage,
        ARRAY_COUNT(first_storage)
    ) == 0);
    CHECK(rasterize_resized_early_z_surface(
        &rasterizer,
        &frame,
        SECOND_WIDTH,
        SECOND_HEIGHT,
        3u,
        1u,
        source_depth,
        second_storage,
        ARRAY_COUNT(second_storage)
    ) == 0);

    soc_rasterizer_shutdown(&rasterizer);
    CHECK(rasterizer.early_z_storage == NULL);
    CHECK(rasterizer.early_z_farthest_depths == NULL);
    CHECK(rasterizer.early_z_pending_masks == NULL);
    CHECK(rasterizer.early_z_block_count == 0u);
    CHECK(rasterizer.early_z_tile_farthest_depths == NULL);
    CHECK(rasterizer.early_z_tile_count == 0u);
    CHECK(rasterizer.initialized == SOC_FALSE);
    return 0;
}

static int test_rasterizer_resize_early_z_lifecycle(void)
{
    CHECK(check_rasterizer_resize_early_z_lifecycle() == 0);
    return 0;
}

static int check_shared_tile_locks_match_serial(
    soc_bool crosses_tile_boundary
)
{
    enum {
        WIDTH = 67,
        HEIGHT = 65,
        RASTERIZER_COUNT = 2,
        SUBMISSION_COUNT = 64,
    };
    static const screen_vertex crossing_triangle0[3] = {
        {29.0, 29.0, 0.72f},
        {36.0, 30.0, 0.72f},
        {30.0, 36.0, 0.72f},
    };
    static const screen_vertex crossing_triangle1[3] = {
        {35.0, 35.0, 0.31f},
        {28.0, 34.0, 0.31f},
        {34.0, 28.0, 0.31f},
    };
    /* Both small constant triangles stay in tile [32,64)x[32,64). */
    static const screen_vertex single_tile_triangle0[3] = {
        {56.0, 56.0, 0.72f},
        {63.0, 57.0, 0.72f},
        {57.0, 63.0, 0.72f},
    };
    static const screen_vertex single_tile_triangle1[3] = {
        {62.0, 62.0, 0.31f},
        {55.0, 61.0, 0.31f},
        {61.0, 55.0, 0.31f},
    };
    const screen_vertex* triangle0 = crosses_tile_boundary == SOC_TRUE
        ? crossing_triangle0
        : single_tile_triangle0;
    const screen_vertex* triangle1 = crosses_tile_boundary == SOC_TRUE
        ? crossing_triangle1
        : single_tile_triangle1;
    const size_t pixel_count = (size_t)WIDTH * HEIGHT;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    const float clear_depth = 0.0f;
    uint32_t indices[3] = {0u, 1u, 2u};
    float positions[RASTERIZER_COUNT][9];
    soc_mesh meshes[RASTERIZER_COUNT];
    const soc_mesh* serial_meshes[RASTERIZER_COUNT];
    raster_capture serial_capture;
    float* shared_depth;
    soc_rasterizer rasterizers[RASTERIZER_COUNT];
    soc_raster_tile_locks tile_locks;
    soc_thread_pool thread_pool;
    locked_raster_state state;
    size_t pixel;
    size_t lock_index;
    uint32_t rasterizer_index;

    write_triangle(
        positions[0],
        WIDTH,
        HEIGHT,
        triangle0,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    write_triangle(
        positions[1],
        WIDTH,
        HEIGHT,
        triangle1,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    for (rasterizer_index = 0u;
         rasterizer_index < RASTERIZER_COUNT;
         ++rasterizer_index) {
        meshes[rasterizer_index] = make_mesh(
            positions[rasterizer_index],
            3u,
            indices,
            (uint32_t)ARRAY_COUNT(indices)
        );
        serial_meshes[rasterizer_index] = &meshes[rasterizer_index];
    }
    CHECK(run_mesh_sequence(
        serial_meshes,
        RASTERIZER_COUNT,
        &frame,
        WIDTH,
        HEIGHT,
        &serial_capture
    ) == 0);

    shared_depth = malloc(pixel_count * sizeof(*shared_depth));
    CHECK(shared_depth != NULL);
    CHECK(soc_raster_tile_locks_initialize(
        &tile_locks,
        WIDTH,
        HEIGHT
    ) == SOC_RESULT_OK);
    CHECK(tile_locks.column_count == 3u);
    CHECK(tile_locks.row_count == 3u);
    CHECK(tile_locks.lock_count == 9u);
    CHECK(soc_thread_pool_initialize(
        &thread_pool,
        RASTERIZER_COUNT
    ) == SOC_RESULT_OK);

    for (pixel = 0u; pixel < pixel_count; ++pixel) {
        shared_depth[pixel] = clear_depth;
    }
    shared_depth[pixel_count - 1u] = 0.456f;
    for (rasterizer_index = 0u;
         rasterizer_index < RASTERIZER_COUNT;
         ++rasterizer_index) {
        CHECK(soc_rasterizer_initialize(
            &rasterizers[rasterizer_index],
            WIDTH,
            HEIGHT,
            shared_depth,
            pixel_count,
            soc_kernel_table_scalar()
        ) == SOC_RESULT_OK);
        CHECK(soc_rasterizer_configure_tile_locks(
            &rasterizers[rasterizer_index],
            &tile_locks
        ) == SOC_RESULT_OK);
        CHECK(soc_rasterizer_begin_frame_no_clear(
            &rasterizers[rasterizer_index],
            &frame
        ) == SOC_RESULT_OK);
    }
    CHECK(float_bits(shared_depth[pixel_count - 1u]) ==
        float_bits(0.456f));
    shared_depth[pixel_count - 1u] = clear_depth;

    memset(&state, 0, sizeof(state));
    state.rasterizers = rasterizers;
    state.meshes = meshes;
    state.object_to_world = identity_matrix();
    state.submission_count = SUBMISSION_COUNT;
    state.results[0] = SOC_RESULT_INTERNAL_ERROR;
    state.results[1] = SOC_RESULT_INTERNAL_ERROR;
    soc_thread_pool_run(
        &thread_pool,
        submit_locked_meshes,
        &state
    );

    for (rasterizer_index = 0u;
         rasterizer_index < RASTERIZER_COUNT;
         ++rasterizer_index) {
        CHECK(state.results[rasterizer_index] == SOC_RESULT_OK);
        CHECK(rasterizers[rasterizer_index].clipped_triangle_count == 0u);
        CHECK(rasterizers[rasterizer_index].rasterized_triangle_count ==
            SUBMISSION_COUNT);
        CHECK(soc_rasterizer_end_frame(
            &rasterizers[rasterizer_index]
        ) == SOC_RESULT_OK);
        soc_rasterizer_shutdown(&rasterizers[rasterizer_index]);
    }
    for (lock_index = 0u;
         lock_index < tile_locks.lock_count;
         ++lock_index) {
        CHECK(atomic_load_explicit(
            &tile_locks.locks[lock_index],
            memory_order_relaxed
        ) == 0u);
    }
    CHECK(depth_buffers_match_f32(
        shared_depth,
        serial_capture.depth,
        pixel_count,
        0x1p-10f
    ));

    soc_thread_pool_shutdown(&thread_pool);
    soc_raster_tile_locks_shutdown(&tile_locks);
    free(shared_depth);
    release_capture(&serial_capture);
    return 0;
}

static int test_shared_tile_locks_and_no_clear_begin(void)
{
    soc_raster_tile_locks invalid_locks;

    CHECK(soc_raster_tile_locks_initialize(
        NULL,
        1u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(soc_raster_tile_locks_initialize(
        &invalid_locks,
        0u,
        1u
    ) == SOC_RESULT_INVALID_ARGUMENT);
    CHECK(invalid_locks.locks == NULL);
    CHECK(check_shared_tile_locks_match_serial(
        SOC_TRUE
    ) == 0);
    CHECK(check_shared_tile_locks_match_serial(
        SOC_FALSE
    ) == 0);
    return 0;
}

int main(void)
{
    if (test_compact_prepared_edges_are_integer_exact() != 0) {
        return 1;
    }
    if (test_q8_snapped_coverage_and_depth_match_f32_math() != 0) {
        return 1;
    }
    if (test_fixed_top_left_coverage_mask() != 0) {
        return 1;
    }
    if (test_shared_edge_masks_are_a_partition() != 0) {
        return 1;
    }
    if (test_non_lattice_shared_edges_have_no_cracks() != 0) {
        return 1;
    }
    if (test_q8_collapsed_triangle_is_discarded() != 0) {
        return 1;
    }
    if (test_clipped_polygon_fans_have_no_cracks() != 0) {
        return 1;
    }
    if (test_varying_depth_matches_f32_math() != 0) {
        return 1;
    }
    if (test_depth_ranges_and_submission_orders() != 0) {
        return 1;
    }
    if (test_odd_block_tails_and_narrow_canaries() != 0) {
        return 1;
    }
    if (test_triangle_range_submission_matches_full_mesh() != 0) {
        return 1;
    }
    if (test_triangle_range_submission_validates_bounds() != 0) {
        return 1;
    }
    if (test_prepared_replay_matches_immediate() != 0) {
        return 1;
    }
    if (test_prepared_list_and_invalid_state_semantics() != 0) {
        return 1;
    }
    if (test_prepared_region_replay() != 0) {
        return 1;
    }
    if (test_masked_prepared_region_replay() != 0) {
        return 1;
    }
    if (test_masked_equal_depth_does_not_create_working_layer() != 0) {
        return 1;
    }
    if (test_prepared_target_replay() != 0) {
        return 1;
    }
    if (test_block_early_z_state_lifecycle() != 0) {
        return 1;
    }
    if (test_partial_block_pending_union() != 0) {
        return 1;
    }
    if (test_prepared_target_block_early_z() != 0) {
        return 1;
    }
    if (test_rasterizer_resize_early_z_lifecycle() != 0) {
        return 1;
    }
    if (test_prepared_plane_early_z_behavior() != 0) {
        return 1;
    }
    if (test_coarse_rebase_crosses_early_z_boundary() != 0) {
        return 1;
    }
    if (test_shared_tile_locks_and_no_clear_begin() != 0) {
        return 1;
    }
    return 0;
}
