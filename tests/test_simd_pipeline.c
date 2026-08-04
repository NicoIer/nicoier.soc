#include "core/soc_context.h"
#include "core/soc_kernels.h"
#include "core/soc_mesh.h"
#include "core/soc_pipeline.h"
#include "core/soc_snapshot.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

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

static float float_from_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static soc_result build_snapshot(
    soc_kernel_backend backend,
    soc_clip_depth_range clip_depth_range,
    soc_depth_direction depth_direction,
    soc_snapshot** out_snapshot
)
{
    static const float positions[] = {
        -1.25f, -0.85f, 0.12f,
         0.90f, -0.95f, 0.30f,
         0.95f,  0.85f, 0.72f,
        -0.80f,  1.10f, 0.55f,
    };
    static const uint16_t indices[] = {
        0u, 1u, 2u,
        0u, 2u, 3u,
    };
    static const soc_mat4 transforms[] = {
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
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 17u,
        .height = 9u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    const soc_mesh_desc mesh_desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_TWO_SIDED,
        .vertices = positions,
        .indices = indices,
        .vertex_count = 4u,
        .vertex_stride = 3u * sizeof(float),
        .position_offset = 0u,
        .index_count = 6u,
        .index_type = SOC_INDEX_UINT16,
    };
    const soc_frame_desc frame = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {0.92f, 0.03f, 0.02f, 0.01f},
            .col1 = {-0.04f, 0.88f, 0.01f, -0.02f},
            .col2 = {0.01f, -0.02f, 0.72f, 0.08f},
            .col3 = {0.02f, -0.01f, 0.14f, 1.00f},
        },
        .clip_depth_range = clip_depth_range,
        .depth_direction = depth_direction,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    soc_occluder_group group = {
        .mesh = NULL,
        .object_to_world = transforms,
        .instance_count = (uint32_t)ARRAY_COUNT(transforms),
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

static int compare_build_stats(
    const soc_build_stats* scalar,
    const soc_build_stats* neon
)
{
    CHECK(scalar->struct_size == neon->struct_size);
    CHECK(scalar->hiz_level_count == neon->hiz_level_count);
    CHECK(scalar->input_triangle_count == neon->input_triangle_count);
    CHECK(scalar->clipped_triangle_count == neon->clipped_triangle_count);
    CHECK(scalar->rasterized_triangle_count ==
        neon->rasterized_triangle_count);
    return 0;
}

static int compare_snapshots(
    const soc_snapshot* scalar,
    const soc_snapshot* neon
)
{
    uint32_t clear_bits;
    soc_bool saw_clear = SOC_FALSE;
    soc_bool saw_drawn = SOC_FALSE;
    size_t index;
    uint32_t level;

    CHECK(scalar != NULL);
    CHECK(neon != NULL);
    clear_bits = scalar->frame.depth_direction == SOC_DEPTH_REVERSED
        ? UINT32_C(0x00000000)
        : UINT32_C(0x3f800000);
    CHECK(scalar->kernels == soc_kernel_table_scalar());
    CHECK(neon->kernels == soc_kernel_table_neon());
    CHECK(compare_build_stats(&scalar->build_stats, &neon->build_stats) == 0);
    CHECK(scalar->build_stats.input_triangle_count == 4u);
    CHECK(scalar->build_stats.rasterized_triangle_count != 0u);
    CHECK(scalar->depth_pyramid.level_count ==
        neon->depth_pyramid.level_count);
    CHECK(scalar->depth_pyramid.element_count ==
        neon->depth_pyramid.element_count);

    for (level = 0u; level < scalar->depth_pyramid.level_count; ++level) {
        const soc_hiz_level* scalar_level =
            &scalar->depth_pyramid.levels[level];
        const soc_hiz_level* neon_level = &neon->depth_pyramid.levels[level];

        CHECK(scalar_level->width == neon_level->width);
        CHECK(scalar_level->height == neon_level->height);
        CHECK(scalar_level->offset == neon_level->offset);
        CHECK(scalar_level->element_count == neon_level->element_count);
    }

    for (index = 0u;
         index < scalar->depth_pyramid.levels[0].element_count;
         ++index) {
        if (float_bits(scalar->depth_pyramid.data[index]) == clear_bits) {
            saw_clear = SOC_TRUE;
        } else {
            saw_drawn = SOC_TRUE;
        }
    }
    CHECK(saw_clear == SOC_TRUE);
    CHECK(saw_drawn == SOC_TRUE);

    if (memcmp(
            scalar->depth_pyramid.data,
            neon->depth_pyramid.data,
            scalar->depth_pyramid.element_count * sizeof(float)
        ) == 0) {
        return 0;
    }

    for (index = 0u;
         index < scalar->depth_pyramid.element_count;
         ++index) {
        const uint32_t scalar_bits =
            float_bits(scalar->depth_pyramid.data[index]);
        const uint32_t neon_bits =
            float_bits(neon->depth_pyramid.data[index]);

        if (scalar_bits != neon_bits) {
            fprintf(
                stderr,
                "Hi-Z bit mismatch at element %zu: %08x != %08x\n",
                index,
                scalar_bits,
                neon_bits
            );
            return 1;
        }
    }
    return 1;
}

static int compare_queries(
    const soc_snapshot* scalar,
    const soc_snapshot* neon
)
{
    const float nan_payload = float_from_bits(UINT32_C(0x7fc12345));
    const soc_bool reversed = scalar->frame.depth_direction ==
        SOC_DEPTH_REVERSED ? SOC_TRUE : SOC_FALSE;
    const soc_aabb bounds[] = {
        {
            .min = {
                -0.03f,
                -0.03f,
                reversed == SOC_TRUE ? 0.05f : 0.82f,
            },
            .max = {
                0.03f,
                0.03f,
                reversed == SOC_TRUE ? 0.10f : 0.87f,
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
            .min = {nan_payload, -0.10f, 0.20f},
            .max = {0.10f, 0.10f, 0.80f},
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
    uint64_t observed_visible = 0u;
    uint64_t observed_occluded = 0u;
    uint64_t observed_unknown = 0u;
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
    CHECK(memcmp(
        scalar_visibility,
        neon_visibility,
        sizeof(scalar_visibility)
    ) == 0);
    CHECK(scalar_visibility[0] == SOC_VISIBILITY_OCCLUDED);
    CHECK(scalar_visibility[1] == SOC_VISIBILITY_VISIBLE);
    CHECK(scalar_visibility[2] == SOC_VISIBILITY_UNKNOWN);
    CHECK(scalar_visibility[3] == SOC_VISIBILITY_UNKNOWN);
    for (index = 0u; index < ARRAY_COUNT(scalar_visibility); ++index) {
        if (scalar_visibility[index] == SOC_VISIBILITY_VISIBLE) {
            ++observed_visible;
        } else if (scalar_visibility[index] == SOC_VISIBILITY_OCCLUDED) {
            ++observed_occluded;
        } else {
            CHECK(scalar_visibility[index] == SOC_VISIBILITY_UNKNOWN);
            ++observed_unknown;
        }
    }
    CHECK(scalar_stats.reserved == neon_stats.reserved);
    CHECK(scalar_stats.tested_aabb_count == neon_stats.tested_aabb_count);
    CHECK(scalar_stats.visible_aabb_count == neon_stats.visible_aabb_count);
    CHECK(scalar_stats.occluded_aabb_count == neon_stats.occluded_aabb_count);
    CHECK(scalar_stats.unknown_aabb_count == neon_stats.unknown_aabb_count);
    CHECK(scalar_stats.tested_aabb_count == ARRAY_COUNT(bounds));
    CHECK(scalar_stats.visible_aabb_count == observed_visible);
    CHECK(scalar_stats.occluded_aabb_count == observed_occluded);
    CHECK(scalar_stats.unknown_aabb_count == observed_unknown);
    CHECK(observed_visible + observed_occluded + observed_unknown ==
        ARRAY_COUNT(bounds));
    CHECK(observed_visible != 0u);
    CHECK(observed_occluded != 0u);
    CHECK(observed_unknown != 0u);
    return 0;
}

static int test_pipeline_differential(void)
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
    size_t direction_index;

    if (soc_kernel_table_neon() == NULL) {
        return 0;
    }

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        for (direction_index = 0u;
             direction_index < ARRAY_COUNT(depth_directions);
             ++direction_index) {
            soc_snapshot* scalar = NULL;
            soc_snapshot* neon = NULL;

            CHECK(build_snapshot(
                SOC_KERNEL_BACKEND_SCALAR,
                clip_depth_ranges[range_index],
                depth_directions[direction_index],
                &scalar
            ) == SOC_RESULT_OK);
            CHECK(build_snapshot(
                SOC_KERNEL_BACKEND_NEON,
                clip_depth_ranges[range_index],
                depth_directions[direction_index],
                &neon
            ) == SOC_RESULT_OK);
            CHECK(compare_snapshots(scalar, neon) == 0);
            CHECK(compare_queries(scalar, neon) == 0);

            soc_snapshot_destroy_internal(neon);
            soc_snapshot_destroy_internal(scalar);
        }
    }
    return 0;
}

int main(void)
{
    return test_pipeline_differential();
}
