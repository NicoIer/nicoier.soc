#include <soc/soc.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

static soc_frame_desc make_frame_desc(void)
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
        .depth_direction = SOC_DEPTH_FORWARD,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    return frame;
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
    soc_mesh** out_mesh
)
{
    static const float vertices[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    static const uint16_t indices[] = {0u, 1u, 2u};
    const soc_mesh_desc mesh_desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_NONE,
        .vertices = vertices,
        .indices = indices,
        .vertex_count = 3u,
        .vertex_stride = 3u * sizeof(float),
        .position_offset = 0u,
        .index_count = 3u,
        .index_type = SOC_INDEX_UINT16,
    };
    return soc_mesh_create(context, &mesh_desc, out_mesh);
}

static soc_occlusion_build_desc make_build_desc(
    const soc_frame_desc* frame,
    const soc_occluder_group* groups,
    uint32_t group_count
)
{
    const soc_occlusion_build_desc desc = {
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = frame,
        .groups = groups,
        .group_count = group_count,
        .group_stride = group_count == 0u
            ? 0u
            : sizeof(soc_occluder_group),
    };
    return desc;
}

static int test_build_query_and_snapshot_lifetime(void)
{
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame = make_frame_desc();
    const soc_aabb bounds[] = {
        {
            .min = {-0.25f, -0.25f, 0.0f},
            .max = {0.25f, 0.25f, 0.25f},
        },
        {
            .min = {0.25f, -0.25f, 0.0f},
            .max = {-0.25f, 0.25f, 0.25f},
        },
    };
    soc_visibility first_visibility[2] = {
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_OCCLUDED,
    };
    soc_visibility second_visibility = SOC_VISIBILITY_OCCLUDED;
    soc_query_stats first_query_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_query_stats second_query_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_build_stats build_stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_hiz_level_info level_info = {
        .struct_size = sizeof(soc_hiz_level_info),
    };
    soc_occluder_group groups[3];
    soc_occlusion_build_desc build_desc;
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* snapshot = NULL;
    float top_depth = -1.0f;

    CHECK_RESULT(create_context(320u, 180u, &context), SOC_RESULT_OK);
    CHECK_RESULT(create_triangle_mesh(context, &mesh), SOC_RESULT_OK);

    groups[0].mesh = mesh;
    groups[0].object_to_world = &identity;
    groups[0].instance_count = 1u;
    groups[0].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    groups[1].mesh = NULL;
    groups[1].object_to_world = NULL;
    groups[1].instance_count = 0u;
    groups[1].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    groups[2].mesh = mesh;
    groups[2].object_to_world = &identity;
    groups[2].instance_count = 1u;
    groups[2].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    build_desc = make_build_desc(&frame, groups, 3u);

    CHECK_RESULT(
        soc_occlusion_build(context, &build_desc, &snapshot),
        SOC_RESULT_OK
    );
    CHECK(snapshot != NULL);
    CHECK_RESULT(
        soc_snapshot_get_build_stats(snapshot, &build_stats),
        SOC_RESULT_OK
    );
    CHECK(build_stats.hiz_level_count == 10u);
    CHECK(build_stats.input_triangle_count == 2u);
    CHECK(build_stats.clipped_triangle_count == 0u);
    CHECK(build_stats.rasterized_triangle_count == 2u);

    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            snapshot,
            0u,
            &level_info,
            NULL,
            0u
        ),
        SOC_RESULT_OK
    );
    CHECK(level_info.level == 0u);
    CHECK(level_info.width == 320u);
    CHECK(level_info.height == 180u);
    CHECK(level_info.required_element_count == 320u * 180u);
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            snapshot,
            0u,
            &level_info,
            &top_depth,
            1u
        ),
        SOC_RESULT_BUFFER_TOO_SMALL
    );
    CHECK(level_info.width == 320u);
    CHECK(level_info.height == 180u);
    CHECK(top_depth == -1.0f);
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            snapshot,
            9u,
            &level_info,
            &top_depth,
            1u
        ),
        SOC_RESULT_OK
    );
    CHECK(level_info.width == 1u);
    CHECK(level_info.height == 1u);
    CHECK(level_info.required_element_count == 1u);
    CHECK(top_depth == 1.0f);
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            snapshot,
            10u,
            &level_info,
            NULL,
            0u
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );

    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            bounds,
            2u,
            first_visibility,
            &first_query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(first_visibility[0] == SOC_VISIBILITY_VISIBLE);
    CHECK(first_visibility[1] == SOC_VISIBILITY_UNKNOWN);
    CHECK(first_query_stats.tested_aabb_count == 2u);
    CHECK(first_query_stats.visible_aabb_count == 1u);
    CHECK(first_query_stats.occluded_aabb_count == 0u);
    CHECK(first_query_stats.unknown_aabb_count == 1u);

    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            bounds,
            1u,
            &second_visibility,
            &second_query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(second_visibility == SOC_VISIBILITY_VISIBLE);
    CHECK(second_query_stats.tested_aabb_count == 1u);
    CHECK(second_query_stats.visible_aabb_count == 1u);
    CHECK(second_query_stats.occluded_aabb_count == 0u);
    CHECK(second_query_stats.unknown_aabb_count == 0u);
    CHECK(first_query_stats.tested_aabb_count == 2u);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            bounds,
            1u,
            &second_visibility,
            NULL
        ),
        SOC_RESULT_OK
    );

    CHECK_RESULT(soc_context_resize(context, 640u, 360u), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    mesh = NULL;
    soc_context_destroy(context);
    context = NULL;

    level_info.struct_size = sizeof(level_info);
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            snapshot,
            0u,
            &level_info,
            NULL,
            0u
        ),
        SOC_RESULT_OK
    );
    CHECK(level_info.width == 320u);
    CHECK(level_info.height == 180u);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            bounds,
            1u,
            &second_visibility,
            &second_query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(second_query_stats.tested_aabb_count == 1u);

    soc_snapshot_destroy(snapshot);
    soc_snapshot_destroy(NULL);
    return 0;
}

