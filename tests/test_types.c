#include <soc/soc_types.h>

#include <stddef.h>

_Static_assert(sizeof(soc_bool) == 1u, "soc_bool must be one byte");
_Static_assert(sizeof(soc_result) == 4u, "soc_result must be four bytes");
_Static_assert(sizeof(soc_visibility) == 1u, "soc_visibility must be one byte");
_Static_assert(sizeof(soc_index_type) == 4u, "soc_index_type must be four bytes");
_Static_assert(sizeof(soc_vector2) == 8u, "soc_vector2 must contain two floats");
_Static_assert(sizeof(soc_vector3) == 12u, "soc_vector3 must contain three floats");
_Static_assert(sizeof(soc_vector4) == 16u, "soc_vector4 must contain four floats");
_Static_assert(sizeof(soc_mat4) == 64u, "soc_mat4 must contain four columns");
_Static_assert(sizeof(soc_aabb) == 24u, "soc_aabb must contain two vectors");
_Static_assert(
    sizeof(soc_config) == SOC_CONFIG_SIZE_V1,
    "soc_config V1 layout changed"
);
_Static_assert(
    sizeof(soc_frame_desc) == SOC_FRAME_DESC_SIZE_V1,
    "soc_frame_desc V1 layout changed"
);
_Static_assert(
    SOC_MESH_DESC_SIZE_V1 <= sizeof(soc_mesh_desc),
    "soc_mesh_desc V1 layout is too small"
);
_Static_assert(
    SOC_OCCLUDER_GROUP_SIZE_V1 <= sizeof(soc_occluder_group),
    "soc_occluder_group V1 layout is too small"
);
_Static_assert(
    SOC_OCCLUSION_BUILD_DESC_SIZE_V1 <= sizeof(soc_occlusion_build_desc),
    "soc_occlusion_build_desc V1 layout is too small"
);
_Static_assert(
    sizeof(soc_build_stats) == SOC_BUILD_STATS_SIZE_V1,
    "soc_build_stats V1 layout changed"
);
_Static_assert(
    sizeof(soc_query_stats) == SOC_QUERY_STATS_SIZE_V1,
    "soc_query_stats V1 layout changed"
);
_Static_assert(
    sizeof(soc_hiz_level_info) == SOC_HIZ_LEVEL_INFO_SIZE_V1,
    "soc_hiz_level_info V1 layout changed"
);
_Static_assert(
    offsetof(soc_frame_desc, clip_from_world) == 4u,
    "soc_frame_desc matrix offset changed"
);
_Static_assert(SOC_FALSE == 0u, "SOC_FALSE must be zero");
_Static_assert(SOC_TRUE != 0u, "SOC_TRUE must be nonzero");
_Static_assert(SOC_RESULT_OK == 0, "SOC_RESULT_OK must be zero");
_Static_assert(
    SOC_RESULT_INVALID_ARGUMENT < 0,
    "error results must be negative"
);
_Static_assert(
    SOC_VISIBILITY_UNKNOWN == 0u,
    "unknown visibility must be zero"
);
_Static_assert(
    SOC_VISIBILITY_VISIBLE != SOC_VISIBILITY_OCCLUDED,
    "visible and occluded values must differ"
);
_Static_assert(
    SOC_ABI_VERSION ==
        ((SOC_ABI_VERSION_MAJOR << 16u) | SOC_ABI_VERSION_MINOR),
    "SOC_ABI_VERSION encoding changed"
);

int main(void)
{
    return 0;
}
