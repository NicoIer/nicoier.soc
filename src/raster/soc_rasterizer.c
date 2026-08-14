#include "raster/soc_rasterizer.h"

#include "core/soc_mesh.h"
#include "platform/soc_memory.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)
    #if defined(_MSC_VER) && !defined(__clang__)
        #include <arm64_neon.h>
    #else
        #include <arm_neon.h>
    #endif
#elif defined(SOC_BUILD_AARCH32_NEON_FMA)
    #if !defined(__arm__) || \
        !(defined(__ARM_NEON) || defined(__ARM_NEON__)) || \
        !defined(__ARM_FEATURE_FMA)
        #error "AArch32 SIMD requires ARM NEON with VFPv4 FMA"
    #endif
    #include <arm_neon.h>
#endif

#define SOC_CLIP_PLANE_COUNT 6u
#define SOC_CLIP_LATERAL_PLANE_MASK UINT8_C(0x0f)
#define SOC_CLIP_DEPTH_PLANE_MASK UINT8_C(0x30)
#define SOC_CLIP_GUARD_BAND_SCALE 4.0f
#define SOC_MAX_CLIPPED_VERTICES 12u
#define SOC_RASTER_SUBPIXEL_BITS 8u
#define SOC_RASTER_SUBPIXEL_SCALE \
    ((int64_t)1 << SOC_RASTER_SUBPIXEL_BITS)
#define SOC_RASTER_SUBPIXEL_HALF \
    (SOC_RASTER_SUBPIXEL_SCALE / 2)
#define SOC_RASTER_BLOCK_SIZE SOC_KERNEL_RASTER_BLOCK_SIZE
#define SOC_RASTER_SUBTILE_HEIGHT (SOC_RASTER_BLOCK_SIZE / 2u)
#define SOC_RASTER_UNTRACKED_TRIANGLE_SIZE UINT32_C(16)
#define SOC_RASTER_ARM32_FUSED_TRIANGLE_SIZE UINT32_C(3)
#define SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT \
    SOC_MESH_POST_TRANSFORM_CACHE_ENTRY_COUNT
#define SOC_POST_TRANSFORM_CACHE_MINIMUM_TRIANGLES UINT32_C(32)

#if defined(_MSC_VER)
#define SOC_NOINLINE __declspec(noinline)
#define SOC_MAYBE_UNUSED
#define SOC_RASTER_FORCE_INLINE static __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_NOINLINE __attribute__((noinline))
#define SOC_MAYBE_UNUSED __attribute__((unused))
#define SOC_RASTER_FORCE_INLINE \
    static inline __attribute__((always_inline))
#else
#define SOC_NOINLINE
#define SOC_MAYBE_UNUSED
#define SOC_RASTER_FORCE_INLINE static inline
#endif

_Static_assert(
    FLT_RADIX == 2 && FLT_MANT_DIG == 24 && sizeof(float) == 4u,
    "soc rasterization requires IEEE-754 binary32"
);
_Static_assert(
    SOC_RASTER_BLOCK_SIZE == 8u,
    "masked rasterization requires 8x4 subtiles"
);

typedef uint8_t soc_clip_outcode;

#define SOC_CLIP_OUTCODE_ALL \
    ((soc_clip_outcode)((1u << SOC_CLIP_PLANE_COUNT) - 1u))

typedef enum soc_clip_classification {
    SOC_CLIP_CLASSIFICATION_ACCEPT = 0,
    SOC_CLIP_CLASSIFICATION_REJECT,
    SOC_CLIP_CLASSIFICATION_PARTIAL,
} soc_clip_classification;

typedef soc_kernel_clip_vertex soc_clip_vertex;

typedef struct soc_screen_vertex {
    float x;
    float y;
    float depth;
    int64_t fixed_x;
    int64_t fixed_y;
} soc_screen_vertex;

typedef struct soc_post_transform_cache {
    soc_clip_vertex vertices[SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT];
    soc_screen_vertex screens[SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT];
    uint32_t indices[SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT];
    soc_clip_outcode outcodes[SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT];
    uint8_t screen_valid[SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT];
} soc_post_transform_cache;

typedef struct soc_fixed_vertex {
    int64_t x;
    int64_t y;
} soc_fixed_vertex;

typedef soc_raster_prepared_edge soc_edge_equation;
typedef soc_raster_prepared_region soc_raster_region;

typedef struct soc_tile_edge {
    float value;
    float step_x;
    float step_y;
} soc_tile_edge;

typedef struct soc_raster_block_edge_cursor {
    soc_tile_edge edges[3];
    int64_t values[3];
    int64_t first_block_increments[3];
    int64_t full_block_increments[3];
} soc_raster_block_edge_cursor;

typedef enum soc_raster_setup_result {
    SOC_RASTER_SETUP_REJECTED = 0,
    SOC_RASTER_SETUP_EMPTY,
    SOC_RASTER_SETUP_READY,
} soc_raster_setup_result;

typedef enum soc_raster_block_classification {
    SOC_RASTER_BLOCK_OUTSIDE = 0,
    SOC_RASTER_BLOCK_PARTIAL,
    SOC_RASTER_BLOCK_FULL,
} soc_raster_block_classification;

typedef soc_raster_prepared_triangle soc_raster_triangle_setup;

typedef struct soc_raster_depth_plane {
    float anchor_x;
    float anchor_y;
    float anchor;
    float step_x;
    float step_y;
} soc_raster_depth_plane;

typedef struct soc_raster_depth_block_candidate {
    float depth_origin;
    float depth_step_x;
    float depth_step_y;
    float nearest_depth;
    float farthest_depth;
    soc_bool is_constant;
} soc_raster_depth_block_candidate;

_Static_assert(
    sizeof(soc_raster_prepared_edge) == 24u,
    "prepared edge layout must remain 24 bytes"
);
_Static_assert(
    sizeof(soc_raster_prepared_triangle) == 108u ||
        sizeof(soc_raster_prepared_triangle) == 112u,
    "prepared triangle layout must remain compact"
);
_Static_assert(
    offsetof(soc_raster_prepared_triangle, end_tile_row) == 106u,
    "prepared triangle field layout must remain compact"
);
_Static_assert(
    SOC_MAX_RASTER_DIMENSION <= (UINT32_C(1) << 20u),
    "compact fixed edges require raster dimensions at most 2^20"
);
_Static_assert(
    (SOC_MAX_RASTER_DIMENSION + SOC_RASTER_LOCK_TILE_SIZE - 1u) /
        SOC_RASTER_LOCK_TILE_SIZE <= UINT16_MAX,
    "prepared tile ranges must fit in uint16_t"
);

static soc_bool checked_size_multiply(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || (right != 0u && left > SIZE_MAX / right)) {
        return SOC_FALSE;
    }

    *out_result = left * right;
    return SOC_TRUE;
}

static soc_bool calculate_early_z_block_grid(
    uint32_t width,
    uint32_t height,
    uint32_t* out_column_count,
    uint32_t* out_row_count,
    size_t* out_block_count
)
{
    const uint32_t column_count =
        width / SOC_RASTER_BLOCK_SIZE +
        (width % SOC_RASTER_BLOCK_SIZE != 0u ? 1u : 0u);
    const uint32_t row_count =
        height / SOC_RASTER_BLOCK_SIZE +
        (height % SOC_RASTER_BLOCK_SIZE != 0u ? 1u : 0u);
    size_t block_count;

    if (out_column_count == NULL ||
        out_row_count == NULL ||
        out_block_count == NULL ||
        !checked_size_multiply(
            (size_t)column_count,
            (size_t)row_count,
            &block_count
        )) {
        return SOC_FALSE;
    }

    *out_column_count = column_count;
    *out_row_count = row_count;
    *out_block_count = block_count;
    return SOC_TRUE;
}

static soc_bool calculate_masked_subtile_grid(
    uint32_t width,
    uint32_t height,
    uint32_t* out_column_count,
    uint32_t* out_row_count,
    size_t* out_subtile_count
)
{
    const uint32_t column_count =
        width / SOC_RASTER_BLOCK_SIZE +
        (width % SOC_RASTER_BLOCK_SIZE != 0u ? 1u : 0u);
    const uint32_t row_count =
        height / SOC_RASTER_SUBTILE_HEIGHT +
        (height % SOC_RASTER_SUBTILE_HEIGHT != 0u ? 1u : 0u);
    size_t subtile_count;

    if (out_column_count == NULL ||
        out_row_count == NULL ||
        out_subtile_count == NULL ||
        !checked_size_multiply(
            (size_t)column_count,
            (size_t)row_count,
            &subtile_count
        )) {
        return SOC_FALSE;
    }

    *out_column_count = column_count;
    *out_row_count = row_count;
    *out_subtile_count = subtile_count;
    return SOC_TRUE;
}

static void* allocate_early_z_storage(
    size_t block_count,
    size_t tile_count,
    float** out_farthest_depths,
    float** out_tile_farthest_depths
)
{
    const size_t farthest_bytes = block_count * sizeof(float);
    const size_t tile_offset =
        (farthest_bytes + 63u) & ~(size_t)63u;
    const size_t allocation_size =
        tile_offset + tile_count * sizeof(float);
    uint8_t* storage = soc_aligned_alloc(64u, allocation_size);

    if (storage != NULL) {
        *out_farthest_depths = (float*)storage;
        *out_tile_farthest_depths = (float*)(storage + tile_offset);
    }
    return storage;
}

static uint64_t* allocate_early_z_pending_masks(size_t block_count)
{
    return soc_aligned_alloc(64u, block_count * sizeof(uint64_t));
}

static soc_bool prepared_list_is_valid(
    const soc_raster_prepared_list* list
)
{
    if (list == NULL || list->count > list->capacity) {
        return SOC_FALSE;
    }
    if (list->capacity == 0u) {
        return list->data == NULL && list->count == 0u
            ? SOC_TRUE
            : SOC_FALSE;
    }
    return list->data != NULL ? SOC_TRUE : SOC_FALSE;
}

soc_result soc_raster_prepared_list_reserve(
    soc_raster_prepared_list* list,
    size_t minimum_capacity
)
{
    soc_raster_prepared_triangle* allocation;
    size_t allocation_size;

    if (prepared_list_is_valid(list) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (minimum_capacity <= list->capacity) {
        return SOC_RESULT_OK;
    }
    if (!checked_size_multiply(
            minimum_capacity,
            sizeof(*allocation),
            &allocation_size
        )) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    allocation = soc_aligned_alloc(128u, allocation_size);
    if (allocation == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    if (list->count != 0u) {
        memcpy(
            allocation,
            list->data,
            list->count * sizeof(*allocation)
        );
    }
    soc_aligned_free(list->data);
    list->data = allocation;
    list->capacity = minimum_capacity;
    return SOC_RESULT_OK;
}

void soc_raster_prepared_list_shutdown(
    soc_raster_prepared_list* list
)
{
    if (list == NULL) {
        return;
    }

    soc_aligned_free(list->data);
    memset(list, 0, sizeof(*list));
}

static soc_result append_prepared_triangle(
    soc_raster_prepared_list* list,
    const soc_raster_prepared_triangle* prepared
)
{
    if (list->count == list->capacity) {
        size_t new_capacity;
        soc_result result;

        if (list->capacity == 0u) {
            new_capacity = 64u;
        } else if (list->capacity <= SIZE_MAX / 2u) {
            new_capacity = list->capacity * 2u;
        } else if (list->capacity < SIZE_MAX) {
            new_capacity = SIZE_MAX;
        } else {
            return SOC_RESULT_OUT_OF_MEMORY;
        }
        result = soc_raster_prepared_list_reserve(list, new_capacity);
        if (result != SOC_RESULT_OK) {
            return result;
        }
    }

    list->data[list->count] = *prepared;
    ++list->count;
    return SOC_RESULT_OK;
}

static SOC_MAYBE_UNUSED soc_bool calculate_tile_lock_grid(
    uint32_t width,
    uint32_t height,
    uint32_t* out_column_count,
    uint32_t* out_row_count,
    size_t* out_lock_count
)
{
    uint32_t column_count;
    uint32_t row_count;
    size_t lock_count;

    if (width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        out_column_count == NULL ||
        out_row_count == NULL ||
        out_lock_count == NULL) {
        return SOC_FALSE;
    }

    column_count = width / SOC_RASTER_LOCK_TILE_SIZE;
    if (width % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++column_count;
    }
    row_count = height / SOC_RASTER_LOCK_TILE_SIZE;
    if (height % SOC_RASTER_LOCK_TILE_SIZE != 0u) {
        ++row_count;
    }
    if (!checked_size_multiply(
            (size_t)column_count,
            (size_t)row_count,
            &lock_count
        )) {
        return SOC_FALSE;
    }

    *out_column_count = column_count;
    *out_row_count = row_count;
    *out_lock_count = lock_count;
    return SOC_TRUE;
}

static uint32_t read_mesh_index(
    const soc_mesh* mesh,
    uint32_t index
)
{
    const unsigned char* source = mesh->indices;

    if (mesh->index_type == SOC_INDEX_UINT16) {
        uint16_t value;
        memcpy(&value, source + (size_t)index * sizeof(value), sizeof(value));
        return value;
    }

    uint32_t value;
    memcpy(&value, source + (size_t)index * sizeof(value), sizeof(value));
    return value;
}

static float reciprocal_f32(float value)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    const float32x2_t input = vdup_n_f32(value);
    float32x2_t reciprocal = vrecpe_f32(input);

    reciprocal = vmul_f32(reciprocal, vrecps_f32(input, reciprocal));
    return vget_lane_f32(reciprocal, 0);
#elif defined(SOC_BUILD_AARCH32_NEON_FMA)
    const float32x2_t input = vdup_n_f32(value);
    float32x2_t reciprocal = vrecpe_f32(input);

    reciprocal = vmul_f32(reciprocal, vrecps_f32(input, reciprocal));
    return vget_lane_f32(reciprocal, 0);
#else
    return 1.0f / value;
#endif
}

static void reciprocal3_f32(
    float value0,
    float value1,
    float value2,
    float out_reciprocal[3]
)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    const float values[4] = {value0, value1, value2, 1.0f};
    const float32x4_t input = vld1q_f32(values);
    float32x4_t reciprocal = vrecpeq_f32(input);
    float results[4];

    reciprocal = vmulq_f32(
        reciprocal,
        vrecpsq_f32(input, reciprocal)
    );
    vst1q_f32(results, reciprocal);
    out_reciprocal[0] = results[0];
    out_reciprocal[1] = results[1];
    out_reciprocal[2] = results[2];
#elif defined(SOC_BUILD_AARCH32_NEON_FMA)
    const float values[4] = {value0, value1, value2, 1.0f};
    const float32x4_t input = vld1q_f32(values);
    float32x4_t reciprocal = vrecpeq_f32(input);
    float results[4];

    reciprocal = vmulq_f32(
        reciprocal,
        vrecpsq_f32(input, reciprocal)
    );
    vst1q_f32(results, reciprocal);
    out_reciprocal[0] = results[0];
    out_reciprocal[1] = results[1];
    out_reciprocal[2] = results[2];
#else
    out_reciprocal[0] = 1.0f / value0;
    out_reciprocal[1] = 1.0f / value1;
    out_reciprocal[2] = 1.0f / value2;
#endif
}

static float clip_plane_distance(
    const soc_clip_vertex* vertex,
    uint32_t plane,
    soc_clip_depth_range depth_range
)
{
    switch (plane) {
        case 0u:
            return vertex->x + vertex->w;
        case 1u:
            return vertex->w - vertex->x;
        case 2u:
            return vertex->y + vertex->w;
        case 3u:
            return vertex->w - vertex->y;
        case 4u:
            return depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
                ? vertex->z
                : vertex->z + vertex->w;
        default:
            return vertex->w - vertex->z;
    }
}

SOC_RASTER_FORCE_INLINE soc_bool triangle_inside_clip_guard_band(
    const soc_clip_vertex vertices[3],
    soc_clip_outcode active_planes
)
{
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        const float guard_w =
            vertices[index].w * SOC_CLIP_GUARD_BAND_SCALE;

        if (((active_planes & (UINT8_C(1) << 0u)) != 0u &&
                vertices[index].x + guard_w < 0.0f) ||
            ((active_planes & (UINT8_C(1) << 1u)) != 0u &&
                guard_w - vertices[index].x < 0.0f) ||
            ((active_planes & (UINT8_C(1) << 2u)) != 0u &&
                vertices[index].y + guard_w < 0.0f) ||
            ((active_planes & (UINT8_C(1) << 3u)) != 0u &&
                guard_w - vertices[index].y < 0.0f)) {
            return SOC_FALSE;
        }
    }
    return SOC_TRUE;
}

static void initialize_post_transform_cache(soc_post_transform_cache* cache)
{
    uint32_t entry;

    for (entry = 0u;
         entry < SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT;
         ++entry) {
        cache->indices[entry] = UINT32_MAX;
    }
}

SOC_RASTER_FORCE_INLINE soc_bool find_post_transform_cache_vertex(
    const soc_post_transform_cache* cache,
    uint32_t mesh_index,
    soc_clip_vertex* out_vertex,
    soc_clip_outcode* out_outcode
)
{
    const uint32_t entry =
        (mesh_index ^ (mesh_index >> 4u)) &
            (SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT - 1u);

    if (cache->indices[entry] == mesh_index) {
        *out_vertex = cache->vertices[entry];
        *out_outcode = cache->outcodes[entry];
        return SOC_TRUE;
    }
    return SOC_FALSE;
}

SOC_RASTER_FORCE_INLINE void store_post_transform_cache_vertex(
    soc_post_transform_cache* cache,
    uint32_t mesh_index,
    const soc_clip_vertex* vertex,
    soc_clip_outcode outcode
)
{
    const uint32_t entry =
        (mesh_index ^ (mesh_index >> 4u)) &
            (SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT - 1u);

    cache->vertices[entry] = *vertex;
    cache->indices[entry] = mesh_index;
    cache->outcodes[entry] = outcode;
    cache->screen_valid[entry] = 0u;
}

SOC_RASTER_FORCE_INLINE soc_bool find_post_transform_cache_screen(
    const soc_post_transform_cache* cache,
    uint32_t mesh_index,
    soc_screen_vertex* out_screen
)
{
    const uint32_t entry =
        (mesh_index ^ (mesh_index >> 4u)) &
            (SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT - 1u);

    if (cache->indices[entry] == mesh_index &&
        cache->screen_valid[entry] != 0u) {
        *out_screen = cache->screens[entry];
        return SOC_TRUE;
    }
    return SOC_FALSE;
}

SOC_RASTER_FORCE_INLINE void store_post_transform_cache_screen(
    soc_post_transform_cache* cache,
    uint32_t mesh_index,
    const soc_screen_vertex* screen
)
{
    const uint32_t entry =
        (mesh_index ^ (mesh_index >> 4u)) &
            (SOC_POST_TRANSFORM_CACHE_ENTRY_COUNT - 1u);

    if (cache->indices[entry] == mesh_index) {
        cache->screens[entry] = *screen;
        cache->screen_valid[entry] = 1u;
    }
}

