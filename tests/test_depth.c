#include <soc/soc.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 8u
#define TEST_HEIGHT 8u
#define TEST_PIXEL_COUNT (TEST_WIDTH * TEST_HEIGHT)
#define DEPTH_EPSILON 0.000001f
#define DEPTH_SENTINEL (-123.0f)

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

#define CHECK_RESULT(expression, expected) \
    do { \
        const soc_result actual_result = (expression); \
        const soc_result expected_result = (expected); \
        if (actual_result != expected_result) { \
            fprintf( \
                stderr, \
                "%s:%d: result was %d, expected %d: %s\n", \
                __FILE__, \
                __LINE__, \
                (int)actual_result, \
                (int)expected_result, \
                #expression \
            ); \
            return 1; \
        } \
    } while (0)

typedef struct frame_capture {
    soc_hiz_level_info level;
    soc_build_stats stats;
    float depth[TEST_PIXEL_COUNT];
} frame_capture;

static int depth_equal(float left, float right)
{
    float difference = left - right;

    if (difference < 0.0f) {
        difference = -difference;
    }
    return difference <= DEPTH_EPSILON;
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

static soc_mat4 z_translation_matrix(float translation)
{
    soc_mat4 matrix = identity_matrix();

    matrix.col3.z = translation;
    return matrix;
}

static soc_frame_desc make_frame_desc(soc_depth_direction depth_direction)
{
    const soc_frame_desc desc = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
            .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
        },
        .clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE,
        .depth_direction = depth_direction,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    return desc;
}

static soc_result create_context(
    uint32_t width,
    uint32_t height,
    soc_context** out_context
)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = width,
        .height = height,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    return soc_context_create(&config, out_context);
}

static soc_result create_triangle_mesh(
    soc_context* context,
    const float* positions,
    const uint16_t* indices,
    uint32_t flags,
    soc_mesh** out_mesh
)
{
    const soc_mesh_desc desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = flags,
        .vertices = positions,
        .indices = indices,
        .vertex_count = 3u,
        .vertex_stride = 3u * sizeof(float),
        .position_offset = 0u,
        .index_count = 3u,
        .index_type = SOC_INDEX_UINT16,
    };
    return soc_mesh_create(context, &desc, out_mesh);
}

static void make_triangle(float depth, float* out_positions)
{
    const float positions[] = {
        -0.75f, -0.75f, depth,
         0.75f, -0.75f, depth,
         0.00f,  0.75f, depth,
    };
    memcpy(out_positions, positions, sizeof(positions));
}

static void make_oversized_triangle(float depth, float* out_positions)
{
    const float positions[] = {
        -1.0f, -1.0f, depth,
         3.0f, -1.0f, depth,
        -1.0f,  3.0f, depth,
    };
    memcpy(out_positions, positions, sizeof(positions));
}

