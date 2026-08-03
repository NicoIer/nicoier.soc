#include <soc/soc.h>

#include <stddef.h>
#include <stdint.h>

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

static int run_pipeline(void)
{
    const float vertices[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const uint16_t indices[] = {0u, 1u, 2u};
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
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
    const soc_mat4 identity = identity_matrix();
    const soc_frame_desc frame_desc = {
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
    const soc_aabb bounds = {
        .min = {-1.0f, -1.0f, 0.0f},
        .max = {1.0f, 1.0f, 1.0f},
    };
    soc_visibility visibility = SOC_VISIBILITY_OCCLUDED;
    soc_hiz_level_info level_info = {
        .struct_size = sizeof(soc_hiz_level_info),
    };
    soc_stats stats = {
        .struct_size = sizeof(soc_stats),
    };
    float depth = -1.0f;
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;

    if (soc_context_create(&config, &context) != SOC_RESULT_OK) {
        return 1;
    }
    if (soc_mesh_create(context, &mesh_desc, &mesh) != SOC_RESULT_OK) {
        soc_context_destroy(context);
        return 1;
    }

    if (soc_occluders_submit(context, mesh, &identity, 1u) !=
        SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_frame_begin(context, &frame_desc) != SOC_RESULT_OK) {
        return 1;
    }
    if (soc_frame_begin(context, &frame_desc) != SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_context_resize(context, 640u, 360u) !=
        SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_mesh_destroy(mesh) != SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_visibility_test_aabbs(context, &bounds, 1u, &visibility) !=
        SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_hiz_level_query(context, 0u, &level_info, NULL, 0u) !=
        SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_occluders_submit(context, mesh, &identity, 1u) != SOC_RESULT_OK) {
        return 1;
    }
    if (soc_occluders_finish(context) != SOC_RESULT_OK) {
        return 1;
    }
    if (soc_occluders_finish(context) != SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_hiz_level_query(context, 0u, &level_info, NULL, 0u) !=
        SOC_RESULT_OK) {
        return 1;
    }
    if (level_info.level != 0u ||
        level_info.width != 320u ||
        level_info.height != 180u ||
        level_info.required_element_count != 320u * 180u) {
        return 1;
    }
    if (soc_hiz_level_query(context, 0u, &level_info, &depth, 1u) !=
        SOC_RESULT_BUFFER_TOO_SMALL) {
        return 1;
    }
    if (soc_hiz_level_query(context, 9u, &level_info, &depth, 1u) !=
        SOC_RESULT_OK) {
        return 1;
    }
    if (level_info.width != 1u ||
        level_info.height != 1u ||
        level_info.required_element_count != 1u ||
        depth != 1.0f) {
        return 1;
    }
    if (soc_hiz_level_query(context, 10u, &level_info, NULL, 0u) !=
        SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    if (soc_visibility_test_aabbs(context, &bounds, 1u, &visibility) !=
        SOC_RESULT_OK) {
        return 1;
    }
    if (visibility != SOC_VISIBILITY_VISIBLE) {
        return 1;
    }
    if (soc_context_get_stats(context, &stats) != SOC_RESULT_OK) {
        return 1;
    }
    if (stats.hiz_level_count != 10u ||
        stats.input_triangle_count != 1u ||
        stats.clipped_triangle_count != 0u ||
        stats.rasterized_triangle_count != 1u ||
        stats.tested_aabb_count != 1u ||
        stats.occluded_aabb_count != 0u) {
        return 1;
    }
    if (soc_frame_end(context) != SOC_RESULT_OK) {
        return 1;
    }
    if (soc_frame_end(context) != SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_hiz_level_query(context, 0u, &level_info, NULL, 0u) !=
        SOC_RESULT_INVALID_STATE) {
        return 1;
    }
    if (soc_context_resize(context, 640u, 360u) != SOC_RESULT_OK) {
        return 1;
    }
    if (soc_mesh_destroy(mesh) != SOC_RESULT_OK) {
        return 1;
    }

    soc_context_destroy(context);
    return 0;
}

static int test_error_paths(void)
{
    const float vertices[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const uint16_t indices[] = {0u, 1u, 2u};
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 32u,
        .height = 16u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
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
    const soc_mat4 identity = identity_matrix();
    soc_frame_desc frame_desc = {
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
    const soc_aabb bounds = {
        .min = {-0.5f, -0.5f, 0.25f},
        .max = {0.5f, 0.5f, 0.75f},
    };
    soc_hiz_level_info level_info = {
        .struct_size = sizeof(soc_hiz_level_info),
    };
    soc_stats stats = {
        .struct_size = sizeof(soc_stats),
    };
    soc_visibility visibility = SOC_VISIBILITY_OCCLUDED;
    soc_context* owner = NULL;
    soc_context* other = NULL;
    soc_mesh* mesh = NULL;

    if (soc_context_create(&config, &owner) != SOC_RESULT_OK ||
        soc_context_create(&config, &other) != SOC_RESULT_OK ||
        soc_mesh_create(owner, &mesh_desc, &mesh) != SOC_RESULT_OK) {
        return 1;
    }

    if (soc_context_resize(NULL, 32u, 16u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_context_resize(owner, 0u, 16u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_context_resize(owner, 32u, 0u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_context_get_stats(NULL, &stats) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_context_get_stats(owner, NULL) !=
            SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }

    stats.struct_size = SOC_STATS_SIZE_V1 - 1u;
    if (soc_context_get_stats(owner, &stats) != SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    stats.struct_size = sizeof(stats);

    if (soc_frame_begin(NULL, &frame_desc) != SOC_RESULT_INVALID_ARGUMENT ||
        soc_frame_begin(owner, NULL) != SOC_RESULT_INVALID_ARGUMENT ||
        soc_occluders_submit(NULL, mesh, &identity, 1u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_occluders_finish(NULL) != SOC_RESULT_INVALID_ARGUMENT ||
        soc_visibility_test_aabbs(NULL, &bounds, 1u, &visibility) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_hiz_level_query(NULL, 0u, &level_info, NULL, 0u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_frame_end(NULL) != SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }

    frame_desc.struct_size = SOC_FRAME_DESC_SIZE_V1 - 1u;
    if (soc_frame_begin(owner, &frame_desc) != SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    frame_desc.struct_size = sizeof(frame_desc);
    frame_desc.flags = 1u;
    if (soc_frame_begin(owner, &frame_desc) != SOC_RESULT_UNSUPPORTED) {
        return 1;
    }
    frame_desc.flags = SOC_FRAME_FLAG_NONE;
    frame_desc.clip_depth_range = UINT32_MAX;
    if (soc_frame_begin(owner, &frame_desc) != SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    frame_desc.clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE;
    frame_desc.depth_direction = UINT32_MAX;
    if (soc_frame_begin(owner, &frame_desc) != SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    frame_desc.depth_direction = SOC_DEPTH_FORWARD;
    frame_desc.front_face = UINT32_MAX;
    if (soc_frame_begin(owner, &frame_desc) != SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    frame_desc.front_face = SOC_FRONT_FACE_CCW;

    if (soc_frame_begin(other, &frame_desc) != SOC_RESULT_OK ||
        soc_occluders_submit(other, NULL, NULL, 0u) != SOC_RESULT_OK ||
        soc_occluders_submit(other, NULL, &identity, 1u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_occluders_submit(other, mesh, NULL, 1u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_occluders_submit(other, mesh, &identity, 1u) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_occluders_finish(other) != SOC_RESULT_OK) {
        return 1;
    }

    if (soc_visibility_test_aabbs(other, NULL, 0u, NULL) != SOC_RESULT_OK ||
        soc_visibility_test_aabbs(other, NULL, 1u, &visibility) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_visibility_test_aabbs(other, &bounds, 1u, NULL) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        visibility != SOC_VISIBILITY_OCCLUDED ||
        soc_hiz_level_query(other, 0u, NULL, NULL, 0u) !=
            SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }

    if (soc_context_get_stats(other, &stats) != SOC_RESULT_OK ||
        stats.input_triangle_count != 0u ||
        stats.clipped_triangle_count != 0u ||
        stats.rasterized_triangle_count != 0u ||
        stats.tested_aabb_count != 0u ||
        stats.occluded_aabb_count != 0u ||
        soc_frame_end(other) != SOC_RESULT_OK) {
        return 1;
    }

    if (soc_mesh_destroy(mesh) != SOC_RESULT_OK) {
        return 1;
    }
    soc_context_destroy(other);
    soc_context_destroy(owner);
    return 0;
}

int main(void)
{
    if (run_pipeline() != 0) {
        return 1;
    }
    return test_error_paths();
}