SOC_RASTER_FORCE_INLINE void transform_triangle_with_post_cache(
    const soc_kernel_mat4_f32* clip_from_object,
    const soc_mesh* mesh,
    const uint32_t mesh_indices[3],
    soc_clip_depth_range depth_range,
    soc_post_transform_cache* cache,
    soc_clip_vertex out_clip[3],
    soc_kernel_clip_metadata* out_metadata
)
{
    soc_clip_outcode outcodes[3];
    uint8_t transform_mask = 0u;
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        if (find_post_transform_cache_vertex(
                cache,
                mesh_indices[index],
                &out_clip[index],
                &outcodes[index]
            ) != SOC_TRUE) {
            transform_mask = (uint8_t)(
                transform_mask | (UINT8_C(1) << index)
            );
        }
    }
    if (transform_mask == 0u) {
        out_metadata->active_planes = (soc_clip_outcode)(
            outcodes[0] | outcodes[1] | outcodes[2]
        );
        out_metadata->common_planes = (soc_clip_outcode)(
            outcodes[0] & outcodes[1] & outcodes[2]
        );
        return;
    }

    soc_kernel_transform_triangle_post_cache_f32(
        clip_from_object,
        mesh->positions_xyz + (size_t)mesh_indices[0] * 3u,
        mesh->positions_xyz + (size_t)mesh_indices[1] * 3u,
        mesh->positions_xyz + (size_t)mesh_indices[2] * 3u,
        depth_range,
        out_clip,
        out_metadata,
        outcodes,
        transform_mask
    );
    for (index = 0u; index < 3u; ++index) {
        if ((transform_mask & (UINT8_C(1) << index)) == 0u) {
            continue;
        }
        store_post_transform_cache_vertex(
            cache,
            mesh_indices[index],
            &out_clip[index],
            outcodes[index]
        );
    }
}

static soc_clip_vertex interpolate_clip_vertex(
    const soc_clip_vertex* start,
    const soc_clip_vertex* end,
    float amount
)
{
    const soc_clip_vertex result = {
        fmaf(end->x - start->x, amount, start->x),
        fmaf(end->y - start->y, amount, start->y),
        fmaf(end->z - start->z, amount, start->z),
        fmaf(end->w - start->w, amount, start->w),
    };
    return result;
}

static uint32_t clip_polygon_against_plane(
    const soc_clip_vertex* input,
    uint32_t input_count,
    soc_clip_vertex* output,
    uint32_t plane,
    soc_clip_depth_range depth_range
)
{
    soc_clip_vertex previous;
    float previous_distance;
    soc_bool previous_inside;
    uint32_t output_count = 0u;
    uint32_t index;

    if (input_count == 0u) {
        return 0u;
    }

    previous = input[input_count - 1u];
    previous_distance = clip_plane_distance(
        &previous,
        plane,
        depth_range
    );
    previous_inside = previous_distance >= 0.0f ? SOC_TRUE : SOC_FALSE;

    for (index = 0u; index < input_count; ++index) {
        const soc_clip_vertex current = input[index];
        const float current_distance = clip_plane_distance(
            &current,
            plane,
            depth_range
        );
        const soc_bool current_inside =
            current_distance >= 0.0f ? SOC_TRUE : SOC_FALSE;

        if (current_inside != previous_inside) {
            const float denominator =
                previous_distance - current_distance;
            float amount = previous_distance * reciprocal_f32(denominator);

            if (amount < 0.0f) {
                amount = 0.0f;
            } else if (amount > 1.0f) {
                amount = 1.0f;
            }

            if (output_count >= SOC_MAX_CLIPPED_VERTICES) {
                return 0u;
            }
            output[output_count] = interpolate_clip_vertex(
                &previous,
                &current,
                amount
            );
            ++output_count;
        }

        if (current_inside == SOC_TRUE) {
            if (output_count >= SOC_MAX_CLIPPED_VERTICES) {
                return 0u;
            }
            output[output_count] = current;
            ++output_count;
        }

        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }

    return output_count;
}

static soc_bool polygon_inside_clip_plane(
    const soc_clip_vertex* vertices,
    uint32_t vertex_count,
    uint32_t plane,
    soc_clip_depth_range depth_range
)
{
    uint32_t index;

    for (index = 0u; index < vertex_count; ++index) {
        if (!(clip_plane_distance(
                &vertices[index],
                plane,
                depth_range
            ) >= 0.0f)) {
            return SOC_FALSE;
        }
    }
    return SOC_TRUE;
}

SOC_RASTER_FORCE_INLINE uint32_t clip_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex input_triangle[3],
    soc_clip_outcode active_planes,
    soc_clip_vertex output_polygon[SOC_MAX_CLIPPED_VERTICES]
)
{
    soc_clip_vertex buffer_a[SOC_MAX_CLIPPED_VERTICES];
    soc_clip_vertex buffer_b[SOC_MAX_CLIPPED_VERTICES];
    soc_clip_vertex* input = buffer_a;
    soc_clip_vertex* output = buffer_b;
    uint32_t vertex_count = 3u;
    uint32_t plane;
    uint32_t index;
    soc_bool polygon_changed = SOC_FALSE;

    for (index = 0u; index < 3u; ++index) {
        buffer_a[index] = input_triangle[index];
    }

    for (plane = 0u; plane < SOC_CLIP_PLANE_COUNT; ++plane) {
        const soc_clip_outcode plane_bit =
            (soc_clip_outcode)(1u << plane);
        soc_clip_vertex* swap;

        if ((active_planes & plane_bit) == 0u &&
            (polygon_changed != SOC_TRUE ||
                polygon_inside_clip_plane(
                    input,
                    vertex_count,
                    plane,
                    rasterizer->frame.clip_depth_range
                ) == SOC_TRUE)) {
            continue;
        }

        vertex_count = clip_polygon_against_plane(
            input,
            vertex_count,
            output,
            plane,
            rasterizer->frame.clip_depth_range
        );
        if (vertex_count == 0u) {
            return 0u;
        }

        swap = input;
        input = output;
        output = swap;
        polygon_changed = SOC_TRUE;
    }

    memcpy(
        output_polygon,
        input,
        (size_t)vertex_count * sizeof(*output_polygon)
    );
    return vertex_count;
}

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static void swap_screen_vertices(
    soc_screen_vertex* left,
    soc_screen_vertex* right
)
{
    const soc_screen_vertex temporary = *left;
    *left = *right;
    *right = temporary;
}

/*
 * AArch32 has a hardware signed-int32-to-float conversion, but Clang lowers
 * signed-int64-to-float to __aeabi_l2f.  Edge values for small screen-space
 * triangles normally fit int32, while the public maximum raster dimensions
 * still require the exact int64 fallback for large edges and areas.
 */
#if defined(SOC_BUILD_AARCH32_NEON_FMA)
static SOC_NOINLINE float fixed_i64_to_f32_wide_arm32(int64_t value)
{
    return (float)value;
}
#endif

SOC_RASTER_FORCE_INLINE float fixed_i64_to_f32(int64_t value)
{
#if defined(SOC_BUILD_AARCH32_NEON_FMA)
    if (__builtin_expect(
            value >= INT32_MIN && value <= INT32_MAX,
            1
        )) {
        return (float)(int32_t)value;
    }
    /* Keep the compiler from speculating __aeabi_l2f before the range test. */
    return fixed_i64_to_f32_wide_arm32(value);
#else
    return (float)value;
#endif
}

/*
 * An accepted guard-band vertex spans at most [-1.5, 2.5] raster extents;
 * therefore both a coordinate and the difference of two coordinates fit a
 * signed int32 under SOC_MAX_RASTER_DIMENSION.  Keep the int64 representation
 * for exact edge arithmetic, but avoid a conversion helper on AArch32.
 */
SOC_RASTER_FORCE_INLINE float screen_fixed_i64_to_f32(int64_t value)
{
#if defined(SOC_BUILD_AARCH32_NEON_FMA)
    return (float)(int32_t)value;
#else
    return (float)value;
#endif
}

_Static_assert(
    SOC_MAX_RASTER_DIMENSION <=
        INT32_MAX / (SOC_RASTER_SUBPIXEL_SCALE * INT64_C(4)),
    "guard-band fixed coordinate deltas must fit int32"
);

static int64_t quantize_screen_coordinate(float coordinate)
{
    const float scaled =
        coordinate * (float)SOC_RASTER_SUBPIXEL_SCALE;
#if defined(SOC_BUILD_AARCH32_NEON_FMA)
    return (int64_t)(int32_t)(scaled + 0.5f);
#else
    return (int64_t)(scaled + 0.5f);
#endif
}

static int64_t quantize_guard_band_screen_coordinate(float coordinate)
{
    const float scaled =
        coordinate * (float)SOC_RASTER_SUBPIXEL_SCALE;

#if defined(SOC_BUILD_AARCH32_NEON_FMA)
    return scaled >= 0.0f
        ? (int64_t)(int32_t)(scaled + 0.5f)
        : -(int64_t)(int32_t)(-scaled + 0.5f);
#else
    return scaled >= 0.0f
        ? (int64_t)(scaled + 0.5f)
        : -(int64_t)(-scaled + 0.5f);
#endif
}

static int64_t fixed_edge_value_at_pixel(
    const soc_edge_equation* edge,
    uint32_t pixel_x,
    uint32_t pixel_y
)
{
    return edge->sample_origin +
        edge->step_x * (int64_t)pixel_x +
        edge->step_y * (int64_t)pixel_y;
}

static void make_tile_edges(
    const soc_raster_triangle_setup* setup,
    uint32_t pixel_x,
    uint32_t pixel_y,
    soc_tile_edge out_edges[3]
)
{
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        const soc_edge_equation* edge = &setup->edges[edge_index];

        out_edges[edge_index].value = fixed_i64_to_f32(
            fixed_edge_value_at_pixel(edge, pixel_x, pixel_y)
        );
        out_edges[edge_index].step_x = fixed_i64_to_f32(edge->step_x);
        out_edges[edge_index].step_y = fixed_i64_to_f32(edge->step_y);
    }
}

SOC_RASTER_FORCE_INLINE void initialize_raster_block_edge_cursor(
    const soc_raster_triangle_setup* setup,
    uint32_t first_pixel_x,
    soc_raster_block_edge_cursor* cursor
)
{
    const uint32_t first_block_advance = SOC_RASTER_BLOCK_SIZE -
        (first_pixel_x & (SOC_RASTER_BLOCK_SIZE - 1u));
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        const soc_edge_equation* edge = &setup->edges[edge_index];

        cursor->edges[edge_index].step_x =
            fixed_i64_to_f32(edge->step_x);
        cursor->edges[edge_index].step_y =
            fixed_i64_to_f32(edge->step_y);
        cursor->first_block_increments[edge_index] =
            edge->step_x * (int64_t)first_block_advance;
        cursor->full_block_increments[edge_index] =
            edge->step_x * (int64_t)SOC_RASTER_BLOCK_SIZE;
    }
}

SOC_RASTER_FORCE_INLINE void reset_raster_block_edge_cursor_row(
    const soc_raster_triangle_setup* setup,
    uint32_t pixel_x,
    uint32_t pixel_y,
    soc_raster_block_edge_cursor* cursor
)
{
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        cursor->values[edge_index] = fixed_edge_value_at_pixel(
            &setup->edges[edge_index],
            pixel_x,
            pixel_y
        );
    }
}

SOC_RASTER_FORCE_INLINE const soc_tile_edge*
materialize_raster_block_edge_cursor(
    soc_raster_block_edge_cursor* cursor
)
{
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        cursor->edges[edge_index].value =
            fixed_i64_to_f32(cursor->values[edge_index]);
    }
    return cursor->edges;
}

SOC_RASTER_FORCE_INLINE void advance_raster_block_edge_cursor(
    soc_raster_block_edge_cursor* cursor,
    soc_bool first_block
)
{
    const int64_t* increments = first_block == SOC_TRUE
        ? cursor->first_block_increments
        : cursor->full_block_increments;
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        cursor->values[edge_index] += increments[edge_index];
    }
}

static soc_edge_equation make_edge_equation(
    const soc_fixed_vertex* start,
    const soc_fixed_vertex* end
)
{
    const int64_t delta_x = end->x - start->x;
    const int64_t delta_y = end->y - start->y;
    const soc_bool top_left = delta_y < 0 ||
            (delta_y == 0 && delta_x > 0)
        ? SOC_TRUE
        : SOC_FALSE;
    const int64_t bias = top_left == SOC_TRUE ? 0 : -1;
    const soc_edge_equation edge = {
        .sample_origin =
            delta_x * (SOC_RASTER_SUBPIXEL_HALF - start->y) -
            delta_y * (SOC_RASTER_SUBPIXEL_HALF - start->x) +
            bias,
        .step_x = -delta_y * SOC_RASTER_SUBPIXEL_SCALE,
        .step_y = delta_x * SOC_RASTER_SUBPIXEL_SCALE,
    };
    return edge;
}

static void configure_snapped_depth_plane(
    const soc_screen_vertex screen[3],
    int64_t fixed_area,
    soc_raster_depth_plane* out_plane
)
{
    const float inverse_subpixel_scale =
        1.0f / (float)SOC_RASTER_SUBPIXEL_SCALE;
    const float delta_x10 = screen_fixed_i64_to_f32(
        screen[1].fixed_x - screen[0].fixed_x
    ) *
        inverse_subpixel_scale;
    const float delta_y10 = screen_fixed_i64_to_f32(
        screen[1].fixed_y - screen[0].fixed_y
    ) *
        inverse_subpixel_scale;
    const float delta_x20 = screen_fixed_i64_to_f32(
        screen[2].fixed_x - screen[0].fixed_x
    ) *
        inverse_subpixel_scale;
    const float delta_y20 = screen_fixed_i64_to_f32(
        screen[2].fixed_y - screen[0].fixed_y
    ) *
        inverse_subpixel_scale;
    const float delta_depth10 = screen[1].depth - screen[0].depth;
    const float delta_depth20 = screen[2].depth - screen[0].depth;
    const float fixed_area_scale =
        (float)SOC_RASTER_SUBPIXEL_SCALE *
        (float)SOC_RASTER_SUBPIXEL_SCALE;
    const float area = fixed_i64_to_f32(fixed_area) / fixed_area_scale;
    const float inverse_area = reciprocal_f32(area);
    const float numerator_x = fmaf(
        -delta_depth20,
        delta_y10,
        delta_depth10 * delta_y20
    );
    const float numerator_y = fmaf(
        -delta_x20,
        delta_depth10,
        delta_x10 * delta_depth20
    );

    out_plane->step_x = numerator_x * inverse_area;
    out_plane->step_y = numerator_y * inverse_area;
}

static void store_prepared_depth_plane(
    soc_raster_triangle_setup* out_setup,
    const soc_raster_depth_plane* plane
)
{
    float sample_origin = fmaf(
        plane->step_x,
        0.5f - plane->anchor_x,
        plane->anchor
    );

    sample_origin = fmaf(
        plane->step_y,
        0.5f - plane->anchor_y,
        sample_origin
    );

    out_setup->depth_sample_origin = sample_origin;
    out_setup->depth_step_x = plane->step_x;
    out_setup->depth_step_y = plane->step_y;
}

static void configure_depth_plane(
    const soc_screen_vertex screen[3],
    int64_t fixed_area,
    soc_raster_triangle_setup* out_setup
)
{
    soc_raster_depth_plane plane;
    const float anchor_x = screen_fixed_i64_to_f32(screen[0].fixed_x) /
        (float)SOC_RASTER_SUBPIXEL_SCALE;
    const float anchor_y = screen_fixed_i64_to_f32(screen[0].fixed_y) /
        (float)SOC_RASTER_SUBPIXEL_SCALE;

    plane.anchor_x = anchor_x;
    plane.anchor_y = anchor_y;
    plane.anchor = screen[0].depth;
    configure_snapped_depth_plane(
        screen,
        fixed_area,
        &plane
    );
    store_prepared_depth_plane(out_setup, &plane);
}

static soc_raster_setup_result prepare_screen_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided,
    soc_screen_vertex screen[3]
)
{
    const soc_clip_vertex* clip_vertices[3] = {clip0, clip1, clip2};
    float inverse_w[3];
    int64_t fixed_area;
    uint32_t index;

    /* Project only x/y until the cheap degeneracy and facing tests pass. */
    for (index = 0u; index < 3u; ++index) {
        if (clip_vertices[index]->w <= 0.0f) {
            return SOC_RASTER_SETUP_REJECTED;
        }
    }
    reciprocal3_f32(
        clip_vertices[0]->w,
        clip_vertices[1]->w,
        clip_vertices[2]->w,
        inverse_w
    );
    for (index = 0u; index < 3u; ++index) {
        const float ndc_x = clamp_float(
            clip_vertices[index]->x * inverse_w[index],
            -1.0f,
            1.0f
        );
        const float ndc_y = clamp_float(
            clip_vertices[index]->y * inverse_w[index],
            -1.0f,
            1.0f
        );

        screen[index].x =
            fmaf(ndc_x, 0.5f, 0.5f) * (float)rasterizer->width;
        screen[index].y =
            fmaf(ndc_y, -0.5f, 0.5f) * (float)rasterizer->height;
        screen[index].fixed_x = quantize_screen_coordinate(screen[index].x);
        screen[index].fixed_y = quantize_screen_coordinate(screen[index].y);
    }

    fixed_area =
        (screen[1].fixed_x - screen[0].fixed_x) *
            (screen[2].fixed_y - screen[0].fixed_y) -
        (screen[1].fixed_y - screen[0].fixed_y) *
            (screen[2].fixed_x - screen[0].fixed_x);
    if (fixed_area == 0) {
        return SOC_RASTER_SETUP_REJECTED;
    }
    if (two_sided != SOC_TRUE) {
        const soc_bool front_facing =
            rasterizer->frame.front_face == SOC_FRONT_FACE_CCW
                ? (fixed_area < 0 ? SOC_TRUE : SOC_FALSE)
                : (fixed_area > 0 ? SOC_TRUE : SOC_FALSE);

        if (front_facing != SOC_TRUE) {
            return SOC_RASTER_SETUP_REJECTED;
        }
    }

    for (index = 0u; index < 3u; ++index) {
        float depth = clip_vertices[index]->z * inverse_w[index];

        if (rasterizer->frame.clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = fmaf(depth, 0.5f, 0.5f);
        }
        screen[index].depth = clamp_float(depth, 0.0f, 1.0f);
    }
    if (fixed_area < 0) {
        swap_screen_vertices(&screen[1], &screen[2]);
    }
    return SOC_RASTER_SETUP_READY;
}

