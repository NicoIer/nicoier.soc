#include <soc/soc.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static soc_mat4 translated_matrix(float x, float y, float z)
{
    soc_mat4 result = identity_matrix();

    result.col3.x = x;
    result.col3.y = y;
    result.col3.z = z;
    return result;
}

static soc_result create_context(
    uint32_t worker_count,
    soc_context** out_context
)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 641u,
        .height = 643u,
        .worker_count = worker_count,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    return soc_context_create(&config, out_context);
}

static soc_result create_mesh(soc_context* context, soc_mesh** out_mesh)
{
    static const float vertices[] = {
        -0.30f, -0.30f, 0.0f,
         0.30f, -0.30f, 0.0f,
         0.00f,  0.30f, 0.0f,
    };
    static const uint16_t indices[] = {0u, 1u, 2u};
    const soc_mesh_desc desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_TWO_SIDED,
        .vertices = vertices,
        .indices = indices,
        .vertex_count = 3u,
        .vertex_stride = 3u * sizeof(float),
        .position_offset = 0u,
        .index_count = 3u,
        .index_type = SOC_INDEX_UINT16,
    };
    return soc_mesh_create(context, &desc, out_mesh);
}

static soc_result create_chunked_mesh(
    soc_context* context,
    soc_mesh** out_mesh
)
{
    enum {
        TRIANGLE_COUNT = 258,
        INDEX_COUNT = TRIANGLE_COUNT * 3,
    };
    static const float vertices[] = {
        -0.30f, -0.30f, 0.4f,
         0.30f, -0.30f, 0.4f,
         0.00f,  0.30f, 0.4f,
        -1.40f, -0.60f, 0.6f,
        -0.20f, -0.60f, 0.6f,
        -0.60f,  0.80f, 0.6f,
         1.20f, -0.50f, 0.8f,
         1.50f,  0.00f, 0.8f,
         1.20f,  0.50f, 0.8f,
    };
    uint16_t indices[INDEX_COUNT];
    soc_mesh_desc desc;
    uint32_t triangle;

    for (triangle = 0u; triangle < TRIANGLE_COUNT; ++triangle) {
        const uint16_t first_vertex =
            (uint16_t)((triangle % 3u) * 3u);
        const uint32_t first_index = triangle * 3u;

        indices[first_index] = first_vertex;
        indices[first_index + 1u] = (uint16_t)(first_vertex + 1u);
        indices[first_index + 2u] = (uint16_t)(first_vertex + 2u);
    }

    desc.struct_size = sizeof(desc);
    desc.flags = SOC_MESH_FLAG_TWO_SIDED;
    desc.vertices = vertices;
    desc.indices = indices;
    desc.vertex_count = 9u;
    desc.vertex_stride = 3u * sizeof(float);
    desc.position_offset = 0u;
    desc.index_count = INDEX_COUNT;
    desc.index_type = SOC_INDEX_UINT16;
    return soc_mesh_create(context, &desc, out_mesh);
}