static int test_empty_build(void)
{
    const soc_frame_desc frame = make_frame_desc();
    const soc_aabb bounds = {
        .min = {-0.25f, -0.25f, 0.25f},
        .max = {0.25f, 0.25f, 0.50f},
    };
    const soc_occlusion_build_desc build_desc =
        make_build_desc(&frame, NULL, 0u);
    soc_build_stats build_stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_query_stats query_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_hiz_level_info level_info = {
        .struct_size = sizeof(soc_hiz_level_info),
    };
    soc_visibility visibility = SOC_VISIBILITY_OCCLUDED;
    soc_context* context = NULL;
    soc_snapshot* snapshot = NULL;

    CHECK_RESULT(create_context(7u, 5u, &context), SOC_RESULT_OK);
    CHECK_RESULT(
        soc_occlusion_build(context, &build_desc, &snapshot),
        SOC_RESULT_OK
    );
    CHECK(snapshot != NULL);
    CHECK_RESULT(
        soc_snapshot_get_build_stats(snapshot, &build_stats),
        SOC_RESULT_OK
    );
    CHECK(build_stats.hiz_level_count == 4u);
    CHECK(build_stats.input_triangle_count == 0u);
    CHECK(build_stats.clipped_triangle_count == 0u);
    CHECK(build_stats.rasterized_triangle_count == 0u);
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            snapshot,
            0u,
            &level_info,
            NULL,
            0u
        ),
        SOC_RESULT_OK
    );
    CHECK(level_info.width == 7u);
    CHECK(level_info.height == 5u);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            NULL,
            0u,
            NULL,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(query_stats.tested_aabb_count == 0u);
    CHECK(query_stats.visible_aabb_count == 0u);
    CHECK(query_stats.occluded_aabb_count == 0u);
    CHECK(query_stats.unknown_aabb_count == 0u);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            &bounds,
            1u,
            &visibility,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(visibility == SOC_VISIBILITY_VISIBLE);
    CHECK(query_stats.tested_aabb_count == 1u);
    CHECK(query_stats.visible_aabb_count == 1u);
    CHECK(query_stats.occluded_aabb_count == 0u);
    CHECK(query_stats.unknown_aabb_count == 0u);

    soc_snapshot_destroy(snapshot);
    soc_context_destroy(context);
    return 0;
}