static soc_raster_setup_result prepare_guard_band_screen_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided,
    soc_screen_vertex screen[3]
)
{
    const soc_clip_vertex* clip_vertices[3] = {clip0, clip1, clip2};
    float inverse_w[3];
    int64_t fixed_area;
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        if (clip_vertices[index]->w <= 0.0f) {
            return SOC_RASTER_SETUP_REJECTED;
        }
    }
    reciprocal3_f32(
        clip_vertices[0]->w,
        clip_vertices[1]->w,
        clip_vertices[2]->w,
        inverse_w
    );
    for (index = 0u; index < 3u; ++index) {
        const float ndc_x = clip_vertices[index]->x * inverse_w[index];
        const float ndc_y = clip_vertices[index]->y * inverse_w[index];

        screen[index].x =
            fmaf(ndc_x, 0.5f, 0.5f) * (float)rasterizer->width;
        screen[index].y =
            fmaf(ndc_y, -0.5f, 0.5f) * (float)rasterizer->height;
        screen[index].fixed_x =
            quantize_guard_band_screen_coordinate(screen[index].x);
        screen[index].fixed_y =
            quantize_guard_band_screen_coordinate(screen[index].y);
    }

    fixed_area =
        (screen[1].fixed_x - screen[0].fixed_x) *
            (screen[2].fixed_y - screen[0].fixed_y) -
        (screen[1].fixed_y - screen[0].fixed_y) *
            (screen[2].fixed_x - screen[0].fixed_x);
    if (fixed_area == 0) {
        return SOC_RASTER_SETUP_REJECTED;
    }
    if (two_sided != SOC_TRUE) {
        const soc_bool front_facing =
            rasterizer->frame.front_face == SOC_FRONT_FACE_CCW
                ? (fixed_area < 0 ? SOC_TRUE : SOC_FALSE)
                : (fixed_area > 0 ? SOC_TRUE : SOC_FALSE);

        if (front_facing != SOC_TRUE) {
            return SOC_RASTER_SETUP_REJECTED;
        }
    }

    for (index = 0u; index < 3u; ++index) {
        float depth = clip_vertices[index]->z * inverse_w[index];

        if (rasterizer->frame.clip_depth_range ==
            SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
            depth = fmaf(depth, 0.5f, 0.5f);
        }
        screen[index].depth = clamp_float(depth, 0.0f, 1.0f);
    }
    if (fixed_area < 0) {
        swap_screen_vertices(&screen[1], &screen[2]);
    }
    return SOC_RASTER_SETUP_READY;
}

static soc_raster_setup_result prepare_cached_screen_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex clip[3],
    const uint32_t mesh_indices[3],
    soc_bool two_sided,
    soc_post_transform_cache* cache,
    soc_screen_vertex screen[3]
)
{
    soc_bool screen_cached[3];
    soc_bool all_screens_cached = SOC_TRUE;
    int64_t fixed_area;
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        screen_cached[index] = find_post_transform_cache_screen(
            cache,
            mesh_indices[index],
            &screen[index]
        );
        if (screen_cached[index] != SOC_TRUE) {
            all_screens_cached = SOC_FALSE;
        }
    }
    if (all_screens_cached != SOC_TRUE) {
        float inverse_w[3];

        for (index = 0u; index < 3u; ++index) {
            if (clip[index].w <= 0.0f) {
                return SOC_RASTER_SETUP_REJECTED;
            }
        }
        reciprocal3_f32(
            clip[0].w,
            clip[1].w,
            clip[2].w,
            inverse_w
        );
        for (index = 0u; index < 3u; ++index) {
            float depth;
            float ndc_x;
            float ndc_y;

            if (screen_cached[index] == SOC_TRUE) {
                continue;
            }
            ndc_x = clamp_float(
                clip[index].x * inverse_w[index],
                -1.0f,
                1.0f
            );
            ndc_y = clamp_float(
                clip[index].y * inverse_w[index],
                -1.0f,
                1.0f
            );
            screen[index].x =
                fmaf(ndc_x, 0.5f, 0.5f) * (float)rasterizer->width;
            screen[index].y =
                fmaf(ndc_y, -0.5f, 0.5f) * (float)rasterizer->height;
            screen[index].fixed_x =
                quantize_screen_coordinate(screen[index].x);
            screen[index].fixed_y =
                quantize_screen_coordinate(screen[index].y);
            depth = clip[index].z * inverse_w[index];
            if (rasterizer->frame.clip_depth_range ==
                SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
                depth = fmaf(depth, 0.5f, 0.5f);
            }
            screen[index].depth = clamp_float(depth, 0.0f, 1.0f);
            store_post_transform_cache_screen(
                cache,
                mesh_indices[index],
                &screen[index]
            );
        }
    }

    fixed_area =
        (screen[1].fixed_x - screen[0].fixed_x) *
            (screen[2].fixed_y - screen[0].fixed_y) -
        (screen[1].fixed_y - screen[0].fixed_y) *
            (screen[2].fixed_x - screen[0].fixed_x);
    if (fixed_area == 0) {
        return SOC_RASTER_SETUP_REJECTED;
    }
    if (two_sided != SOC_TRUE) {
        const soc_bool front_facing =
            rasterizer->frame.front_face == SOC_FRONT_FACE_CCW
                ? (fixed_area < 0 ? SOC_TRUE : SOC_FALSE)
                : (fixed_area > 0 ? SOC_TRUE : SOC_FALSE);

        if (front_facing != SOC_TRUE) {
            return SOC_RASTER_SETUP_REJECTED;
        }
    }
    if (fixed_area < 0) {
        swap_screen_vertices(&screen[1], &screen[2]);
    }
    return SOC_RASTER_SETUP_READY;
}

static void configure_shared_fan_depth_plane(
    const soc_screen_vertex plane_triangle[3],
    soc_raster_depth_plane* out_plane
)
{
    int64_t fixed_area;

    fixed_area =
        (plane_triangle[1].fixed_x - plane_triangle[0].fixed_x) *
            (plane_triangle[2].fixed_y - plane_triangle[0].fixed_y) -
        (plane_triangle[1].fixed_y - plane_triangle[0].fixed_y) *
            (plane_triangle[2].fixed_x - plane_triangle[0].fixed_x);
    out_plane->anchor_x = screen_fixed_i64_to_f32(
        plane_triangle[0].fixed_x
    ) /
        (float)SOC_RASTER_SUBPIXEL_SCALE;
    out_plane->anchor_y = screen_fixed_i64_to_f32(
        plane_triangle[0].fixed_y
    ) /
        (float)SOC_RASTER_SUBPIXEL_SCALE;
    out_plane->anchor = plane_triangle[0].depth;
    configure_snapped_depth_plane(
        plane_triangle,
        fixed_area,
        out_plane
    );
}

static soc_raster_setup_result setup_raster_triangle(
    const soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    const soc_raster_depth_plane* shared_depth_plane,
    soc_raster_triangle_setup* out_setup
)
{
    soc_fixed_vertex fixed[3];
    int64_t minimum_x;
    int64_t maximum_x;
    int64_t minimum_y;
    int64_t maximum_y;
    int64_t fixed_area;
    uint32_t index;

    for (index = 0u; index < 3u; ++index) {
        fixed[index].x = screen[index].fixed_x;
        fixed[index].y = screen[index].fixed_y;
    }
    fixed_area =
        (fixed[1].x - fixed[0].x) *
            (fixed[2].y - fixed[0].y) -
        (fixed[1].y - fixed[0].y) *
            (fixed[2].x - fixed[0].x);
    if (fixed_area <= 0) {
        return SOC_RASTER_SETUP_REJECTED;
    }

    minimum_x = fixed[0].x;
    maximum_x = fixed[0].x;
    minimum_y = fixed[0].y;
    maximum_y = fixed[0].y;
    for (index = 1u; index < 3u; ++index) {
        if (fixed[index].x < minimum_x) {
            minimum_x = fixed[index].x;
        }
        if (fixed[index].x > maximum_x) {
            maximum_x = fixed[index].x;
        }
        if (fixed[index].y < minimum_y) {
            minimum_y = fixed[index].y;
        }
        if (fixed[index].y > maximum_y) {
            maximum_y = fixed[index].y;
        }
    }

    if (maximum_x < SOC_RASTER_SUBPIXEL_HALF ||
        maximum_y < SOC_RASTER_SUBPIXEL_HALF) {
        return SOC_RASTER_SETUP_EMPTY;
    }

    out_setup->bounds.minimum_x =
        minimum_x <= SOC_RASTER_SUBPIXEL_HALF
        ? 0u
        : (uint32_t)(
            (minimum_x - SOC_RASTER_SUBPIXEL_HALF +
                SOC_RASTER_SUBPIXEL_SCALE - 1) /
            SOC_RASTER_SUBPIXEL_SCALE
        );
    out_setup->bounds.minimum_y =
        minimum_y <= SOC_RASTER_SUBPIXEL_HALF
        ? 0u
        : (uint32_t)(
            (minimum_y - SOC_RASTER_SUBPIXEL_HALF +
                SOC_RASTER_SUBPIXEL_SCALE - 1) /
            SOC_RASTER_SUBPIXEL_SCALE
        );
    out_setup->bounds.end_x = (uint32_t)(
        (maximum_x - SOC_RASTER_SUBPIXEL_HALF) /
            SOC_RASTER_SUBPIXEL_SCALE +
        1
    );
    out_setup->bounds.end_y = (uint32_t)(
        (maximum_y - SOC_RASTER_SUBPIXEL_HALF) /
            SOC_RASTER_SUBPIXEL_SCALE +
        1
    );
    if (out_setup->bounds.end_x > rasterizer->width) {
        out_setup->bounds.end_x = rasterizer->width;
    }
    if (out_setup->bounds.end_y > rasterizer->height) {
        out_setup->bounds.end_y = rasterizer->height;
    }
    if (out_setup->bounds.minimum_x >= out_setup->bounds.end_x ||
        out_setup->bounds.minimum_y >= out_setup->bounds.end_y) {
        return SOC_RASTER_SETUP_EMPTY;
    }

    out_setup->first_tile_column = (uint16_t)(
        out_setup->bounds.minimum_x / SOC_RASTER_LOCK_TILE_SIZE
    );
    out_setup->first_tile_row = (uint16_t)(
        out_setup->bounds.minimum_y / SOC_RASTER_LOCK_TILE_SIZE
    );
    out_setup->end_tile_column = (uint16_t)(
        (out_setup->bounds.end_x - 1u) /
            SOC_RASTER_LOCK_TILE_SIZE + 1u
    );
    out_setup->end_tile_row = (uint16_t)(
        (out_setup->bounds.end_y - 1u) /
            SOC_RASTER_LOCK_TILE_SIZE + 1u
    );

    out_setup->edges[0] = make_edge_equation(&fixed[1], &fixed[2]);
    out_setup->edges[1] = make_edge_equation(&fixed[2], &fixed[0]);
    out_setup->edges[2] = make_edge_equation(&fixed[0], &fixed[1]);

    if (shared_depth_plane != NULL) {
        store_prepared_depth_plane(out_setup, shared_depth_plane);
    } else if (screen[0].depth == screen[1].depth &&
        screen[0].depth == screen[2].depth) {
        out_setup->depth_sample_origin = screen[0].depth;
        out_setup->depth_step_x = 0.0f;
        out_setup->depth_step_y = 0.0f;
    } else {
        configure_depth_plane(
            screen,
            fixed_area,
            out_setup
        );
    }
    return SOC_RASTER_SETUP_READY;
}

static float clamp_depth(float depth)
{
    return clamp_float(depth, 0.0f, 1.0f);
}

static float make_depth_plane_block_origin(
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y
)
{
    float block_depth = fmaf(
        setup->depth_step_x,
        (float)block_x,
        setup->depth_sample_origin
    );

    return fmaf(setup->depth_step_y, (float)block_y, block_depth);
}

/*
 * Reproduce the depth kernel's f32 origin/step conversion and two-stage FMA
 * at all four rectangle corners.
 */
static void configure_depth_block_candidate(
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    soc_raster_depth_block_candidate* out_candidate
)
{
    float x_offsets[2];
    float y_offsets[2];
    uint32_t corner_y;

    if (setup->depth_step_x == 0.0f && setup->depth_step_y == 0.0f) {
        out_candidate->depth_origin = clamp_depth(
            setup->depth_sample_origin
        );
        out_candidate->depth_step_x = 0.0f;
        out_candidate->depth_step_y = 0.0f;
        out_candidate->nearest_depth = out_candidate->depth_origin;
        out_candidate->farthest_depth = out_candidate->depth_origin;
        out_candidate->is_constant = SOC_TRUE;
        return;
    }

    out_candidate->depth_origin = make_depth_plane_block_origin(
        setup,
        block_x,
        block_y
    );
    out_candidate->depth_step_x = (float)setup->depth_step_x;
    out_candidate->depth_step_y = (float)setup->depth_step_y;
    out_candidate->nearest_depth = 0.0f;
    out_candidate->farthest_depth = 0.0f;
    out_candidate->is_constant = SOC_FALSE;
    x_offsets[0] = 0.0f;
    x_offsets[1] = (float)(block_width - 1u);
    y_offsets[0] = 0.0f;
    y_offsets[1] = (float)(block_height - 1u);

    for (corner_y = 0u; corner_y < 2u; ++corner_y) {
        const float row_depth = fmaf(
            out_candidate->depth_step_y,
            y_offsets[corner_y],
            out_candidate->depth_origin
        );
        uint32_t corner_x;

        for (corner_x = 0u; corner_x < 2u; ++corner_x) {
            const float candidate = clamp_depth(
                fmaf(
                    out_candidate->depth_step_x,
                    x_offsets[corner_x],
                    row_depth
                )
            );

            if (corner_x == 0u && corner_y == 0u) {
                out_candidate->nearest_depth = candidate;
                out_candidate->farthest_depth = candidate;
            } else {
                if (candidate > out_candidate->nearest_depth) {
                    out_candidate->nearest_depth = candidate;
                }
                if (candidate < out_candidate->farthest_depth) {
                    out_candidate->farthest_depth = candidate;
                }
            }
        }
    }
}

/*
 * Bound every 8x8 fine-kernel rebase covered by a coarse region.  Binary32
 * conversion and FMA are monotonic, so the four endpoint block origins plus
 * the complete [0,7] local-offset envelope contain every fine candidate.
 */
static void configure_coarse_depth_candidate(
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region,
    soc_raster_depth_block_candidate* out_candidate
)
{
    uint32_t last_block_x =
        (region->end_x - 1u) & ~(SOC_RASTER_BLOCK_SIZE - 1u);
    uint32_t last_block_y =
        (region->end_y - 1u) & ~(SOC_RASTER_BLOCK_SIZE - 1u);
    float minimum_origin;
    float maximum_origin;
    float origins[2];
    uint32_t origin_index;

    if (setup->depth_step_x == 0.0f && setup->depth_step_y == 0.0f) {
        configure_depth_block_candidate(
            setup,
            region->minimum_x,
            region->minimum_y,
            1u,
            1u,
            out_candidate
        );
        return;
    }
    if (last_block_x < region->minimum_x) {
        last_block_x = region->minimum_x;
    }
    if (last_block_y < region->minimum_y) {
        last_block_y = region->minimum_y;
    }
    minimum_origin = make_depth_plane_block_origin(
        setup,
        region->minimum_x,
        region->minimum_y
    );
    maximum_origin = minimum_origin;
    {
        const uint32_t block_x[2] = {
            region->minimum_x,
            last_block_x,
        };
        const uint32_t block_y[2] = {
            region->minimum_y,
            last_block_y,
        };
        uint32_t y_index;

        for (y_index = 0u; y_index < 2u; ++y_index) {
            uint32_t x_index;

            for (x_index = 0u; x_index < 2u; ++x_index) {
                const float origin = make_depth_plane_block_origin(
                    setup,
                    block_x[x_index],
                    block_y[y_index]
                );

                if (origin < minimum_origin) {
                    minimum_origin = origin;
                }
                if (origin > maximum_origin) {
                    maximum_origin = origin;
                }
            }
        }
    }
    out_candidate->depth_origin = minimum_origin;
    out_candidate->depth_step_x = (float)setup->depth_step_x;
    out_candidate->depth_step_y = (float)setup->depth_step_y;
    out_candidate->is_constant = SOC_FALSE;
    origins[0] = minimum_origin;
    origins[1] = maximum_origin;
    for (origin_index = 0u; origin_index < 2u; ++origin_index) {
        uint32_t offset_y_index;

        for (offset_y_index = 0u;
             offset_y_index < 2u;
             ++offset_y_index) {
            const float offset_y = offset_y_index == 0u
                ? 0.0f
                : (float)(SOC_RASTER_BLOCK_SIZE - 1u);
            const float row_depth = fmaf(
                out_candidate->depth_step_y,
                offset_y,
                origins[origin_index]
            );
            uint32_t offset_x_index;

            for (offset_x_index = 0u;
                 offset_x_index < 2u;
                 ++offset_x_index) {
                const float offset_x = offset_x_index == 0u
                    ? 0.0f
                    : (float)(SOC_RASTER_BLOCK_SIZE - 1u);
                const float candidate = clamp_depth(
                    fmaf(
                        out_candidate->depth_step_x,
                        offset_x,
                        row_depth
                    )
                );

                if (origin_index == 0u && offset_y_index == 0u &&
                    offset_x_index == 0u) {
                    out_candidate->nearest_depth = candidate;
                    out_candidate->farthest_depth = candidate;
                } else {
                    if (candidate > out_candidate->nearest_depth) {
                        out_candidate->nearest_depth = candidate;
                    }
                    if (candidate < out_candidate->farthest_depth) {
                        out_candidate->farthest_depth = candidate;
                    }
                }
            }
        }
    }
}

static size_t find_early_z_block_index(
    const soc_rasterizer* rasterizer,
    uint32_t block_x,
    uint32_t block_y
)
{
    const uint32_t block_column = block_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_row = block_y / SOC_RASTER_BLOCK_SIZE;

    return (size_t)block_row * rasterizer->block_column_count +
        block_column;
}

static const float* find_small_early_z_farthest_depth(
    const soc_rasterizer* rasterizer,
    const soc_raster_region* region
)
{
    const uint32_t block_column =
        region->minimum_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_row =
        region->minimum_y / SOC_RASTER_BLOCK_SIZE;

    if ((region->end_x - 1u) / SOC_RASTER_BLOCK_SIZE != block_column ||
        (region->end_y - 1u) / SOC_RASTER_BLOCK_SIZE != block_row) {
        return NULL;
    }
    return &rasterizer->early_z_farthest_depths[
        (size_t)block_row * rasterizer->block_column_count + block_column
    ];
}

static soc_bool depth_block_is_early_z_rejected(
    const soc_raster_depth_block_candidate* candidate,
    float farthest_depth
)
{
    return candidate->nearest_depth <= farthest_depth
        ? SOC_TRUE
        : SOC_FALSE;
}

static void store_small_constant_depth(
    soc_rasterizer* rasterizer,
    uint32_t pixel_x,
    uint32_t pixel_y,
    float candidate_depth
)
{
    const size_t depth_index =
        (size_t)pixel_y * rasterizer->width + pixel_x;
    const float stored_depth = rasterizer->depth[depth_index];
    const soc_bool passes_depth = candidate_depth > stored_depth
        ? SOC_TRUE
        : SOC_FALSE;

    if (passes_depth == SOC_TRUE) {
        rasterizer->depth[depth_index] = candidate_depth;
    }
}