static soc_result create_tiled_hot_mesh(
    soc_context* context,
    soc_mesh** out_mesh
)
{
    enum {
        HOT_TRIANGLE_COUNT = 1100,
        TRIANGLE_COUNT = HOT_TRIANGLE_COUNT + 2,
        INDEX_COUNT = TRIANGLE_COUNT * 3,
    };
    static const float vertices[] = {
        /* Hot tile (column 7, row 7), with competing depths. */
        -0.28f,  0.24f, 0.25f,
        -0.24f,  0.24f, 0.25f,
        -0.26f,  0.28f, 0.25f,
        -0.28f,  0.24f, 0.75f,
        -0.24f,  0.24f, 0.75f,
        -0.26f,  0.28f, 0.75f,
        /* Normal tile in the same tile row (column 9, row 7). */
        -0.10f,  0.24f, 0.50f,
        -0.06f,  0.24f, 0.50f,
        -0.08f,  0.28f, 0.50f,
        /* Normal-only tile row (column 9, row 12). */
        -0.10f, -0.28f, 0.60f,
        -0.06f, -0.28f, 0.60f,
        -0.08f, -0.24f, 0.60f,
    };
    uint16_t indices[INDEX_COUNT];
    soc_mesh_desc desc;
    uint32_t triangle;

    for (triangle = 0u; triangle < HOT_TRIANGLE_COUNT; ++triangle) {
        const uint16_t first_vertex =
            (uint16_t)((triangle % 2u) * 3u);
        const uint32_t first_index = triangle * 3u;

        indices[first_index] = first_vertex;
        indices[first_index + 1u] = (uint16_t)(first_vertex + 1u);
        indices[first_index + 2u] = (uint16_t)(first_vertex + 2u);
    }
    indices[HOT_TRIANGLE_COUNT * 3u] = 6u;
    indices[HOT_TRIANGLE_COUNT * 3u + 1u] = 7u;
    indices[HOT_TRIANGLE_COUNT * 3u + 2u] = 8u;
    indices[(HOT_TRIANGLE_COUNT + 1u) * 3u] = 9u;
    indices[(HOT_TRIANGLE_COUNT + 1u) * 3u + 1u] = 10u;
    indices[(HOT_TRIANGLE_COUNT + 1u) * 3u + 2u] = 11u;

    desc.struct_size = sizeof(desc);
    desc.flags = SOC_MESH_FLAG_TWO_SIDED;
    desc.vertices = vertices;
    desc.indices = indices;
    desc.vertex_count = 12u;
    desc.vertex_stride = 3u * sizeof(float);
    desc.position_offset = 0u;
    desc.index_count = INDEX_COUNT;
    desc.index_type = SOC_INDEX_UINT16;
    return soc_mesh_create(context, &desc, out_mesh);
}

static soc_result create_long_thin_mesh(
    soc_context* context,
    soc_mesh** out_mesh
)
{
    enum {
        TRIANGLE_COUNT = 4096,
        INDEX_COUNT = TRIANGLE_COUNT * 3,
    };
    static const float vertices[] = {
        -0.95f, -0.95f, 0.6f,
         0.95f,  0.95f, 0.6f,
         0.95f,  0.94f, 0.6f,
    };
    uint16_t indices[INDEX_COUNT];
    soc_mesh_desc desc;
    uint32_t triangle;

    for (triangle = 0u; triangle < TRIANGLE_COUNT; ++triangle) {
        const uint32_t first_index = triangle * 3u;

        indices[first_index] = 0u;
        indices[first_index + 1u] = 1u;
        indices[first_index + 2u] = 2u;
    }
    desc.struct_size = sizeof(desc);
    desc.flags = SOC_MESH_FLAG_TWO_SIDED;
    desc.vertices = vertices;
    desc.indices = indices;
    desc.vertex_count = 3u;
    desc.vertex_stride = 3u * sizeof(float);
    desc.position_offset = 0u;
    desc.index_count = INDEX_COUNT;
    desc.index_type = SOC_INDEX_UINT16;
    return soc_mesh_create(context, &desc, out_mesh);
}

