#include <soc/soc.h>

#include "core/soc_pipeline.h"
#include "core/soc_snapshot.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define QUERY_COUNT 4096u
#define TEST_WIDTH 101u
#define TEST_HEIGHT 53u
#define TEST_PIXEL_COUNT (TEST_WIDTH * TEST_HEIGHT)
#define PARALLEL_TRIANGLE_COUNT 4098u
#define PARALLEL_INDEX_COUNT (PARALLEL_TRIANGLE_COUNT * 3u)
#define PARALLEL_REPEAT_COUNT 8u

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

typedef struct test_scene {
    soc_context* context;
    soc_mesh* full;
    soc_mesh* left;
    soc_mesh* center;
    soc_mesh* far_strip;
    soc_mesh* layered;
} test_scene;

static soc_aabb query_bounds[QUERY_COUNT];
static soc_visibility masked_visibility[QUERY_COUNT];
static soc_visibility reference_visibility[QUERY_COUNT];
static soc_visibility parallel_visibility[QUERY_COUNT];
static float masked_depth_pixels[TEST_PIXEL_COUNT];
static float reference_depth_pixels[TEST_PIXEL_COUNT];

static const soc_mat4 identity_transform = {
    .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
    .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
    .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
    .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
};

static soc_frame_desc make_reverse_z_frame(void)
{
    const soc_frame_desc frame = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .col2 = {0.0f, 0.0f, 0.0f, 1.0f},
            .col3 = {0.0f, 0.0f, 1.0f, 0.0f},
        },
        .clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    return frame;
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

static uint32_t next_random(uint32_t* state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static float random_unit(uint32_t* state)
{
    return (float)(next_random(state) >> 8u) *
        (1.0f / 16777216.0f);
}

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void initialize_queries(void)
{
    static const soc_aabb fixed[] = {
        {{-0.20f, -0.20f, 3.00f}, {0.20f, 0.20f, 3.40f}},
        {{-1.80f, -1.20f, 3.00f}, {-1.55f, -0.95f, 3.25f}},
        {{1.55f, 0.95f, 3.00f}, {1.80f, 1.20f, 3.25f}},
        {{-2.95f, -0.12f, 3.00f}, {-2.75f, 0.12f, 3.20f}},
        {{2.75f, -0.12f, 3.00f}, {2.95f, 0.12f, 3.20f}},
        {{-0.15f, -2.95f, 3.00f}, {0.15f, -2.75f, 3.20f}},
        {{-0.15f, 2.75f, 3.00f}, {0.15f, 2.95f, 3.20f}},
        {{-0.20f, -0.20f, 1.02f}, {0.20f, 0.20f, 1.12f}},
        {{-1.05f, -0.15f, 1.05f}, {-0.85f, 0.15f, 1.15f}},
        {{0.85f, -0.15f, 1.05f}, {1.05f, 0.15f, 1.15f}},
        {{-0.80f, -0.30f, 1.72f}, {-0.55f, -0.05f, 1.92f}},
        {{0.45f, 0.25f, 1.72f}, {0.70f, 0.50f, 1.92f}},
    };
    uint32_t state = UINT32_C(0x534f4301);
    uint32_t index;

    for (index = 0u; index < (uint32_t)ARRAY_COUNT(fixed); ++index) {
        query_bounds[index] = fixed[index];
    }
    for (; index < QUERY_COUNT; ++index) {
        const float center_ndc_x = random_unit(&state) * 1.90f - 0.95f;
        const float center_ndc_y = random_unit(&state) * 1.90f - 0.95f;
        const float half_ndc_x = 0.004f + random_unit(&state) * 0.11f;
        const float half_ndc_y = 0.004f + random_unit(&state) * 0.11f;
        float minimum_z;
        float maximum_z;
        float center_x;
        float center_y;
        float half_x;
        float half_y;

        switch (index % 4u) {
        case 0u:
        case 1u:
            minimum_z = 2.20f + random_unit(&state) * 4.80f;
            maximum_z = minimum_z + 0.03f + random_unit(&state) * 0.55f;
            break;
        case 2u:
            minimum_z = 1.01f + random_unit(&state) * 0.25f;
            maximum_z = minimum_z + 0.02f + random_unit(&state) * 0.10f;
            break;
        default:
            minimum_z = 1.28f + random_unit(&state) * 1.25f;
            maximum_z = minimum_z + 0.03f + random_unit(&state) * 0.30f;
            break;
        }

        center_x = center_ndc_x * minimum_z;
        center_y = center_ndc_y * minimum_z;
        half_x = half_ndc_x * minimum_z;
        half_y = half_ndc_y * minimum_z;
        query_bounds[index] = make_aabb(
            center_x - half_x,
            center_y - half_y,
            minimum_z,
            center_x + half_x,
            center_y + half_y,
            maximum_z
        );
    }
}

static soc_result create_mesh(
    soc_context* context,
    const float* positions,
    uint32_t vertex_count,
    const uint16_t* indices,
    uint32_t index_count,
    soc_mesh** out_mesh
)
{
    const soc_mesh_desc desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_TWO_SIDED,
        .vertices = positions,
        .indices = indices,
        .vertex_count = vertex_count,
        .vertex_stride = 3u * sizeof(float),
        .position_offset = 0u,
        .index_count = index_count,
        .index_type = SOC_INDEX_UINT16,
    };
    return soc_mesh_create(context, &desc, out_mesh);
}