/* Small constant triangles do not amortize 8x8 state maintenance. */
static void rasterize_small_constant_triangle(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const soc_raster_region* region = &setup->bounds;
    const float candidate_depth = clamp_depth(setup->depth_sample_origin);
    const float* early_z_farthest_depth =
        find_small_early_z_farthest_depth(rasterizer, region);
    soc_tile_edge tile_edges[3];
    uint32_t pixel_y;

    if (early_z_farthest_depth != NULL &&
        candidate_depth <= *early_z_farthest_depth) {
        return;
    }
    make_tile_edges(
        setup,
        region->minimum_x,
        region->minimum_y,
        tile_edges
    );
    for (pixel_y = region->minimum_y; pixel_y < region->end_y; ++pixel_y) {
        float edge0 = tile_edges[0].value;
        float edge1 = tile_edges[1].value;
        float edge2 = tile_edges[2].value;
        uint32_t pixel_x;

        for (pixel_x = region->minimum_x;
             pixel_x < region->end_x;
             ++pixel_x) {
            if (edge0 >= 0 && edge1 >= 0 && edge2 >= 0) {
                store_small_constant_depth(
                    rasterizer,
                    pixel_x,
                    pixel_y,
                    candidate_depth
                );
            }
            edge0 += tile_edges[0].step_x;
            edge1 += tile_edges[1].step_x;
            edge2 += tile_edges[2].step_x;
        }
        tile_edges[0].value += tile_edges[0].step_y;
        tile_edges[1].value += tile_edges[1].step_y;
        tile_edges[2].value += tile_edges[2].step_y;
    }
}

static soc_raster_block_classification classify_raster_block(
    const soc_tile_edge edges[3],
    uint32_t block_width,
    uint32_t block_height
);

static uint64_t make_raster_block_mask(
    const soc_tile_edge edges[3],
    uint32_t block_width,
    uint32_t block_height,
    soc_raster_block_classification classification
);

static void rasterize_depth_block(
    soc_rasterizer* rasterizer,
    const soc_raster_depth_block_candidate* candidate,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask
);

#if defined(SOC_BUILD_AARCH32_NEON_FMA)
SOC_RASTER_FORCE_INLINE soc_bool fixed_edges_cover_sample_arm32(
    int64_t edge0,
    int64_t edge1,
    int64_t edge2
)
{
    const uint64_t sign_bits =
        (uint64_t)edge0 | (uint64_t)edge1 | (uint64_t)edge2;

    return (sign_bits >> 63u) == 0u ? SOC_TRUE : SOC_FALSE;
}

/*
 * Tiny varying triangles do not amortize a float edge materialization, a
 * 64-bit coverage mask and an indirect 8x8 kernel call.  Keep coverage in
 * exact Q8 fixed point, but preserve the depth kernel's row/column FMA order.
 */
static SOC_NOINLINE void rasterize_tiny_plane_triangle_fused_arm32(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    static const float lane_offsets_values[4] = {
        0.0f, 1.0f, 2.0f, 3.0f,
    };
    const soc_raster_region* region = &setup->bounds;
    const uint32_t block_width = region->end_x - region->minimum_x;
    const uint32_t block_height = region->end_y - region->minimum_y;
    const float32x4_t lane_offsets = vld1q_f32(lane_offsets_values);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t one = vdupq_n_f32(1.0f);
    const float depth_origin = make_depth_plane_block_origin(
        setup,
        region->minimum_x,
        region->minimum_y
    );
    int64_t row_edge0 = fixed_edge_value_at_pixel(
        &setup->edges[0],
        region->minimum_x,
        region->minimum_y
    );
    int64_t row_edge1 = fixed_edge_value_at_pixel(
        &setup->edges[1],
        region->minimum_x,
        region->minimum_y
    );
    int64_t row_edge2 = fixed_edge_value_at_pixel(
        &setup->edges[2],
        region->minimum_x,
        region->minimum_y
    );
    float* destination_row = rasterizer->depth +
        (size_t)region->minimum_y * rasterizer->width + region->minimum_x;
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        int64_t edge0 = row_edge0;
        int64_t edge1 = row_edge1;
        int64_t edge2 = row_edge2;
        const soc_bool inside0 = fixed_edges_cover_sample_arm32(
            edge0,
            edge1,
            edge2
        );
        soc_bool inside1 = SOC_FALSE;
        soc_bool inside2 = SOC_FALSE;

        if (block_width >= 2u) {
            edge0 += setup->edges[0].step_x;
            edge1 += setup->edges[1].step_x;
            edge2 += setup->edges[2].step_x;
            inside1 = fixed_edges_cover_sample_arm32(edge0, edge1, edge2);
        }
        if (block_width >= 3u) {
            edge0 += setup->edges[0].step_x;
            edge1 += setup->edges[1].step_x;
            edge2 += setup->edges[2].step_x;
            inside2 = fixed_edges_cover_sample_arm32(edge0, edge1, edge2);
        }

        if (inside0 == SOC_TRUE || inside1 == SOC_TRUE ||
            inside2 == SOC_TRUE) {
            const float row_depth = fmaf(
                setup->depth_step_y,
                (float)row,
                depth_origin
            );
            float32x4_t candidates = vfmaq_n_f32(
                vdupq_n_f32(row_depth),
                lane_offsets,
                setup->depth_step_x
            );

            candidates = vmaxq_f32(candidates, zero);
            candidates = vminq_f32(candidates, one);
            if (inside0 == SOC_TRUE) {
                const float candidate = vgetq_lane_f32(candidates, 0);

                if (candidate > destination_row[0]) {
                    destination_row[0] = candidate;
                }
            }
            if (inside1 == SOC_TRUE) {
                const float candidate = vgetq_lane_f32(candidates, 1);

                if (candidate > destination_row[1]) {
                    destination_row[1] = candidate;
                }
            }
            if (inside2 == SOC_TRUE) {
                const float candidate = vgetq_lane_f32(candidates, 2);

                if (candidate > destination_row[2]) {
                    destination_row[2] = candidate;
                }
            }
        }

        row_edge0 += setup->edges[0].step_y;
        row_edge1 += setup->edges[1].step_y;
        row_edge2 += setup->edges[2].step_y;
        destination_row += rasterizer->width;
    }
}
#endif

/* A <=8x8 varying triangle is cheaper as one rectangular kernel call. */
static void rasterize_small_plane_triangle_untracked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const soc_raster_region* region = &setup->bounds;
    const uint32_t block_width = region->end_x - region->minimum_x;
    const uint32_t block_height = region->end_y - region->minimum_y;
    soc_tile_edge tile_edges[3];
    soc_raster_depth_block_candidate depth_candidate;
    uint64_t coverage_mask;

#if defined(SOC_BUILD_AARCH32_NEON_FMA)
    if (block_width <= SOC_RASTER_ARM32_FUSED_TRIANGLE_SIZE &&
        block_height <= SOC_RASTER_ARM32_FUSED_TRIANGLE_SIZE) {
        rasterize_tiny_plane_triangle_fused_arm32(rasterizer, setup);
        return;
    }
#endif

    make_tile_edges(
        setup,
        region->minimum_x,
        region->minimum_y,
        tile_edges
    );
    depth_candidate.depth_origin = make_depth_plane_block_origin(
        setup,
        region->minimum_x,
        region->minimum_y
    );
    depth_candidate.depth_step_x = (float)setup->depth_step_x;
    depth_candidate.depth_step_y = (float)setup->depth_step_y;
    depth_candidate.is_constant = SOC_FALSE;
    coverage_mask = make_raster_block_mask(
        tile_edges,
        block_width,
        block_height,
        SOC_RASTER_BLOCK_PARTIAL
    );
    if (coverage_mask != 0u) {
        rasterize_depth_block(
            rasterizer,
            &depth_candidate,
            region->minimum_x,
            region->minimum_y,
            block_width,
            block_height,
            coverage_mask
        );
    }
}

static void rasterize_small_triangle_blocks_untracked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const soc_raster_region* region = &setup->bounds;
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);

    for (; aligned_y < region->end_y;
         aligned_y += SOC_RASTER_BLOCK_SIZE) {
        const uint32_t block_y = aligned_y < region->minimum_y
            ? region->minimum_y
            : aligned_y;
        const uint32_t block_end_y = aligned_y + SOC_RASTER_BLOCK_SIZE <
                region->end_y
            ? aligned_y + SOC_RASTER_BLOCK_SIZE
            : region->end_y;
        const uint32_t block_height = block_end_y - block_y;
        uint32_t aligned_x = region->minimum_x &
            ~(SOC_RASTER_BLOCK_SIZE - 1u);

        for (; aligned_x < region->end_x;
             aligned_x += SOC_RASTER_BLOCK_SIZE) {
            const uint32_t block_x = aligned_x < region->minimum_x
                ? region->minimum_x
                : aligned_x;
            const uint32_t block_end_x = aligned_x +
                    SOC_RASTER_BLOCK_SIZE < region->end_x
                ? aligned_x + SOC_RASTER_BLOCK_SIZE
                : region->end_x;
            const uint32_t block_width = block_end_x - block_x;
            soc_tile_edge tile_edges[3];
            soc_raster_block_classification classification;
            soc_raster_depth_block_candidate depth_candidate;
            uint64_t coverage_mask;
            make_tile_edges(setup, block_x, block_y, tile_edges);
            classification = classify_raster_block(
                tile_edges,
                block_width,
                block_height
            );
            if (classification == SOC_RASTER_BLOCK_OUTSIDE) {
                continue;
            }
            if (setup->depth_step_x == 0.0f &&
                setup->depth_step_y == 0.0f) {
                depth_candidate.depth_origin = clamp_depth(
                    setup->depth_sample_origin
                );
                depth_candidate.depth_step_x = 0.0f;
                depth_candidate.depth_step_y = 0.0f;
                depth_candidate.is_constant = SOC_TRUE;
            } else {
                depth_candidate.depth_origin =
                    make_depth_plane_block_origin(
                        setup,
                        block_x,
                        block_y
                    );
                depth_candidate.depth_step_x =
                    (float)setup->depth_step_x;
                depth_candidate.depth_step_y =
                    (float)setup->depth_step_y;
                depth_candidate.is_constant = SOC_FALSE;
            }
            coverage_mask = make_raster_block_mask(
                tile_edges,
                block_width,
                block_height,
                classification
            );
            if (coverage_mask != 0u) {
                rasterize_depth_block(
                    rasterizer,
                    &depth_candidate,
                    block_x,
                    block_y,
                    block_width,
                    block_height,
                    coverage_mask
                );
            }
        }
    }
}

static uint64_t make_physical_block_mask(
    uint32_t block_width,
    uint32_t block_height
)
{
    if (block_width == SOC_RASTER_BLOCK_SIZE &&
        block_height == SOC_RASTER_BLOCK_SIZE) {
        return UINT64_MAX;
    }

    const uint64_t row_mask =
        (UINT64_C(1) << block_width) - UINT64_C(1);
    uint64_t mask = 0u;
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        mask |= row_mask << (row * SOC_RASTER_BLOCK_SIZE);
    }
    return mask;
}

static float make_masked_farthest_depth(
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height
)
{
    const uint32_t farthest_x = setup->depth_step_x < 0.0f
        ? block_x + block_width - 1u
        : block_x;
    const uint32_t farthest_y = setup->depth_step_y < 0.0f
        ? block_y + block_height - 1u
        : block_y;
    float depth = fmaf(
        setup->depth_step_x,
        (float)farthest_x,
        setup->depth_sample_origin
    );

    depth = fmaf(setup->depth_step_y, (float)farthest_y, depth);
    return clamp_depth(depth);
}

#if defined(SOC_BUILD_AARCH32_NEON_FMA)
/*
 * A tiny block is cheaper to cover directly in Q8 fixed point than to
 * convert three 64-bit edges to float, classify four corners, and construct
 * the same mask through the general SIMD path.  Keeping every edge test in
 * fixed point also preserves the exact top-left bias from make_edge_equation.
 */
SOC_RASTER_FORCE_INLINE uint32_t
make_masked_tiny_fixed_coverage_arm32(
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height
)
{
    int64_t row_edge0 = fixed_edge_value_at_pixel(
        &setup->edges[0],
        block_x,
        block_y
    );
    int64_t row_edge1 = fixed_edge_value_at_pixel(
        &setup->edges[1],
        block_x,
        block_y
    );
    int64_t row_edge2 = fixed_edge_value_at_pixel(
        &setup->edges[2],
        block_x,
        block_y
    );
    uint32_t coverage = 0u;
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        int64_t edge0 = row_edge0;
        int64_t edge1 = row_edge1;
        int64_t edge2 = row_edge2;
        uint32_t row_coverage = 0u;
        uint32_t column;

        for (column = 0u; column < block_width; ++column) {
            if (fixed_edges_cover_sample_arm32(edge0, edge1, edge2) ==
                SOC_TRUE) {
                row_coverage |= UINT32_C(1) << column;
            }
            edge0 += setup->edges[0].step_x;
            edge1 += setup->edges[1].step_x;
            edge2 += setup->edges[2].step_x;
        }
        coverage |= row_coverage << (row * SOC_RASTER_BLOCK_SIZE);
        row_edge0 += setup->edges[0].step_y;
        row_edge1 += setup->edges[1].step_y;
        row_edge2 += setup->edges[2].step_y;
    }
    return coverage;
}
#endif

static void update_masked_quick_subtile(
    soc_rasterizer* rasterizer,
    size_t subtile_index,
    uint32_t coverage,
    uint32_t valid_mask,
    float triangle_depth
)
{
    float z0;
    float z1;
    float z1_min;
    uint32_t mask;
    soc_bool discard_working_layer;

    coverage &= valid_mask;
    if (coverage == 0u) {
        return;
    }

    z0 = rasterizer->masked_z0[subtile_index];
    if (triangle_depth <= z0) {
        return;
    }

    z1 = rasterizer->masked_z1[subtile_index];
    mask = rasterizer->masked_masks[subtile_index];
    discard_working_layer =
        coverage == valid_mask ||
        2.0f * z1 < triangle_depth + z0
            ? SOC_TRUE
            : SOC_FALSE;
    if (discard_working_layer == SOC_TRUE) {
        mask = 0u;
        z1_min = triangle_depth;
    } else {
        z1_min = triangle_depth < z1 ? triangle_depth : z1;
    }

    mask |= coverage;
    if (mask == valid_mask) {
        const soc_bool establishes_reference = z0 < 0.0f
            ? SOC_TRUE
            : SOC_FALSE;

        rasterizer->masked_z0[subtile_index] = z1_min;
        rasterizer->masked_z1[subtile_index] = FLT_MAX;
        rasterizer->masked_masks[subtile_index] = 0u;
        if (establishes_reference == SOC_TRUE &&
            rasterizer->tile_locks == NULL &&
            rasterizer->masked_reference_count != SIZE_MAX) {
            ++rasterizer->masked_reference_count;
            if (rasterizer->masked_reference_count ==
                rasterizer->masked_subtile_count) {
                float farthest = rasterizer->masked_z0[0];
                size_t index;

                for (index = 1u;
                     index < rasterizer->masked_subtile_count;
                     ++index) {
                    const float candidate = rasterizer->masked_z0[index];

                    if (candidate < farthest) {
                        farthest = candidate;
                    }
                }
                rasterizer->masked_frame_farthest_depth = farthest;
            }
        }
        return;
    }

    rasterizer->masked_z1[subtile_index] = z1_min;
    rasterizer->masked_masks[subtile_index] = mask;
}

static void update_masked_quick_block(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    uint32_t aligned_x,
    uint32_t aligned_y,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint32_t physical_width,
    uint32_t physical_height,
    uint64_t cell_coverage_mask
)
{
    const uint64_t physical_mask = make_physical_block_mask(
        physical_width,
        physical_height
    );
    const uint32_t block_end_y = block_y + block_height;
    uint32_t subtile_half;

    for (subtile_half = 0u; subtile_half < 2u; ++subtile_half) {
        const uint32_t bit_offset = subtile_half * 32u;
        const uint32_t coverage =
            (uint32_t)(cell_coverage_mask >> bit_offset);
        const uint32_t valid_mask =
            (uint32_t)(physical_mask >> bit_offset);
        const uint32_t subtile_y =
            aligned_y + subtile_half * SOC_RASTER_SUBTILE_HEIGHT;
        const uint32_t candidate_y = block_y > subtile_y
            ? block_y
            : subtile_y;
        const uint32_t subtile_end_y =
            subtile_y + SOC_RASTER_SUBTILE_HEIGHT;
        const uint32_t candidate_end_y = block_end_y < subtile_end_y
            ? block_end_y
            : subtile_end_y;
        size_t subtile_index;

        if (coverage == 0u || valid_mask == 0u ||
            candidate_y >= candidate_end_y) {
            continue;
        }
        subtile_index =
            (size_t)(aligned_y / SOC_RASTER_SUBTILE_HEIGHT +
                subtile_half) *
                rasterizer->masked_subtile_column_count +
            aligned_x / SOC_RASTER_BLOCK_SIZE;
        update_masked_quick_subtile(
            rasterizer,
            subtile_index,
            coverage,
            valid_mask,
            make_masked_farthest_depth(
                setup,
                block_x,
                candidate_y,
                block_width,
                candidate_end_y - candidate_y
            )
        );
    }
}

static soc_bool masked_subtile_range_is_depth_rejected(
    const soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    soc_raster_depth_block_candidate candidate;
    const uint32_t first_column =
        region->minimum_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t end_column =
        (region->end_x + SOC_RASTER_BLOCK_SIZE - 1u) /
        SOC_RASTER_BLOCK_SIZE;
    const uint32_t first_row =
        region->minimum_y / SOC_RASTER_SUBTILE_HEIGHT;
    const uint32_t end_row =
        (region->end_y + SOC_RASTER_SUBTILE_HEIGHT - 1u) /
        SOC_RASTER_SUBTILE_HEIGHT;
    uint32_t row;

    configure_coarse_depth_candidate(setup, region, &candidate);
    for (row = first_row; row < end_row; ++row) {
        const size_t row_offset =
            (size_t)row * rasterizer->masked_subtile_column_count;
        uint32_t column;

        for (column = first_column; column < end_column; ++column) {
            if (candidate.nearest_depth >
                rasterizer->masked_z0[row_offset + column]) {
                return SOC_FALSE;
            }
        }
    }
    return SOC_TRUE;
}

