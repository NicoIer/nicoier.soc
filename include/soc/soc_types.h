#ifndef SOC_TYPES_H_INCLUDED
#define SOC_TYPES_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#define SOC_ABI_VERSION_MAJOR 3u
#define SOC_ABI_VERSION_MINOR 1u
#define SOC_ABI_VERSION \
    ((SOC_ABI_VERSION_MAJOR << 16u) | SOC_ABI_VERSION_MINOR)

/* Q8 raster coordinates are defined for each viewport axis up to this size. */
#define SOC_MAX_RASTER_DIMENSION UINT32_C(1048576)

typedef uint8_t soc_bool;

#define SOC_FALSE ((soc_bool)0u)
#define SOC_TRUE ((soc_bool)1u)

typedef int32_t soc_result;

#define SOC_RESULT_OK ((soc_result)0)
#define SOC_RESULT_INVALID_ARGUMENT ((soc_result)-1)
#define SOC_RESULT_OUT_OF_MEMORY ((soc_result)-2)
#define SOC_RESULT_UNSUPPORTED ((soc_result)-3)
#define SOC_RESULT_INTERNAL_ERROR ((soc_result)-4)
#define SOC_RESULT_INVALID_STATE ((soc_result)-5)
#define SOC_RESULT_BUFFER_TOO_SMALL ((soc_result)-6)

typedef struct soc_context soc_context;
typedef struct soc_mesh soc_mesh;
typedef struct soc_snapshot soc_snapshot;

typedef uint32_t soc_cpu_architecture;

#define SOC_CPU_ARCHITECTURE_UNKNOWN ((soc_cpu_architecture)0u)
#define SOC_CPU_ARCHITECTURE_X86 ((soc_cpu_architecture)1u)
#define SOC_CPU_ARCHITECTURE_ARM32 ((soc_cpu_architecture)2u)
#define SOC_CPU_ARCHITECTURE_ARM64 ((soc_cpu_architecture)3u)

typedef uint32_t soc_cpu_feature_flags;

#define SOC_CPU_FEATURE_NONE ((soc_cpu_feature_flags)0u)
#define SOC_CPU_FEATURE_SSE2 ((soc_cpu_feature_flags)(1u << 0u))
#define SOC_CPU_FEATURE_SSE4_1 ((soc_cpu_feature_flags)(1u << 1u))
#define SOC_CPU_FEATURE_AVX2 ((soc_cpu_feature_flags)(1u << 2u))
#define SOC_CPU_FEATURE_NEON ((soc_cpu_feature_flags)(1u << 3u))
#define SOC_CPU_FEATURE_ALL_KNOWN \
    (SOC_CPU_FEATURE_SSE2 | SOC_CPU_FEATURE_SSE4_1 | \
        SOC_CPU_FEATURE_AVX2 | SOC_CPU_FEATURE_NEON)

typedef uint32_t soc_execution_backend;

#define SOC_EXECUTION_BACKEND_SCALAR ((soc_execution_backend)0u)
#define SOC_EXECUTION_BACKEND_NEON ((soc_execution_backend)1u)

typedef struct soc_runtime_info {
    uint32_t struct_size;
    soc_cpu_architecture cpu_architecture;
    /* ISA features usable by the current process and operating system. */
    soc_cpu_feature_flags cpu_features;
    /* The backend actually selected by this context. */
    soc_execution_backend execution_backend;
    /* Total execution lanes, including the thread which calls build. */
    uint32_t worker_count;
} soc_runtime_info;

#define SOC_RUNTIME_INFO_SIZE_V1 \
    ((uint32_t)(offsetof(soc_runtime_info, worker_count) + sizeof(uint32_t)))

typedef struct soc_vector2 {
    float x;
    float y;
} soc_vector2;

typedef struct soc_vector3 {
    float x;
    float y;
    float z;
} soc_vector3;

typedef struct soc_vector4 {
    float x;
    float y;
    float z;
    float w;
} soc_vector4;

typedef struct soc_mat4 {
    soc_vector4 col0;
    soc_vector4 col1;
    soc_vector4 col2;
    soc_vector4 col3;
} soc_mat4;

typedef struct soc_aabb {
    soc_vector3 min;
    soc_vector3 max;
} soc_aabb;

typedef uint8_t soc_visibility;

/*
 * Zero-initialized output is fail-open. Cull an object only when its result is
 * exactly SOC_VISIBILITY_OCCLUDED.
 */
#define SOC_VISIBILITY_UNKNOWN ((soc_visibility)0u)
#define SOC_VISIBILITY_VISIBLE ((soc_visibility)1u)
#define SOC_VISIBILITY_OCCLUDED ((soc_visibility)2u)

typedef uint32_t soc_index_type;

#define SOC_INDEX_UINT16 ((soc_index_type)0u)
#define SOC_INDEX_UINT32 ((soc_index_type)1u)

typedef uint32_t soc_clip_depth_range;

/* Homogeneous clip-space Z is [0, w] or [-w, w], respectively. */
#define SOC_CLIP_DEPTH_ZERO_TO_ONE ((soc_clip_depth_range)0u)
#define SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE ((soc_clip_depth_range)1u)

