#ifndef SOC_TYPES_H_INCLUDED
#define SOC_TYPES_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#define SOC_ABI_VERSION_MAJOR 1u
#define SOC_ABI_VERSION_MINOR 0u
#define SOC_ABI_VERSION \
    ((SOC_ABI_VERSION_MAJOR << 16u) | SOC_ABI_VERSION_MINOR)

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

#define SOC_CLIP_DEPTH_ZERO_TO_ONE ((soc_clip_depth_range)0u)
#define SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE ((soc_clip_depth_range)1u)

typedef uint32_t soc_depth_direction;

#define SOC_DEPTH_FORWARD ((soc_depth_direction)0u)
#define SOC_DEPTH_REVERSED ((soc_depth_direction)1u)

typedef uint32_t soc_front_face;

#define SOC_FRONT_FACE_CCW ((soc_front_face)0u)
#define SOC_FRONT_FACE_CW ((soc_front_face)1u)

#define SOC_CONFIG_FLAG_NONE 0u

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
    soc_mat4 clip_from_world;
    soc_clip_depth_range clip_depth_range;
    soc_depth_direction depth_direction;
    soc_front_face front_face;
    uint32_t flags;
} soc_frame_desc;

#define SOC_FRAME_DESC_SIZE_V1 \
    ((uint32_t)(offsetof(soc_frame_desc, flags) + sizeof(uint32_t)))

#define SOC_MESH_FLAG_NONE 0u
#define SOC_MESH_FLAG_TWO_SIDED (1u << 0u)

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

typedef struct soc_stats {
    uint32_t struct_size;
    uint32_t hiz_level_count;

    uint64_t input_triangle_count;
    uint64_t clipped_triangle_count;
    uint64_t rasterized_triangle_count;
    uint64_t tested_aabb_count;
    uint64_t occluded_aabb_count;
} soc_stats;

#define SOC_STATS_SIZE_V1 \
    ((uint32_t)(offsetof(soc_stats, occluded_aabb_count) + sizeof(uint64_t)))

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