static soc_bool try_rasterize_masked_single_subtile(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    const uint32_t first_column =
        region->minimum_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t first_row =
        region->minimum_y / SOC_RASTER_SUBTILE_HEIGHT;
    const uint32_t last_column =
        (region->end_x - 1u) / SOC_RASTER_BLOCK_SIZE;
    const uint32_t last_row =
        (region->end_y - 1u) / SOC_RASTER_SUBTILE_HEIGHT;
    const uint32_t block_width = region->end_x - region->minimum_x;
    const uint32_t block_height = region->end_y - region->minimum_y;
    const uint32_t aligned_x = first_column * SOC_RASTER_BLOCK_SIZE;
    const uint32_t aligned_y = first_row * SOC_RASTER_SUBTILE_HEIGHT;
    const size_t subtile_index =
        (size_t)first_row * rasterizer->masked_subtile_column_count +
        first_column;
    soc_raster_block_classification classification;
    soc_tile_edge tile_edges[3];
    uint64_t coverage;
    uint32_t valid_mask;

    if (first_column != last_column || first_row != last_row) {
        return SOC_FALSE;
    }

#if defined(SOC_BUILD_AARCH32_NEON_FMA)
    if (block_width <= SOC_RASTER_ARM32_FUSED_TRIANGLE_SIZE &&
        block_height <= SOC_RASTER_ARM32_FUSED_TRIANGLE_SIZE) {
        coverage = make_masked_tiny_fixed_coverage_arm32(
            setup,
            region->minimum_x,
            region->minimum_y,
            block_width,
            block_height
        );
    } else
#endif
    {
        make_tile_edges(
            setup,
            region->minimum_x,
            region->minimum_y,
            tile_edges
        );
        classification = classify_raster_block(
            tile_edges,
            block_width,
            block_height
        );
        if (classification == SOC_RASTER_BLOCK_OUTSIDE) {
            return SOC_TRUE;
        }
        coverage = make_raster_block_mask(
            tile_edges,
            block_width,
            block_height,
            classification
        );
    }
    if (coverage == 0u) {
        return SOC_TRUE;
    }
    coverage <<=
        (region->minimum_y - aligned_y) * SOC_RASTER_BLOCK_SIZE +
        region->minimum_x - aligned_x;
    valid_mask = (uint32_t)make_physical_block_mask(
        rasterizer->width - aligned_x < SOC_RASTER_BLOCK_SIZE
            ? rasterizer->width - aligned_x
            : SOC_RASTER_BLOCK_SIZE,
        rasterizer->height - aligned_y < SOC_RASTER_SUBTILE_HEIGHT
            ? rasterizer->height - aligned_y
            : SOC_RASTER_SUBTILE_HEIGHT
    );
    update_masked_quick_subtile(
        rasterizer,
        subtile_index,
        (uint32_t)coverage,
        valid_mask,
        make_masked_farthest_depth(
            setup,
            region->minimum_x,
            region->minimum_y,
            block_width,
            block_height
        )
    );
    return SOC_TRUE;
}

static void rasterize_triangle_masked_subtiles(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_SUBTILE_HEIGHT - 1u);

    for (; aligned_y < region->end_y;
         aligned_y += SOC_RASTER_SUBTILE_HEIGHT) {
        const uint32_t block_y = aligned_y < region->minimum_y
            ? region->minimum_y
            : aligned_y;
        const uint32_t aligned_end_y =
            aligned_y + SOC_RASTER_SUBTILE_HEIGHT;
        const uint32_t block_end_y = aligned_end_y < region->end_y
            ? aligned_end_y
            : region->end_y;
        const uint32_t block_height = block_end_y - block_y;
        uint32_t aligned_x = region->minimum_x &
            ~(SOC_RASTER_BLOCK_SIZE - 1u);

        for (; aligned_x < region->end_x;
             aligned_x += SOC_RASTER_BLOCK_SIZE) {
            const uint32_t block_x = aligned_x < region->minimum_x
                ? region->minimum_x
                : aligned_x;
            const uint32_t aligned_end_x =
                aligned_x + SOC_RASTER_BLOCK_SIZE;
            const uint32_t block_end_x = aligned_end_x < region->end_x
                ? aligned_end_x
                : region->end_x;
            const uint32_t block_width = block_end_x - block_x;
            const size_t subtile_index =
                (size_t)(aligned_y / SOC_RASTER_SUBTILE_HEIGHT) *
                    rasterizer->masked_subtile_column_count +
                aligned_x / SOC_RASTER_BLOCK_SIZE;
            soc_tile_edge tile_edges[3];
            soc_raster_block_classification classification;
            uint64_t coverage;
            uint32_t valid_mask;

            if (setup->depth_step_x == 0.0f &&
                setup->depth_step_y == 0.0f &&
                rasterizer->masked_z0[subtile_index] >= 0.0f) {
                const float depth = clamp_depth(
                    setup->depth_sample_origin
                );

                if (depth <= rasterizer->masked_z0[subtile_index]) {
                    continue;
                }
            }
#if defined(SOC_BUILD_AARCH32_NEON_FMA)
            if (block_width <= SOC_RASTER_ARM32_FUSED_TRIANGLE_SIZE &&
                block_height <= SOC_RASTER_ARM32_FUSED_TRIANGLE_SIZE) {
                coverage = make_masked_tiny_fixed_coverage_arm32(
                    setup,
                    block_x,
                    block_y,
                    block_width,
                    block_height
                );
            } else
#endif
            {
                make_tile_edges(setup, block_x, block_y, tile_edges);
                classification = classify_raster_block(
                    tile_edges,
                    block_width,
                    block_height
                );
                if (classification == SOC_RASTER_BLOCK_OUTSIDE) {
                    continue;
                }
                coverage = make_raster_block_mask(
                    tile_edges,
                    block_width,
                    block_height,
                    classification
                );
            }
            if (coverage == 0u) {
                continue;
            }
            coverage <<=
                (block_y - aligned_y) * SOC_RASTER_BLOCK_SIZE +
                block_x - aligned_x;
            valid_mask = (uint32_t)make_physical_block_mask(
                rasterizer->width - aligned_x < SOC_RASTER_BLOCK_SIZE
                    ? rasterizer->width - aligned_x
                    : SOC_RASTER_BLOCK_SIZE,
                rasterizer->height - aligned_y <
                        SOC_RASTER_SUBTILE_HEIGHT
                    ? rasterizer->height - aligned_y
                    : SOC_RASTER_SUBTILE_HEIGHT
            );
            update_masked_quick_subtile(
                rasterizer,
                subtile_index,
                (uint32_t)coverage,
                valid_mask,
                make_masked_farthest_depth(
                    setup,
                    block_x,
                    block_y,
                    block_width,
                    block_height
                )
            );
        }
    }
}

static size_t find_target_early_z_block_index(
    const soc_raster_target* target,
    uint32_t block_x,
    uint32_t block_y
)
{
    const uint32_t first_block_column =
        target->origin_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t first_block_row =
        target->origin_y / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_column =
        block_x / SOC_RASTER_BLOCK_SIZE;
    const uint32_t block_row = block_y / SOC_RASTER_BLOCK_SIZE;

    return (size_t)(block_row - first_block_row) *
        target->early_z_column_count +
        (block_column - first_block_column);
}

static float scan_depth_block_summary(
    const float* depth,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height
)
{
    float summary = depth[0];
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        const float* depth_row = depth + (size_t)row * row_stride;
        uint32_t column = row == 0u ? 1u : 0u;

        for (; column < block_width; ++column) {
            const float candidate = depth_row[column];

            if (candidate < summary) {
                summary = candidate;
            }
        }
    }
    return summary == 0.0f ? 0.0f : summary;
}

static soc_bool update_early_z_after_store(
    float* farthest_depth,
    uint64_t* pending_mask_storage,
    const soc_raster_depth_block_candidate* candidate,
    uint64_t coverage_mask,
    const float* physical_depth,
    size_t row_stride,
    uint32_t physical_width,
    uint32_t physical_height
)
{
    const float untouched_depth = -1.0f;
    const float clear_depth = 0.0f;
    const float previous_farthest_depth = *farthest_depth;
    const soc_bool replaces_farthest =
        candidate->farthest_depth > previous_farthest_depth
            ? SOC_TRUE
            : SOC_FALSE;
    uint64_t pending_mask;
    uint64_t physical_mask;

    if (replaces_farthest != SOC_TRUE) {
        return SOC_FALSE;
    }
    physical_mask = make_physical_block_mask(
        physical_width,
        physical_height
    );
    if (coverage_mask == physical_mask) {
        *farthest_depth = candidate->farthest_depth;
        if (previous_farthest_depth == untouched_depth ||
            *pending_mask_storage != 0u) {
            *pending_mask_storage = 0u;
        }
        return SOC_TRUE;
    }
    pending_mask = (previous_farthest_depth == untouched_depth
            ? physical_mask
            : (*pending_mask_storage == 0u
                ? physical_mask
                : *pending_mask_storage)) &
        ~coverage_mask;
    if (pending_mask != 0u) {
        *pending_mask_storage = pending_mask;
        if (previous_farthest_depth == untouched_depth) {
            *farthest_depth = clear_depth;
        }
        return SOC_FALSE;
    }

    *farthest_depth = scan_depth_block_summary(
        physical_depth,
        row_stride,
        physical_width,
        physical_height
    );
    *pending_mask_storage = 0u;
    return SOC_TRUE;
}

static void rebuild_coarse_early_z(soc_rasterizer* rasterizer)
{
    const float clear_depth = 0.0f;
    const float untouched_depth = -1.0f;
    uint32_t tile_row;

    if (rasterizer->early_z_coarse_dirty != SOC_TRUE) {
        return;
    }
    rasterizer->early_z_ready_tile_count = 0u;
    for (tile_row = 0u;
         tile_row < rasterizer->early_z_tile_row_count;
         ++tile_row) {
        const uint32_t first_block_row = tile_row * 4u;
        const uint32_t block_row_count =
            rasterizer->block_row_count - first_block_row < 4u
                ? rasterizer->block_row_count - first_block_row
                : 4u;
        uint32_t tile_column;

        for (tile_column = 0u;
             tile_column < rasterizer->early_z_tile_column_count;
             ++tile_column) {
            const uint32_t first_block_column = tile_column * 4u;
            const uint32_t block_column_count =
                rasterizer->block_column_count - first_block_column < 4u
                    ? rasterizer->block_column_count - first_block_column
                    : 4u;
            const size_t tile_index =
                (size_t)tile_row *
                    rasterizer->early_z_tile_column_count +
                tile_column;
            float summary = rasterizer->early_z_farthest_depths[
                (size_t)first_block_row * rasterizer->block_column_count +
                first_block_column
            ];
            uint32_t row;

            for (row = 0u; row < block_row_count; ++row) {
                uint32_t column = row == 0u ? 1u : 0u;

                for (; column < block_column_count; ++column) {
                    const float candidate =
                        rasterizer->early_z_farthest_depths[
                        (size_t)(first_block_row + row) *
                            rasterizer->block_column_count +
                        first_block_column + column
                    ];

                    if (candidate < summary) {
                        summary = candidate;
                    }
                }
            }
            rasterizer->early_z_tile_farthest_depths[tile_index] =
                summary;
            if (summary != clear_depth && summary != untouched_depth) {
                ++rasterizer->early_z_ready_tile_count;
            }
        }
    }
    if (rasterizer->early_z_ready_tile_count ==
        rasterizer->early_z_tile_count) {
        size_t tile_index;
        float summary = rasterizer->early_z_tile_farthest_depths[0];

        for (tile_index = 1u;
             tile_index < rasterizer->early_z_tile_count;
             ++tile_index) {
            const float candidate =
                rasterizer->early_z_tile_farthest_depths[tile_index];

            if (candidate < summary) {
                summary = candidate;
            }
        }
        rasterizer->early_z_frame_farthest_depth = summary;
    }
    rasterizer->early_z_coarse_dirty = SOC_FALSE;
}

/* Match the partial-mask kernel's four-lane FMA grouping. A separately
 * rounded extent can cross zero under large endpoint cancellation. */
static float tile_edge_value_at_column(
    const soc_tile_edge* edge,
    float row_value,
    uint32_t column
)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    const uint32_t group_column = column & ~UINT32_C(3);
    const float group_value = fmaf(
        edge->step_x,
        (float)group_column,
        row_value
    );

    return fmaf(
        edge->step_x,
        (float)(column - group_column),
        group_value
    );
#elif defined(SOC_BUILD_AARCH32_NEON_FMA)
    const uint32_t group_column = column & ~UINT32_C(3);
    const float group_value = fmaf(
        edge->step_x,
        (float)group_column,
        row_value
    );

    return fmaf(
        edge->step_x,
        (float)(column - group_column),
        group_value
    );
#else
    float value = row_value;
    uint32_t index;

    for (index = 0u; index < column; ++index) {
        value += edge->step_x;
    }
    return value;
#endif
}

static soc_raster_block_classification classify_raster_block(
    const soc_tile_edge edges[3],
    uint32_t block_width,
    uint32_t block_height
)
{
    soc_bool fully_covered = SOC_TRUE;
    uint32_t edge_index;

    for (edge_index = 0u; edge_index < 3u; ++edge_index) {
        const soc_tile_edge* edge = &edges[edge_index];
        const uint32_t last_column = block_width - 1u;
        const float first_row = fmaf(
            edge->step_y,
            0.0f,
            edge->value
        );
        const float last_row = fmaf(
            edge->step_y,
            (float)(block_height - 1u),
            edge->value
        );
        const float corners[4] = {
            tile_edge_value_at_column(edge, first_row, 0u),
            tile_edge_value_at_column(edge, first_row, last_column),
            tile_edge_value_at_column(edge, last_row, 0u),
            tile_edge_value_at_column(edge, last_row, last_column),
        };
        float minimum = corners[0];
        float maximum = corners[0];
        uint32_t corner;

        for (corner = 1u; corner < 4u; ++corner) {
            if (corners[corner] < minimum) {
                minimum = corners[corner];
            }
            if (corners[corner] > maximum) {
                maximum = corners[corner];
            }
        }

        if (maximum < 0) {
            return SOC_RASTER_BLOCK_OUTSIDE;
        }
        if (minimum < 0) {
            fully_covered = SOC_FALSE;
        }
    }

    return fully_covered == SOC_TRUE
        ? SOC_RASTER_BLOCK_FULL
        : SOC_RASTER_BLOCK_PARTIAL;
}

static uint64_t make_raster_block_mask(
    const soc_tile_edge edges[3],
    uint32_t block_width,
    uint32_t block_height,
    soc_raster_block_classification classification
)
{
    uint64_t coverage_mask = 0u;
    uint32_t row;

    if (classification == SOC_RASTER_BLOCK_FULL) {
        const uint64_t row_mask =
            (UINT64_C(1) << block_width) - UINT64_C(1);

        for (row = 0u; row < block_height; ++row) {
            coverage_mask |= row_mask << (row * SOC_RASTER_BLOCK_SIZE);
        }
        return coverage_mask;
    }

    for (row = 0u; row < block_height; ++row) {
        const float row_edge0 = fmaf(
            edges[0].step_y,
            (float)row,
            edges[0].value
        );
        const float row_edge1 = fmaf(
            edges[1].step_y,
            (float)row,
            edges[1].value
        );
        const float row_edge2 = fmaf(
            edges[2].step_y,
            (float)row,
            edges[2].value
        );

#if defined(__aarch64__) || defined(_M_ARM64)
        static const float lane_offsets_values[4] = {
            0.0f, 1.0f, 2.0f, 3.0f,
        };
        const float32x4_t lane_offsets =
            vld1q_f32(lane_offsets_values);
        const float32x4_t zero = vdupq_n_f32(0.0f);
        uint32_t column = 0u;

        while (column < block_width) {
            const uint32_t lane_count = block_width - column < 4u
                ? block_width - column
                : 4u;
            const float column_offset = (float)column;
            const float32x4_t edge0 = vfmaq_n_f32(
                vdupq_n_f32(fmaf(
                    edges[0].step_x,
                    column_offset,
                    row_edge0
                )),
                lane_offsets,
                edges[0].step_x
            );
            const float32x4_t edge1 = vfmaq_n_f32(
                vdupq_n_f32(fmaf(
                    edges[1].step_x,
                    column_offset,
                    row_edge1
                )),
                lane_offsets,
                edges[1].step_x
            );
            const float32x4_t edge2 = vfmaq_n_f32(
                vdupq_n_f32(fmaf(
                    edges[2].step_x,
                    column_offset,
                    row_edge2
                )),
                lane_offsets,
                edges[2].step_x
            );
            const uint32x4_t inside = vandq_u32(
                vandq_u32(vcgeq_f32(edge0, zero), vcgeq_f32(edge1, zero)),
                vcgeq_f32(edge2, zero)
            );
            uint32_t bits =
                (vgetq_lane_u32(inside, 0) & UINT32_C(1)) |
                (vgetq_lane_u32(inside, 1) & UINT32_C(2)) |
                (vgetq_lane_u32(inside, 2) & UINT32_C(4)) |
                (vgetq_lane_u32(inside, 3) & UINT32_C(8));

            bits &= (UINT32_C(1) << lane_count) - UINT32_C(1);
            coverage_mask |= (uint64_t)bits <<
                (row * SOC_RASTER_BLOCK_SIZE + column);
            column += lane_count;
        }
#elif defined(SOC_BUILD_AARCH32_NEON_FMA)
        static const float lane_offsets_values[4] = {
            0.0f, 1.0f, 2.0f, 3.0f,
        };
        static const uint16_t lane_weights_values[4] = {1u, 2u, 4u, 8u};
        const float32x4_t lane_offsets =
            vld1q_f32(lane_offsets_values);
        const uint16x4_t lane_weights =
            vld1_u16(lane_weights_values);
        const float32x4_t zero = vdupq_n_f32(0.0f);
        uint32_t column = 0u;

        while (column < block_width) {
            const uint32_t lane_count = block_width - column < 4u
                ? block_width - column
                : 4u;
            const float column_offset = (float)column;
            const float32x4_t edge0 = vfmaq_n_f32(
                vdupq_n_f32(fmaf(
                    edges[0].step_x,
                    column_offset,
                    row_edge0
                )),
                lane_offsets,
                edges[0].step_x
            );
            const float32x4_t edge1 = vfmaq_n_f32(
                vdupq_n_f32(fmaf(
                    edges[1].step_x,
                    column_offset,
                    row_edge1
                )),
                lane_offsets,
                edges[1].step_x
            );
            const float32x4_t edge2 = vfmaq_n_f32(
                vdupq_n_f32(fmaf(
                    edges[2].step_x,
                    column_offset,
                    row_edge2
                )),
                lane_offsets,
                edges[2].step_x
            );
            const uint32x4_t inside = vandq_u32(
                vandq_u32(vcgeq_f32(edge0, zero), vcgeq_f32(edge1, zero)),
                vcgeq_f32(edge2, zero)
            );
            const uint16x4_t weighted = vmul_u16(
                vmovn_u32(vshrq_n_u32(inside, 31)),
                lane_weights
            );
            const uint16x4_t pair_sums = vpadd_u16(weighted, weighted);
            const uint32_t bits = (uint32_t)vget_lane_u16(
                vpadd_u16(pair_sums, pair_sums),
                0
            ) & ((UINT32_C(1) << lane_count) - UINT32_C(1));

            coverage_mask |= (uint64_t)bits <<
                (row * SOC_RASTER_BLOCK_SIZE + column);
            column += lane_count;
        }
#else
        float edge0 = row_edge0;
        float edge1 = row_edge1;
        float edge2 = row_edge2;
        uint32_t column;

        for (column = 0u; column < block_width; ++column) {
            if (edge0 >= 0.0f && edge1 >= 0.0f && edge2 >= 0.0f) {
                coverage_mask |= UINT64_C(1) <<
                    (row * SOC_RASTER_BLOCK_SIZE + column);
            }
            edge0 += edges[0].step_x;
            edge1 += edges[1].step_x;
            edge2 += edges[2].step_x;
        }
#endif
    }
    return coverage_mask;
}

