#include <soc/soc.h>

#include "core/soc_context.h"
#include "core/soc_mesh.h"
#include "core/soc_pipeline.h"
#include "core/soc_snapshot.h"

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

soc_result SOC_CALL soc_occlusion_build(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
)
{
    return soc_occlusion_build_internal(context, desc, out_snapshot);
}

void SOC_CALL soc_snapshot_destroy(soc_snapshot* snapshot)
{
    soc_snapshot_destroy_internal(snapshot);
}

soc_result SOC_CALL soc_snapshot_test_aabbs(
    const soc_snapshot* snapshot,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_query_stats* out_stats
)
{
    return soc_snapshot_test_aabbs_internal(
        snapshot,
        world_bounds,
        bounds_count,
        out_visibility,
        out_stats
    );
}

soc_result SOC_CALL soc_snapshot_get_build_stats(
    const soc_snapshot* snapshot,
    soc_build_stats* out_stats
)
{
    return soc_snapshot_get_build_stats_internal(snapshot, out_stats);
}

soc_result SOC_CALL soc_snapshot_hiz_level_query(
    const soc_snapshot* snapshot,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
)
{
    return soc_snapshot_hiz_level_query_internal(
        snapshot,
        level,
        out_info,
        out_depth,
        out_depth_count
    );
}
