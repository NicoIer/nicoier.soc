#ifndef SOC_H_INCLUDED
#define SOC_H_INCLUDED

#include <soc/soc_types.h>

#if defined(_WIN32)
    #if defined(SOC_STATIC)
        #define SOC_API
    #elif defined(SOC_BUILDING_LIBRARY)
        #define SOC_API __declspec(dllexport)
    #else
        #define SOC_API __declspec(dllimport)
    #endif
    #define SOC_CALL __cdecl
#elif defined(__GNUC__) || defined(__clang__)
    #define SOC_API __attribute__((visibility("default")))
    #define SOC_CALL
#else
    #define SOC_API
    #define SOC_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

SOC_API uint32_t SOC_CALL soc_get_abi_version(void);

SOC_API soc_result SOC_CALL soc_context_create(
    const soc_config* config,
    soc_context** out_context
);

SOC_API void SOC_CALL soc_context_destroy(soc_context* context);

SOC_API soc_result SOC_CALL soc_context_resize(
    soc_context* context,
    uint32_t width,
    uint32_t height
);

SOC_API soc_result SOC_CALL soc_mesh_create(
    soc_context* context,
    const soc_mesh_desc* desc,
    soc_mesh** out_mesh
);

SOC_API soc_result SOC_CALL soc_mesh_destroy(soc_mesh* mesh);

SOC_API soc_result SOC_CALL soc_frame_begin(
    soc_context* context,
    const soc_frame_desc* desc
);

SOC_API soc_result SOC_CALL soc_occluders_submit(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
);

SOC_API soc_result SOC_CALL soc_occluders_finish(soc_context* context);

SOC_API soc_result SOC_CALL soc_visibility_test_aabbs(
    soc_context* context,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility
);

SOC_API soc_result SOC_CALL soc_frame_end(soc_context* context);

SOC_API soc_result SOC_CALL soc_context_get_stats(
    const soc_context* context,
    soc_stats* out_stats
);

SOC_API soc_result SOC_CALL soc_hiz_level_query(
    const soc_context* context,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
);

#ifdef __cplusplus
}
#endif

#endif