static void rasterize_depth_block(
    soc_rasterizer* rasterizer,
    const soc_raster_depth_block_candidate* candidate,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask
)
{
    if (candidate->is_constant == SOC_TRUE) {
        rasterizer->kernels->store_constant_depth_block_f32(
            rasterizer->depth +
                (size_t)block_y * rasterizer->width + block_x,
            rasterizer->width,
            block_width,
            block_height,
            coverage_mask,
            candidate->depth_origin
        );
        return;
    }

    rasterizer->kernels->store_depth_plane_block_f32(
        rasterizer->depth +
            (size_t)block_y * rasterizer->width + block_x,
        rasterizer->width,
        block_width,
        block_height,
        coverage_mask,
        candidate->depth_origin,
        candidate->depth_step_x,
        candidate->depth_step_y
    );
}

static void rasterize_triangle_blocks_fine(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    const uint32_t first_aligned_x = region->minimum_x &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);
    soc_raster_block_edge_cursor edge_cursor;
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);

    initialize_raster_block_edge_cursor(
        setup,
        region->minimum_x,
        &edge_cursor
    );
    for (; aligned_y < region->end_y;
         aligned_y += SOC_RASTER_BLOCK_SIZE) {
        const uint32_t block_y = aligned_y < region->minimum_y
            ? region->minimum_y
            : aligned_y;
        const uint32_t aligned_end_y =
            aligned_y + SOC_RASTER_BLOCK_SIZE;
        const uint32_t block_end_y = aligned_end_y < region->end_y
            ? aligned_end_y
            : region->end_y;
        const uint32_t block_height = block_end_y - block_y;
        uint32_t aligned_x = first_aligned_x;

        reset_raster_block_edge_cursor_row(
            setup,
            region->minimum_x,
            block_y,
            &edge_cursor
        );

        for (; aligned_x < region->end_x;
             aligned_x += SOC_RASTER_BLOCK_SIZE) {
            const uint32_t block_x = aligned_x < region->minimum_x
                ? region->minimum_x
                : aligned_x;
            const uint32_t aligned_end_x =
                aligned_x + SOC_RASTER_BLOCK_SIZE;
            const uint32_t block_end_x = aligned_end_x < region->end_x
                ? aligned_end_x
                : region->end_x;
            const uint32_t block_width = block_end_x - block_x;
            const uint32_t physical_end_x = aligned_end_x <
                    rasterizer->width
                ? aligned_end_x
                : rasterizer->width;
            const uint32_t physical_end_y = aligned_end_y <
                    rasterizer->height
                ? aligned_end_y
                : rasterizer->height;
            const uint32_t physical_width = physical_end_x - aligned_x;
            const uint32_t physical_height = physical_end_y - aligned_y;
            const soc_tile_edge* tile_edges =
                materialize_raster_block_edge_cursor(&edge_cursor);
            soc_raster_block_classification classification;
            size_t early_z_index;
            soc_raster_depth_block_candidate depth_candidate;
            uint64_t coverage_mask;
            uint64_t cell_coverage_mask;
            classification = classify_raster_block(
                tile_edges,
                block_width,
                block_height
            );
            if (classification == SOC_RASTER_BLOCK_OUTSIDE) {
                goto advance_fine_block;
            }
            if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
                coverage_mask = make_raster_block_mask(
                    tile_edges,
                    block_width,
                    block_height,
                    classification
                );
                if (coverage_mask != 0u) {
                    cell_coverage_mask = coverage_mask << (
                        (block_y - aligned_y) * SOC_RASTER_BLOCK_SIZE +
                        block_x - aligned_x
                    );
                    update_masked_quick_block(
                        rasterizer,
                        setup,
                        aligned_x,
                        aligned_y,
                        block_x,
                        block_y,
                        block_width,
                        block_height,
                        physical_width,
                        physical_height,
                        cell_coverage_mask
                    );
                }
                goto advance_fine_block;
            }
            early_z_index = find_early_z_block_index(
                rasterizer,
                aligned_x,
                aligned_y
            );
            configure_depth_block_candidate(
                setup,
                block_x,
                block_y,
                block_width,
                block_height,
                &depth_candidate
            );
            if (depth_block_is_early_z_rejected(
                    &depth_candidate,
                    rasterizer->early_z_farthest_depths[early_z_index]
                ) == SOC_TRUE) {
                goto advance_fine_block;
            }
            coverage_mask = make_raster_block_mask(
                tile_edges,
                block_width,
                block_height,
                classification
            );
            if (coverage_mask != 0u) {
                cell_coverage_mask = coverage_mask << (
                    (block_y - aligned_y) * SOC_RASTER_BLOCK_SIZE +
                    block_x - aligned_x
                );
                rasterize_depth_block(
                    rasterizer,
                    &depth_candidate,
                    block_x,
                    block_y,
                    block_width,
                    block_height,
                    coverage_mask
                );
                if (update_early_z_after_store(
                        &rasterizer->early_z_farthest_depths[
                            early_z_index
                        ],
                        &rasterizer->early_z_pending_masks[early_z_index],
                        &depth_candidate,
                        cell_coverage_mask,
                        rasterizer->depth +
                            (size_t)aligned_y * rasterizer->width +
                            aligned_x,
                        rasterizer->width,
                        physical_width,
                        physical_height
                    ) == SOC_TRUE) {
                    if (rasterizer->early_z_coarse_dirty != SOC_TRUE) {
                        rasterizer->early_z_coarse_dirty = SOC_TRUE;
                    }
                }
            }
advance_fine_block:
            advance_raster_block_edge_cursor(
                &edge_cursor,
                aligned_x == first_aligned_x ? SOC_TRUE : SOC_FALSE
            );
        }
    }
}

static void rasterize_triangle_blocks(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region
)
{
    const float clear_depth = 0.0f;
    soc_raster_depth_block_candidate frame_candidate;
    uint32_t tile_y = region->minimum_y &
        ~(SOC_RASTER_LOCK_TILE_SIZE - 1u);

    if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
        if (rasterizer->masked_frame_farthest_depth >= 0.0f) {
            soc_raster_depth_block_candidate candidate;

            configure_coarse_depth_candidate(setup, region, &candidate);
            if (candidate.nearest_depth <=
                rasterizer->masked_frame_farthest_depth) {
                return;
            }
        }
        if (try_rasterize_masked_single_subtile(
                rasterizer,
                setup,
                region
            ) == SOC_TRUE) {
            return;
        }
        if (region->end_x - region->minimum_x >= 32u &&
            region->end_y - region->minimum_y >= 16u &&
            masked_subtile_range_is_depth_rejected(
                rasterizer,
                setup,
                region
            ) == SOC_TRUE) {
            return;
        }
        if (setup->depth_step_x == 0.0f &&
            setup->depth_step_y == 0.0f &&
            (region->end_x - region->minimum_x >= 16u ||
                region->end_y - region->minimum_y >= 8u)) {
            rasterize_triangle_blocks_fine(rasterizer, setup, region);
        } else {
            rasterize_triangle_masked_subtiles(rasterizer, setup, region);
        }
        return;
    }

    if (rasterizer->early_z_pending_masks == NULL) {
        rasterizer->early_z_pending_masks =
            allocate_early_z_pending_masks(
                rasterizer->early_z_block_count
            );
    }
    if (rasterizer->early_z_ready_tile_count !=
        rasterizer->early_z_tile_count) {
        rasterize_triangle_blocks_fine(rasterizer, setup, region);
        return;
    }
    configure_coarse_depth_candidate(
        setup,
        region,
        &frame_candidate
    );
    if (frame_candidate.nearest_depth <=
        rasterizer->early_z_frame_farthest_depth) {
        return;
    }
    if (frame_candidate.farthest_depth >
        rasterizer->early_z_frame_farthest_depth) {
        rasterize_triangle_blocks_fine(rasterizer, setup, region);
        return;
    }

    for (; tile_y < region->end_y; tile_y += SOC_RASTER_LOCK_TILE_SIZE) {
        const uint32_t tile_end_y = tile_y + SOC_RASTER_LOCK_TILE_SIZE <
                region->end_y
            ? tile_y + SOC_RASTER_LOCK_TILE_SIZE
            : region->end_y;
        const uint32_t minimum_y = tile_y < region->minimum_y
            ? region->minimum_y
            : tile_y;
        uint32_t tile_x = region->minimum_x &
            ~(SOC_RASTER_LOCK_TILE_SIZE - 1u);

        for (; tile_x < region->end_x;
             tile_x += SOC_RASTER_LOCK_TILE_SIZE) {
            const uint32_t tile_end_x = tile_x +
                    SOC_RASTER_LOCK_TILE_SIZE < region->end_x
                ? tile_x + SOC_RASTER_LOCK_TILE_SIZE
                : region->end_x;
            const uint32_t minimum_x = tile_x < region->minimum_x
                ? region->minimum_x
                : tile_x;
            const float early_z_farthest_depth =
                rasterizer->early_z_tile_farthest_depths[
                    (size_t)(tile_y / SOC_RASTER_LOCK_TILE_SIZE) *
                        rasterizer->early_z_tile_column_count +
                    tile_x / SOC_RASTER_LOCK_TILE_SIZE
                ];
            soc_raster_region tile_region;

            tile_region.minimum_x = minimum_x;
            tile_region.minimum_y = minimum_y;
            tile_region.end_x = tile_end_x;
            tile_region.end_y = tile_end_y;
            if (early_z_farthest_depth != clear_depth) {
                soc_raster_depth_block_candidate candidate;

                configure_coarse_depth_candidate(
                    setup,
                    &tile_region,
                    &candidate
                );
                if (candidate.nearest_depth <= early_z_farthest_depth) {
                    continue;
                }
            }
            rasterize_triangle_blocks_fine(
                rasterizer,
                setup,
                &tile_region
            );
        }
    }
}

static void rasterize_depth_block_to_target(
    soc_rasterizer* rasterizer,
    const soc_raster_depth_block_candidate* candidate,
    const soc_raster_target* target,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask
)
{
    const size_t local_x = (size_t)(block_x - target->origin_x);
    const size_t local_y = (size_t)(block_y - target->origin_y);
    float* destination = target->depth +
        local_y * target->row_stride + local_x;

    if (candidate->is_constant == SOC_TRUE) {
        rasterizer->kernels->store_constant_depth_block_f32(
            destination,
            target->row_stride,
            block_width,
            block_height,
            coverage_mask,
            candidate->depth_origin
        );
        return;
    }

    rasterizer->kernels->store_depth_plane_block_f32(
        destination,
        target->row_stride,
        block_width,
        block_height,
        coverage_mask,
        candidate->depth_origin,
        candidate->depth_step_x,
        candidate->depth_step_y
    );
}

static void rasterize_triangle_blocks_to_target(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    const soc_raster_region* region,
    const soc_raster_target* target
)
{
    const uint32_t target_end_x = target->origin_x + target->width;
    const uint32_t target_end_y = target->origin_y + target->height;
    const uint32_t first_aligned_x = region->minimum_x &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);
    soc_raster_block_edge_cursor edge_cursor;
    uint32_t aligned_y = region->minimum_y &
        ~(SOC_RASTER_BLOCK_SIZE - 1u);

    initialize_raster_block_edge_cursor(
        setup,
        region->minimum_x,
        &edge_cursor
    );
    for (; aligned_y < region->end_y;
         aligned_y += SOC_RASTER_BLOCK_SIZE) {
        const uint32_t block_y = aligned_y < region->minimum_y
            ? region->minimum_y
            : aligned_y;
        const uint32_t aligned_end_y =
            aligned_y + SOC_RASTER_BLOCK_SIZE;
        const uint32_t block_end_y = aligned_end_y < region->end_y
            ? aligned_end_y
            : region->end_y;
        const uint32_t block_height = block_end_y - block_y;
        uint32_t aligned_x = first_aligned_x;

        reset_raster_block_edge_cursor_row(
            setup,
            region->minimum_x,
            block_y,
            &edge_cursor
        );

        for (; aligned_x < region->end_x;
             aligned_x += SOC_RASTER_BLOCK_SIZE) {
            const uint32_t block_x = aligned_x < region->minimum_x
                ? region->minimum_x
                : aligned_x;
            const uint32_t aligned_end_x =
                aligned_x + SOC_RASTER_BLOCK_SIZE;
            const uint32_t block_end_x = aligned_end_x < region->end_x
                ? aligned_end_x
                : region->end_x;
            const uint32_t block_width = block_end_x - block_x;
            const uint32_t physical_end_x = aligned_end_x < target_end_x
                ? aligned_end_x
                : target_end_x;
            const uint32_t physical_end_y = aligned_end_y < target_end_y
                ? aligned_end_y
                : target_end_y;
            const uint32_t physical_width = physical_end_x - aligned_x;
            const uint32_t physical_height = physical_end_y - aligned_y;
            const soc_tile_edge* tile_edges =
                materialize_raster_block_edge_cursor(&edge_cursor);
            soc_raster_block_classification classification;
            size_t early_z_index;
            soc_raster_depth_block_candidate depth_candidate;
            uint64_t coverage_mask;
            uint64_t cell_coverage_mask;
            classification = classify_raster_block(
                tile_edges,
                block_width,
                block_height
            );
            if (classification == SOC_RASTER_BLOCK_OUTSIDE) {
                goto advance_target_block;
            }
            early_z_index = find_target_early_z_block_index(
                target,
                aligned_x,
                aligned_y
            );
            configure_depth_block_candidate(
                setup,
                block_x,
                block_y,
                block_width,
                block_height,
                &depth_candidate
            );
            if (depth_block_is_early_z_rejected(
                    &depth_candidate,
                    target->early_z_farthest_depths[early_z_index]
                ) == SOC_TRUE) {
                goto advance_target_block;
            }
            coverage_mask = make_raster_block_mask(
                tile_edges,
                block_width,
                block_height,
                classification
            );
            if (coverage_mask != 0u) {
                cell_coverage_mask = coverage_mask << (
                    (block_y - aligned_y) * SOC_RASTER_BLOCK_SIZE +
                    block_x - aligned_x
                );
                rasterize_depth_block_to_target(
                    rasterizer,
                    &depth_candidate,
                    target,
                    block_x,
                    block_y,
                    block_width,
                    block_height,
                    coverage_mask
                );
                (void)update_early_z_after_store(
                    &target->early_z_farthest_depths[early_z_index],
                    &target->early_z_pending_masks[early_z_index],
                    &depth_candidate,
                    cell_coverage_mask,
                    target->depth +
                        (size_t)(aligned_y - target->origin_y) *
                            target->row_stride +
                        (aligned_x - target->origin_x),
                    target->row_stride,
                    physical_width,
                    physical_height
                );
            }
advance_target_block:
            advance_raster_block_edge_cursor(
                &edge_cursor,
                aligned_x == first_aligned_x ? SOC_TRUE : SOC_FALSE
            );
        }
    }
}

static void acquire_tile_lock(atomic_uint* lock)
{
    for (;;) {
        if (atomic_exchange_explicit(
                lock,
                1u,
                memory_order_acquire
            ) == 0u) {
            return;
        }
        while (atomic_load_explicit(lock, memory_order_relaxed) != 0u) {
        }
    }
}

static void release_tile_lock(atomic_uint* lock)
{
    atomic_store_explicit(lock, 0u, memory_order_release);
}

static soc_bool try_rasterize_single_tile_locked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    soc_raster_tile_locks* tile_locks = rasterizer->tile_locks;
    const uint32_t bounds_width =
        setup->bounds.end_x - setup->bounds.minimum_x;
    const uint32_t bounds_height =
        setup->bounds.end_y - setup->bounds.minimum_y;
    const uint32_t first_tile_x =
        setup->bounds.minimum_x / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t first_tile_y =
        setup->bounds.minimum_y / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t last_tile_x =
        (setup->bounds.end_x - 1u) / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t last_tile_y =
        (setup->bounds.end_y - 1u) / SOC_RASTER_LOCK_TILE_SIZE;
    size_t lock_index;
    atomic_uint* lock;

    if (first_tile_x != last_tile_x || first_tile_y != last_tile_y) {
        return SOC_FALSE;
    }

    lock_index =
        (size_t)first_tile_y * tile_locks->column_count + first_tile_x;
    lock = &tile_locks->locks[lock_index];
    acquire_tile_lock(lock);
    if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
        rasterize_triangle_blocks(
            rasterizer,
            setup,
            &setup->bounds
        );
        release_tile_lock(lock);
        return SOC_TRUE;
    }
    if (bounds_width <= SOC_RASTER_BLOCK_SIZE &&
        bounds_height <= SOC_RASTER_BLOCK_SIZE) {
        if (setup->depth_step_x == 0.0f &&
            setup->depth_step_y == 0.0f) {
            rasterize_small_constant_triangle(rasterizer, setup);
        } else {
            rasterize_small_plane_triangle_untracked(
                rasterizer,
                setup
            );
        }
    } else if (bounds_width <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE &&
        bounds_height <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE) {
        rasterize_small_triangle_blocks_untracked(rasterizer, setup);
    } else {
        rasterize_triangle_blocks(rasterizer, setup, &setup->bounds);
    }
    release_tile_lock(lock);
    return SOC_TRUE;
}

static void rasterize_triangle_tiles_locked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    soc_raster_tile_locks* tile_locks = rasterizer->tile_locks;
    const uint32_t first_tile_x =
        setup->bounds.minimum_x / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t first_tile_y =
        setup->bounds.minimum_y / SOC_RASTER_LOCK_TILE_SIZE;
    const uint32_t end_tile_x =
        (setup->bounds.end_x - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u;
    const uint32_t end_tile_y =
        (setup->bounds.end_y - 1u) / SOC_RASTER_LOCK_TILE_SIZE + 1u;
    uint32_t tile_y;

    for (tile_y = first_tile_y; tile_y < end_tile_y; ++tile_y) {
        const uint32_t tile_minimum_y =
            tile_y * SOC_RASTER_LOCK_TILE_SIZE;
        const uint32_t tile_end_y =
            tile_minimum_y + SOC_RASTER_LOCK_TILE_SIZE;
        uint32_t tile_x;

        for (tile_x = first_tile_x; tile_x < end_tile_x; ++tile_x) {
            const uint32_t tile_minimum_x =
                tile_x * SOC_RASTER_LOCK_TILE_SIZE;
            const uint32_t tile_end_x =
                tile_minimum_x + SOC_RASTER_LOCK_TILE_SIZE;
            const size_t lock_index =
                (size_t)tile_y * tile_locks->column_count + tile_x;
            soc_raster_region region;
            atomic_uint* lock = &tile_locks->locks[lock_index];

            region.minimum_x = setup->bounds.minimum_x > tile_minimum_x
                ? setup->bounds.minimum_x
                : tile_minimum_x;
            region.minimum_y = setup->bounds.minimum_y > tile_minimum_y
                ? setup->bounds.minimum_y
                : tile_minimum_y;
            region.end_x = setup->bounds.end_x < tile_end_x
                ? setup->bounds.end_x
                : tile_end_x;
            region.end_y = setup->bounds.end_y < tile_end_y
                ? setup->bounds.end_y
                : tile_end_y;

            acquire_tile_lock(lock);
            rasterize_triangle_blocks(rasterizer, setup, &region);
            release_tile_lock(lock);
        }
    }
}