static soc_result create_tiled_depth_sort_mesh(
    soc_context* context,
    soc_mesh** out_mesh
)
{
    enum {
        LAYERS_PER_WORK_ITEM = 8,
        VISIBLE_TRIANGLES_PER_WORK_ITEM = LAYERS_PER_WORK_ITEM * 2,
        VISIBLE_TRIANGLE_COUNT = VISIBLE_TRIANGLES_PER_WORK_ITEM * 2,
        TRIANGLE_COUNT = 2049,
        VERTEX_COUNT = TRIANGLE_COUNT * 3,
        INDEX_COUNT = TRIANGLE_COUNT * 3,
    };
    float* vertices = (float*)malloc(
        (size_t)VERTEX_COUNT * 3u * sizeof(float)
    );
    uint16_t* indices = (uint16_t*)malloc(
        (size_t)INDEX_COUNT * sizeof(uint16_t)
    );
    soc_mesh_desc desc;
    soc_result result;
    uint32_t triangle;

    if (vertices == NULL || indices == NULL) {
        free(vertices);
        free(indices);
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    for (triangle = 0u; triangle < TRIANGLE_COUNT; ++triangle) {
        float* vertex = &vertices[(size_t)triangle * 9u];
        uint32_t visible_triangle = UINT32_MAX;
        float depth = 0.5f;

        if (triangle < VISIBLE_TRIANGLES_PER_WORK_ITEM) {
            visible_triangle = triangle;
        } else if (triangle >= 1024u &&
            triangle < 1024u + VISIBLE_TRIANGLES_PER_WORK_ITEM) {
            visible_triangle = triangle - 1024u;
        }
        if (visible_triangle != UINT32_MAX) {
            const uint32_t layer = visible_triangle / 2u;
            const uint32_t depth_layer =
                LAYERS_PER_WORK_ITEM - 1u - layer;
            const float fraction = (float)depth_layer /
                (float)(LAYERS_PER_WORK_ITEM - 1u);

            depth = 0.95f + (0.35f - 0.95f) * fraction;
            vertex[0] = -1.0f;
            vertex[1] = -1.0f;
            if ((visible_triangle & 1u) == 0u) {
                vertex[3] = 1.0f;
                vertex[4] = -1.0f;
                vertex[6] = 1.0f;
                vertex[7] = 1.0f;
            } else {
                vertex[3] = 1.0f;
                vertex[4] = 1.0f;
                vertex[6] = -1.0f;
                vertex[7] = 1.0f;
            }
        } else {
            vertex[0] = 2.0f;
            vertex[1] = -1.0f;
            vertex[3] = 4.0f;
            vertex[4] = -1.0f;
            vertex[6] = 2.0f;
            vertex[7] = 3.0f;
        }
        vertex[2] = depth;
        vertex[5] = depth;
        vertex[8] = depth;
        indices[(size_t)triangle * 3u + 0u] =
            (uint16_t)(triangle * 3u + 0u);
        indices[(size_t)triangle * 3u + 1u] =
            (uint16_t)(triangle * 3u + 1u);
        indices[(size_t)triangle * 3u + 2u] =
            (uint16_t)(triangle * 3u + 2u);
    }

    desc.struct_size = sizeof(desc);
    desc.flags = SOC_MESH_FLAG_TWO_SIDED;
    desc.vertices = vertices;
    desc.indices = indices;
    desc.vertex_count = VERTEX_COUNT;
    desc.vertex_stride = 3u * sizeof(float);
    desc.position_offset = 0u;
    desc.index_count = INDEX_COUNT;
    desc.index_type = SOC_INDEX_UINT16;
    result = soc_mesh_create(context, &desc, out_mesh);
    free(vertices);
    free(indices);
    return result;
}

static soc_frame_desc make_frame(void)
{
    const soc_frame_desc frame = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
            .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
        },
        .clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    return frame;
}