static soc_result capture_frame_with_desc(
    soc_context* context,
    const soc_frame_desc* frame_desc,
    soc_mesh* const* meshes,
    const soc_mat4* const* object_to_world_arrays,
    const uint32_t* instance_counts,
    uint32_t submission_count,
    frame_capture* out_capture
)
{
    const soc_mat4 identity = identity_matrix();
    soc_occluder_group* groups = NULL;
    soc_occlusion_build_desc build_desc;
    soc_snapshot* snapshot = NULL;
    soc_result result;
    uint32_t submission_index;
    uint32_t pixel;

    memset(out_capture, 0, sizeof(*out_capture));
    out_capture->level.struct_size = sizeof(out_capture->level);
    out_capture->stats.struct_size = sizeof(out_capture->stats);
    for (pixel = 0u; pixel < TEST_PIXEL_COUNT; ++pixel) {
        out_capture->depth[pixel] = DEPTH_SENTINEL;
    }

    if (submission_count != 0u) {
        groups = calloc(submission_count, sizeof(*groups));
        if (groups == NULL) {
            return SOC_RESULT_OUT_OF_MEMORY;
        }
    }

    for (submission_index = 0u;
         submission_index < submission_count;
         ++submission_index) {
        const soc_mat4* object_to_world = object_to_world_arrays == NULL
            ? &identity
            : object_to_world_arrays[submission_index];
        const uint32_t instance_count = instance_counts == NULL
            ? 1u
            : instance_counts[submission_index];

        groups[submission_index].mesh = meshes[submission_index];
        groups[submission_index].object_to_world = object_to_world;
        groups[submission_index].instance_count = instance_count;
        groups[submission_index].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    }

    build_desc.struct_size = sizeof(build_desc);
    build_desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    build_desc.frame = frame_desc;
    build_desc.groups = groups;
    build_desc.group_count = submission_count;
    build_desc.group_stride = submission_count == 0u
        ? 0u
        : sizeof(*groups);

    result = soc_occlusion_build(context, &build_desc, &snapshot);
    free(groups);
    if (result != SOC_RESULT_OK) {
        return result;
    }

    result = soc_snapshot_hiz_level_query(
        snapshot,
        0u,
        &out_capture->level,
        out_capture->depth,
        TEST_PIXEL_COUNT
    );
    if (result != SOC_RESULT_OK) {
        soc_snapshot_destroy(snapshot);
        return result;
    }

    result = soc_snapshot_get_build_stats(snapshot, &out_capture->stats);
    if (result != SOC_RESULT_OK) {
        soc_snapshot_destroy(snapshot);
        return result;
    }

    soc_snapshot_destroy(snapshot);
    return SOC_RESULT_OK;
}

static soc_result capture_frame(
    soc_context* context,
    soc_depth_direction depth_direction,
    soc_mesh* const* meshes,
    uint32_t mesh_count,
    frame_capture* out_capture
)
{
    const soc_frame_desc frame_desc = make_frame_desc(depth_direction);

    return capture_frame_with_desc(
        context,
        &frame_desc,
        meshes,
        NULL,
        NULL,
        mesh_count,
        out_capture
    );
}

static int check_layout(
    const frame_capture* capture,
    uint32_t width,
    uint32_t height
)
{
    const uint64_t pixel_count = (uint64_t)width * height;

    if (capture->level.level != 0u ||
        capture->level.width != width ||
        capture->level.height != height ||
        capture->level.required_element_count != pixel_count) {
        fprintf(
            stderr,
            "unexpected Level 0 layout: level=%u, %ux%u, count=%llu\n",
            capture->level.level,
            capture->level.width,
            capture->level.height,
            (unsigned long long)capture->level.required_element_count
        );
        return 1;
    }
    return 0;
}

static int check_stats(
    const frame_capture* capture,
    uint32_t hiz_level_count,
    uint64_t input_triangles,
    uint64_t clipped_triangles,
    uint64_t rasterized_triangles
)
{
    const soc_build_stats* stats = &capture->stats;

    if (stats->hiz_level_count != hiz_level_count ||
        stats->input_triangle_count != input_triangles ||
        stats->clipped_triangle_count != clipped_triangles ||
        stats->rasterized_triangle_count != rasterized_triangles) {
        fprintf(
            stderr,
            "unexpected stats: hiz=%u, input=%llu, clipped=%llu, "
            "rasterized=%llu\n",
            stats->hiz_level_count,
            (unsigned long long)stats->input_triangle_count,
            (unsigned long long)stats->clipped_triangle_count,
            (unsigned long long)stats->rasterized_triangle_count
        );
        return 1;
    }
    return 0;
}

