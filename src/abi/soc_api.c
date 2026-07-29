#include <soc/soc.h>

#include "core/soc_context.h"
#include "core/soc_mesh.h"
#include "core/soc_pipeline.h"

uint32_t SOC_CALL soc_get_abi_version(void)
{
    return SOC_ABI_VERSION;
}

soc_result SOC_CALL soc_context_create(
    const soc_config* config,
    soc_context** out_context
)
{
    return soc_context_create_internal(config, out_context);
}

void SOC_CALL soc_context_destroy(soc_context* context)
{
    soc_context_destroy_internal(context);
}

soc_result SOC_CALL soc_context_resize(
    soc_context* context,
    uint32_t width,
    uint32_t height
)
{
    return soc_context_resize_internal(context, width, height);
}

soc_result SOC_CALL soc_mesh_create(
    soc_context* context,
    const soc_mesh_desc* desc,
    soc_mesh** out_mesh
)
{
    return soc_mesh_create_internal(context, desc, out_mesh);
}

soc_result SOC_CALL soc_mesh_destroy(soc_mesh* mesh)
{
    return soc_mesh_destroy_internal(mesh);
}

soc_result SOC_CALL soc_frame_begin(
    soc_context* context,
    const soc_frame_desc* desc
)
{
    return soc_frame_begin_internal(context, desc);
}

soc_result SOC_CALL soc_occluders_submit(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
)
{
    return soc_occluders_submit_internal(
        context,
        mesh,
        object_to_world,
        instance_count
    );
}

soc_result SOC_CALL soc_occluders_finish(soc_context* context)
{
    return soc_occluders_finish_internal(context);
}

soc_result SOC_CALL soc_visibility_test_aabbs(
    soc_context* context,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility
)
{
    return soc_visibility_test_aabbs_internal(
        context,
        world_bounds,
        bounds_count,
        out_visibility
    );
}

soc_result SOC_CALL soc_frame_end(soc_context* context)
{
    return soc_frame_end_internal(context);
}

soc_result SOC_CALL soc_context_get_stats(
    const soc_context* context,
    soc_stats* out_stats
)
{
    return soc_context_get_stats_internal(context, out_stats);
}

soc_result SOC_CALL soc_hiz_level_query(
    const soc_context* context,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
)
{
    return soc_hiz_level_query_internal(
        context,
        level,
        out_info,
        out_depth,
        out_depth_count
    );
}