static void rasterize_triangle_setup_unlocked(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    const uint32_t bounds_width =
        setup->bounds.end_x - setup->bounds.minimum_x;
    const uint32_t bounds_height =
        setup->bounds.end_y - setup->bounds.minimum_y;

    if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
        rasterize_triangle_blocks(
            rasterizer,
            setup,
            &setup->bounds
        );
        return;
    }

    if (bounds_width <= SOC_RASTER_BLOCK_SIZE &&
        bounds_height <= SOC_RASTER_BLOCK_SIZE) {
        if (setup->depth_step_x == 0.0f &&
            setup->depth_step_y == 0.0f) {
            rasterize_small_constant_triangle(rasterizer, setup);
        } else {
            rasterize_small_plane_triangle_untracked(
                rasterizer,
                setup
            );
        }
        return;
    }
    if (bounds_width <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE &&
        bounds_height <= SOC_RASTER_UNTRACKED_TRIANGLE_SIZE) {
        rasterize_small_triangle_blocks_untracked(rasterizer, setup);
        return;
    }
    rasterize_triangle_blocks(rasterizer, setup, &setup->bounds);
}

static SOC_NOINLINE void rasterize_triangle_setup(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
)
{
    if (rasterizer->tile_locks != NULL) {
        if (try_rasterize_single_tile_locked(
                rasterizer,
                setup
            ) == SOC_TRUE) {
            return;
        }
        rasterize_triangle_tiles_locked(rasterizer, setup);
        return;
    }

    rasterize_triangle_setup_unlocked(rasterizer, setup);
}

static soc_result process_screen_triangle(
    soc_rasterizer* rasterizer,
    const soc_screen_vertex screen[3],
    const soc_raster_depth_plane* shared_depth_plane,
    soc_raster_prepared_list* prepared,
    soc_bool* out_rasterized
)
{
    soc_raster_triangle_setup setup;
    const soc_raster_setup_result setup_result = setup_raster_triangle(
        rasterizer,
        screen,
        shared_depth_plane,
        &setup
    );

    *out_rasterized = SOC_FALSE;
    if (setup_result == SOC_RASTER_SETUP_REJECTED) {
        return SOC_RESULT_OK;
    }
    *out_rasterized = SOC_TRUE;
    if (setup_result == SOC_RASTER_SETUP_EMPTY) {
        return SOC_RESULT_OK;
    }
    if (prepared != NULL) {
        return append_prepared_triangle(prepared, &setup);
    }

    rasterize_triangle_setup(rasterizer, &setup);
    return SOC_RESULT_OK;
}

static soc_result process_clip_triangle(
    soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided,
    soc_raster_prepared_list* prepared,
    soc_bool* out_rasterized
)
{
    soc_screen_vertex screen[3];
    const soc_raster_setup_result setup_result = prepare_screen_triangle(
        rasterizer,
        clip0,
        clip1,
        clip2,
        two_sided,
        screen
    );

    *out_rasterized = SOC_FALSE;
    if (setup_result == SOC_RASTER_SETUP_REJECTED) {
        return SOC_RESULT_OK;
    }
    return process_screen_triangle(
        rasterizer,
        screen,
        NULL,
        prepared,
        out_rasterized
    );
}

static soc_result process_guard_band_clip_triangle(
    soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided,
    soc_raster_prepared_list* prepared,
    soc_bool* out_rasterized
)
{
    soc_screen_vertex screen[3];
    const soc_raster_setup_result setup_result =
        prepare_guard_band_screen_triangle(
            rasterizer,
            clip0,
            clip1,
            clip2,
            two_sided,
            screen
        );

    *out_rasterized = SOC_FALSE;
    if (setup_result == SOC_RASTER_SETUP_REJECTED) {
        return SOC_RESULT_OK;
    }
    return process_screen_triangle(
        rasterizer,
        screen,
        NULL,
        prepared,
        out_rasterized
    );
}