static int check_triangle_depth(
    const frame_capture* capture,
    float drawn_depth,
    float clear_depth
)
{
    const uint32_t center_pixels[] = {
        3u * TEST_WIDTH + 3u,
        3u * TEST_WIDTH + 4u,
        4u * TEST_WIDTH + 3u,
        4u * TEST_WIDTH + 4u,
    };
    const uint32_t corner_pixels[] = {
        0u,
        TEST_WIDTH - 1u,
        (TEST_HEIGHT - 1u) * TEST_WIDTH,
        TEST_PIXEL_COUNT - 1u,
    };
    uint32_t drawn_count = 0u;
    uint32_t pixel;

    for (pixel = 0u; pixel < TEST_PIXEL_COUNT; ++pixel) {
        const float value = capture->depth[pixel];

        if (depth_equal(value, drawn_depth)) {
            ++drawn_count;
        } else if (!depth_equal(value, clear_depth)) {
            fprintf(
                stderr,
                "pixel %u has depth %.9g, expected %.9g or clear %.9g\n",
                pixel,
                (double)value,
                (double)drawn_depth,
                (double)clear_depth
            );
            return 1;
        }
    }

    for (pixel = 0u;
         pixel < sizeof(center_pixels) / sizeof(center_pixels[0]);
         ++pixel) {
        const uint32_t index = center_pixels[pixel];

        if (!depth_equal(capture->depth[index], drawn_depth)) {
            fprintf(
                stderr,
                "center pixel %u has depth %.9g, expected %.9g\n",
                index,
                (double)capture->depth[index],
                (double)drawn_depth
            );
            return 1;
        }
    }

    for (pixel = 0u;
         pixel < sizeof(corner_pixels) / sizeof(corner_pixels[0]);
         ++pixel) {
        const uint32_t index = corner_pixels[pixel];

        if (!depth_equal(capture->depth[index], clear_depth)) {
            fprintf(
                stderr,
                "corner pixel %u has depth %.9g, expected clear %.9g\n",
                index,
                (double)capture->depth[index],
                (double)clear_depth
            );
            return 1;
        }
    }

    if (drawn_count == 0u || drawn_count == TEST_PIXEL_COUNT) {
        fprintf(stderr, "unexpected drawn pixel count: %u\n", drawn_count);
        return 1;
    }
    return 0;
}

static int check_all_depth(
    const frame_capture* capture,
    uint32_t pixel_count,
    float expected_depth
)
{
    uint32_t pixel;

    for (pixel = 0u; pixel < pixel_count; ++pixel) {
        if (!depth_equal(capture->depth[pixel], expected_depth)) {
            fprintf(
                stderr,
                "pixel %u has depth %.9g, expected %.9g\n",
                pixel,
                (double)capture->depth[pixel],
                (double)expected_depth
            );
            return 1;
        }
    }
    return 0;
}

