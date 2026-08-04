#include "core/soc_mesh.h"
#include "raster/soc_rasterizer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define CANARY_WORD_COUNT 8u
#define MAX_CANARY_PIXEL_COUNT (19u * 17u)

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
    soc_clip_depth_range clip_depth_range,
    soc_depth_direction depth_direction
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
        .depth_direction = depth_direction,
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

static float clear_depth_for(soc_depth_direction depth_direction)
{
    return depth_direction == SOC_DEPTH_REVERSED ? 0.0f : 1.0f;
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
        pixel_count
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
    const raster_capture* capture,
    soc_depth_direction depth_direction
)
{
    const uint32_t clear_bits = float_bits(
        clear_depth_for(depth_direction)
    );
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

static int test_non_lattice_coverage_and_depth_are_conservative(void)
{
    enum {
        WIDTH = 31,
        HEIGHT = 23,
        TRIANGLE_COUNT = 64,
    };
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_DEPTH_FORWARD
    );
    const uint32_t clear_bits = float_bits(1.0f);
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
                uint32_t edge;

                if (float_bits(stored_depth) == clear_bits) {
                    continue;
                }
                for (edge = 0u; edge < 3u; ++edge) {
                    CHECK(continuous_edge_contains_sample(
                        &reconstructed[(edge + 1u) % 3u],
                        &reconstructed[(edge + 2u) % 3u],
                        (double)x + 0.5,
                        (double)y + 0.5
                    ));
                }
                {
                    const double point_x = (double)x + 0.5;
                    const double point_y = (double)y + 0.5;
                    const double area = continuous_edge_value(
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
                    const double exact_depth = (
                        weight0 * reconstructed[0].depth +
                        weight1 * reconstructed[1].depth +
                        weight2 * reconstructed[2].depth
                    ) / area;

                    CHECK((double)stored_depth >= exact_depth);
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
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_DEPTH_FORWARD
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

    mask = capture_coverage_mask_8x8(&capture, SOC_DEPTH_FORWARD);
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
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_DEPTH_FORWARD
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
    first_mask = capture_coverage_mask_8x8(
        &first_capture,
        SOC_DEPTH_FORWARD
    );
    second_mask = capture_coverage_mask_8x8(
        &second_capture,
        SOC_DEPTH_FORWARD
    );
    release_capture(&first_capture);
    release_capture(&second_capture);

    CHECK((first_mask & second_mask) == 0u);
    CHECK((first_mask | second_mask) == expected_union);
    return 0;
}

static int check_varying_depth_direction(
    soc_depth_direction depth_direction
)
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
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        depth_direction
    );
    const uint32_t clear_bits = float_bits(
        clear_depth_for(depth_direction)
    );
    raster_capture capture;
    uint32_t covered_count = 0u;
    uint32_t one_ulp_count = 0u;
    uint32_t two_ulp_count = 0u;
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
            double exact;
            float nearest;
            uint32_t nearest_bits;
            uint32_t ulps;

            if (stored_bits == clear_bits) {
                continue;
            }

            exact = 0.125 +
                0.5 * ((double)x + 0.5 - 1.0) / 12.0 +
                0.75 * ((double)y + 0.5 - 1.0) / 12.0;
            nearest = (float)exact;
            nearest_bits = float_bits(nearest);
            if (depth_direction == SOC_DEPTH_REVERSED) {
                CHECK((double)stored <= exact);
                CHECK(stored_bits <= nearest_bits);
                ulps = nearest_bits - stored_bits;
            } else {
                CHECK((double)stored >= exact);
                CHECK(stored_bits >= nearest_bits);
                ulps = stored_bits - nearest_bits;
            }
            CHECK(ulps >= 1u);
            CHECK(ulps <= 2u);
            if (ulps == 1u) {
                ++one_ulp_count;
            } else {
                ++two_ulp_count;
            }
            ++covered_count;
        }
    }

    release_capture(&capture);
    CHECK(covered_count > 32u);
    CHECK(one_ulp_count > 0u);
    CHECK(two_ulp_count > 0u);
    return 0;
}

static int test_varying_depth_is_guarded_in_depth_direction(void)
{
    CHECK(check_varying_depth_direction(SOC_DEPTH_FORWARD) == 0);
    CHECK(check_varying_depth_direction(SOC_DEPTH_REVERSED) == 0);
    return 0;
}

static int check_ill_conditioned_depth_uses_far_vertex(
    soc_depth_direction depth_direction
)
{
    /*
     * The Q8 determinant is only 3 while its two product terms are large.
     * Pixel (32, 32) is the exact centroid and has edge value 1 on all three
     * sides, so coverage remains valid while affine slope setup is rejected
     * as ill-conditioned.
     */
    static const screen_vertex vertices[3] = {
        {2319.0 / 256.0, 2321.0 / 256.0, 0.2375114858f},
        {11320.0 / 256.0, 11319.0 / 256.0, 0.6312448382f},
        {11321.0 / 256.0, 11320.0 / 256.0, 0.6312448382f},
    };
    uint32_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_mesh mesh;
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        depth_direction
    );
    const float farthest_depth =
        depth_direction == SOC_DEPTH_REVERSED
            ? vertices[0].depth
            : vertices[1].depth;
    const uint32_t farthest_bits = float_bits(farthest_depth);
    raster_capture capture;
    uint32_t stored_bits;
    uint32_t ulps;

    write_triangle(
        positions,
        64u,
        64u,
        vertices,
        SOC_CLIP_DEPTH_ZERO_TO_ONE
    );
    mesh = make_mesh(positions, 3u, indices, 3u);
    CHECK(run_one_mesh(&mesh, &frame, 64u, 64u, &capture) == 0);
    CHECK(capture.rasterized_triangle_count == 1u);

    stored_bits = float_bits(capture.depth[32u * 64u + 32u]);
    if (depth_direction == SOC_DEPTH_REVERSED) {
        CHECK(stored_bits < farthest_bits);
        ulps = farthest_bits - stored_bits;
    } else {
        CHECK(stored_bits > farthest_bits);
        ulps = stored_bits - farthest_bits;
    }
    release_capture(&capture);
    CHECK(ulps >= 1u);
    CHECK(ulps <= 2u);
    return 0;
}