static int compare_snapshots(
    const soc_snapshot* single,
    const soc_snapshot* parallel
)
{
    soc_build_stats single_stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_build_stats parallel_stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    uint32_t level;

    CHECK_RESULT(
        soc_snapshot_get_build_stats(single, &single_stats),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_get_build_stats(parallel, &parallel_stats),
        SOC_RESULT_OK
    );
    CHECK(single_stats.hiz_level_count == parallel_stats.hiz_level_count);
    CHECK(
        single_stats.input_triangle_count ==
        parallel_stats.input_triangle_count
    );
    CHECK(
        single_stats.clipped_triangle_count ==
        parallel_stats.clipped_triangle_count
    );
    CHECK(
        single_stats.rasterized_triangle_count ==
        parallel_stats.rasterized_triangle_count
    );

    for (level = 0u; level < single_stats.hiz_level_count; ++level) {
        soc_hiz_level_info single_info = {
            .struct_size = sizeof(soc_hiz_level_info),
        };
        soc_hiz_level_info parallel_info = {
            .struct_size = sizeof(soc_hiz_level_info),
        };
        float* single_depth;
        float* parallel_depth;
        size_t byte_count;
        size_t depth_index;

        CHECK_RESULT(
            soc_snapshot_hiz_level_query(
                single,
                level,
                &single_info,
                NULL,
                0u
            ),
            SOC_RESULT_OK
        );
        CHECK_RESULT(
            soc_snapshot_hiz_level_query(
                parallel,
                level,
                &parallel_info,
                NULL,
                0u
            ),
            SOC_RESULT_OK
        );
        CHECK(single_info.width == parallel_info.width);
        CHECK(single_info.height == parallel_info.height);
        CHECK(
            single_info.required_element_count ==
            parallel_info.required_element_count
        );
        CHECK(
            single_info.required_element_count <=
            SIZE_MAX / sizeof(float)
        );

        byte_count =
            (size_t)single_info.required_element_count * sizeof(float);
        single_depth = (float*)malloc(byte_count);
        parallel_depth = (float*)malloc(byte_count);
        CHECK(single_depth != NULL);
        CHECK(parallel_depth != NULL);
        CHECK_RESULT(
            soc_snapshot_hiz_level_query(
                single,
                level,
                &single_info,
                single_depth,
                single_info.required_element_count
            ),
            SOC_RESULT_OK
        );
        CHECK_RESULT(
            soc_snapshot_hiz_level_query(
                parallel,
                level,
                &parallel_info,
                parallel_depth,
                parallel_info.required_element_count
            ),
            SOC_RESULT_OK
        );
        for (depth_index = 0u;
             depth_index < (size_t)single_info.required_element_count;
             ++depth_index) {
            float difference = single_depth[depth_index] -
                parallel_depth[depth_index];
            float scale = single_depth[depth_index];

            if (difference < 0.0f) {
                difference = -difference;
            }
            if (scale < 0.0f) {
                scale = -scale;
            }
            CHECK(difference <= 2.0e-5f * (1.0f + scale));
        }
        free(parallel_depth);
        free(single_depth);
    }

    return 0;
}