static int test_single_triangle_level_zero(void)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_mesh* meshes[1];
    frame_capture capture;

    make_triangle(0.375f, positions);
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = mesh;

    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &capture),
        SOC_RESULT_OK
    );
    CHECK(check_layout(&capture, TEST_WIDTH, TEST_HEIGHT) == 0);
    CHECK(check_triangle_depth(&capture, 0.375f, 1.0f) == 0);
    CHECK(check_stats(&capture, 4u, 1u, 0u, 1u) == 0);

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_depth_order(
    soc_depth_direction direction,
    float near_depth,
    float far_depth,
    float clear_depth
)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    float near_positions[9];
    float far_positions[9];
    soc_context* context = NULL;
    soc_mesh* near_mesh = NULL;
    soc_mesh* far_mesh = NULL;
    soc_mesh* meshes[2];
    frame_capture capture;

    make_triangle(near_depth, near_positions);
    make_triangle(far_depth, far_positions);
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            near_positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &near_mesh
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            far_positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &far_mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = near_mesh;
    meshes[1] = far_mesh;

    CHECK_RESULT(
        capture_frame(context, direction, meshes, 2u, &capture),
        SOC_RESULT_OK
    );
    CHECK(check_layout(&capture, TEST_WIDTH, TEST_HEIGHT) == 0);
    CHECK(check_triangle_depth(&capture, near_depth, clear_depth) == 0);
    CHECK(check_stats(&capture, 4u, 2u, 0u, 2u) == 0);

    CHECK_RESULT(soc_mesh_destroy(near_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(far_mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_forward_and_reversed_depth(void)
{
    if (test_depth_order(
            SOC_DEPTH_FORWARD,
            0.20f,
            0.80f,
            1.0f
        ) != 0) {
        return 1;
    }
    if (test_depth_order(
            SOC_DEPTH_REVERSED,
            0.80f,
            0.20f,
            0.0f
        ) != 0) {
        return 1;
    }
    return 0;
}

static int test_negative_one_to_one_depth_mapping(void)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_mesh* meshes[1];
    soc_frame_desc frame_desc = make_frame_desc(SOC_DEPTH_FORWARD);
    frame_capture capture;

    make_triangle(0.0f, positions);
    frame_desc.clip_depth_range = SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE;
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = mesh;

    CHECK_RESULT(
        capture_frame_with_desc(
            context,
            &frame_desc,
            meshes,
            NULL,
            NULL,
            1u,
            &capture
        ),
        SOC_RESULT_OK
    );
    CHECK(check_layout(&capture, TEST_WIDTH, TEST_HEIGHT) == 0);
    CHECK(check_triangle_depth(&capture, 0.50f, 1.0f) == 0);
    CHECK(check_stats(&capture, 4u, 1u, 0u, 1u) == 0);

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_homogeneous_scale_invariance(void)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    const float homogeneous_scale = 1.0e-13f;
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_mesh* meshes[1];
    soc_frame_desc scaled_frame = make_frame_desc(SOC_DEPTH_FORWARD);
    frame_capture identity_capture;
    frame_capture scaled_capture;
    uint32_t pixel;

    make_triangle(0.375f, positions);
    scaled_frame.clip_from_world.col0.x = homogeneous_scale;
    scaled_frame.clip_from_world.col1.y = homogeneous_scale;
    scaled_frame.clip_from_world.col2.z = homogeneous_scale;
    scaled_frame.clip_from_world.col3.w = homogeneous_scale;

    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = mesh;

    CHECK_RESULT(
        capture_frame(
            context,
            SOC_DEPTH_FORWARD,
            meshes,
            1u,
            &identity_capture
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        capture_frame_with_desc(
            context,
            &scaled_frame,
            meshes,
            NULL,
            NULL,
            1u,
            &scaled_capture
        ),
        SOC_RESULT_OK
    );

    CHECK(check_triangle_depth(&identity_capture, 0.375f, 1.0f) == 0);
    CHECK(check_triangle_depth(&scaled_capture, 0.375f, 1.0f) == 0);
    CHECK(check_stats(&identity_capture, 4u, 1u, 0u, 1u) == 0);
    CHECK(check_stats(&scaled_capture, 4u, 1u, 0u, 1u) == 0);
    for (pixel = 0u; pixel < TEST_PIXEL_COUNT; ++pixel) {
        if (!depth_equal(
                identity_capture.depth[pixel],
                scaled_capture.depth[pixel]
            )) {
            fprintf(
                stderr,
                "homogeneous scale changed pixel %u from %.9g to %.9g\n",
                pixel,
                (double)identity_capture.depth[pixel],
                (double)scaled_capture.depth[pixel]
            );
            return 1;
        }
    }

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_instance_transform_depth(void)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    const uint32_t instance_counts[] = {2u};
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_mesh* meshes[1];
    soc_mat4 transforms[2];
    const soc_mat4* transform_arrays[1];
    const soc_frame_desc frame_desc = make_frame_desc(SOC_DEPTH_FORWARD);
    frame_capture capture;

    make_triangle(0.0f, positions);
    transforms[0] = z_translation_matrix(0.75f);
    transforms[1] = z_translation_matrix(0.25f);
    transform_arrays[0] = transforms;
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = mesh;

    CHECK_RESULT(
        capture_frame_with_desc(
            context,
            &frame_desc,
            meshes,
            transform_arrays,
            instance_counts,
            1u,
            &capture
        ),
        SOC_RESULT_OK
    );
    CHECK(check_layout(&capture, TEST_WIDTH, TEST_HEIGHT) == 0);
    CHECK(check_triangle_depth(&capture, 0.25f, 1.0f) == 0);
    CHECK(check_stats(&capture, 4u, 2u, 0u, 2u) == 0);

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_front_face_culling(void)
{
    const uint16_t ccw_indices[] = {0u, 1u, 2u};
    const uint16_t reversed_indices[] = {0u, 2u, 1u};
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* front_mesh = NULL;
    soc_mesh* back_mesh = NULL;
    soc_mesh* meshes[1];
    frame_capture front;
    frame_capture back;

    make_triangle(0.35f, positions);
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            ccw_indices,
            SOC_MESH_FLAG_NONE,
            &front_mesh
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            reversed_indices,
            SOC_MESH_FLAG_NONE,
            &back_mesh
        ),
        SOC_RESULT_OK
    );

    meshes[0] = front_mesh;
    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &front),
        SOC_RESULT_OK
    );
    CHECK(check_triangle_depth(&front, 0.35f, 1.0f) == 0);
    CHECK(check_stats(&front, 4u, 1u, 0u, 1u) == 0);

    meshes[0] = back_mesh;
    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &back),
        SOC_RESULT_OK
    );
    CHECK(check_all_depth(&back, TEST_PIXEL_COUNT, 1.0f) == 0);
    CHECK(check_stats(&back, 4u, 1u, 0u, 0u) == 0);

    CHECK_RESULT(soc_mesh_destroy(front_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(back_mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_two_sided_winding_and_frame_clear(void)
{
    const uint16_t ccw_indices[] = {0u, 1u, 2u};
    const uint16_t reversed_indices[] = {0u, 2u, 1u};
    float first_positions[9];
    float second_positions[9];
    soc_context* context = NULL;
    soc_mesh* first_mesh = NULL;
    soc_mesh* second_mesh = NULL;
    soc_mesh* meshes[1];
    frame_capture first;
    frame_capture second;
    uint32_t pixel;
    uint32_t covered_count = 0u;

    make_triangle(0.25f, first_positions);
    make_triangle(0.625f, second_positions);
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            first_positions,
            ccw_indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &first_mesh
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            second_positions,
            reversed_indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &second_mesh
        ),
        SOC_RESULT_OK
    );

    meshes[0] = first_mesh;
    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &first),
        SOC_RESULT_OK
    );
    meshes[0] = second_mesh;
    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &second),
        SOC_RESULT_OK
    );

    CHECK(check_triangle_depth(&first, 0.25f, 1.0f) == 0);
    CHECK(check_triangle_depth(&second, 0.625f, 1.0f) == 0);
    CHECK(check_stats(&first, 4u, 1u, 0u, 1u) == 0);
    CHECK(check_stats(&second, 4u, 1u, 0u, 1u) == 0);

    for (pixel = 0u; pixel < TEST_PIXEL_COUNT; ++pixel) {
        const int first_covered = depth_equal(first.depth[pixel], 0.25f);
        const int second_covered = depth_equal(second.depth[pixel], 0.625f);

        if (first_covered != second_covered) {
            fprintf(
                stderr,
                "two-sided winding changed coverage at pixel %u\n",
                pixel
            );
            return 1;
        }
        if (first_covered) {
            ++covered_count;
        }
    }
    CHECK(covered_count > 0u);

    CHECK_RESULT(soc_mesh_destroy(first_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(second_mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_clipped_oversized_triangle(void)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_mesh* meshes[1];
    frame_capture capture;

    make_oversized_triangle(0.40f, positions);
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = mesh;

    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &capture),
        SOC_RESULT_OK
    );
    CHECK(check_layout(&capture, TEST_WIDTH, TEST_HEIGHT) == 0);
    CHECK(check_all_depth(&capture, TEST_PIXEL_COUNT, 0.40f) == 0);
    CHECK(check_stats(&capture, 4u, 1u, 1u, 2u) == 0);

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_fullscreen_hiz_levels(
    soc_depth_direction depth_direction,
    float expected_depth
)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    const soc_mat4 identity = identity_matrix();
    soc_frame_desc frame_desc = make_frame_desc(depth_direction);
    soc_occluder_group group;
    soc_occlusion_build_desc build_desc;
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* snapshot = NULL;
    uint32_t expected_width = TEST_WIDTH;
    uint32_t expected_height = TEST_HEIGHT;
    uint32_t level;

    make_oversized_triangle(expected_depth, positions);
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );

    group.mesh = mesh;
    group.object_to_world = &identity;
    group.instance_count = 1u;
    group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    build_desc.struct_size = sizeof(build_desc);
    build_desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    build_desc.frame = &frame_desc;
    build_desc.groups = &group;
    build_desc.group_count = 1u;
    build_desc.group_stride = sizeof(group);
    CHECK_RESULT(
        soc_occlusion_build(context, &build_desc, &snapshot),
        SOC_RESULT_OK
    );

    for (level = 0u; level < 4u; ++level) {
        soc_hiz_level_info info = {
            .struct_size = sizeof(soc_hiz_level_info),
        };
        float depth[TEST_PIXEL_COUNT];
        const uint32_t expected_count =
            expected_width * expected_height;
        uint32_t index;

        for (index = 0u; index < TEST_PIXEL_COUNT; ++index) {
            depth[index] = DEPTH_SENTINEL;
        }
        CHECK_RESULT(
            soc_snapshot_hiz_level_query(
                snapshot,
                level,
                &info,
                depth,
                TEST_PIXEL_COUNT
            ),
            SOC_RESULT_OK
        );
        CHECK(info.level == level);
        CHECK(info.width == expected_width);
        CHECK(info.height == expected_height);
        CHECK(info.required_element_count == expected_count);
        for (index = 0u; index < expected_count; ++index) {
            CHECK(depth_equal(depth[index], expected_depth));
        }
        for (index = expected_count; index < TEST_PIXEL_COUNT; ++index) {
            CHECK(depth_equal(depth[index], DEPTH_SENTINEL));
        }

        expected_width = expected_width / 2u + expected_width % 2u;
        expected_height = expected_height / 2u + expected_height % 2u;
    }

    soc_snapshot_destroy(snapshot);
    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_hiz_pipeline_integration(void)
{
    if (test_fullscreen_hiz_levels(SOC_DEPTH_FORWARD, 0.40f) != 0) {
        return 1;
    }
    if (test_fullscreen_hiz_levels(SOC_DEPTH_REVERSED, 0.60f) != 0) {
        return 1;
    }
    return 0;
}