typedef struct reverse_z_screen_vertex {
    float x;
    float y;
    float world_z;
} reverse_z_screen_vertex;

static void write_reverse_z_screen_vertex(
    float* destination,
    const reverse_z_screen_vertex* source
)
{
    const float ndc_x = 2.0f * source->x / (float)TEST_WIDTH - 1.0f;
    const float ndc_y = 1.0f -
        2.0f * source->y / (float)TEST_HEIGHT;

    destination[0] = ndc_x * source->world_z;
    destination[1] = ndc_y * source->world_z;
    destination[2] = source->world_z;
}

static soc_result create_parallel_order_mesh(
    soc_context* context,
    soc_bool reverse_order,
    soc_mesh** out_mesh
)
{
    static const reverse_z_screen_vertex patterns[6][3] = {
        {
            {0.10f, 0.10f, 2.80f},
            {100.90f, 0.10f, 2.80f},
            {0.10f, 52.90f, 2.80f},
        },
        {
            {100.90f, 52.90f, 1.25f},
            {0.10f, 52.90f, 1.25f},
            {100.90f, 0.10f, 1.25f},
        },
        {
            {0.25f, 0.25f, 1.35f},
            {100.75f, 52.75f, 2.60f},
            {0.25f, 52.75f, 1.70f},
        },
        {
            {100.75f, 0.25f, 2.20f},
            {0.25f, 0.25f, 1.15f},
            {100.75f, 52.75f, 1.60f},
        },
        {
            {31.10f, 0.25f, 1.30f},
            {33.40f, 52.75f, 2.40f},
            {28.70f, 52.75f, 1.80f},
        },
        {
            {94.20f, 28.10f, 2.70f},
            {100.90f, 52.90f, 1.25f},
            {94.10f, 52.90f, 1.90f},
        },
    };
    float positions[6u * 9u];
    uint16_t* indices = malloc(
        (size_t)PARALLEL_INDEX_COUNT * sizeof(*indices)
    );
    soc_result result;
    uint32_t pattern;
    uint32_t triangle;

    if (indices == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    for (pattern = 0u; pattern < 6u; ++pattern) {
        uint32_t vertex;

        for (vertex = 0u; vertex < 3u; ++vertex) {
            write_reverse_z_screen_vertex(
                positions + (size_t)(pattern * 3u + vertex) * 3u,
                &patterns[pattern][vertex]
            );
        }
    }
    for (triangle = 0u;
         triangle < PARALLEL_TRIANGLE_COUNT;
         ++triangle) {
        const uint32_t ordered_triangle = reverse_order == SOC_TRUE
            ? PARALLEL_TRIANGLE_COUNT - 1u - triangle
            : triangle;
        const uint32_t ordered_pattern = ordered_triangle < 4096u
            ? ordered_triangle / 1024u
            : 4u + ordered_triangle - 4096u;
        const uint16_t first_vertex =
            (uint16_t)(ordered_pattern * 3u);
        const size_t first_index = (size_t)triangle * 3u;

        indices[first_index] = first_vertex;
        indices[first_index + 1u] = (uint16_t)(first_vertex + 1u);
        indices[first_index + 2u] = (uint16_t)(first_vertex + 2u);
    }
    result = create_mesh(
        context,
        positions,
        18u,
        indices,
        PARALLEL_INDEX_COUNT,
        out_mesh
    );
    free(indices);
    return result;
}

static soc_result create_test_context(
    uint32_t worker_count,
    soc_context** out_context
)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .worker_count = worker_count,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    return soc_context_create(&config, out_context);
}