typedef uint32_t soc_front_face;

/* Winding is evaluated in NDC XY with positive Y up, before viewport Y flip. */
#define SOC_FRONT_FACE_CCW ((soc_front_face)0u)
#define SOC_FRONT_FACE_CW ((soc_front_face)1u)

#define SOC_CONFIG_FLAG_NONE 0u
#define SOC_MAX_WORKER_COUNT UINT32_C(256)

typedef struct soc_config {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t worker_count;
    uint32_t flags;
} soc_config;

#define SOC_CONFIG_SIZE_V1 \
    ((uint32_t)(offsetof(soc_config, flags) + sizeof(uint32_t)))

#define SOC_FRAME_FLAG_NONE 0u

typedef struct soc_frame_desc {
    uint32_t struct_size;
    /* Reverse-Z projection; stored depth maps near to 1 and far toward 0. */
    soc_mat4 clip_from_world;
    soc_clip_depth_range clip_depth_range;
    soc_front_face front_face;
    uint32_t flags;
} soc_frame_desc;

#define SOC_FRAME_DESC_SIZE_V1 \
    ((uint32_t)(offsetof(soc_frame_desc, flags) + sizeof(uint32_t)))

#define SOC_OCCLUDER_GROUP_FLAG_NONE 0u

/*
 * A group references one immutable mesh and a contiguous array of instance
 * transforms. The pointed-to transforms are borrowed only for the duration of
 * soc_occlusion_build().
 */
typedef struct soc_occluder_group {
    const soc_mesh* mesh;
    const soc_mat4* object_to_world;
    uint32_t instance_count;
    uint32_t flags;
} soc_occluder_group;

#define SOC_OCCLUDER_GROUP_SIZE_V1 \
    ((uint32_t)(offsetof(soc_occluder_group, flags) + sizeof(uint32_t)))

#define SOC_OCCLUSION_BUILD_FLAG_NONE 0u

/*
 * Describes the complete occluder input for one immutable snapshot. frame,
 * groups, and the transforms referenced by each group are borrowed only for the
 * duration of soc_occlusion_build(). group_stride allows future group records
 * to append fields without changing this container.
 */
typedef struct soc_occlusion_build_desc {
    uint32_t struct_size;
    uint32_t flags;
    const soc_frame_desc* frame;
    const soc_occluder_group* groups;
    uint32_t group_count;
    uint32_t group_stride;
} soc_occlusion_build_desc;

#define SOC_OCCLUSION_BUILD_DESC_SIZE_V1 \
    ((uint32_t)(offsetof(soc_occlusion_build_desc, group_stride) + \
        sizeof(uint32_t)))

#define SOC_MESH_FLAG_NONE 0u
/* Disable face culling for this mesh. */
#define SOC_MESH_FLAG_TWO_SIDED (1u << 0u)

/*
 * Version 1 describes borrowed, read-only mesh input. Vertices are
 * vertex_stride-byte records with three consecutive float position components
 * at position_offset. Indices are tightly packed elements selected by
 * index_type. V1 has no buffer-size fields; callers must provide complete
 * readable ranges for the duration of soc_mesh_create().
 */
typedef struct soc_mesh_desc {
    uint32_t struct_size;
    uint32_t flags;

    const void* vertices;
    const void* indices;

    uint32_t vertex_count;
    uint32_t vertex_stride;
    uint32_t position_offset;
    uint32_t index_count;
    soc_index_type index_type;
} soc_mesh_desc;

#define SOC_MESH_DESC_SIZE_V1 \
    ((uint32_t)(offsetof(soc_mesh_desc, index_type) + sizeof(soc_index_type)))

typedef struct soc_build_stats {
    uint32_t struct_size;
    uint32_t hiz_level_count;

    /* Submitted source triangles, including all instances. */
    uint64_t input_triangle_count;
    /* Source triangles changed or rejected by homogeneous clipping. */
    uint64_t clipped_triangle_count;
    /* Post-clip triangles that survive face and degeneracy checks. */
    uint64_t rasterized_triangle_count;
} soc_build_stats;

#define SOC_BUILD_STATS_SIZE_V1 \
    ((uint32_t)(offsetof(soc_build_stats, rasterized_triangle_count) + \
        sizeof(uint64_t)))

typedef struct soc_query_stats {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t tested_aabb_count;
    uint64_t visible_aabb_count;
    uint64_t occluded_aabb_count;
    uint64_t unknown_aabb_count;
} soc_query_stats;

#define SOC_QUERY_STATS_SIZE_V1 \
    ((uint32_t)(offsetof(soc_query_stats, unknown_aabb_count) + \
        sizeof(uint64_t)))

typedef struct soc_hiz_level_info {
    uint32_t struct_size;
    uint32_t level;
    uint32_t width;
    uint32_t height;
    uint64_t required_element_count;
} soc_hiz_level_info;

#define SOC_HIZ_LEVEL_INFO_SIZE_V1 \
    ((uint32_t)(offsetof(soc_hiz_level_info, required_element_count) + \
        sizeof(uint64_t)))

#endif
