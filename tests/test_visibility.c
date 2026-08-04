#include <soc/soc.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define RANDOM_AABB_COUNT 257u

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

static soc_frame_desc make_perspective_frame_desc(
    soc_clip_depth_range clip_depth_range,
    soc_depth_direction depth_direction
)
{
    soc_frame_desc desc = make_frame_desc(depth_direction);

    /*
     * All four conventions use w = world z and a world-space near plane at
     * z = 1. Forward depth maps z = 1 to zero and infinity to one; reversed
     * depth maps z = 1 to one and infinity to zero.
     */
    desc.clip_depth_range = clip_depth_range;
    desc.clip_from_world.col2.w = 1.0f;
    desc.clip_from_world.col3.w = 0.0f;
    if (clip_depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE) {
        desc.clip_from_world.col2.z =
            depth_direction == SOC_DEPTH_FORWARD ? 1.0f : 0.0f;
        desc.clip_from_world.col3.z =
            depth_direction == SOC_DEPTH_FORWARD ? -1.0f : 1.0f;
    } else {
        desc.clip_from_world.col2.z =
            depth_direction == SOC_DEPTH_FORWARD ? 1.0f : -1.0f;
        desc.clip_from_world.col3.z =
            depth_direction == SOC_DEPTH_FORWARD ? -2.0f : 2.0f;
    }
    return desc;
}

static soc_aabb make_aabb(
    float minimum_x,
    float minimum_y,
    float minimum_z,
    float maximum_x,
    float maximum_y,
    float maximum_z
)
{
    const soc_aabb bounds = {
        .min = {minimum_x, minimum_y, minimum_z},
        .max = {maximum_x, maximum_y, maximum_z},
    };
    return bounds;
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
    const float positions[9],
    soc_mesh** out_mesh
)
{
    const uint16_t indices[] = {0u, 1u, 2u};
    const soc_mesh_desc desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_TWO_SIDED,
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

static int check_visibility(
    const soc_visibility* actual,
    const soc_visibility* expected,
    size_t count
)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (actual[index] != expected[index]) {
            fprintf(
                stderr,
                "visibility[%zu] was %u, expected %u\n",
                index,
                (unsigned int)actual[index],
                (unsigned int)expected[index]
            );
            return 1;
        }
    }
    return 0;
}

static int check_query_stats(
    const soc_query_stats* stats,
    uint64_t tested,
    uint64_t visible,
    uint64_t occluded,
    uint64_t unknown
)
{
    if (stats->tested_aabb_count != tested ||
        stats->visible_aabb_count != visible ||
        stats->occluded_aabb_count != occluded ||
        stats->unknown_aabb_count != unknown) {
        fprintf(
            stderr,
            "query stats were tested=%llu, visible=%llu, occluded=%llu, "
            "unknown=%llu; expected tested=%llu, visible=%llu, "
            "occluded=%llu, unknown=%llu\n",
            (unsigned long long)stats->tested_aabb_count,
            (unsigned long long)stats->visible_aabb_count,
            (unsigned long long)stats->occluded_aabb_count,
            (unsigned long long)stats->unknown_aabb_count,
            (unsigned long long)tested,
            (unsigned long long)visible,
            (unsigned long long)occluded,
            (unsigned long long)unknown
        );
        return 1;
    }
    return 0;
}

static soc_result build_empty_snapshot(
    soc_context* context,
    const soc_frame_desc* frame_desc,
    soc_snapshot** out_snapshot
)
{
    const soc_occlusion_build_desc build_desc = {
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = frame_desc,
        .groups = NULL,
        .group_count = 0u,
        .group_stride = 0u,
    };

    return soc_occlusion_build(context, &build_desc, out_snapshot);
}

