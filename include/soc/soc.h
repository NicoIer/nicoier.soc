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

/*
 * All floating-point inputs to this API must be finite and within ranges that
 * keep the documented calculations representable. Passing NaN, infinity, or
 * values that overflow intermediate calculations violates the API contract
 * and results in undefined behavior.
 */

SOC_API uint32_t SOC_CALL soc_get_abi_version(void);

/*
 * config->worker_count is the total execution lane count, including the
 * thread which calls build. Zero selects the online logical CPU count (clamped
 * to 1..SOC_MAX_WORKER_COUNT); one keeps build execution serial.
 */
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

/*
 * Reports the CPU ISA features available to this process and the execution
 * backend actually selected by this context. A NEON-capable CPU only means
 * SIMD is active when execution_backend is SOC_EXECUTION_BACKEND_NEON.
 * The caller must initialize out_info->struct_size before calling.
 */
SOC_API soc_result SOC_CALL soc_context_get_runtime_info(
    const soc_context* context,
    soc_runtime_info* out_info
);

/*
 * Synchronously validates and copies the described positions and indices into
 * an immutable native-owned mesh. No caller pointer is retained on success, so
 * the source buffers may be released when the call returns. On failure,
 * *out_mesh is null and no partial mesh is attached to the context.
 *
 * soc_mesh_desc V1 has no buffer-size fields. The caller must keep sufficiently
 * large input buffers valid and unmodified for the duration of this call.
 */
SOC_API soc_result SOC_CALL soc_mesh_create(
    soc_context* context,
    const soc_mesh_desc* desc,
    soc_mesh** out_mesh
);

SOC_API soc_result SOC_CALL soc_mesh_destroy(soc_mesh* mesh);

/*
 * Synchronously consumes the complete occluder input and publishes an immutable
 * snapshot containing Level 0 depth, derived Hi-Z levels, and build statistics.
 * No caller-owned frame, group, or transform pointer is retained after this
 * call.
 * A successful snapshot no longer references its source meshes or context and
 * may outlive both.
 */
SOC_API soc_result SOC_CALL soc_occlusion_build(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
);

/* Accepts null and releases all storage owned by the snapshot. */
SOC_API void SOC_CALL soc_snapshot_destroy(soc_snapshot* snapshot);

/*
 * Conservatively projects world-space AABBs and tests them against Hi-Z.
 * SOC_VISIBILITY_OCCLUDED is written only when the selected Hi-Z coverage
 * strictly proves the complete projected bounds to be behind occluder depth.
 * Unordered, near-plane-crossing, or otherwise unprojectable bounds produce
 * SOC_VISIBILITY_UNKNOWN (fail-open); other bounds produce
 * SOC_VISIBILITY_VISIBLE. The frame's clip depth range is honored under the
 * Reverse-Z convention. When out_stats is non-null it receives counters for
 * this call only; the snapshot itself is never mutated by a query.
 */
SOC_API soc_result SOC_CALL soc_snapshot_test_aabbs(
    const soc_snapshot* snapshot,
    const soc_aabb* world_bounds,
    uint32_t bounds_count,
    soc_visibility* out_visibility,
    soc_query_stats* out_stats
);

SOC_API soc_result SOC_CALL soc_snapshot_get_build_stats(
    const soc_snapshot* snapshot,
    soc_build_stats* out_stats
);

/*
 * Copies a depth Level into caller storage. Level 0 contains rasterized depth.
 * Each higher Level has ceil(previous / 2) dimensions and stores the minimum
 * of its valid Reverse-Z children.
 */
SOC_API soc_result SOC_CALL soc_snapshot_hiz_level_query(
    const soc_snapshot* snapshot,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
);

#ifdef __cplusplus
}
#endif

#endif