static int test_zero_to_one_near_plane_clipping(void)
{
    const float positions[] = {
        -0.75f, -0.75f, -0.25f,
         0.75f, -0.75f,  0.50f,
         0.00f,  0.75f,  0.50f,
    };
    const uint16_t indices[] = {0u, 1u, 2u};
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_mesh* meshes[1];
    frame_capture capture;
    uint32_t drawn_count = 0u;
    uint32_t pixel;

    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = mesh;

    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &capture),
        SOC_RESULT_OK
    );
    CHECK(check_layout(&capture, TEST_WIDTH, TEST_HEIGHT) == 0);
    CHECK(check_stats(&capture, 4u, 1u, 1u, 2u) == 0);

    for (pixel = 0u; pixel < TEST_PIXEL_COUNT; ++pixel) {
        const float depth = capture.depth[pixel];

        if (depth_equal(depth, 1.0f)) {
            continue;
        }
        if (depth < -DEPTH_EPSILON || depth > 0.50f + DEPTH_EPSILON) {
            fprintf(
                stderr,
                "near-clipped pixel %u has out-of-range depth %.9g\n",
                pixel,
                (double)depth
            );
            return 1;
        }
        ++drawn_count;
    }
    CHECK(drawn_count > 0u);
    CHECK(drawn_count < TEST_PIXEL_COUNT);

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_resize_and_empty_frame_clear(void)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    float positions[9];
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_mesh* meshes[1];
    soc_snapshot* snapshot = NULL;
    const soc_frame_desc frame_desc = make_frame_desc(SOC_DEPTH_FORWARD);
    frame_capture drawn;
    frame_capture cleared;
    uint32_t pixel;

    make_triangle(0.30f, positions);
    CHECK_RESULT(
        create_context(TEST_WIDTH, TEST_HEIGHT, &context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_triangle_mesh(
            context,
            positions,
            indices,
            SOC_MESH_FLAG_TWO_SIDED,
            &mesh
        ),
        SOC_RESULT_OK
    );
    meshes[0] = mesh;

    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, meshes, 1u, &drawn),
        SOC_RESULT_OK
    );
    CHECK(check_triangle_depth(&drawn, 0.30f, 1.0f) == 0);

    CHECK_RESULT(soc_context_resize(context, 5u, 3u), SOC_RESULT_OK);
    CHECK_RESULT(
        capture_frame(context, SOC_DEPTH_FORWARD, NULL, 0u, &cleared),
        SOC_RESULT_OK
    );
    CHECK(check_layout(&cleared, 5u, 3u) == 0);
    CHECK(check_all_depth(&cleared, 15u, 1.0f) == 0);
    CHECK(check_stats(&cleared, 4u, 0u, 0u, 0u) == 0);
    for (pixel = 15u; pixel < TEST_PIXEL_COUNT; ++pixel) {
        CHECK(depth_equal(cleared.depth[pixel], DEPTH_SENTINEL));
    }

    {
        const soc_occlusion_build_desc build_desc = {
            .struct_size = sizeof(soc_occlusion_build_desc),
            .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
            .frame = &frame_desc,
            .groups = NULL,
            .group_count = 0u,
            .group_stride = 0u,
        };
        CHECK_RESULT(
            soc_occlusion_build(context, &build_desc, &snapshot),
            SOC_RESULT_OK
        );
    }
    {
        uint32_t expected_width = 5u;
        uint32_t expected_height = 3u;
        uint32_t level;

        for (level = 0u; level < 4u; ++level) {
            soc_hiz_level_info info = {
                .struct_size = sizeof(soc_hiz_level_info),
            };
            float depth[TEST_PIXEL_COUNT];
            const uint32_t expected_count =
                expected_width * expected_height;

            for (pixel = 0u; pixel < TEST_PIXEL_COUNT; ++pixel) {
                depth[pixel] = DEPTH_SENTINEL;
            }
            CHECK_RESULT(
                soc_snapshot_hiz_level_query(
                    snapshot,
                    level,
                    &info,
                    depth,
                    TEST_PIXEL_COUNT
                ),
                SOC_RESULT_OK
            );
            CHECK(info.level == level);
            CHECK(info.width == expected_width);
            CHECK(info.height == expected_height);
            CHECK(info.required_element_count == expected_count);
            for (pixel = 0u; pixel < expected_count; ++pixel) {
                CHECK(depth_equal(depth[pixel], 1.0f));
            }
            for (pixel = expected_count;
                 pixel < TEST_PIXEL_COUNT;
                 ++pixel) {
                CHECK(depth_equal(depth[pixel], DEPTH_SENTINEL));
            }

            expected_width =
                expected_width / 2u + expected_width % 2u;
            expected_height =
                expected_height / 2u + expected_height % 2u;
        }
    }
    soc_snapshot_destroy(snapshot);

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

int main(void)
{
    if (test_single_triangle_level_zero() != 0) {
        return 1;
    }
    if (test_forward_and_reversed_depth() != 0) {
        return 1;
    }
    if (test_negative_one_to_one_depth_mapping() != 0) {
        return 1;
    }
    if (test_homogeneous_scale_invariance() != 0) {
        return 1;
    }
    if (test_instance_transform_depth() != 0) {
        return 1;
    }
    if (test_front_face_culling() != 0) {
        return 1;
    }
    if (test_two_sided_winding_and_frame_clear() != 0) {
        return 1;
    }
    if (test_clipped_oversized_triangle() != 0) {
        return 1;
    }
    if (test_hiz_pipeline_integration() != 0) {
        return 1;
    }
    if (test_zero_to_one_near_plane_clipping() != 0) {
        return 1;
    }
    if (test_resize_and_empty_frame_clear() != 0) {
        return 1;
    }
    return 0;
}