static soc_result build_single_group_snapshot(
    soc_context* context,
    const soc_frame_desc* frame_desc,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    soc_snapshot** out_snapshot
)
{
    const soc_occluder_group group = {
        .mesh = mesh,
        .object_to_world = object_to_world,
        .instance_count = 1u,
        .flags = SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    const soc_occlusion_build_desc build_desc = {
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = frame_desc,
        .groups = &group,
        .group_count = 1u,
        .group_stride = sizeof(group),
    };

    return soc_occlusion_build(context, &build_desc, out_snapshot);
}

static int test_empty_frame_and_fail_open_inputs(void)
{
    soc_frame_desc frame_desc = make_frame_desc(SOC_DEPTH_FORWARD);
    soc_aabb bounds[] = {
        make_aabb(-0.25f, -0.25f, 0.20f, 0.25f, 0.25f, 0.30f),
        make_aabb(0.25f, -0.25f, 0.20f, -0.25f, 0.25f, 0.30f),
        make_aabb(-0.25f, -0.25f, 0.20f, 0.25f, 0.25f, 0.30f),
        make_aabb(-0.25f, -0.25f, 0.20f, 0.25f, 0.25f, 0.30f),
        make_aabb(-0.25f, -0.25f, -0.10f, 0.25f, 0.25f, 0.10f),
        make_aabb(0.80f, -0.25f, 0.20f, 1.20f, 0.25f, 0.30f),
        make_aabb(1.20f, -0.25f, 0.20f, 1.50f, 0.25f, 0.30f),
        make_aabb(1.20f, -0.25f, -0.10f, 1.50f, 0.25f, 0.10f),
    };
    const soc_visibility expected[] = {
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_UNKNOWN,
    };
    soc_visibility actual[ARRAY_COUNT(bounds)];
    const soc_aabb finite_bounds =
        make_aabb(-0.1f, -0.1f, 0.2f, 0.1f, 0.1f, 0.3f);
    const soc_aabb reversed_near_crossing =
        make_aabb(-0.1f, -0.1f, 0.9f, 0.1f, 0.1f, 1.1f);
    const soc_aabb origin = make_aabb(
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
    );
    soc_visibility w_zero_visibility = SOC_VISIBILITY_OCCLUDED;
    soc_visibility tiny_w_visibility = SOC_VISIBILITY_OCCLUDED;
    soc_visibility reversed_near_visibility = SOC_VISIBILITY_OCCLUDED;
    soc_query_stats query_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_context* context = NULL;
    soc_snapshot* snapshot = NULL;
    size_t index;

    bounds[2].min.x = NAN;
    bounds[3].max.y = INFINITY;
    for (index = 0u; index < ARRAY_COUNT(actual); ++index) {
        actual[index] = SOC_VISIBILITY_OCCLUDED;
    }

    CHECK_RESULT(create_context(7u, 5u, &context), SOC_RESULT_OK);
    CHECK_RESULT(
        build_empty_snapshot(context, &frame_desc, &snapshot),
        SOC_RESULT_OK
    );

    CHECK_RESULT(
        soc_snapshot_test_aabbs(snapshot, NULL, 0u, NULL, &query_stats),
        SOC_RESULT_OK
    );
    CHECK(check_query_stats(&query_stats, 0u, 0u, 0u, 0u) == 0);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(snapshot, NULL, 1u, actual, &query_stats),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(snapshot, bounds, 1u, NULL, &query_stats),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(check_query_stats(&query_stats, 0u, 0u, 0u, 0u) == 0);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            bounds,
            (uint32_t)ARRAY_COUNT(bounds),
            actual,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(check_visibility(
        actual,
        expected,
        ARRAY_COUNT(expected)
    ) == 0);
    CHECK(check_query_stats(
        &query_stats,
        ARRAY_COUNT(bounds),
        3u,
        0u,
        5u
    ) == 0);
    soc_snapshot_destroy(snapshot);
    snapshot = NULL;

    /*
     * A finite, ordered AABB is still fail-open when its homogeneous
     * projection cannot produce a positive W.
     */
    frame_desc.clip_from_world.col3.w = 0.0f;
    CHECK_RESULT(
        build_empty_snapshot(context, &frame_desc, &snapshot),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            &finite_bounds,
            1u,
            &w_zero_visibility,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(w_zero_visibility == SOC_VISIBILITY_UNKNOWN);
    CHECK(check_query_stats(&query_stats, 1u, 0u, 0u, 1u) == 0);
    soc_snapshot_destroy(snapshot);
    snapshot = NULL;

    /*
     * A positive W smaller than the transform error margin must expand the
     * projection conservatively. A negative safety margin could otherwise
     * invert the pixel bounds and incorrectly report occlusion.
     */
    frame_desc = make_frame_desc(SOC_DEPTH_FORWARD);
    frame_desc.clip_from_world.col2.z = 0.0f;
    frame_desc.clip_from_world.col3.z = 5.0e-16f;
    frame_desc.clip_from_world.col2.w = 0.0f;
    frame_desc.clip_from_world.col3.w = 1.0e-15f;
    CHECK_RESULT(
        build_empty_snapshot(context, &frame_desc, &snapshot),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            &origin,
            1u,
            &tiny_w_visibility,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(tiny_w_visibility == SOC_VISIBILITY_VISIBLE);
    CHECK(check_query_stats(&query_stats, 1u, 1u, 0u, 0u) == 0);
    soc_snapshot_destroy(snapshot);
    snapshot = NULL;

    frame_desc = make_frame_desc(SOC_DEPTH_REVERSED);
    CHECK_RESULT(
        build_empty_snapshot(context, &frame_desc, &snapshot),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            &reversed_near_crossing,
            1u,
            &reversed_near_visibility,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(reversed_near_visibility == SOC_VISIBILITY_UNKNOWN);
    CHECK(check_query_stats(&query_stats, 1u, 0u, 0u, 1u) == 0);
    soc_snapshot_destroy(snapshot);

    soc_context_destroy(context);
    return 0;
}

static int test_fullscreen_depth_direction(
    soc_depth_direction depth_direction,
    soc_clip_depth_range clip_depth_range
)
{
    const float occluder_depth =
        clip_depth_range == SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
            ? 0.0f
            : 0.50f;
    const float positions[] = {
        -1.0f, -1.0f, occluder_depth,
         3.0f, -1.0f, occluder_depth,
        -1.0f,  3.0f, occluder_depth,
    };
    const soc_mat4 identity = identity_matrix();
    soc_frame_desc frame_desc = make_frame_desc(depth_direction);
    soc_aabb first_batch[3];
    soc_aabb second_batch[2];
    const soc_visibility first_expected[] = {
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_OCCLUDED,
    };
    const soc_visibility second_expected[] = {
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_VISIBLE,
    };
    soc_visibility first_actual[ARRAY_COUNT(first_batch)] = {0};
    soc_visibility second_actual[ARRAY_COUNT(second_batch)] = {0};
    soc_query_stats first_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_query_stats second_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* snapshot = NULL;

    frame_desc.clip_depth_range = clip_depth_range;
    if (clip_depth_range == SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE &&
        depth_direction == SOC_DEPTH_FORWARD) {
        first_batch[0] =
            make_aabb(-0.20f, -0.20f, -0.60f, 0.20f, 0.20f, -0.40f);
        first_batch[1] =
            make_aabb(-0.55f, -0.55f, 0.00f, 0.55f, 0.55f, 0.30f);
        first_batch[2] =
            make_aabb(-0.85f, -0.85f, 0.40f, 0.85f, 0.85f, 0.60f);
    } else if (
        clip_depth_range == SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
    ) {
        first_batch[0] =
            make_aabb(-0.20f, -0.20f, 0.40f, 0.20f, 0.20f, 0.60f);
        first_batch[1] =
            make_aabb(-0.55f, -0.55f, -0.30f, 0.55f, 0.55f, 0.00f);
        first_batch[2] =
            make_aabb(-0.85f, -0.85f, -0.60f, 0.85f, 0.85f, -0.40f);
    } else if (depth_direction == SOC_DEPTH_FORWARD) {
        first_batch[0] =
            make_aabb(-0.20f, -0.20f, 0.20f, 0.20f, 0.20f, 0.30f);
        first_batch[1] =
            make_aabb(-0.55f, -0.55f, 0.50f, 0.55f, 0.55f, 0.65f);
        first_batch[2] =
            make_aabb(-0.85f, -0.85f, 0.70f, 0.85f, 0.85f, 0.80f);
    } else {
        first_batch[0] =
            make_aabb(-0.20f, -0.20f, 0.70f, 0.20f, 0.20f, 0.80f);
        first_batch[1] =
            make_aabb(-0.55f, -0.55f, 0.35f, 0.55f, 0.55f, 0.50f);
        first_batch[2] =
            make_aabb(-0.85f, -0.85f, 0.20f, 0.85f, 0.85f, 0.30f);
    }
    second_batch[0] = first_batch[2];
    second_batch[1] = first_batch[0];

    CHECK_RESULT(create_context(7u, 5u, &context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_triangle_mesh(context, positions, &mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        build_single_group_snapshot(
            context,
            &frame_desc,
            mesh,
            &identity,
            &snapshot
        ),
        SOC_RESULT_OK
    );

    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            first_batch,
            (uint32_t)ARRAY_COUNT(first_batch),
            first_actual,
            &first_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(check_visibility(
        first_actual,
        first_expected,
        ARRAY_COUNT(first_expected)
    ) == 0);
    CHECK(check_query_stats(&first_stats, 3u, 2u, 1u, 0u) == 0);

    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            second_batch,
            (uint32_t)ARRAY_COUNT(second_batch),
            second_actual,
            &second_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(check_visibility(
        second_actual,
        second_expected,
        ARRAY_COUNT(second_expected)
    ) == 0);
    CHECK(check_query_stats(&second_stats, 2u, 1u, 1u, 0u) == 0);

    soc_snapshot_destroy(snapshot);
    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_forward_and_reversed_fullscreen_occlusion(void)
{
    if (test_fullscreen_depth_direction(
            SOC_DEPTH_FORWARD,
            SOC_CLIP_DEPTH_ZERO_TO_ONE
        ) != 0) {
        return 1;
    }
    if (test_fullscreen_depth_direction(
            SOC_DEPTH_REVERSED,
            SOC_CLIP_DEPTH_ZERO_TO_ONE
        ) != 0) {
        return 1;
    }
    return 0;
}

static int test_negative_one_to_one_depth_mapping(void)
{
    if (test_fullscreen_depth_direction(
            SOC_DEPTH_FORWARD,
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
        ) != 0) {
        return 1;
    }
    if (test_fullscreen_depth_direction(
            SOC_DEPTH_REVERSED,
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
        ) != 0) {
        return 1;
    }
    return 0;
}

static int test_perspective_convention_batch(
    soc_clip_depth_range clip_depth_range,
    soc_depth_direction depth_direction
)
{
    const float positions[] = {
        -2.0f, -2.0f, 2.0f,
         6.0f, -2.0f, 2.0f,
        -2.0f,  6.0f, 2.0f,
    };
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame_desc = make_perspective_frame_desc(
        clip_depth_range,
        depth_direction
    );
    soc_aabb bounds[] = {
        make_aabb(-0.20f, -0.20f, 1.20f, 0.20f, 0.20f, 1.50f),
        make_aabb(-0.80f, -0.80f, 3.00f, 0.80f, 0.80f, 4.00f),
        make_aabb(5.00f, -0.25f, 2.00f, 6.00f, 0.25f, 3.00f),
        make_aabb(-0.10f, -0.10f, 0.80f, 0.10f, 0.10f, 1.20f),
        make_aabb(-0.10f, -0.10f, -2.00f, 0.10f, 0.10f, -0.50f),
        make_aabb(0.00f, 0.00f, 3.50f, 0.00f, 0.00f, 3.50f),
        make_aabb(0.20f, -0.20f, 3.00f, -0.20f, 0.20f, 4.00f),
        make_aabb(-0.20f, -0.20f, 3.00f, 0.20f, 0.20f, 4.00f),
        make_aabb(-0.20f, -0.20f, 3.00f, 0.20f, 0.20f, 4.00f),
    };
    const soc_visibility expected[] = {
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_UNKNOWN,
    };
    soc_visibility actual[ARRAY_COUNT(bounds)] = {0};
    soc_query_stats query_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* snapshot = NULL;

    bounds[7].min.x = NAN;
    bounds[8].max.y = INFINITY;

    CHECK_RESULT(create_context(9u, 6u, &context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_triangle_mesh(context, positions, &mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        build_single_group_snapshot(
            context,
            &frame_desc,
            mesh,
            &identity,
            &snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            bounds,
            (uint32_t)ARRAY_COUNT(bounds),
            actual,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(check_visibility(
        actual,
        expected,
        ARRAY_COUNT(expected)
    ) == 0);
    CHECK(check_query_stats(
        &query_stats,
        ARRAY_COUNT(bounds),
        2u,
        2u,
        5u
    ) == 0);

    soc_snapshot_destroy(snapshot);
    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_perspective_projection_conventions(void)
{
    const soc_clip_depth_range clip_depth_ranges[] = {
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
    };
    const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
    };
    size_t range_index;
    size_t direction_index;

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        for (direction_index = 0u;
             direction_index < ARRAY_COUNT(depth_directions);
             ++direction_index) {
            if (test_perspective_convention_batch(
                    clip_depth_ranges[range_index],
                    depth_directions[direction_index]
                ) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static uint32_t deterministic_random_u32(uint32_t* state)
{
    uint32_t value = *state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static float deterministic_random_unit(uint32_t* state)
{
    return (float)(deterministic_random_u32(state) >> 8u) *
        (1.0f / 16777216.0f);
}

static void make_deterministic_random_aabbs(
    soc_aabb bounds[RANDOM_AABB_COUNT]
)
{
    uint32_t state = UINT32_C(0x534F4301);
    uint32_t index;

    for (index = 0u; index < RANDOM_AABB_COUNT; ++index) {
        const float random_x = deterministic_random_unit(&state);
        const float random_y = deterministic_random_unit(&state);
        const float random_z = deterministic_random_unit(&state);
        const float radius =
            0.005f + 0.12f * deterministic_random_unit(&state);
        const float centered_x = (random_x * 2.0f - 1.0f) * 0.75f;
        const float centered_y = (random_y * 2.0f - 1.0f) * 0.75f;

        switch (index % 10u) {
        case 0u: {
            const float minimum_z = 2.50f + random_z * 1.50f;

            bounds[index] = make_aabb(
                centered_x - radius,
                centered_y - radius,
                minimum_z,
                centered_x + radius,
                centered_y + radius,
                minimum_z + 0.20f
            );
            break;
        }
        case 1u: {
            const float minimum_z = 1.10f + random_z * 0.30f;

            bounds[index] = make_aabb(
                centered_x - radius,
                centered_y - radius,
                minimum_z,
                centered_x + radius,
                centered_y + radius,
                minimum_z + 0.20f
            );
            break;
        }
        case 2u:
            bounds[index] = make_aabb(
                5.0f + random_x,
                centered_y - radius,
                2.0f,
                6.0f + random_x,
                centered_y + radius,
                3.0f
            );
            break;
        case 3u:
            bounds[index] = make_aabb(
                centered_x - radius,
                centered_y - radius,
                0.80f,
                centered_x + radius,
                centered_y + radius,
                1.20f
            );
            break;
        case 4u:
            bounds[index] = make_aabb(
                centered_x - radius,
                centered_y - radius,
                -2.0f,
                centered_x + radius,
                centered_y + radius,
                -0.5f
            );
            break;
        case 5u: {
            const float minimum_z = 1.05f + random_z * 3.50f;

            bounds[index] = make_aabb(
                centered_x - radius,
                centered_y - radius,
                minimum_z,
                centered_x + radius,
                centered_y + radius,
                minimum_z + radius
            );
            break;
        }
        case 6u:
            bounds[index] = make_aabb(
                centered_x,
                centered_y,
                3.0f,
                centered_x,
                centered_y,
                3.0f
            );
            break;
        case 7u:
            bounds[index] = make_aabb(
                centered_x + radius,
                centered_y - radius,
                2.0f,
                centered_x - radius,
                centered_y + radius,
                3.0f
            );
            break;
        case 8u:
            bounds[index] = make_aabb(
                NAN,
                centered_y - radius,
                2.0f,
                centered_x + radius,
                centered_y + radius,
                3.0f
            );
            break;
        default:
            bounds[index] = make_aabb(
                centered_x - radius,
                centered_y - radius,
                2.0f,
                centered_x + radius,
                INFINITY,
                3.0f
            );
            break;
        }
    }
}

static soc_visibility expected_deterministic_visibility(
    uint32_t index,
    const soc_aabb* bounds
)
{
    switch (index % 10u) {
    case 0u:
    case 6u:
        return SOC_VISIBILITY_OCCLUDED;
    case 1u:
    case 2u:
        return SOC_VISIBILITY_VISIBLE;
    case 5u:
        return bounds->min.z <= 2.0f
            ? SOC_VISIBILITY_VISIBLE
            : SOC_VISIBILITY_OCCLUDED;
    default:
        return SOC_VISIBILITY_UNKNOWN;
    }
}

/*
 * Public-API consistency and conservative-behavior coverage. This deliberately
 * does not claim to be an implementation differential: bulk, singleton, and
 * stats-free calls currently dispatch through the same library path.
 */
static int test_bulk_query_consistency_for_convention(
    soc_clip_depth_range clip_depth_range,
    soc_depth_direction depth_direction
)
{
    const float positions[] = {
        -2.0f, -2.0f, 2.0f,
         6.0f, -2.0f, 2.0f,
        -2.0f,  6.0f, 2.0f,
    };
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame_desc = make_perspective_frame_desc(
        clip_depth_range,
        depth_direction
    );
    soc_aabb bounds[RANDOM_AABB_COUNT];
    soc_visibility bulk[RANDOM_AABB_COUNT] = {0};
    soc_visibility repeated[RANDOM_AABB_COUNT] = {0};
    soc_visibility without_stats[RANDOM_AABB_COUNT] = {0};
    soc_visibility empty[RANDOM_AABB_COUNT] = {0};
    soc_query_stats bulk_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_query_stats repeated_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_query_stats empty_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* occluded_snapshot = NULL;
    soc_snapshot* empty_snapshot = NULL;
    uint64_t visible_count = 0u;
    uint64_t occluded_count = 0u;
    uint64_t unknown_count = 0u;
    uint64_t empty_visible_count = 0u;
    uint64_t empty_unknown_count = 0u;
    uint32_t index;

    make_deterministic_random_aabbs(bounds);
    CHECK_RESULT(create_context(31u, 19u, &context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_triangle_mesh(context, positions, &mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        build_single_group_snapshot(
            context,
            &frame_desc,
            mesh,
            &identity,
            &occluded_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        build_empty_snapshot(context, &frame_desc, &empty_snapshot),
        SOC_RESULT_OK
    );

    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            occluded_snapshot,
            bounds,
            RANDOM_AABB_COUNT,
            bulk,
            &bulk_stats
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            occluded_snapshot,
            bounds,
            RANDOM_AABB_COUNT,
            repeated,
            &repeated_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(check_visibility(repeated, bulk, RANDOM_AABB_COUNT) == 0);

    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            occluded_snapshot,
            bounds,
            RANDOM_AABB_COUNT,
            without_stats,
            NULL
        ),
        SOC_RESULT_OK
    );
    CHECK(check_visibility(without_stats, bulk, RANDOM_AABB_COUNT) == 0);

    for (index = 0u; index < RANDOM_AABB_COUNT; ++index) {
        const soc_visibility expected = expected_deterministic_visibility(
            index,
            &bounds[index]
        );
        soc_visibility scalar = SOC_VISIBILITY_UNKNOWN;
        soc_query_stats scalar_stats = {
            .struct_size = sizeof(soc_query_stats),
        };
        uint64_t scalar_visible;
        uint64_t scalar_occluded;
        uint64_t scalar_unknown;

        CHECK_RESULT(
            soc_snapshot_test_aabbs(
                occluded_snapshot,
                &bounds[index],
                1u,
                &scalar,
                &scalar_stats
            ),
            SOC_RESULT_OK
        );
        if (bulk[index] != expected) {
            fprintf(
                stderr,
                "random visibility[%u] was %u, expected %u "
                "(clip range %u, depth direction %u)\n",
                index,
                (unsigned int)bulk[index],
                (unsigned int)expected,
                (unsigned int)clip_depth_range,
                (unsigned int)depth_direction
            );
            return 1;
        }
        CHECK(scalar == bulk[index]);
        CHECK(scalar == SOC_VISIBILITY_VISIBLE ||
            scalar == SOC_VISIBILITY_OCCLUDED ||
            scalar == SOC_VISIBILITY_UNKNOWN);

        scalar_visible = scalar == SOC_VISIBILITY_VISIBLE ? 1u : 0u;
        scalar_occluded = scalar == SOC_VISIBILITY_OCCLUDED ? 1u : 0u;
        scalar_unknown = scalar == SOC_VISIBILITY_UNKNOWN ? 1u : 0u;
        CHECK(check_query_stats(
            &scalar_stats,
            1u,
            scalar_visible,
            scalar_occluded,
            scalar_unknown
        ) == 0);
        visible_count += scalar_visible;
        occluded_count += scalar_occluded;
        unknown_count += scalar_unknown;
    }
    CHECK(visible_count > 0u);
    CHECK(occluded_count > 0u);
    CHECK(unknown_count > 0u);
    CHECK(check_query_stats(
        &bulk_stats,
        RANDOM_AABB_COUNT,
        visible_count,
        occluded_count,
        unknown_count
    ) == 0);
    CHECK(check_query_stats(
        &repeated_stats,
        RANDOM_AABB_COUNT,
        visible_count,
        occluded_count,
        unknown_count
    ) == 0);

    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            empty_snapshot,
            bounds,
            RANDOM_AABB_COUNT,
            empty,
            &empty_stats
        ),
        SOC_RESULT_OK
    );
    for (index = 0u; index < RANDOM_AABB_COUNT; ++index) {
        CHECK(empty[index] != SOC_VISIBILITY_OCCLUDED);
        if (empty[index] == SOC_VISIBILITY_UNKNOWN) {
            ++empty_unknown_count;
            CHECK(bulk[index] == SOC_VISIBILITY_UNKNOWN);
        } else {
            CHECK(empty[index] == SOC_VISIBILITY_VISIBLE);
            ++empty_visible_count;
            CHECK(bulk[index] != SOC_VISIBILITY_UNKNOWN);
        }
        if (bulk[index] == SOC_VISIBILITY_OCCLUDED) {
            CHECK(empty[index] == SOC_VISIBILITY_VISIBLE);
        }
    }
    CHECK(check_query_stats(
        &empty_stats,
        RANDOM_AABB_COUNT,
        empty_visible_count,
        0u,
        empty_unknown_count
    ) == 0);

    soc_snapshot_destroy(empty_snapshot);
    soc_snapshot_destroy(occluded_snapshot);
    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

static int test_deterministic_bulk_query_consistency_and_conservatism(void)
{
    const soc_clip_depth_range clip_depth_ranges[] = {
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
    };
    const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
    };
    size_t range_index;
    size_t direction_index;

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        for (direction_index = 0u;
             direction_index < ARRAY_COUNT(depth_directions);
             ++direction_index) {
            if (test_bulk_query_consistency_for_convention(
                    clip_depth_ranges[range_index],
                    depth_directions[direction_index]
                ) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int test_partial_coverage_and_offscreen_are_conservative(void)
{
    const float positions[] = {
        -0.80f, -0.80f, 0.35f,
         0.80f, -0.80f, 0.35f,
         0.00f,  0.80f, 0.35f,
    };
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame_desc = make_frame_desc(SOC_DEPTH_FORWARD);
    const soc_aabb bounds[] = {
        {
            .min = {-0.10f, -0.10f, 0.70f},
            .max = {0.10f, 0.10f, 0.80f},
        },
        {
            .min = {-0.60f, -0.60f, 0.70f},
            .max = {0.60f, 0.60f, 0.80f},
        },
        {
            .min = {0.90f, -0.20f, 0.70f},
            .max = {1.20f, 0.20f, 0.80f},
        },
        {
            .min = {1.20f, -0.20f, 0.70f},
            .max = {1.50f, 0.20f, 0.80f},
        },
    };
    const soc_visibility expected[] = {
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_VISIBLE,
    };
    soc_visibility actual[ARRAY_COUNT(bounds)] = {0};
    soc_query_stats query_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* snapshot = NULL;

    CHECK_RESULT(create_context(7u, 5u, &context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_triangle_mesh(context, positions, &mesh),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        build_single_group_snapshot(
            context,
            &frame_desc,
            mesh,
            &identity,
            &snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            snapshot,
            bounds,
            (uint32_t)ARRAY_COUNT(bounds),
            actual,
            &query_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(check_visibility(
        actual,
        expected,
        ARRAY_COUNT(expected)
    ) == 0);
    CHECK(check_query_stats(
        &query_stats,
        ARRAY_COUNT(bounds),
        3u,
        1u,
        0u
    ) == 0);

    soc_snapshot_destroy(snapshot);
    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    soc_context_destroy(context);
    return 0;
}

int main(void)
{
    if (test_empty_frame_and_fail_open_inputs() != 0) {
        return 1;
    }
    if (test_forward_and_reversed_fullscreen_occlusion() != 0) {
        return 1;
    }
    if (test_negative_one_to_one_depth_mapping() != 0) {
        return 1;
    }
    if (test_perspective_projection_conventions() != 0) {
        return 1;
    }
    if (test_deterministic_bulk_query_consistency_and_conservatism() != 0) {
        return 1;
    }
    if (test_partial_coverage_and_offscreen_are_conservative() != 0) {
        return 1;
    }
    return 0;
}