static soc_bool dense_level_zero_proves_occluded(
    const soc_snapshot* reference,
    const soc_aabb* bounds
)
{
    soc_hiz level_zero = reference->depth_pyramid;
    soc_projected_aabb projected;

    if (soc_project_aabb_scalar(
            &reference->query_context,
            bounds,
            &projected
        ) != SOC_AABB_PROJECTION_VALID) {
        return SOC_FALSE;
    }

    level_zero.level_count = 1u;
    return soc_test_projected_aabb_dense_scalar(
        &level_zero,
        &projected
    ) == SOC_VISIBILITY_OCCLUDED
        ? SOC_TRUE
        : SOC_FALSE;
}

static int initialize_scene(test_scene* scene)
{
    static const uint16_t quad_indices[] = {
        0u, 1u, 2u,
        0u, 2u, 3u,
    };
    static const uint16_t layered_indices[] = {
         0u,  1u,  2u,
         3u,  4u,  5u,
         6u,  7u,  8u,
         9u, 10u, 11u,
        12u, 13u, 14u,
        15u, 16u, 17u,
    };
    static const float full_positions[] = {
        -2.00f, -2.00f, 2.00f,
         2.00f, -2.00f, 2.00f,
         2.00f,  2.00f, 2.00f,
        -2.00f,  2.00f, 2.00f,
    };
    static const float left_positions[] = {
        -1.2825f, -1.1475f, 1.35f,
         0.2025f, -1.1475f, 1.35f,
         0.2025f,  1.0125f, 1.35f,
        -1.2825f,  1.0125f, 1.35f,
    };
    static const float center_positions[] = {
        -0.3300f, -0.9075f, 1.65f,
         1.4850f, -0.9075f, 1.65f,
         1.4850f,  1.4850f, 1.65f,
        -0.3300f,  1.4850f, 1.65f,
    };
    static const float far_strip_positions[] = {
        -2.6600f, -0.5040f, 2.80f,
         2.6600f, -0.5040f, 2.80f,
         2.6600f,  0.5040f, 2.80f,
        -2.6600f,  0.5040f, 2.80f,
    };
    /*
     * Six partially overlapping triangles with perspective-varying depth.
     * The first three overlap near the viewport center at three distinct
     * depth planes; the thin fourth triangle creates sparse 8x4 masks.
     */
    static const float layered_positions[] = {
        -1.0625f, -0.9375f, 1.25f,
         1.3640f, -1.0540f, 1.55f,
        -0.2340f,  1.7550f, 1.95f,

        -1.2600f,  0.7700f, 1.40f,
         1.3500f,  1.5300f, 1.80f,
         0.5850f, -1.1700f, 1.30f,

        -1.1900f, -0.1700f, 1.70f,
         0.3125f, -1.1250f, 1.25f,
         1.8040f,  1.4300f, 2.20f,

        -1.2825f,  0.0405f, 1.35f,
         1.6625f,  0.1925f, 1.75f,
        -0.3625f,  0.2900f, 1.45f,

        -0.2070f, -0.1840f, 1.15f,
         0.5060f, -0.2760f, 2.30f,
         0.0320f,  0.3840f, 1.60f,

        -1.6900f, -1.8200f, 2.60f,
         2.3200f, -0.8700f, 2.90f,
         0.2400f,  1.9680f, 2.40f,
    };
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .worker_count = 1u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };

    scene->context = NULL;
    scene->full = NULL;
    scene->left = NULL;
    scene->center = NULL;
    scene->far_strip = NULL;
    scene->layered = NULL;

    CHECK_RESULT(
        soc_context_create(&config, &scene->context),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_mesh(
            scene->context,
            full_positions,
            4u,
            quad_indices,
            (uint32_t)ARRAY_COUNT(quad_indices),
            &scene->full
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_mesh(
            scene->context,
            left_positions,
            4u,
            quad_indices,
            (uint32_t)ARRAY_COUNT(quad_indices),
            &scene->left
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_mesh(
            scene->context,
            center_positions,
            4u,
            quad_indices,
            (uint32_t)ARRAY_COUNT(quad_indices),
            &scene->center
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_mesh(
            scene->context,
            far_strip_positions,
            4u,
            quad_indices,
            (uint32_t)ARRAY_COUNT(quad_indices),
            &scene->far_strip
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_mesh(
            scene->context,
            layered_positions,
            18u,
            layered_indices,
            (uint32_t)ARRAY_COUNT(layered_indices),
            &scene->layered
        ),
        SOC_RESULT_OK
    );
    return 0;
}

static void shutdown_scene(test_scene* scene)
{
    if (scene->layered != NULL) {
        (void)soc_mesh_destroy(scene->layered);
    }
    if (scene->far_strip != NULL) {
        (void)soc_mesh_destroy(scene->far_strip);
    }
    if (scene->center != NULL) {
        (void)soc_mesh_destroy(scene->center);
    }
    if (scene->left != NULL) {
        (void)soc_mesh_destroy(scene->left);
    }
    if (scene->full != NULL) {
        (void)soc_mesh_destroy(scene->full);
    }
    soc_context_destroy(scene->context);
}

static int compare_expanded_level_zero(
    const char* name,
    const soc_snapshot* masked,
    const soc_snapshot* reference,
    uint64_t* out_guard_ulp_pixel_count,
    uint32_t* out_maximum_ulp_difference,
    uint64_t* out_unsafe_pixel_count
)
{
    soc_hiz_level_info masked_info = {
        .struct_size = sizeof(soc_hiz_level_info),
    };
    soc_hiz_level_info reference_info = {
        .struct_size = sizeof(soc_hiz_level_info),
    };
    uint64_t guard_ulp_pixel_count = 0u;
    uint64_t unsafe_pixel_count = 0u;
    uint32_t maximum_ulp_difference = 0u;
    uint32_t index;

    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            masked,
            0u,
            &masked_info,
            masked_depth_pixels,
            TEST_PIXEL_COUNT
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_hiz_level_query(
            reference,
            0u,
            &reference_info,
            reference_depth_pixels,
            TEST_PIXEL_COUNT
        ),
        SOC_RESULT_OK
    );
    CHECK(masked_info.width == TEST_WIDTH);
    CHECK(masked_info.height == TEST_HEIGHT);
    CHECK(masked_info.required_element_count == TEST_PIXEL_COUNT);
    CHECK(reference_info.width == masked_info.width);
    CHECK(reference_info.height == masked_info.height);
    CHECK(reference_info.required_element_count ==
        masked_info.required_element_count);

    for (index = 0u; index < TEST_PIXEL_COUNT; ++index) {
        const float masked_depth = masked_depth_pixels[index];
        const float reference_depth = reference_depth_pixels[index];

        if (!(masked_depth <= reference_depth)) {
            const uint32_t masked_bits = float_bits(masked_depth);
            const uint32_t reference_bits = float_bits(reference_depth);
            const uint32_t ulp_difference =
                masked_bits - reference_bits;

            /*
             * The masked candidate is evaluated directly in f64 while the
             * dense kernel reproduces a converted f32 plane. Both are far
             * biased, but their conservative values may differ within the
             * dense plane kernel's explicit guard band.
             */
            if (ulp_difference <= SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS) {
                ++guard_ulp_pixel_count;
                if (ulp_difference > maximum_ulp_difference) {
                    maximum_ulp_difference = ulp_difference;
                }
                continue;
            }
            if (unsafe_pixel_count < 8u) {
                fprintf(
                    stderr,
                    "%s: unsafe expanded depth at {%u,%u}: "
                    "masked=%.9g reference=%.9g\n",
                    name,
                    index % TEST_WIDTH,
                    index / TEST_WIDTH,
                    masked_depth,
                    reference_depth
                );
            }
            ++unsafe_pixel_count;
        }
    }

    *out_guard_ulp_pixel_count = guard_ulp_pixel_count;
    *out_maximum_ulp_difference = maximum_ulp_difference;
    *out_unsafe_pixel_count = unsafe_pixel_count;
    return 0;
}

static int compare_workload(
    const char* name,
    soc_context* context,
    const soc_frame_desc* frame,
    const soc_occluder_group* groups,
    uint32_t group_count,
    soc_bool require_center_occlusion
)
{
    const soc_occlusion_build_desc build = {
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = frame,
        .groups = groups,
        .group_count = group_count,
        .group_stride = sizeof(soc_occluder_group),
    };
    soc_query_stats masked_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_query_stats reference_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_snapshot* masked = NULL;
    soc_snapshot* reference = NULL;
    uint64_t unsafe_occlusion_count = 0u;
    uint64_t guard_ulp_depth_pixel_count = 0u;
    uint64_t unsafe_depth_pixel_count = 0u;
    uint32_t maximum_depth_ulp_difference = 0u;
    uint64_t reference_query_disagreement_count = 0u;
    uint32_t index;

    CHECK_RESULT(
        soc_occlusion_build(context, &build, &masked),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_occlusion_build_dense_internal(
            context,
            &build,
            &reference
        ),
        SOC_RESULT_OK
    );
    CHECK(masked->depth_pyramid.masked == SOC_TRUE);
    CHECK(reference->depth_pyramid.masked == SOC_FALSE);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            masked,
            query_bounds,
            QUERY_COUNT,
            masked_visibility,
            &masked_stats
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            reference,
            query_bounds,
            QUERY_COUNT,
            reference_visibility,
            &reference_stats
        ),
        SOC_RESULT_OK
    );
    CHECK(compare_expanded_level_zero(
        name,
        masked,
        reference,
        &guard_ulp_depth_pixel_count,
        &maximum_depth_ulp_difference,
        &unsafe_depth_pixel_count
    ) == 0);

    for (index = 0u; index < QUERY_COUNT; ++index) {
        if (masked_visibility[index] != SOC_VISIBILITY_OCCLUDED) {
            continue;
        }
        if (reference_visibility[index] != SOC_VISIBILITY_OCCLUDED) {
            ++reference_query_disagreement_count;
        }
        if (dense_level_zero_proves_occluded(
                reference,
                &query_bounds[index]
            ) != SOC_TRUE) {
            if (unsafe_occlusion_count < 8u) {
                fprintf(
                    stderr,
                    "%s: unsafe occlusion at query %u "
                    "(masked=%u, reference=%u, "
                    "min={%.6f,%.6f,%.6f}, max={%.6f,%.6f,%.6f})\n",
                    name,
                    index,
                    (unsigned int)masked_visibility[index],
                    (unsigned int)reference_visibility[index],
                    query_bounds[index].min.x,
                    query_bounds[index].min.y,
                    query_bounds[index].min.z,
                    query_bounds[index].max.x,
                    query_bounds[index].max.y,
                    query_bounds[index].max.z
                );
            }
            ++unsafe_occlusion_count;
        }
    }

    printf(
        "msoc differential %-20s reference=%llu masked=%llu "
        "query-disagreements=%llu unsafe=%llu "
        "guard-ulp-pixels=%llu max-ulp=%u unsafe-pixels=%llu\n",
        name,
        (unsigned long long)reference_stats.occluded_aabb_count,
        (unsigned long long)masked_stats.occluded_aabb_count,
        (unsigned long long)reference_query_disagreement_count,
        (unsigned long long)unsafe_occlusion_count,
        (unsigned long long)guard_ulp_depth_pixel_count,
        maximum_depth_ulp_difference,
        (unsigned long long)unsafe_depth_pixel_count
    );
    soc_snapshot_destroy(reference);
    soc_snapshot_destroy(masked);

    CHECK(unsafe_occlusion_count == 0u);
    CHECK(unsafe_depth_pixel_count == 0u);
    if (require_center_occlusion == SOC_TRUE) {
        CHECK(masked_visibility[0] == SOC_VISIBILITY_OCCLUDED);
    }

    return 0;
}

