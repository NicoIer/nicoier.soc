#include "core/soc_context.h"
#include "core/soc_kernels.h"
#include "core/soc_mesh.h"
#include "core/soc_pipeline.h"
#include "core/soc_snapshot.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define TEST_WIDTH 17u
#define TEST_HEIGHT 9u
#define TEST_PIXEL_COUNT ((size_t)TEST_WIDTH * (size_t)TEST_HEIGHT)
#define DEPTH_ABSOLUTE_TOLERANCE 5.0e-5f
#define DEPTH_RELATIVE_TOLERANCE 5.0e-5f

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

static soc_bool depth_within_tolerance(float left, float right)
{
    const float difference = fabsf(left - right);
    const float absolute_left = fabsf(left);
    const float absolute_right = fabsf(right);
    const float scale = absolute_left > absolute_right
        ? absolute_left
        : absolute_right;

    return difference <= DEPTH_ABSOLUTE_TOLERANCE +
        DEPTH_RELATIVE_TOLERANCE * scale
            ? SOC_TRUE
            : SOC_FALSE;
}

static soc_result build_snapshot(
    soc_kernel_backend backend,
    soc_clip_depth_range clip_depth_range,
    soc_bool constant_depth,
    soc_snapshot** out_snapshot
)
{
    static const float varying_positions[] = {
        -1.25f, -0.85f, 0.12f,
         0.90f, -0.95f, 0.30f,
         0.95f,  0.85f, 0.72f,
        -0.80f,  1.10f, 0.55f,
    };
    static const uint16_t varying_indices[] = {
        0u, 1u, 2u,
        0u, 2u, 3u,
    };
    static const float constant_positions[] = {
        -1.0f, -1.0f, 0.35f,
         2.0f, -1.0f, 0.35f,
        -1.0f,  2.0f, 0.35f,
    };
    static const uint16_t constant_indices[] = {0u, 1u, 2u};
    static const soc_mat4 varying_transforms[] = {
        {
            .col0 = {0.84f, 0.02f, 0.00f, 0.00f},
            .col1 = {-0.03f, 0.82f, 0.00f, 0.00f},
            .col2 = {0.00f, 0.00f, 0.90f, 0.00f},
            .col3 = {-0.08f, 0.01f, 0.02f, 1.00f},
        },
        {
            .col0 = {0.46f, -0.04f, 0.00f, 0.00f},
            .col1 = {0.05f, 0.51f, 0.00f, 0.00f},
            .col2 = {0.00f, 0.00f, 0.78f, 0.00f},
            .col3 = {0.31f, -0.16f, 0.08f, 1.00f},
        },
    };
    static const soc_mat4 identity_transform = {
        .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
        .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
        .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
        .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    const soc_mesh_desc mesh_desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_TWO_SIDED,
        .vertices = constant_depth == SOC_TRUE
            ? constant_positions
            : varying_positions,
        .indices = constant_depth == SOC_TRUE
            ? constant_indices
            : varying_indices,
        .vertex_count = constant_depth == SOC_TRUE ? 3u : 4u,
        .vertex_stride = 3u * sizeof(float),
        .position_offset = 0u,
        .index_count = constant_depth == SOC_TRUE ? 3u : 6u,
        .index_type = SOC_INDEX_UINT16,
    };
    soc_frame_desc frame = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {0.92f, 0.03f, 0.02f, 0.01f},
            .col1 = {-0.04f, 0.88f, 0.01f, -0.02f},
            .col2 = {0.01f, -0.02f, 0.72f, 0.08f},
            .col3 = {0.02f, -0.01f, 0.14f, 1.00f},
        },
        .clip_depth_range = clip_depth_range,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    soc_occluder_group group = {
        .mesh = NULL,
        .object_to_world = constant_depth == SOC_TRUE
            ? &identity_transform
            : varying_transforms,
        .instance_count = constant_depth == SOC_TRUE
            ? 1u
            : (uint32_t)ARRAY_COUNT(varying_transforms),
        .flags = SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    soc_occlusion_build_desc build = {
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = &frame,
        .groups = &group,
        .group_count = 1u,
        .group_stride = sizeof(group),
    };
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* snapshot = NULL;
    soc_result result;

    if (constant_depth == SOC_TRUE) {
        frame.clip_from_world = identity_transform;
    }

    if (out_snapshot == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    *out_snapshot = NULL;

    result = soc_context_create_for_backend_for_testing_internal(
        &config,
        backend,
        &context
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }
    result = soc_mesh_create_internal(context, &mesh_desc, &mesh);
    if (result == SOC_RESULT_OK) {
        group.mesh = mesh;
        result = soc_occlusion_build_internal(context, &build, &snapshot);
    }

    if (mesh != NULL) {
        (void)soc_mesh_destroy_internal(mesh);
    }
    soc_context_destroy_internal(context);

    if (result != SOC_RESULT_OK) {
        soc_snapshot_destroy_internal(snapshot);
        return result;
    }
    *out_snapshot = snapshot;
    return SOC_RESULT_OK;
}

static int validate_build_stats(
    const soc_snapshot* snapshot,
    uint64_t expected_input_triangle_count
)
{
    const soc_build_stats* stats = &snapshot->build_stats;

    CHECK(stats->struct_size == sizeof(*stats));
    CHECK(stats->hiz_level_count == snapshot->depth_pyramid.level_count);
    CHECK(stats->input_triangle_count == expected_input_triangle_count);
    CHECK(stats->rasterized_triangle_count != 0u);
    return 0;
}

static int validate_masked_layout(const soc_snapshot* snapshot)
{
    const soc_hiz* hiz = &snapshot->depth_pyramid;
    const uint32_t expected_block_width =
        (TEST_WIDTH + SOC_HIZ_MASK_BLOCK_WIDTH - 1u) /
        SOC_HIZ_MASK_BLOCK_WIDTH;
    const uint32_t expected_block_height =
        (TEST_HEIGHT + SOC_HIZ_MASK_BLOCK_HEIGHT - 1u) /
        SOC_HIZ_MASK_BLOCK_HEIGHT;
    size_t expected_offset = 0u;
    size_t index;
    uint32_t level;

    CHECK(hiz->initialized == SOC_TRUE);
    CHECK(hiz->masked == SOC_TRUE);
    CHECK(hiz->pixel_width == TEST_WIDTH);
    CHECK(hiz->pixel_height == TEST_HEIGHT);
    CHECK(hiz->data != NULL);
    CHECK(hiz->working_depth != NULL);
    CHECK(hiz->layer_masks != NULL);
    CHECK(hiz->level_count != 0u);
    CHECK(hiz->levels[0].width == expected_block_width);
    CHECK(hiz->levels[0].height == expected_block_height);

    for (level = 0u; level < hiz->level_count; ++level) {
        const soc_hiz_level* metadata = &hiz->levels[level];

        CHECK(metadata->width != 0u);
        CHECK(metadata->height != 0u);
        CHECK(metadata->offset == expected_offset);
        CHECK(metadata->element_count ==
            (size_t)metadata->width * metadata->height);
        expected_offset += metadata->element_count;
    }
    CHECK(expected_offset == hiz->element_count);

    for (index = 0u;
         index < hiz->levels[0].element_count;
         ++index) {
        const uint32_t block_x =
            (uint32_t)(index % hiz->levels[0].width);
        const uint32_t block_y =
            (uint32_t)(index / hiz->levels[0].width);
        const uint32_t first_x = block_x * SOC_HIZ_MASK_BLOCK_WIDTH;
        const uint32_t first_y = block_y * SOC_HIZ_MASK_BLOCK_HEIGHT;
        const float z0 = hiz->data[index];
        const float z1 = hiz->working_depth[index];
        uint32_t valid_mask = 0u;
        uint32_t local_y;

        CHECK(z0 == -1.0f || (z0 >= 0.0f && z0 <= 1.0f));
        CHECK(z1 == FLT_MAX || (z1 >= 0.0f && z1 <= 1.0f));
        for (local_y = 0u;
             local_y < SOC_HIZ_MASK_BLOCK_HEIGHT &&
                first_y + local_y < TEST_HEIGHT;
             ++local_y) {
            uint32_t local_x;

            for (local_x = 0u;
                 local_x < SOC_HIZ_MASK_BLOCK_WIDTH &&
                    first_x + local_x < TEST_WIDTH;
                 ++local_x) {
                valid_mask |= UINT32_C(1) <<
                    (local_y * SOC_HIZ_MASK_BLOCK_WIDTH + local_x);
            }
        }
        CHECK((hiz->layer_masks[index] & ~valid_mask) == 0u);
    }
    return 0;
}

static int compare_public_depth_pyramids(
    const soc_snapshot* scalar,
    const soc_snapshot* neon
)
{
    float scalar_depth[TEST_PIXEL_COUNT];
    float neon_depth[TEST_PIXEL_COUNT];
    soc_bool scalar_saw_clear = SOC_FALSE;
    soc_bool scalar_saw_drawn = SOC_FALSE;
    soc_bool neon_saw_clear = SOC_FALSE;
    soc_bool neon_saw_drawn = SOC_FALSE;
    uint32_t level;

    CHECK(scalar->depth_pyramid.level_count ==
        neon->depth_pyramid.level_count);
    for (level = 0u;
         level < scalar->depth_pyramid.level_count;
         ++level) {
        soc_hiz_level_info scalar_info = {
            .struct_size = sizeof(scalar_info),
        };
        soc_hiz_level_info neon_info = {
            .struct_size = sizeof(neon_info),
        };
        uint64_t index;

        CHECK(soc_snapshot_hiz_level_query_internal(
            scalar,
            level,
            &scalar_info,
            NULL,
            0u
        ) == SOC_RESULT_OK);
        CHECK(soc_snapshot_hiz_level_query_internal(
            neon,
            level,
            &neon_info,
            NULL,
            0u
        ) == SOC_RESULT_OK);
        CHECK(scalar_info.level == level);
        CHECK(neon_info.level == level);
        CHECK(scalar_info.width == neon_info.width);
        CHECK(scalar_info.height == neon_info.height);
        CHECK(scalar_info.required_element_count ==
            neon_info.required_element_count);
        CHECK(scalar_info.required_element_count <= TEST_PIXEL_COUNT);
        CHECK(soc_snapshot_hiz_level_query_internal(
            scalar,
            level,
            &scalar_info,
            scalar_depth,
            TEST_PIXEL_COUNT
        ) == SOC_RESULT_OK);
        CHECK(soc_snapshot_hiz_level_query_internal(
            neon,
            level,
            &neon_info,
            neon_depth,
            TEST_PIXEL_COUNT
        ) == SOC_RESULT_OK);

        for (index = 0u;
             index < scalar_info.required_element_count;
             ++index) {
            const float scalar_value = scalar_depth[index];
            const float neon_value = neon_depth[index];

            CHECK(scalar_value >= 0.0f && scalar_value <= 1.0f);
            CHECK(neon_value >= 0.0f && neon_value <= 1.0f);
            if (level == 0u) {
                scalar_saw_clear = scalar_value == 0.0f
                    ? SOC_TRUE
                    : scalar_saw_clear;
                scalar_saw_drawn = scalar_value > 0.0f
                    ? SOC_TRUE
                    : scalar_saw_drawn;
                neon_saw_clear = neon_value == 0.0f
                    ? SOC_TRUE
                    : neon_saw_clear;
                neon_saw_drawn = neon_value > 0.0f
                    ? SOC_TRUE
                    : neon_saw_drawn;
            }

            if (depth_within_tolerance(
                    scalar_value,
                    neon_value
                ) != SOC_TRUE) {
                fprintf(
                    stderr,
                    "Hi-Z numeric mismatch at level %u element %llu: "
                    "%.9g != %.9g\n",
                    level,
                    (unsigned long long)index,
                    (double)scalar_value,
                    (double)neon_value
                );
                return 1;
            }
        }
    }
    CHECK(scalar_saw_clear == SOC_TRUE);
    CHECK(scalar_saw_drawn == SOC_TRUE);
    CHECK(neon_saw_clear == SOC_TRUE);
    CHECK(neon_saw_drawn == SOC_TRUE);
    return 0;
}

static int compare_snapshots(
    const soc_snapshot* scalar,
    const soc_snapshot* neon,
    uint64_t expected_input_triangle_count
)
{
    CHECK(scalar != NULL);
    CHECK(neon != NULL);
    CHECK(scalar->kernels == soc_kernel_table_scalar());
    CHECK(neon->kernels == soc_kernel_table_neon());
    CHECK(validate_build_stats(
        scalar,
        expected_input_triangle_count
    ) == 0);
    CHECK(validate_build_stats(
        neon,
        expected_input_triangle_count
    ) == 0);
    CHECK(validate_masked_layout(scalar) == 0);
    CHECK(validate_masked_layout(neon) == 0);
    return compare_public_depth_pyramids(scalar, neon);
}

static int compare_queries(
    const soc_snapshot* scalar,
    const soc_snapshot* neon
)
{
    const soc_aabb bounds[] = {
        {
            .min = {
                -0.03f,
                -0.03f,
                0.05f,
            },
            .max = {
                0.03f,
                0.03f,
                0.10f,
            },
        },
        {
            .min = {1.50f, 1.50f, 0.20f},
            .max = {2.00f, 2.00f, 0.80f},
        },
        {
            .min = {0.25f, -0.25f, 0.20f},
            .max = {-0.25f, 0.25f, 0.80f},
        },
        {
            .min = {-0.10f, 0.10f, 0.20f},
            .max = {0.10f, -0.10f, 0.80f},
        },
        {
            .min = {-0.0f, 0.0f, 0.50f},
            .max = {0.0f, -0.0f, 0.50f},
        },
        {
            .min = {-10.0f, -10.0f, -10.0f},
            .max = {10.0f, 10.0f, 10.0f},
        },
        {
            .min = {-0.45f, -0.35f, 0.15f},
            .max = {0.55f, 0.45f, 0.75f},
        },
    };
    soc_visibility scalar_visibility[ARRAY_COUNT(bounds)];
    soc_visibility neon_visibility[ARRAY_COUNT(bounds)];
    soc_query_stats scalar_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_query_stats neon_stats = {
        .struct_size = sizeof(soc_query_stats),
    };
    soc_result scalar_result;
    soc_result neon_result;
    uint64_t scalar_visible = 0u;
    uint64_t scalar_occluded = 0u;
    uint64_t scalar_unknown = 0u;
    uint64_t neon_visible = 0u;
    uint64_t neon_occluded = 0u;
    uint64_t neon_unknown = 0u;
    size_t index;

    memset(scalar_visibility, 0xa5, sizeof(scalar_visibility));
    memset(neon_visibility, 0x5a, sizeof(neon_visibility));
    scalar_result = soc_snapshot_test_aabbs_internal(
        scalar,
        bounds,
        (uint32_t)ARRAY_COUNT(bounds),
        scalar_visibility,
        &scalar_stats
    );
    neon_result = soc_snapshot_test_aabbs_internal(
        neon,
        bounds,
        (uint32_t)ARRAY_COUNT(bounds),
        neon_visibility,
        &neon_stats
    );

    CHECK(scalar_result == SOC_RESULT_OK);
    CHECK(neon_result == scalar_result);
    for (index = 0u; index < ARRAY_COUNT(bounds); ++index) {
        CHECK(scalar_visibility[index] == SOC_VISIBILITY_VISIBLE ||
            scalar_visibility[index] == SOC_VISIBILITY_OCCLUDED ||
            scalar_visibility[index] == SOC_VISIBILITY_UNKNOWN);
        CHECK(neon_visibility[index] == SOC_VISIBILITY_VISIBLE ||
            neon_visibility[index] == SOC_VISIBILITY_OCCLUDED ||
            neon_visibility[index] == SOC_VISIBILITY_UNKNOWN);
        CHECK(scalar_visibility[index] == neon_visibility[index]);

        if (scalar_visibility[index] == SOC_VISIBILITY_VISIBLE) {
            ++scalar_visible;
        } else if (scalar_visibility[index] == SOC_VISIBILITY_OCCLUDED) {
            ++scalar_occluded;
        } else {
            ++scalar_unknown;
        }
        if (neon_visibility[index] == SOC_VISIBILITY_VISIBLE) {
            ++neon_visible;
        } else if (neon_visibility[index] == SOC_VISIBILITY_OCCLUDED) {
            ++neon_occluded;
        } else {
            ++neon_unknown;
        }
    }

    /*
     * These checks describe the public mathematical contract rather than an
     * internal representation: robust in-frustum boxes classify normally,
     * an outside box is visible, malformed bounds are unknown, and a box
     * crossing non-positive W remains unknown.
     */
    CHECK(scalar_visibility[0] != SOC_VISIBILITY_UNKNOWN);
    CHECK(scalar_visibility[1] == SOC_VISIBILITY_VISIBLE);
    CHECK(scalar_visibility[2] == SOC_VISIBILITY_UNKNOWN);
    CHECK(scalar_visibility[3] == SOC_VISIBILITY_UNKNOWN);
    CHECK(scalar_visibility[4] != SOC_VISIBILITY_UNKNOWN);
    CHECK(scalar_visibility[5] == SOC_VISIBILITY_UNKNOWN);
    CHECK(scalar_visibility[6] != SOC_VISIBILITY_UNKNOWN);

    CHECK(scalar_stats.reserved == 0u);
    CHECK(scalar_stats.tested_aabb_count == ARRAY_COUNT(bounds));
    CHECK(scalar_stats.visible_aabb_count == scalar_visible);
    CHECK(scalar_stats.occluded_aabb_count == scalar_occluded);
    CHECK(scalar_stats.unknown_aabb_count == scalar_unknown);
    CHECK(scalar_visible + scalar_occluded + scalar_unknown ==
        ARRAY_COUNT(bounds));
    CHECK(scalar_visible != 0u);
    CHECK(scalar_unknown != 0u);

    CHECK(neon_stats.reserved == 0u);
    CHECK(neon_stats.tested_aabb_count == ARRAY_COUNT(bounds));
    CHECK(neon_stats.visible_aabb_count == neon_visible);
    CHECK(neon_stats.occluded_aabb_count == neon_occluded);
    CHECK(neon_stats.unknown_aabb_count == neon_unknown);
    CHECK(neon_visible + neon_occluded + neon_unknown ==
        ARRAY_COUNT(bounds));
    CHECK(neon_visible != 0u);
    CHECK(neon_unknown != 0u);
    return 0;
}

static int test_pipeline_differential(void)
{
    static const soc_clip_depth_range clip_depth_ranges[] = {
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
    };
    size_t range_index;

    if (soc_kernel_table_neon() == NULL) {
        return 0;
    }

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        soc_snapshot* scalar = NULL;
        soc_snapshot* neon = NULL;

        CHECK(build_snapshot(
            SOC_KERNEL_BACKEND_SCALAR,
            clip_depth_ranges[range_index],
            SOC_FALSE,
            &scalar
        ) == SOC_RESULT_OK);
        CHECK(build_snapshot(
            SOC_KERNEL_BACKEND_NEON,
            clip_depth_ranges[range_index],
            SOC_FALSE,
            &neon
        ) == SOC_RESULT_OK);
        CHECK(compare_snapshots(scalar, neon, 4u) == 0);
        CHECK(compare_queries(scalar, neon) == 0);

        soc_snapshot_destroy_internal(neon);
        soc_snapshot_destroy_internal(scalar);
    }
    return 0;
}

static int test_constant_depth_pipeline_differential(void)
{
    static const soc_clip_depth_range clip_depth_ranges[] = {
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
    };
    size_t range_index;

    if (soc_kernel_table_neon() == NULL) {
        return 0;
    }

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        soc_snapshot* scalar = NULL;
        soc_snapshot* neon = NULL;

        CHECK(build_snapshot(
            SOC_KERNEL_BACKEND_SCALAR,
            clip_depth_ranges[range_index],
            SOC_TRUE,
            &scalar
        ) == SOC_RESULT_OK);
        CHECK(build_snapshot(
            SOC_KERNEL_BACKEND_NEON,
            clip_depth_ranges[range_index],
            SOC_TRUE,
            &neon
        ) == SOC_RESULT_OK);
        CHECK(compare_snapshots(scalar, neon, 1u) == 0);

        soc_snapshot_destroy_internal(neon);
        soc_snapshot_destroy_internal(scalar);
    }
    return 0;
}

int main(void)
{
    if (test_pipeline_differential() != 0) {
        return 1;
    }
    return test_constant_depth_pipeline_differential();
}