static int test_error_paths(void)
{
    const soc_mat4 identity = identity_matrix();
    soc_frame_desc frame = make_frame_desc();
    const soc_aabb bounds = {
        .min = {-0.25f, -0.25f, 0.25f},
        .max = {0.25f, 0.25f, 0.50f},
    };
    soc_occluder_group groups[2];
    soc_occlusion_build_desc build_desc;
    soc_build_stats build_stats = {
        .struct_size = sizeof(soc_build_stats),
    };
    soc_query_stats query_stats = {
        .struct_size = sizeof(soc_query_stats),
        .reserved = 17u,
        .tested_aabb_count = 11u,
        .visible_aabb_count = 12u,
        .occluded_aabb_count = 13u,
        .unknown_aabb_count = 14u,
    };
    soc_hiz_level_info level_info = {
        .struct_size = sizeof(soc_hiz_level_info),
    };
    soc_visibility visibility = SOC_VISIBILITY_OCCLUDED;
    soc_context* owner = NULL;
    soc_context* other = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* baseline = NULL;
    soc_snapshot* output = NULL;

    CHECK_RESULT(create_context(32u, 16u, &owner), SOC_RESULT_OK);
    CHECK_RESULT(create_context(32u, 16u, &other), SOC_RESULT_OK);
    CHECK_RESULT(create_triangle_mesh(owner, &mesh), SOC_RESULT_OK);
    CHECK_RESULT(
        soc_context_resize(NULL, 32u, 16u),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_context_resize(owner, 0u, 16u),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_context_resize(owner, 32u, 0u),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_context_resize(
            owner,
            SOC_MAX_RASTER_DIMENSION + 1u,
            16u
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );

    build_desc = make_build_desc(&frame, NULL, 0u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &baseline),
        SOC_RESULT_OK
    );

    output = (soc_snapshot*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_occlusion_build(NULL, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(output == NULL);
    output = (soc_snapshot*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_occlusion_build(owner, NULL, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(output == NULL);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, NULL),
        SOC_RESULT_INVALID_ARGUMENT
    );

    build_desc.struct_size = SOC_OCCLUSION_BUILD_DESC_SIZE_V1 - 1u;
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(output == NULL);
    build_desc = make_build_desc(NULL, NULL, 0u);
    output = (soc_snapshot*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(output == NULL);
    build_desc = make_build_desc(&frame, NULL, 0u);
    build_desc.flags = 1u;
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_UNSUPPORTED
    );
    CHECK(output == NULL);
    frame.struct_size = SOC_FRAME_DESC_SIZE_V1 - 1u;
    build_desc = make_build_desc(&frame, NULL, 0u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    frame.struct_size = sizeof(frame);
    frame.flags = 1u;
    build_desc = make_build_desc(&frame, NULL, 0u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_UNSUPPORTED
    );
    frame.flags = SOC_FRAME_FLAG_NONE;
    frame.clip_depth_range = UINT32_MAX;
    build_desc = make_build_desc(&frame, NULL, 0u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    frame.clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE;
    frame.depth_direction = UINT32_MAX;
    build_desc = make_build_desc(&frame, NULL, 0u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    frame.depth_direction = SOC_DEPTH_FORWARD;
    frame.front_face = UINT32_MAX;
    build_desc = make_build_desc(&frame, NULL, 0u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );

    groups[0].mesh = mesh;
    groups[0].object_to_world = &identity;
    groups[0].instance_count = 1u;
    groups[0].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    frame.front_face = SOC_FRONT_FACE_CCW;
    build_desc = make_build_desc(&frame, groups, 1u);
    CHECK_RESULT(
        soc_occlusion_build(other, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(output == NULL);

    groups[0].mesh = mesh;
    groups[0].object_to_world = NULL;
    groups[0].instance_count = 0u;
    groups[0].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    build_desc = make_build_desc(&frame, groups, 1u);
    CHECK_RESULT(
        soc_occlusion_build(other, &build_desc, &output),
        SOC_RESULT_OK
    );
    CHECK(output != NULL);
    soc_snapshot_destroy(output);
    output = NULL;

    groups[0].mesh = NULL;
    groups[0].object_to_world = &identity;
    groups[0].instance_count = 1u;
    groups[0].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    build_desc = make_build_desc(&frame, groups, 1u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    groups[0].mesh = mesh;
    groups[0].object_to_world = NULL;
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    groups[0].object_to_world = &identity;
    groups[0].flags = 1u;
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_UNSUPPORTED
    );

    build_desc = make_build_desc(&frame, NULL, 1u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    groups[0].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    build_desc = make_build_desc(&frame, groups, 1u);
    build_desc.group_stride = SOC_OCCLUDER_GROUP_SIZE_V1 - 1u;
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );

    groups[0].mesh = mesh;
    groups[0].object_to_world = &identity;
    groups[0].instance_count = 1u;
    groups[0].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    groups[1].mesh = NULL;
    groups[1].object_to_world = &identity;
    groups[1].instance_count = 1u;
    groups[1].flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    build_desc = make_build_desc(&frame, groups, 2u);
    CHECK_RESULT(
        soc_occlusion_build(owner, &build_desc, &output),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(output == NULL);
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            baseline,
            0u,
            &level_info,
            NULL,
            0u
        ),
        SOC_RESULT_OK
    );

    CHECK_RESULT(
        soc_snapshot_get_build_stats(NULL, &build_stats),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_snapshot_get_build_stats(baseline, NULL),
        SOC_RESULT_INVALID_ARGUMENT
    );
    build_stats.struct_size = SOC_BUILD_STATS_SIZE_V1 - 1u;
    CHECK_RESULT(
        soc_snapshot_get_build_stats(baseline, &build_stats),
        SOC_RESULT_INVALID_ARGUMENT
    );
    build_stats.struct_size = sizeof(build_stats);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            NULL,
            &bounds,
            1u,
            &visibility,
            &query_stats
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            baseline,
            NULL,
            1u,
            &visibility,
            &query_stats
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            baseline,
            &bounds,
            1u,
            NULL,
            &query_stats
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(visibility == SOC_VISIBILITY_OCCLUDED);
    CHECK(query_stats.tested_aabb_count == 11u);
    query_stats.struct_size = SOC_QUERY_STATS_SIZE_V1 - 1u;
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            baseline,
            &bounds,
            1u,
            &visibility,
            &query_stats
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(visibility == SOC_VISIBILITY_OCCLUDED);
    CHECK(query_stats.tested_aabb_count == 11u);
    query_stats.struct_size = sizeof(query_stats);
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            NULL,
            0u,
            &level_info,
            NULL,
            0u
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            baseline,
            0u,
            NULL,
            NULL,
            0u
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );

    soc_snapshot_destroy(baseline);
    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(other);
    soc_context_destroy(owner);
    return 0;
}

int main(void)
{
    if (test_build_query_and_snapshot_lifetime() != 0) {
        return 1;
    }
    if (test_empty_build() != 0) {
        return 1;
    }
    return test_error_paths();
}