static soc_bool masked_snapshots_are_raw_equal(
    const soc_snapshot* left,
    const soc_snapshot* right
)
{
    const soc_hiz* left_hiz = &left->depth_pyramid;
    const soc_hiz* right_hiz = &right->depth_pyramid;
    uint32_t level;

    if (left_hiz->masked != SOC_TRUE ||
        right_hiz->masked != SOC_TRUE ||
        left_hiz->pixel_width != right_hiz->pixel_width ||
        left_hiz->pixel_height != right_hiz->pixel_height ||
        left_hiz->level_count != right_hiz->level_count ||
        left_hiz->element_count != right_hiz->element_count) {
        return SOC_FALSE;
    }
    for (level = 0u; level < left_hiz->level_count; ++level) {
        const soc_hiz_level* left_level = &left_hiz->levels[level];
        const soc_hiz_level* right_level = &right_hiz->levels[level];

        if (left_level->width != right_level->width ||
            left_level->height != right_level->height ||
            left_level->offset != right_level->offset ||
            left_level->element_count != right_level->element_count) {
            return SOC_FALSE;
        }
    }
    if (memcmp(
            left_hiz->data,
            right_hiz->data,
            left_hiz->element_count * sizeof(*left_hiz->data)
        ) != 0 ||
        memcmp(
            left_hiz->working_depth,
            right_hiz->working_depth,
            left_hiz->levels[0].element_count *
                sizeof(*left_hiz->working_depth)
        ) != 0 ||
        memcmp(
            left_hiz->layer_masks,
            right_hiz->layer_masks,
            left_hiz->levels[0].element_count *
                sizeof(*left_hiz->layer_masks)
        ) != 0) {
        return SOC_FALSE;
    }
    return SOC_TRUE;
}