static int test_worker_results_match(void)
{
    soc_mat4 transforms[16];
    const soc_frame_desc frame = make_frame();
    soc_occluder_group single_group;
    soc_occluder_group parallel_group;
    soc_occlusion_build_desc single_desc;
    soc_occlusion_build_desc parallel_desc;
    soc_context* single_context = NULL;
    soc_context* parallel_context = NULL;
    soc_mesh* single_mesh = NULL;
    soc_mesh* parallel_mesh = NULL;
    soc_snapshot* single_snapshot = NULL;
    soc_snapshot* parallel_snapshot = NULL;
    uint32_t index;

    for (index = 0u; index < 16u; ++index) {
        const float x = ((float)(index % 4u) - 1.5f) * 0.45f;
        const float y = ((float)(index / 4u) - 1.5f) * 0.35f;
        const float z = 0.1f + (float)(index % 8u) * 0.1f;

        transforms[index] = translated_matrix(x, y, z);
    }

    CHECK_RESULT(create_context(1u, &single_context), SOC_RESULT_OK);
    CHECK_RESULT(create_context(4u, &parallel_context), SOC_RESULT_OK);
    CHECK_RESULT(create_mesh(single_context, &single_mesh), SOC_RESULT_OK);
    CHECK_RESULT(create_mesh(parallel_context, &parallel_mesh), SOC_RESULT_OK);

    single_group.mesh = single_mesh;
    single_group.object_to_world = transforms;
    single_group.instance_count = 16u;
    single_group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    parallel_group = single_group;
    parallel_group.mesh = parallel_mesh;

    single_desc.struct_size = sizeof(single_desc);
    single_desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    single_desc.frame = &frame;
    single_desc.groups = &single_group;
    single_desc.group_count = 1u;
    single_desc.group_stride = sizeof(single_group);
    parallel_desc = single_desc;
    parallel_desc.groups = &parallel_group;

    CHECK_RESULT(
        soc_occlusion_build(
            single_context,
            &single_desc,
            &single_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_occlusion_build(
            parallel_context,
            &parallel_desc,
            &parallel_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(single_snapshot != NULL);
    CHECK(parallel_snapshot != NULL);
    CHECK(compare_snapshots(single_snapshot, parallel_snapshot) == 0);

    soc_snapshot_destroy(parallel_snapshot);
    soc_snapshot_destroy(single_snapshot);
    CHECK_RESULT(soc_mesh_destroy(parallel_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(single_mesh), SOC_RESULT_OK);
    soc_context_destroy(parallel_context);
    soc_context_destroy(single_context);
    return 0;
}

static int test_chunked_mesh_results_match(void)
{
    const soc_mat4 transform = identity_matrix();
    const soc_frame_desc frame = make_frame();
    soc_occluder_group single_group;
    soc_occluder_group parallel_group;
    soc_occlusion_build_desc single_desc;
    soc_occlusion_build_desc parallel_desc;
    soc_build_stats stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_context* single_context = NULL;
    soc_context* parallel_context = NULL;
    soc_mesh* single_mesh = NULL;
    soc_mesh* parallel_mesh = NULL;
    soc_snapshot* single_snapshot = NULL;
    soc_snapshot* parallel_snapshot = NULL;

    CHECK_RESULT(create_context(1u, &single_context), SOC_RESULT_OK);
    CHECK_RESULT(create_context(4u, &parallel_context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_chunked_mesh(single_context, &single_mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_chunked_mesh(parallel_context, &parallel_mesh),
        SOC_RESULT_OK
    );

    single_group.mesh = single_mesh;
    single_group.object_to_world = &transform;
    single_group.instance_count = 1u;
    single_group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    parallel_group = single_group;
    parallel_group.mesh = parallel_mesh;

    single_desc.struct_size = sizeof(single_desc);
    single_desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    single_desc.frame = &frame;
    single_desc.groups = &single_group;
    single_desc.group_count = 1u;
    single_desc.group_stride = sizeof(single_group);
    parallel_desc = single_desc;
    parallel_desc.groups = &parallel_group;

    CHECK_RESULT(
        soc_occlusion_build(
            single_context,
            &single_desc,
            &single_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_occlusion_build(
            parallel_context,
            &parallel_desc,
            &parallel_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(compare_snapshots(single_snapshot, parallel_snapshot) == 0);
    CHECK_RESULT(
        soc_snapshot_get_build_stats(parallel_snapshot, &stats),
        SOC_RESULT_OK
    );
    CHECK(stats.input_triangle_count == 258u);
    CHECK(stats.clipped_triangle_count != 0u);
    CHECK(stats.rasterized_triangle_count != 0u);

    soc_snapshot_destroy(parallel_snapshot);
    soc_snapshot_destroy(single_snapshot);
    CHECK_RESULT(soc_mesh_destroy(parallel_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(single_mesh), SOC_RESULT_OK);
    soc_context_destroy(parallel_context);
    soc_context_destroy(single_context);
    return 0;
}

static int test_tiled_hot_merge_results_match(void)
{
    enum {
        HOT_TRIANGLE_COUNT = 1100,
        TRIANGLE_COUNT = HOT_TRIANGLE_COUNT + 2,
    };
    const soc_mat4 transform = identity_matrix();
    const soc_frame_desc frame = make_frame();
    soc_occluder_group single_group;
    soc_occluder_group parallel_group;
    soc_occlusion_build_desc single_desc;
    soc_occlusion_build_desc parallel_desc;
    soc_build_stats stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_context* single_context = NULL;
    soc_context* parallel_context = NULL;
    soc_mesh* single_mesh = NULL;
    soc_mesh* parallel_mesh = NULL;
    soc_snapshot* single_snapshot = NULL;
    soc_snapshot* parallel_snapshot = NULL;

    /*
     * With four workers, 1102 source triangles form five private work items,
     * below the selector's 4 * 4 private threshold, so this build is tiled.
     * The first 1100 prepared records occupy one tile, exceeding the 1024
     * hot threshold. The final records create a normal tile in that hot row
     * and a normal-only row; the 641x643 target also leaves empty and tail
     * tile rows for the fused row-HiZ paths.
     */
    CHECK_RESULT(create_context(1u, &single_context), SOC_RESULT_OK);
    CHECK_RESULT(create_context(4u, &parallel_context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_tiled_hot_mesh(single_context, &single_mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_tiled_hot_mesh(parallel_context, &parallel_mesh),
        SOC_RESULT_OK
    );

    single_group.mesh = single_mesh;
    single_group.object_to_world = &transform;
    single_group.instance_count = 1u;
    single_group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    parallel_group = single_group;
    parallel_group.mesh = parallel_mesh;

    single_desc.struct_size = sizeof(single_desc);
    single_desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    single_desc.frame = &frame;
    single_desc.groups = &single_group;
    single_desc.group_count = 1u;
    single_desc.group_stride = sizeof(single_group);
    parallel_desc = single_desc;
    parallel_desc.groups = &parallel_group;

    CHECK_RESULT(
        soc_occlusion_build(
            single_context,
            &single_desc,
            &single_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_occlusion_build(
            parallel_context,
            &parallel_desc,
            &parallel_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(compare_snapshots(single_snapshot, parallel_snapshot) == 0);
    CHECK_RESULT(
        soc_snapshot_get_build_stats(parallel_snapshot, &stats),
        SOC_RESULT_OK
    );
    CHECK(stats.input_triangle_count == TRIANGLE_COUNT);
    CHECK(stats.clipped_triangle_count == 0u);
    CHECK(stats.rasterized_triangle_count == TRIANGLE_COUNT);

    soc_snapshot_destroy(parallel_snapshot);
    soc_snapshot_destroy(single_snapshot);
    CHECK_RESULT(soc_mesh_destroy(parallel_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(single_mesh), SOC_RESULT_OK);
    soc_context_destroy(parallel_context);
    soc_context_destroy(single_context);
    return 0;
}

static int test_long_thin_tiled_results_match(void)
{
    enum { TRIANGLE_COUNT = 4096 };
    const soc_mat4 transform = identity_matrix();
    const soc_frame_desc frame = make_frame();
    soc_occluder_group single_group;
    soc_occluder_group parallel_group;
    soc_occlusion_build_desc single_desc;
    soc_occlusion_build_desc parallel_desc;
    soc_build_stats stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_context* single_context = NULL;
    soc_context* parallel_context = NULL;
    soc_mesh* single_mesh = NULL;
    soc_mesh* parallel_mesh = NULL;
    soc_snapshot* single_snapshot = NULL;
    soc_snapshot* parallel_snapshot = NULL;

    CHECK_RESULT(create_context(1u, &single_context), SOC_RESULT_OK);
    CHECK_RESULT(create_context(4u, &parallel_context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_long_thin_mesh(single_context, &single_mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_long_thin_mesh(parallel_context, &parallel_mesh),
        SOC_RESULT_OK
    );

    single_group.mesh = single_mesh;
    single_group.object_to_world = &transform;
    single_group.instance_count = 1u;
    single_group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    parallel_group = single_group;
    parallel_group.mesh = parallel_mesh;

    single_desc.struct_size = sizeof(single_desc);
    single_desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    single_desc.frame = &frame;
    single_desc.groups = &single_group;
    single_desc.group_count = 1u;
    single_desc.group_stride = sizeof(single_group);
    parallel_desc = single_desc;
    parallel_desc.groups = &parallel_group;

    CHECK_RESULT(
        soc_occlusion_build(
            single_context,
            &single_desc,
            &single_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_occlusion_build(
            parallel_context,
            &parallel_desc,
            &parallel_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(compare_snapshots(single_snapshot, parallel_snapshot) == 0);
    CHECK_RESULT(
        soc_snapshot_get_build_stats(parallel_snapshot, &stats),
        SOC_RESULT_OK
    );
    CHECK(stats.input_triangle_count == TRIANGLE_COUNT);
    CHECK(stats.clipped_triangle_count == 0u);
    CHECK(stats.rasterized_triangle_count == TRIANGLE_COUNT);

    soc_snapshot_destroy(parallel_snapshot);
    soc_snapshot_destroy(single_snapshot);
    CHECK_RESULT(soc_mesh_destroy(parallel_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(single_mesh), SOC_RESULT_OK);
    soc_context_destroy(parallel_context);
    soc_context_destroy(single_context);
    return 0;
}

static int test_tiled_depth_sort_results_match(void)
{
    enum {
        TRIANGLE_COUNT = 2049,
        VISIBLE_TRIANGLE_COUNT = 32,
    };
    const soc_mat4 transform = identity_matrix();
    const soc_frame_desc frame = make_frame();
    soc_occluder_group single_group;
    soc_occluder_group parallel_group;
    soc_occlusion_build_desc single_desc;
    soc_occlusion_build_desc parallel_desc;
    soc_build_stats stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_context* single_context = NULL;
    soc_context* parallel_context = NULL;
    soc_mesh* single_mesh = NULL;
    soc_mesh* parallel_mesh = NULL;
    soc_snapshot* single_snapshot = NULL;
    soc_snapshot* parallel_snapshot = NULL;

    /*
     * 2049 triangles select dense instead of masked. Nine 256-triangle
     * private work items are below the four-worker private threshold, so
     * the parallel build uses tiled dense replay. Visible layers are split
     * between two 1024-triangle prepare items; each lane-local segment is
     * far-to-near, so the 8..64 global tile sort path is exercised even when
     * the items land on different prepare lanes.
     */
    CHECK_RESULT(create_context(1u, &single_context), SOC_RESULT_OK);
    CHECK_RESULT(create_context(4u, &parallel_context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_tiled_depth_sort_mesh(single_context, &single_mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_tiled_depth_sort_mesh(parallel_context, &parallel_mesh),
        SOC_RESULT_OK
    );

    single_group.mesh = single_mesh;
    single_group.object_to_world = &transform;
    single_group.instance_count = 1u;
    single_group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    parallel_group = single_group;
    parallel_group.mesh = parallel_mesh;

    single_desc.struct_size = sizeof(single_desc);
    single_desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    single_desc.frame = &frame;
    single_desc.groups = &single_group;
    single_desc.group_count = 1u;
    single_desc.group_stride = sizeof(single_group);
    parallel_desc = single_desc;
    parallel_desc.groups = &parallel_group;

    CHECK_RESULT(
        soc_occlusion_build(
            single_context,
            &single_desc,
            &single_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_occlusion_build(
            parallel_context,
            &parallel_desc,
            &parallel_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(compare_snapshots(single_snapshot, parallel_snapshot) == 0);
    CHECK_RESULT(
        soc_snapshot_get_build_stats(parallel_snapshot, &stats),
        SOC_RESULT_OK
    );
    CHECK(stats.input_triangle_count == TRIANGLE_COUNT);
    CHECK(
        stats.clipped_triangle_count ==
        TRIANGLE_COUNT - VISIBLE_TRIANGLE_COUNT
    );
    CHECK(stats.rasterized_triangle_count == VISIBLE_TRIANGLE_COUNT);

    soc_snapshot_destroy(parallel_snapshot);
    soc_snapshot_destroy(single_snapshot);
    CHECK_RESULT(soc_mesh_destroy(parallel_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(single_mesh), SOC_RESULT_OK);
    soc_context_destroy(parallel_context);
    soc_context_destroy(single_context);
    return 0;
}

int main(void)
{
    if (test_worker_results_match() != 0) {
        return 1;
    }
    if (test_chunked_mesh_results_match() != 0) {
        return 1;
    }
    if (test_long_thin_tiled_results_match() != 0) {
        return 1;
    }
    if (test_tiled_depth_sort_results_match() != 0) {
        return 1;
    }
    return test_tiled_hot_merge_results_match();
}
