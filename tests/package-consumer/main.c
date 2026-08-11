#include <soc/soc.h>

int main(void)
{
    soc_occluder_group group = {0};
    soc_occlusion_build_desc build = {0};
    soc_frame_desc frame = {0};
    soc_build_stats build_stats = {0};
    soc_query_stats query_stats = {0};
    soc_snapshot* snapshot = NULL;
    const soc_bool device_supported = soc_device_is_supported();

    if (soc_get_abi_version() != SOC_ABI_VERSION ||
        SOC_ABI_VERSION_MAJOR != 3u) {
        return 1;
    }
    if (device_supported == SOC_FALSE) {
        return 0;
    }
    if (device_supported != SOC_TRUE) {
        return 1;
    }

    group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
    build.struct_size = sizeof(build);
    build.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    frame.struct_size = sizeof(frame);
    build.frame = &frame;
    build.groups = &group;
    build.group_count = 1u;
    build.group_stride = sizeof(group);
    build_stats.struct_size = sizeof(build_stats);
    query_stats.struct_size = sizeof(query_stats);

    soc_snapshot_destroy(snapshot);

    return SOC_OCCLUDER_GROUP_SIZE_V1 <= sizeof(group) &&
        SOC_OCCLUSION_BUILD_DESC_SIZE_V1 <= sizeof(build) &&
        SOC_BUILD_STATS_SIZE_V1 <= sizeof(build_stats) &&
        SOC_QUERY_STATS_SIZE_V1 <= sizeof(query_stats)
        ? 0
        : 1;
}