static int compare_masked_snapshots_exact(
    const soc_snapshot* serial,
    const soc_snapshot* parallel
)
{
    CHECK(masked_snapshots_are_raw_equal(serial, parallel) == SOC_TRUE);
    CHECK(serial->build_stats.hiz_level_count ==
        parallel->build_stats.hiz_level_count);
    CHECK(serial->build_stats.input_triangle_count ==
        parallel->build_stats.input_triangle_count);
    CHECK(serial->build_stats.clipped_triangle_count ==
        parallel->build_stats.clipped_triangle_count);
    CHECK(serial->build_stats.rasterized_triangle_count ==
        parallel->build_stats.rasterized_triangle_count);
    return 0;
}

static int check_query_stats_equal(
    const soc_query_stats* left,
    const soc_query_stats* right
)
{
    CHECK(left->tested_aabb_count == right->tested_aabb_count);
    CHECK(left->visible_aabb_count == right->visible_aabb_count);
    CHECK(left->occluded_aabb_count == right->occluded_aabb_count);
    CHECK(left->unknown_aabb_count == right->unknown_aabb_count);
    return 0;
}

static int test_parallel_masked_matches_serial(void)
{
    static const uint32_t worker_counts[] = {2u, 4u};
    const soc_frame_desc frame = make_reverse_z_frame();
    soc_context* serial_context = NULL;
    soc_mesh* serial_mesh = NULL;
    soc_mesh* reversed_mesh = NULL;
    soc_occluder_group serial_group;
    soc_occluder_group reversed_group;
    soc_occlusion_build_desc serial_build;
    soc_occlusion_build_desc reversed_build;
    soc_snapshot* serial_snapshot = NULL;
    soc_snapshot* reversed_snapshot = NULL;
    soc_snapshot* dense_snapshot = NULL;
    soc_query_stats serial_query_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    uint32_t worker_case;

    CHECK_RESULT(create_test_context(1u, &serial_context), SOC_RESULT_OK);
    CHECK_RESULT(
        create_parallel_order_mesh(
            serial_context,
            SOC_FALSE,
            &serial_mesh
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        create_parallel_order_mesh(
            serial_context,
            SOC_TRUE,
            &reversed_mesh
        ),
        SOC_RESULT_OK
    );

    serial_group = (soc_occluder_group){
        serial_mesh,
        &identity_transform,
        1u,
        SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    reversed_group = serial_group;
    reversed_group.mesh = reversed_mesh;
    serial_build = (soc_occlusion_build_desc){
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = &frame,
        .groups = &serial_group,
        .group_count = 1u,
        .group_stride = sizeof(soc_occluder_group),
    };
    reversed_build = serial_build;
    reversed_build.groups = &reversed_group;

    CHECK_RESULT(
        soc_occlusion_build_masked_internal(
            serial_context,
            &serial_build,
            &serial_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(serial_snapshot != NULL);
    CHECK(serial_snapshot->depth_pyramid.masked == SOC_TRUE);
    CHECK(serial_snapshot->masked_parallel == SOC_FALSE);
    CHECK(serial_snapshot->build_stats.input_triangle_count ==
        PARALLEL_TRIANGLE_COUNT);
    CHECK_RESULT(
        soc_occlusion_build_masked_internal(
            serial_context,
            &reversed_build,
            &reversed_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(reversed_snapshot->masked_parallel == SOC_FALSE);
    CHECK(masked_snapshots_are_raw_equal(
        serial_snapshot,
        reversed_snapshot
    ) == SOC_FALSE);
    CHECK_RESULT(
        soc_occlusion_build_dense_internal(
            serial_context,
            &serial_build,
            &dense_snapshot
        ),
        SOC_RESULT_OK
    );
    CHECK(dense_snapshot->depth_pyramid.masked == SOC_FALSE);
    CHECK_RESULT(
        soc_snapshot_test_aabbs(
            serial_snapshot,
            query_bounds,
            QUERY_COUNT,
            masked_visibility,
            &serial_query_stats
        ),
        SOC_RESULT_OK
    );

    for (worker_case = 0u;
         worker_case < (uint32_t)ARRAY_COUNT(worker_counts);
         ++worker_case) {
        const uint32_t worker_count = worker_counts[worker_case];
        soc_context* parallel_context = NULL;
        soc_mesh* parallel_mesh = NULL;
        soc_occluder_group parallel_group;
        soc_occlusion_build_desc parallel_build;
        uint32_t repeat;

        CHECK_RESULT(
            create_test_context(worker_count, &parallel_context),
            SOC_RESULT_OK
        );
        CHECK_RESULT(
            create_parallel_order_mesh(
                parallel_context,
                SOC_FALSE,
                &parallel_mesh
            ),
            SOC_RESULT_OK
        );
        parallel_group = serial_group;
        parallel_group.mesh = parallel_mesh;
        parallel_build = serial_build;
        parallel_build.groups = &parallel_group;

        for (repeat = 0u;
             repeat < PARALLEL_REPEAT_COUNT;
             ++repeat) {
            soc_snapshot* parallel_snapshot = NULL;
            soc_query_stats parallel_query_stats = {
                .struct_size = sizeof(soc_query_stats),
            };

            CHECK_RESULT(
                soc_occlusion_build_masked_internal(
                    parallel_context,
                    &parallel_build,
                    &parallel_snapshot
                ),
                SOC_RESULT_OK
            );
            CHECK(parallel_snapshot != NULL);
            CHECK(parallel_snapshot->masked_parallel == SOC_TRUE);
            CHECK(compare_masked_snapshots_exact(
                serial_snapshot,
                parallel_snapshot
            ) == 0);
            CHECK_RESULT(
                soc_snapshot_test_aabbs(
                    parallel_snapshot,
                    query_bounds,
                    QUERY_COUNT,
                    parallel_visibility,
                    &parallel_query_stats
                ),
                SOC_RESULT_OK
            );
            CHECK(memcmp(
                masked_visibility,
                parallel_visibility,
                sizeof(masked_visibility)
            ) == 0);
            CHECK(check_query_stats_equal(
                &serial_query_stats,
                &parallel_query_stats
            ) == 0);

            if (repeat == 0u) {
                uint64_t guard_ulp_depth_pixel_count = 0u;
                uint64_t unsafe_depth_pixel_count = 0u;
                uint64_t unsafe_occlusion_count = 0u;
                uint32_t maximum_depth_ulp_difference = 0u;
                uint32_t query;

                CHECK(compare_expanded_level_zero(
                    worker_count == 2u
                        ? "parallel-msoc-2w"
                        : "parallel-msoc-4w",
                    parallel_snapshot,
                    dense_snapshot,
                    &guard_ulp_depth_pixel_count,
                    &maximum_depth_ulp_difference,
                    &unsafe_depth_pixel_count
                ) == 0);
                for (query = 0u; query < QUERY_COUNT; ++query) {
                    if (parallel_visibility[query] ==
                            SOC_VISIBILITY_OCCLUDED &&
                        dense_level_zero_proves_occluded(
                            dense_snapshot,
                            &query_bounds[query]
                        ) != SOC_TRUE) {
                        ++unsafe_occlusion_count;
                    }
                }
                printf(
                    "parallel msoc workers=%u repeats=%u unsafe=%llu "
                    "guard-ulp-pixels=%llu max-ulp=%u "
                    "unsafe-pixels=%llu\n",
                    worker_count,
                    PARALLEL_REPEAT_COUNT,
                    (unsigned long long)unsafe_occlusion_count,
                    (unsigned long long)guard_ulp_depth_pixel_count,
                    maximum_depth_ulp_difference,
                    (unsigned long long)unsafe_depth_pixel_count
                );
                CHECK(unsafe_occlusion_count == 0u);
                CHECK(unsafe_depth_pixel_count == 0u);
            }
            soc_snapshot_destroy(parallel_snapshot);
        }

        CHECK_RESULT(soc_mesh_destroy(parallel_mesh), SOC_RESULT_OK);
        soc_context_destroy(parallel_context);
    }

    soc_snapshot_destroy(dense_snapshot);
    soc_snapshot_destroy(reversed_snapshot);
    soc_snapshot_destroy(serial_snapshot);
    CHECK_RESULT(soc_mesh_destroy(reversed_mesh), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_destroy(serial_mesh), SOC_RESULT_OK);
    soc_context_destroy(serial_context);
    return 0;
}

int main(void)
{
    const soc_frame_desc frame = make_reverse_z_frame();
    test_scene scene;
    soc_occluder_group full_only[1];
    soc_occluder_group full_first[4];
    soc_occluder_group full_last[4];
    soc_occluder_group partial_overlap[3];
    soc_occluder_group varying_sparse_layers[1];
    int failed = 0;

    initialize_queries();
    CHECK(initialize_scene(&scene) == 0);

    full_first[0] = (soc_occluder_group){
        scene.full, &identity_transform, 1u, SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    full_first[1] = (soc_occluder_group){
        scene.left, &identity_transform, 1u, SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    full_first[2] = (soc_occluder_group){
        scene.center, &identity_transform, 1u, SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    full_first[3] = (soc_occluder_group){
        scene.far_strip,
        &identity_transform,
        1u,
        SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    full_last[0] = full_first[3];
    full_last[1] = full_first[2];
    full_last[2] = full_first[1];
    full_last[3] = full_first[0];
    partial_overlap[0] = full_first[1];
    partial_overlap[1] = full_first[3];
    partial_overlap[2] = full_first[2];
    varying_sparse_layers[0] = (soc_occluder_group){
        scene.layered,
        &identity_transform,
        1u,
        SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    full_only[0] = full_first[0];

    failed |= compare_workload(
        "full-only-101x53",
        scene.context,
        &frame,
        full_only,
        (uint32_t)ARRAY_COUNT(full_only),
        SOC_TRUE
    );
    failed |= compare_workload(
        "full-first-101x53",
        scene.context,
        &frame,
        full_first,
        (uint32_t)ARRAY_COUNT(full_first),
        SOC_TRUE
    );
    failed |= compare_workload(
        "full-last-101x53",
        scene.context,
        &frame,
        full_last,
        (uint32_t)ARRAY_COUNT(full_last),
        SOC_TRUE
    );
    failed |= compare_workload(
        "partial-overlap",
        scene.context,
        &frame,
        partial_overlap,
        (uint32_t)ARRAY_COUNT(partial_overlap),
        SOC_TRUE
    );
    failed |= compare_workload(
        "varying-sparse-layers",
        scene.context,
        &frame,
        varying_sparse_layers,
        (uint32_t)ARRAY_COUNT(varying_sparse_layers),
        SOC_TRUE
    );

    shutdown_scene(&scene);
    failed |= test_parallel_masked_matches_serial();
    return failed == 0 ? 0 : 1;
}