soc_result soc_raster_tile_locks_initialize(
    soc_raster_tile_locks* tile_locks,
    uint32_t width,
    uint32_t height
)
{
    uint32_t column_count;
    uint32_t row_count;
    size_t lock_count;
    size_t lock_bytes;
    atomic_uint* locks;
    size_t lock_index;

    if (tile_locks == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    memset(tile_locks, 0, sizeof(*tile_locks));
    if (!calculate_tile_lock_grid(
            width,
            height,
            &column_count,
            &row_count,
            &lock_count
        ) ||
        !checked_size_multiply(
            lock_count,
            sizeof(*locks),
            &lock_bytes
        )) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    locks = malloc(lock_bytes);
    if (locks == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    for (lock_index = 0u; lock_index < lock_count; ++lock_index) {
        atomic_init(&locks[lock_index], 0u);
    }

    tile_locks->column_count = column_count;
    tile_locks->row_count = row_count;
    tile_locks->lock_count = lock_count;
    tile_locks->locks = locks;
    return SOC_RESULT_OK;
}

void soc_raster_tile_locks_shutdown(
    soc_raster_tile_locks* tile_locks
)
{
    if (tile_locks == NULL) {
        return;
    }

    free((void*)tile_locks->locks);
    memset(tile_locks, 0, sizeof(*tile_locks));
}

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count,
    const soc_kernel_table* kernels
)
{
    size_t required_element_count;
    uint32_t block_column_count;
    uint32_t block_row_count;
    size_t early_z_block_count;
    uint32_t early_z_tile_column_count;
    uint32_t early_z_tile_row_count;
    size_t early_z_tile_count;
    void* early_z_storage;
    float* early_z_farthest_depths;
    float* early_z_tile_farthest_depths;

    if (rasterizer == NULL ||
        width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        depth == NULL ||
        kernels == NULL ||
        kernels->clear_f32 == NULL ||
        kernels->store_constant_depth_block_f32 == NULL ||
        kernels->store_depth_plane_block_f32 == NULL ||
        !checked_size_multiply(
            (size_t)width,
            (size_t)height,
            &required_element_count
        ) ||
        !calculate_early_z_block_grid(
            width,
            height,
            &block_column_count,
            &block_row_count,
            &early_z_block_count
        ) ||
        !calculate_tile_lock_grid(
            width,
            height,
            &early_z_tile_column_count,
            &early_z_tile_row_count,
            &early_z_tile_count
        ) ||
        depth_element_count < required_element_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    early_z_storage = allocate_early_z_storage(
        early_z_block_count,
        early_z_tile_count,
        &early_z_farthest_depths,
        &early_z_tile_farthest_depths
    );
    if (early_z_storage == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    memset(rasterizer, 0, sizeof(*rasterizer));
    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->depth_element_count = required_element_count;
    rasterizer->depth = depth;
    rasterizer->block_column_count = block_column_count;
    rasterizer->block_row_count = block_row_count;
    rasterizer->early_z_block_count = early_z_block_count;
    rasterizer->early_z_storage = early_z_storage;
    rasterizer->early_z_farthest_depths = early_z_farthest_depths;
    rasterizer->early_z_tile_column_count = early_z_tile_column_count;
    rasterizer->early_z_tile_row_count = early_z_tile_row_count;
    rasterizer->early_z_tile_count = early_z_tile_count;
    rasterizer->early_z_tile_farthest_depths =
        early_z_tile_farthest_depths;
    rasterizer->mode = SOC_RASTERIZER_MODE_DENSE;
    rasterizer->kernels = kernels;
    rasterizer->clipped_triangle_count = 0u;
    rasterizer->rasterized_triangle_count = 0u;
    rasterizer->initialized = SOC_TRUE;
    rasterizer->frame_active = SOC_FALSE;
    memset(&rasterizer->frame, 0, sizeof(rasterizer->frame));
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_initialize_masked(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* z0,
    float* z1,
    uint32_t* masks,
    uint32_t subtile_column_count,
    uint32_t subtile_row_count,
    const soc_kernel_table* kernels
)
{
    uint32_t expected_subtile_column_count;
    uint32_t expected_subtile_row_count;
    size_t subtile_count;
    uint32_t block_column_count;
    uint32_t block_row_count;
    size_t block_count;

    if (rasterizer == NULL ||
        width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        z0 == NULL ||
        z1 == NULL ||
        masks == NULL ||
        kernels == NULL ||
        kernels->clear_f32 == NULL ||
        !calculate_masked_subtile_grid(
            width,
            height,
            &expected_subtile_column_count,
            &expected_subtile_row_count,
            &subtile_count
        ) ||
        subtile_column_count != expected_subtile_column_count ||
        subtile_row_count != expected_subtile_row_count ||
        !calculate_early_z_block_grid(
            width,
            height,
            &block_column_count,
            &block_row_count,
            &block_count
        )) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    memset(rasterizer, 0, sizeof(*rasterizer));
    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->block_column_count = block_column_count;
    rasterizer->block_row_count = block_row_count;
    rasterizer->masked_z0 = z0;
    rasterizer->masked_z1 = z1;
    rasterizer->masked_masks = masks;
    rasterizer->masked_subtile_column_count = subtile_column_count;
    rasterizer->masked_subtile_row_count = subtile_row_count;
    rasterizer->masked_subtile_count = subtile_count;
    rasterizer->mode = SOC_RASTERIZER_MODE_MASKED;
    rasterizer->kernels = kernels;
    rasterizer->initialized = SOC_TRUE;
    return SOC_RESULT_OK;
}

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL) {
        return;
    }

    if (rasterizer->initialized == SOC_TRUE) {
        soc_aligned_free(rasterizer->early_z_storage);
        soc_aligned_free(rasterizer->early_z_pending_masks);
    }
    memset(rasterizer, 0, sizeof(*rasterizer));
}

soc_result soc_rasterizer_resize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count
)
{
    size_t required_element_count;
    uint32_t block_column_count;
    uint32_t block_row_count;
    size_t early_z_block_count;
    uint32_t early_z_tile_column_count;
    uint32_t early_z_tile_row_count;
    size_t early_z_tile_count;
    void* early_z_storage;
    float* early_z_farthest_depths;
    uint64_t* early_z_pending_masks;
    float* early_z_tile_farthest_depths;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION ||
        depth == NULL ||
        !checked_size_multiply(
            (size_t)width,
            (size_t)height,
            &required_element_count
        ) ||
        !calculate_early_z_block_grid(
            width,
            height,
            &block_column_count,
            &block_row_count,
            &early_z_block_count
        ) ||
        !calculate_tile_lock_grid(
            width,
            height,
            &early_z_tile_column_count,
            &early_z_tile_row_count,
            &early_z_tile_count
        ) ||
        depth_element_count < required_element_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->mode != SOC_RASTERIZER_MODE_DENSE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    early_z_storage = rasterizer->early_z_storage;
    early_z_farthest_depths = rasterizer->early_z_farthest_depths;
    early_z_pending_masks = rasterizer->early_z_pending_masks;
    early_z_tile_farthest_depths =
        rasterizer->early_z_tile_farthest_depths;
    if (early_z_block_count != rasterizer->early_z_block_count ||
        early_z_tile_count != rasterizer->early_z_tile_count) {
        early_z_storage = allocate_early_z_storage(
            early_z_block_count,
            early_z_tile_count,
            &early_z_farthest_depths,
            &early_z_tile_farthest_depths
        );
        if (early_z_storage == NULL) {
            return SOC_RESULT_OUT_OF_MEMORY;
        }
        if (early_z_pending_masks != NULL &&
            early_z_block_count != rasterizer->early_z_block_count) {
            early_z_pending_masks = allocate_early_z_pending_masks(
                early_z_block_count
            );
            if (early_z_pending_masks == NULL) {
                soc_aligned_free(early_z_storage);
                return SOC_RESULT_OUT_OF_MEMORY;
            }
            soc_aligned_free(rasterizer->early_z_pending_masks);
        }
        soc_aligned_free(rasterizer->early_z_storage);
    }
    rasterizer->width = width;
    rasterizer->height = height;
    rasterizer->depth_element_count = required_element_count;
    rasterizer->depth = depth;
    rasterizer->block_column_count = block_column_count;
    rasterizer->block_row_count = block_row_count;
    rasterizer->early_z_block_count = early_z_block_count;
    rasterizer->early_z_storage = early_z_storage;
    rasterizer->early_z_farthest_depths = early_z_farthest_depths;
    rasterizer->early_z_pending_masks = early_z_pending_masks;
    rasterizer->early_z_tile_column_count = early_z_tile_column_count;
    rasterizer->early_z_tile_row_count = early_z_tile_row_count;
    rasterizer->early_z_tile_count = early_z_tile_count;
    rasterizer->early_z_tile_farthest_depths =
        early_z_tile_farthest_depths;
    rasterizer->tile_locks = NULL;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_configure_tile_locks(
    soc_rasterizer* rasterizer,
    soc_raster_tile_locks* tile_locks
)
{
    uint32_t expected_column_count;
    uint32_t expected_row_count;
    size_t expected_lock_count;

    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (tile_locks == NULL) {
        rasterizer->tile_locks = NULL;
        return SOC_RESULT_OK;
    }
    if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (!calculate_tile_lock_grid(
            rasterizer->width,
            rasterizer->height,
            &expected_column_count,
            &expected_row_count,
            &expected_lock_count
        ) ||
        tile_locks->locks == NULL ||
        tile_locks->column_count != expected_column_count ||
        tile_locks->row_count != expected_row_count ||
        tile_locks->lock_count != expected_lock_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    rasterizer->tile_locks = tile_locks;
    return SOC_RESULT_OK;
}

static soc_result begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc,
    soc_bool clear_depth
)
{
    float initial_depth;
    float untouched_depth;

    if (rasterizer == NULL ||
        rasterizer->initialized != SOC_TRUE ||
        desc == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active == SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    initial_depth = 0.0f;
    untouched_depth = -1.0f;
    rasterizer->frame = *desc;
    if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
        if (clear_depth == SOC_TRUE) {
            rasterizer->kernels->clear_f32(
                rasterizer->masked_z0,
                rasterizer->masked_subtile_count,
                untouched_depth
            );
            rasterizer->kernels->clear_f32(
                rasterizer->masked_z1,
                rasterizer->masked_subtile_count,
                FLT_MAX
            );
            memset(
                rasterizer->masked_masks,
                0,
                rasterizer->masked_subtile_count *
                    sizeof(*rasterizer->masked_masks)
            );
        }
        rasterizer->clipped_triangle_count = 0u;
        rasterizer->rasterized_triangle_count = 0u;
        rasterizer->masked_reference_count = clear_depth == SOC_TRUE
            ? 0u
            : SIZE_MAX;
        rasterizer->masked_frame_farthest_depth = -1.0f;
        rasterizer->frame_active = SOC_TRUE;
        return SOC_RESULT_OK;
    }
    if (rasterizer->depth == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (clear_depth == SOC_TRUE) {
        rasterizer->kernels->clear_f32(
            rasterizer->depth,
            rasterizer->depth_element_count,
            initial_depth
        );
    }
    rasterizer->kernels->clear_f32(
        rasterizer->early_z_farthest_depths,
        rasterizer->early_z_block_count,
        untouched_depth
    );
    rasterizer->early_z_ready_tile_count = 0u;
    rasterizer->early_z_frame_farthest_depth = initial_depth;
    rasterizer->early_z_coarse_dirty = SOC_FALSE;
    rasterizer->clipped_triangle_count = 0u;
    rasterizer->rasterized_triangle_count = 0u;
    rasterizer->frame_active = SOC_TRUE;
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
)
{
    return begin_frame(rasterizer, desc, SOC_TRUE);
}

soc_result soc_rasterizer_begin_frame_no_clear(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
)
{
    return begin_frame(rasterizer, desc, SOC_FALSE);
}

static SOC_NOINLINE soc_result process_occluder_triangles_cached(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count,
    soc_raster_prepared_list* prepared
)
{
    soc_kernel_mat4_f32 clip_from_world;
    soc_kernel_mat4_f32 object_to_world_f32;
    soc_kernel_mat4_f32 clip_from_object;
    soc_post_transform_cache post_transform_cache;
    uint32_t triangle;
    const uint32_t triangle_end = triangle_begin + triangle_count;
    const soc_bool two_sided =
        (mesh->flags & SOC_MESH_FLAG_TWO_SIDED) != 0u
            ? SOC_TRUE
            : SOC_FALSE;

    soc_kernel_mat4_f32_from_f32(
        &rasterizer->frame.clip_from_world,
        &clip_from_world
    );
    soc_kernel_mat4_f32_from_f32(
        object_to_world,
        &object_to_world_f32
    );
    soc_kernel_mat4_f32_multiply(
        &clip_from_world,
        &object_to_world_f32,
        &clip_from_object
    );
    initialize_post_transform_cache(&post_transform_cache);

    for (triangle = triangle_begin; triangle < triangle_end; ++triangle) {
        soc_clip_vertex clip_triangle_vertices[3];
        soc_clip_vertex clipped_polygon[SOC_MAX_CLIPPED_VERTICES];
        soc_raster_depth_plane fan_depth_plane;
        const soc_raster_depth_plane* shared_depth_plane = NULL;
        soc_clip_outcode active_planes;
        soc_clip_classification clip_classification;
        soc_kernel_clip_metadata clip_metadata;
        soc_bool uses_guard_band = SOC_FALSE;
        uint32_t clipped_vertex_count;
        uint32_t fan_index;
        const uint32_t mesh_indices[3] = {
            read_mesh_index(mesh, triangle * 3u),
            read_mesh_index(mesh, triangle * 3u + 1u),
            read_mesh_index(mesh, triangle * 3u + 2u),
        };

        transform_triangle_with_post_cache(
            &clip_from_object,
            mesh,
            mesh_indices,
            rasterizer->frame.clip_depth_range,
            &post_transform_cache,
            clip_triangle_vertices,
            &clip_metadata
        );
        active_planes = clip_metadata.active_planes;
        if (clip_metadata.common_planes != 0u) {
            clip_classification = SOC_CLIP_CLASSIFICATION_REJECT;
        } else {
            clip_classification = active_planes == 0u
                ? SOC_CLIP_CLASSIFICATION_ACCEPT
                : SOC_CLIP_CLASSIFICATION_PARTIAL;
        }
        if (clip_classification == SOC_CLIP_CLASSIFICATION_REJECT) {
            ++rasterizer->clipped_triangle_count;
            continue;
        }

        if (clip_classification == SOC_CLIP_CLASSIFICATION_ACCEPT) {
            soc_bool was_rasterized;
            soc_result result;
            soc_screen_vertex screen[3];
            const soc_raster_setup_result setup_result =
                prepare_cached_screen_triangle(
                    rasterizer,
                    clip_triangle_vertices,
                    mesh_indices,
                    two_sided,
                    &post_transform_cache,
                    screen
                );

            if (setup_result == SOC_RASTER_SETUP_REJECTED) {
                continue;
            }
            result = process_screen_triangle(
                rasterizer,
                screen,
                NULL,
                prepared,
                &was_rasterized
            );

            if (result != SOC_RESULT_OK) {
                return result;
            }
            if (was_rasterized == SOC_TRUE) {
                ++rasterizer->rasterized_triangle_count;
            }
            continue;
        }

        ++rasterizer->clipped_triangle_count;
        if ((active_planes & SOC_CLIP_LATERAL_PLANE_MASK) != 0u &&
            triangle_inside_clip_guard_band(
                clip_triangle_vertices,
                active_planes
            ) == SOC_TRUE) {
            uses_guard_band = SOC_TRUE;
            active_planes = (soc_clip_outcode)(
                active_planes & SOC_CLIP_DEPTH_PLANE_MASK
            );
            if (active_planes == 0u) {
                soc_bool was_rasterized;
                const soc_result result =
                    process_guard_band_clip_triangle(
                        rasterizer,
                        &clip_triangle_vertices[0],
                        &clip_triangle_vertices[1],
                        &clip_triangle_vertices[2],
                        two_sided,
                        prepared,
                        &was_rasterized
                    );

                if (result != SOC_RESULT_OK) {
                    return result;
                }
                if (was_rasterized == SOC_TRUE) {
                    ++rasterizer->rasterized_triangle_count;
                }
                continue;
            }
        }
        clipped_vertex_count = clip_triangle(
            rasterizer,
            clip_triangle_vertices,
            active_planes,
            clipped_polygon
        );
        if (clipped_vertex_count < 3u) {
            continue;
        }

        for (fan_index = 1u;
             fan_index + 1u < clipped_vertex_count;
             ++fan_index) {
            soc_screen_vertex screen[3];
            const soc_raster_setup_result setup_result =
                uses_guard_band == SOC_TRUE
                ? prepare_guard_band_screen_triangle(
                    rasterizer,
                    &clipped_polygon[0],
                    &clipped_polygon[fan_index],
                    &clipped_polygon[fan_index + 1u],
                    two_sided,
                    screen
                )
                : prepare_screen_triangle(
                    rasterizer,
                    &clipped_polygon[0],
                    &clipped_polygon[fan_index],
                    &clipped_polygon[fan_index + 1u],
                    two_sided,
                    screen
                );

            if (setup_result == SOC_RASTER_SETUP_REJECTED) {
                continue;
            }
            if (clipped_vertex_count > 3u &&
                shared_depth_plane == NULL) {
                configure_shared_fan_depth_plane(
                    screen,
                    &fan_depth_plane
                );
                shared_depth_plane = &fan_depth_plane;
            }
            {
                soc_bool was_rasterized;
                const soc_result result = process_screen_triangle(
                    rasterizer,
                    screen,
                    shared_depth_plane,
                    prepared,
                    &was_rasterized
                );

                if (result != SOC_RESULT_OK) {
                    return result;
                }
                if (was_rasterized == SOC_TRUE) {
                    ++rasterizer->rasterized_triangle_count;
                }
            }
        }
    }

    return SOC_RESULT_OK;
}

static soc_result process_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count,
    soc_raster_prepared_list* prepared
)
{
    if (triangle_count >= SOC_POST_TRANSFORM_CACHE_MINIMUM_TRIANGLES &&
        mesh->use_post_transform_cache == SOC_TRUE) {
        return process_occluder_triangles_cached(
            rasterizer,
            mesh,
            object_to_world,
            triangle_begin,
            triangle_count,
            prepared
        );
    }

    soc_kernel_mat4_f32 clip_from_world;
    soc_kernel_mat4_f32 object_to_world_f32;
    soc_kernel_mat4_f32 clip_from_object;
    uint32_t triangle;
    const uint32_t triangle_end = triangle_begin + triangle_count;
    const soc_bool two_sided =
        (mesh->flags & SOC_MESH_FLAG_TWO_SIDED) != 0u
            ? SOC_TRUE
            : SOC_FALSE;

    soc_kernel_mat4_f32_from_f32(
        &rasterizer->frame.clip_from_world,
        &clip_from_world
    );
    soc_kernel_mat4_f32_from_f32(
        object_to_world,
        &object_to_world_f32
    );
    soc_kernel_mat4_f32_multiply(
        &clip_from_world,
        &object_to_world_f32,
        &clip_from_object
    );

    for (triangle = triangle_begin; triangle < triangle_end; ++triangle) {
        soc_clip_vertex clip_triangle_vertices[3];
        soc_clip_vertex clipped_polygon[SOC_MAX_CLIPPED_VERTICES];
        soc_raster_depth_plane fan_depth_plane;
        const soc_raster_depth_plane* shared_depth_plane = NULL;
        soc_clip_outcode active_planes;
        soc_clip_classification clip_classification;
        soc_kernel_clip_metadata clip_metadata;
        soc_bool uses_guard_band = SOC_FALSE;
        uint32_t clipped_vertex_count;
        uint32_t fan_index;
        const uint32_t mesh_index0 = read_mesh_index(
            mesh,
            triangle * 3u
        );
        const uint32_t mesh_index1 = read_mesh_index(
            mesh,
            triangle * 3u + 1u
        );
        const uint32_t mesh_index2 = read_mesh_index(
            mesh,
            triangle * 3u + 2u
        );

        soc_kernel_transform_triangle_f32(
            &clip_from_object,
            mesh->positions_xyz + (size_t)mesh_index0 * 3u,
            mesh->positions_xyz + (size_t)mesh_index1 * 3u,
            mesh->positions_xyz + (size_t)mesh_index2 * 3u,
            rasterizer->frame.clip_depth_range,
            clip_triangle_vertices,
            &clip_metadata
        );
        active_planes = clip_metadata.active_planes;
        if (clip_metadata.common_planes != 0u) {
            clip_classification = SOC_CLIP_CLASSIFICATION_REJECT;
        } else {
            clip_classification = active_planes == 0u
                ? SOC_CLIP_CLASSIFICATION_ACCEPT
                : SOC_CLIP_CLASSIFICATION_PARTIAL;
        }
        if (clip_classification == SOC_CLIP_CLASSIFICATION_REJECT) {
            ++rasterizer->clipped_triangle_count;
            continue;
        }

        if (clip_classification == SOC_CLIP_CLASSIFICATION_ACCEPT) {
            soc_bool was_rasterized;
            const soc_result result = process_clip_triangle(
                rasterizer,
                &clip_triangle_vertices[0],
                &clip_triangle_vertices[1],
                &clip_triangle_vertices[2],
                two_sided,
                prepared,
                &was_rasterized
            );

            if (result != SOC_RESULT_OK) {
                return result;
            }
            if (was_rasterized == SOC_TRUE) {
                ++rasterizer->rasterized_triangle_count;
            }
            continue;
        }

        ++rasterizer->clipped_triangle_count;
        if ((active_planes & SOC_CLIP_LATERAL_PLANE_MASK) != 0u &&
            triangle_inside_clip_guard_band(
                clip_triangle_vertices,
                active_planes
            ) == SOC_TRUE) {
            uses_guard_band = SOC_TRUE;
            active_planes = (soc_clip_outcode)(
                active_planes & SOC_CLIP_DEPTH_PLANE_MASK
            );
            if (active_planes == 0u) {
                soc_bool was_rasterized;
                const soc_result result =
                    process_guard_band_clip_triangle(
                        rasterizer,
                        &clip_triangle_vertices[0],
                        &clip_triangle_vertices[1],
                        &clip_triangle_vertices[2],
                        two_sided,
                        prepared,
                        &was_rasterized
                    );

                if (result != SOC_RESULT_OK) {
                    return result;
                }
                if (was_rasterized == SOC_TRUE) {
                    ++rasterizer->rasterized_triangle_count;
                }
                continue;
            }
        }
        clipped_vertex_count = clip_triangle(
            rasterizer,
            clip_triangle_vertices,
            active_planes,
            clipped_polygon
        );
        if (clipped_vertex_count < 3u) {
            continue;
        }

        for (fan_index = 1u;
             fan_index + 1u < clipped_vertex_count;
             ++fan_index) {
            soc_screen_vertex screen[3];
            const soc_raster_setup_result setup_result =
                uses_guard_band == SOC_TRUE
                ? prepare_guard_band_screen_triangle(
                    rasterizer,
                    &clipped_polygon[0],
                    &clipped_polygon[fan_index],
                    &clipped_polygon[fan_index + 1u],
                    two_sided,
                    screen
                )
                : prepare_screen_triangle(
                    rasterizer,
                    &clipped_polygon[0],
                    &clipped_polygon[fan_index],
                    &clipped_polygon[fan_index + 1u],
                    two_sided,
                    screen
                );

            if (setup_result == SOC_RASTER_SETUP_REJECTED) {
                continue;
            }
            if (clipped_vertex_count > 3u &&
                shared_depth_plane == NULL) {
                configure_shared_fan_depth_plane(
                    screen,
                    &fan_depth_plane
                );
                shared_depth_plane = &fan_depth_plane;
            }
            {
                soc_bool was_rasterized;
                const soc_result result = process_screen_triangle(
                    rasterizer,
                    screen,
                    shared_depth_plane,
                    prepared,
                    &was_rasterized
                );

                if (result != SOC_RESULT_OK) {
                    return result;
                }
                if (was_rasterized == SOC_TRUE) {
                    ++rasterizer->rasterized_triangle_count;
                }
            }
        }
    }

    return SOC_RESULT_OK;
}

static soc_bool occluder_triangle_range_is_valid(
    const soc_mesh* mesh,
    uint32_t triangle_begin,
    uint32_t triangle_count
)
{
    const uint32_t mesh_triangle_count = mesh != NULL
        ? mesh->index_count / 3u
        : 0u;

    return triangle_count != 0u &&
        triangle_begin < mesh_triangle_count &&
        triangle_count <= mesh_triangle_count - triangle_begin
        ? SOC_TRUE
        : SOC_FALSE;
}

soc_result soc_rasterizer_submit_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count
)
{
    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        mesh == NULL ||
        mesh->positions_xyz == NULL ||
        mesh->indices == NULL ||
        object_to_world == NULL ||
        occluder_triangle_range_is_valid(
            mesh,
            triangle_begin,
            triangle_count
        ) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    return process_occluder_triangles(
        rasterizer,
        mesh,
        object_to_world,
        triangle_begin,
        triangle_count,
        NULL
    );
}

soc_result soc_rasterizer_prepare_occluder_triangles(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t triangle_begin,
    uint32_t triangle_count,
    soc_raster_prepared_list* prepared
)
{
    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (mesh == NULL ||
        mesh->positions_xyz == NULL ||
        mesh->indices == NULL ||
        object_to_world == NULL ||
        prepared_list_is_valid(prepared) != SOC_TRUE ||
        occluder_triangle_range_is_valid(
            mesh,
            triangle_begin,
            triangle_count
        ) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    return process_occluder_triangles(
        rasterizer,
        mesh,
        object_to_world,
        triangle_begin,
        triangle_count,
        prepared
    );
}

soc_result soc_rasterizer_rasterize_prepared_triangles(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    size_t prepared_count
)
{
    size_t index;

    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (prepared_count != 0u && prepared == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (index = 0u; index < prepared_count; ++index) {
        rasterize_triangle_setup(rasterizer, &prepared[index]);
    }
    return SOC_RESULT_OK;
}

void soc_rasterizer_rasterize_prepared_region_unchecked(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region
)
{
    soc_raster_region intersection;

    intersection.minimum_x =
        prepared->bounds.minimum_x > region->minimum_x
            ? prepared->bounds.minimum_x
            : region->minimum_x;
    intersection.minimum_y =
        prepared->bounds.minimum_y > region->minimum_y
            ? prepared->bounds.minimum_y
            : region->minimum_y;
    intersection.end_x = prepared->bounds.end_x < region->end_x
        ? prepared->bounds.end_x
        : region->end_x;
    intersection.end_y = prepared->bounds.end_y < region->end_y
        ? prepared->bounds.end_y
        : region->end_y;

    if (intersection.minimum_x >= intersection.end_x ||
        intersection.minimum_y >= intersection.end_y) {
        return;
    }
    if (intersection.minimum_x == prepared->bounds.minimum_x &&
        intersection.minimum_y == prepared->bounds.minimum_y &&
        intersection.end_x == prepared->bounds.end_x &&
        intersection.end_y == prepared->bounds.end_y) {
        rasterize_triangle_setup_unlocked(rasterizer, prepared);
        return;
    }

    rasterize_triangle_blocks(rasterizer, prepared, &intersection);
}

soc_result soc_rasterizer_rasterize_prepared_region(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region
)
{
    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (prepared == NULL ||
        region == NULL ||
        region->minimum_x > region->end_x ||
        region->minimum_y > region->end_y ||
        prepared->bounds.minimum_x >= prepared->bounds.end_x ||
        prepared->bounds.minimum_y >= prepared->bounds.end_y ||
        prepared->bounds.end_x > rasterizer->width ||
        prepared->bounds.end_y > rasterizer->height) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    soc_rasterizer_rasterize_prepared_region_unchecked(
        rasterizer,
        prepared,
        region
    );
    return SOC_RESULT_OK;
}

static soc_bool raster_target_is_valid(
    const soc_raster_target* target
)
{
    size_t last_row_offset;
    size_t required_elements;

    if (target == NULL ||
        target->depth == NULL ||
        target->width == 0u ||
        target->height == 0u ||
        target->row_stride < (size_t)target->width ||
        target->width > UINT32_MAX - target->origin_x ||
        target->height > UINT32_MAX - target->origin_y ||
        !checked_size_multiply(
            (size_t)(target->height - 1u),
            target->row_stride,
            &last_row_offset
        ) ||
        (size_t)target->width > SIZE_MAX - last_row_offset) {
        return SOC_FALSE;
    }

    required_elements = last_row_offset + (size_t)target->width;
    if (target->element_count < required_elements) {
        return SOC_FALSE;
    }
    return SOC_TRUE;
}

void soc_raster_target_reset_early_z_unchecked(
    soc_raster_target* target
)
{
    size_t block_index;

    for (block_index = 0u;
         block_index < target->early_z_block_count;
         ++block_index) {
        target->early_z_farthest_depths[block_index] = -1.0f;
    }
}

void soc_rasterizer_rasterize_prepared_region_to_target_unchecked(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region,
    const soc_raster_target* target
)
{
    soc_raster_region intersection;

    intersection.minimum_x =
        prepared->bounds.minimum_x > region->minimum_x
            ? prepared->bounds.minimum_x
            : region->minimum_x;
    intersection.minimum_y =
        prepared->bounds.minimum_y > region->minimum_y
            ? prepared->bounds.minimum_y
            : region->minimum_y;
    intersection.end_x = prepared->bounds.end_x < region->end_x
        ? prepared->bounds.end_x
        : region->end_x;
    intersection.end_y = prepared->bounds.end_y < region->end_y
        ? prepared->bounds.end_y
        : region->end_y;
    if (intersection.minimum_x >= intersection.end_x ||
        intersection.minimum_y >= intersection.end_y) {
        return;
    }

    rasterize_triangle_blocks_to_target(
        rasterizer,
        prepared,
        &intersection,
        target
    );
}

soc_result soc_rasterizer_rasterize_prepared_region_to_target(
    soc_rasterizer* rasterizer,
    const soc_raster_prepared_triangle* prepared,
    const soc_raster_prepared_region* region,
    const soc_raster_target* target
)
{
    soc_raster_region framebuffer_region;
    uint32_t target_end_x;
    uint32_t target_end_y;

    if (rasterizer == NULL || rasterizer->initialized != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (rasterizer->mode == SOC_RASTERIZER_MODE_MASKED) {
        return SOC_RESULT_INVALID_STATE;
    }
    if (prepared == NULL ||
        region == NULL ||
        region->minimum_x > region->end_x ||
        region->minimum_y > region->end_y ||
        prepared->bounds.minimum_x >= prepared->bounds.end_x ||
        prepared->bounds.minimum_y >= prepared->bounds.end_y ||
        prepared->bounds.end_x > rasterizer->width ||
        prepared->bounds.end_y > rasterizer->height ||
        raster_target_is_valid(target) != SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    framebuffer_region.minimum_x = region->minimum_x;
    framebuffer_region.minimum_y = region->minimum_y;
    framebuffer_region.end_x = region->end_x < rasterizer->width
        ? region->end_x
        : rasterizer->width;
    framebuffer_region.end_y = region->end_y < rasterizer->height
        ? region->end_y
        : rasterizer->height;
    if (framebuffer_region.minimum_x >= framebuffer_region.end_x ||
        framebuffer_region.minimum_y >= framebuffer_region.end_y) {
        return SOC_RESULT_OK;
    }

    target_end_x = target->origin_x + target->width;
    target_end_y = target->origin_y + target->height;
    if (target->origin_x > framebuffer_region.minimum_x ||
        target->origin_y > framebuffer_region.minimum_y ||
        target_end_x < framebuffer_region.end_x ||
        target_end_y < framebuffer_region.end_y) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    soc_rasterizer_rasterize_prepared_region_to_target_unchecked(
        rasterizer,
        prepared,
        &framebuffer_region,
        target
    );
    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_submit_occluders(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
)
{
    size_t transform_byte_count;
    uint32_t instance;
    const uint32_t triangle_count = mesh != NULL
        ? mesh->index_count / 3u
        : 0u;

    if (rasterizer == NULL ||
        rasterizer->frame_active != SOC_TRUE ||
        mesh == NULL ||
        mesh->positions_xyz == NULL ||
        mesh->indices == NULL ||
        object_to_world == NULL ||
        instance_count == 0u ||
        !checked_size_multiply(
            (size_t)instance_count,
            sizeof(*object_to_world),
            &transform_byte_count
        )) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (rasterizer->early_z_ready_tile_count !=
        rasterizer->early_z_tile_count) {
        rebuild_coarse_early_z(rasterizer);
    }

    for (instance = 0u; instance < instance_count; ++instance) {
        const soc_result result =
            soc_rasterizer_submit_occluder_triangles(
                rasterizer,
                mesh,
                &object_to_world[instance],
                0u,
                triangle_count
            );

        if (result != SOC_RESULT_OK) {
            return result;
        }
        if (instance + 1u < instance_count &&
            rasterizer->early_z_ready_tile_count !=
                rasterizer->early_z_tile_count) {
            rebuild_coarse_early_z(rasterizer);
        }
    }

    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_finish_occluders(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL || rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    return SOC_RESULT_OK;
}

soc_result soc_rasterizer_end_frame(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL || rasterizer->frame_active != SOC_TRUE) {
        return SOC_RESULT_INVALID_STATE;
    }

    rasterizer->frame_active = SOC_FALSE;
    return SOC_RESULT_OK;
}