static int test_ill_conditioned_depth_is_conservative(void)
{
    CHECK(check_ill_conditioned_depth_uses_far_vertex(
        SOC_DEPTH_FORWARD
    ) == 0);
    CHECK(check_ill_conditioned_depth_uses_far_vertex(
        SOC_DEPTH_REVERSED
    ) == 0);
    return 0;
}

static int check_depth_range_direction_and_submission_order(
    soc_clip_depth_range clip_depth_range,
    soc_depth_direction depth_direction
)
{
    static const double triangle_xy[6] = {
        1.0, 1.0,
        15.0, 1.0,
        1.0, 15.0,
    };
    const float winning_depth =
        depth_direction == SOC_DEPTH_REVERSED ? 0.75f : 0.25f;
    const float losing_depth =
        depth_direction == SOC_DEPTH_REVERSED ? 0.25f : 0.75f;
    const uint32_t clear_bits = float_bits(
        clear_depth_for(depth_direction)
    );
    const uint32_t winning_bits = float_bits(winning_depth);
    uint32_t indices[] = {0u, 1u, 2u};
    float winning_positions[9];
    float losing_positions[9];
    screen_vertex winning_vertices[3];
    screen_vertex losing_vertices[3];
    soc_mesh winning_mesh;
    soc_mesh losing_mesh;
    const soc_mesh* losing_then_winning[2];
    const soc_mesh* winning_then_losing[2];
    const soc_frame_desc frame = make_frame_desc(
        clip_depth_range,
        depth_direction
    );
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
        const uint32_t first_bits = float_bits(first_capture.depth[pixel]);
        const uint32_t second_bits = float_bits(second_capture.depth[pixel]);

        CHECK(first_bits == second_bits);
        if (first_bits != clear_bits) {
            uint32_t ulps;

            if (depth_direction == SOC_DEPTH_REVERSED) {
                CHECK(first_bits < winning_bits);
                ulps = winning_bits - first_bits;
            } else {
                CHECK(first_bits > winning_bits);
                ulps = first_bits - winning_bits;
            }
            CHECK(ulps >= 1u);
            CHECK(ulps <= 2u);
            ++covered_count;
        }
    }
    release_capture(&first_capture);
    release_capture(&second_capture);
    CHECK(covered_count > 64u);
    return 0;
}

static int test_depth_ranges_directions_and_submission_orders(void)
{
    static const soc_clip_depth_range clip_depth_ranges[] = {
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
    };
    static const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
    };
    size_t range_index;

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        size_t direction_index;

        for (direction_index = 0u;
             direction_index < ARRAY_COUNT(depth_directions);
             ++direction_index) {
            CHECK(check_depth_range_direction_and_submission_order(
                clip_depth_ranges[range_index],
                depth_directions[direction_index]
            ) == 0);
        }
    }
    return 0;
}

static int check_canary_dimensions(
    uint32_t width,
    uint32_t height,
    soc_depth_direction depth_direction
)
{
    static const uint32_t canary_bits = UINT32_C(0x7fc12345);
    const size_t pixel_count = (size_t)width * height;
    const float clear_depth = clear_depth_for(depth_direction);
    const uint32_t clear_bits = float_bits(clear_depth);
    const uint32_t source_depth_bits = float_bits(0.5f);
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc(
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        depth_direction
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
        pixel_count
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
            uint32_t ulps;

            if (depth_direction == SOC_DEPTH_REVERSED) {
                CHECK(bits < source_depth_bits);
                ulps = source_depth_bits - bits;
            } else {
                CHECK(bits > source_depth_bits);
                ulps = bits - source_depth_bits;
            }
            CHECK(ulps >= 1u);
            CHECK(ulps <= 2u);
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
    static const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
    };
    size_t dimension_index;

    for (dimension_index = 0u;
         dimension_index < ARRAY_COUNT(dimensions);
         ++dimension_index) {
        size_t direction_index;

        for (direction_index = 0u;
             direction_index < ARRAY_COUNT(depth_directions);
             ++direction_index) {
            CHECK(check_canary_dimensions(
                dimensions[dimension_index][0],
                dimensions[dimension_index][1],
                depth_directions[direction_index]
            ) == 0);
        }
    }
    return 0;
}

int main(void)
{
    if (test_non_lattice_coverage_and_depth_are_conservative() != 0) {
        return 1;
    }
    if (test_fixed_top_left_coverage_mask() != 0) {
        return 1;
    }
    if (test_shared_edge_masks_are_a_partition() != 0) {
        return 1;
    }
    if (test_varying_depth_is_guarded_in_depth_direction() != 0) {
        return 1;
    }
    if (test_ill_conditioned_depth_is_conservative() != 0) {
        return 1;
    }
    if (test_depth_ranges_directions_and_submission_orders() != 0) {
        return 1;
    }
    if (test_odd_block_tails_and_narrow_canaries() != 0) {
        return 1;
    }
    return 0;
}
