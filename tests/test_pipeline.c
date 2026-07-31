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

int main(void)
{
    return run_pipeline();
}
