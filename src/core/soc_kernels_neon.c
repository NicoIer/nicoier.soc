#include "core/soc_kernels.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#include "occlusion/soc_visibility.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

#include <math.h>

#if defined(_MSC_VER)
#define SOC_NEON_FORCE_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_NEON_FORCE_INLINE inline __attribute__((always_inline))
#else
#define SOC_NEON_FORCE_INLINE inline
#endif

#define SOC_DEFINE_MERGE_DEPTH_PLANES_F32_NEON_IMPL(name, compare) \
static size_t name( \
    float* level_zero, \
    const float* scratch_planes, \
    size_t element_count, \
    size_t scratch_plane_stride, \
    uint32_t lane_count \
) \
{ \
    const uint32_t scratch_plane_count = lane_count - 1u; \
    size_t element_index = 0u; \
    while (element_count - element_index >= 32u) { \
        float32x4_t merged0 = vld1q_f32(level_zero + element_index + 0u); \
        float32x4_t merged1 = vld1q_f32(level_zero + element_index + 4u); \
        float32x4_t merged2 = vld1q_f32(level_zero + element_index + 8u); \
        float32x4_t merged3 = vld1q_f32(level_zero + element_index + 12u); \
        float32x4_t merged4 = vld1q_f32(level_zero + element_index + 16u); \
        float32x4_t merged5 = vld1q_f32(level_zero + element_index + 20u); \
        float32x4_t merged6 = vld1q_f32(level_zero + element_index + 24u); \
        float32x4_t merged7 = vld1q_f32(level_zero + element_index + 28u); \
        const float* scratch = scratch_planes + element_index; \
        uint32_t plane_index; \
        for (plane_index = 0u; \
             plane_index < scratch_plane_count; \
             ++plane_index) { \
            const float32x4_t candidate0 = vld1q_f32(scratch + 0u); \
            const float32x4_t candidate1 = vld1q_f32(scratch + 4u); \
            const float32x4_t candidate2 = vld1q_f32(scratch + 8u); \
            const float32x4_t candidate3 = vld1q_f32(scratch + 12u); \
            const float32x4_t candidate4 = vld1q_f32(scratch + 16u); \
            const float32x4_t candidate5 = vld1q_f32(scratch + 20u); \
            const float32x4_t candidate6 = vld1q_f32(scratch + 24u); \
            const float32x4_t candidate7 = vld1q_f32(scratch + 28u); \
            merged0 = vbslq_f32( \
                compare(candidate0, merged0), candidate0, merged0); \
            merged1 = vbslq_f32( \
                compare(candidate1, merged1), candidate1, merged1); \
            merged2 = vbslq_f32( \
                compare(candidate2, merged2), candidate2, merged2); \
            merged3 = vbslq_f32( \
                compare(candidate3, merged3), candidate3, merged3); \
            merged4 = vbslq_f32( \
                compare(candidate4, merged4), candidate4, merged4); \
            merged5 = vbslq_f32( \
                compare(candidate5, merged5), candidate5, merged5); \
            merged6 = vbslq_f32( \
                compare(candidate6, merged6), candidate6, merged6); \
            merged7 = vbslq_f32( \
                compare(candidate7, merged7), candidate7, merged7); \
            scratch += scratch_plane_stride; \
        } \
        vst1q_f32(level_zero + element_index + 0u, merged0); \
        vst1q_f32(level_zero + element_index + 4u, merged1); \
        vst1q_f32(level_zero + element_index + 8u, merged2); \
        vst1q_f32(level_zero + element_index + 12u, merged3); \
        vst1q_f32(level_zero + element_index + 16u, merged4); \
        vst1q_f32(level_zero + element_index + 20u, merged5); \
        vst1q_f32(level_zero + element_index + 24u, merged6); \
        vst1q_f32(level_zero + element_index + 28u, merged7); \
        element_index += 32u; \
    } \
    while (element_count - element_index >= 4u) { \
        float32x4_t merged = vld1q_f32(level_zero + element_index); \
        const float* scratch = scratch_planes + element_index; \
        uint32_t plane_index; \
        for (plane_index = 0u; \
             plane_index < scratch_plane_count; \
             ++plane_index) { \
            const float32x4_t candidate = vld1q_f32(scratch); \
            merged = vbslq_f32( \
                compare(candidate, merged), candidate, merged); \
            scratch += scratch_plane_stride; \
        } \
        vst1q_f32(level_zero + element_index, merged); \
        element_index += 4u; \
    } \
    return element_index; \
}

SOC_DEFINE_MERGE_DEPTH_PLANES_F32_NEON_IMPL(
    merge_depth_planes_f32_neon_vectorized,
    vcgtq_f32
)

#undef SOC_DEFINE_MERGE_DEPTH_PLANES_F32_NEON_IMPL

static void merge_depth_planes_f32_neon(
    float* level_zero,
    const float* scratch_planes,
    size_t element_count,
    size_t scratch_plane_stride,
    uint32_t lane_count
)
{
    size_t merged_element_count;

    if (lane_count <= 1u || element_count == 0u) {
        return;
    }
    merged_element_count = merge_depth_planes_f32_neon_vectorized(
        level_zero,
        scratch_planes,
        element_count,
        scratch_plane_stride,
        lane_count
    );
    if (merged_element_count != element_count) {
        soc_kernel_merge_depth_planes_f32_scalar(
            level_zero + merged_element_count,
            scratch_planes + merged_element_count,
            element_count - merged_element_count,
            scratch_plane_stride,
            lane_count
        );
    }
}

static float32x4_t reduce_depth_neon(
    float32x4_t accumulated,
    float32x4_t candidate
)
{
    const uint32x4_t mask = vcltq_f32(candidate, accumulated);

    return vbslq_f32(mask, candidate, accumulated);
}

static float reduce_depth_scalar(float accumulated, float candidate)
{
    return candidate < accumulated ? candidate : accumulated;
}

static void store_constant_depth_block_f32_neon(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float candidate_depth
)
{
    static const uint32_t lane_bits_values[4] = {1u, 2u, 4u, 8u};
    const uint32x4_t lane_bits = vld1q_u32(lane_bits_values);
    const float32x4_t candidates = vdupq_n_f32(candidate_depth);
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        float* destination_row = destination + (size_t)row * row_stride;
        const uint32_t row_mask = (uint32_t)(
            coverage_mask >> (row * SOC_KERNEL_RASTER_BLOCK_SIZE)
        );
        uint32_t column = 0u;

        while (block_width - column >= 4u) {
            const uint32_t lane_mask = (row_mask >> column) & 0x0fu;
            const float32x4_t stored =
                vld1q_f32(destination_row + column);

            if (lane_mask != 0u) {
                float32x4_t compared_stored = stored;

                if (lane_mask != 0x0fu) {
                    const uint32x4_t covered = vtstq_u32(
                        vdupq_n_u32(lane_mask),
                        lane_bits
                    );

                    /* Compare the candidate with itself in uncovered lanes
                     * so their store-mask bits remain clear. */
                    compared_stored = vbslq_f32(
                        covered,
                        stored,
                        candidates
                    );
                }
                const uint32x4_t store_mask =
                    vcgtq_f32(candidates, compared_stored);
                const float32x4_t merged =
                    vbslq_f32(store_mask, candidates, stored);

                vst1q_f32(destination_row + column, merged);
            }
            column += 4u;
        }

        for (; column < block_width; ++column) {
            const float stored_depth = destination_row[column];

            if ((row_mask & (UINT32_C(1) << column)) != 0u &&
                candidate_depth > stored_depth) {
                destination_row[column] = candidate_depth;
            }
        }
    }
}

static SOC_NEON_FORCE_INLINE float32x4_t
clamp_plane_depth_neon(float32x4_t depth)
{
    const float32x4_t zero = vdupq_n_f32(0.0f);

    depth = vmaxq_f32(depth, zero);
    return vminq_f32(depth, vdupq_n_f32(1.0f));
}

static SOC_NEON_FORCE_INLINE float
clamp_plane_depth_neon_tail(float depth)
{
    return depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
}

static void store_depth_plane_block_f32_neon(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float depth_origin,
    float depth_step_x,
    float depth_step_y
)
{
    static const uint32_t lane_bits_values[4] = {1u, 2u, 4u, 8u};
    static const float lane_offsets_values[4] = {0.0f, 1.0f, 2.0f, 3.0f};
    const uint32x4_t lane_bits = vld1q_u32(lane_bits_values);
    const float32x4_t lane_offsets = vld1q_f32(lane_offsets_values);
    uint32_t row;

    for (row = 0u; row < block_height; ++row) {
        float* destination_row = destination + (size_t)row * row_stride;
        const uint32_t row_mask = (uint32_t)(
            coverage_mask >> (row * SOC_KERNEL_RASTER_BLOCK_SIZE)
        );
        const float row_depth = fmaf(
            depth_step_y,
            (float)row,
            depth_origin
        );
        uint32_t column = 0u;

        while (block_width - column >= 4u) {
            const uint32_t lane_mask = (row_mask >> column) & 0x0fu;
            const float32x4_t stored =
                vld1q_f32(destination_row + column);

            if (lane_mask != 0u) {
                const float32x4_t columns = vaddq_f32(
                    lane_offsets,
                    vdupq_n_f32((float)column)
                );
                float32x4_t candidates = vfmaq_n_f32(
                    vdupq_n_f32(row_depth),
                    columns,
                    depth_step_x
                );
                float32x4_t compared_stored = stored;
                uint32x4_t store_mask;

                candidates = clamp_plane_depth_neon(candidates);
                if (lane_mask != 0x0fu) {
                    const uint32x4_t covered = vtstq_u32(
                        vdupq_n_u32(lane_mask),
                        lane_bits
                    );

                    compared_stored = vbslq_f32(
                        covered,
                        stored,
                        candidates
                    );
                }
                store_mask = vcgtq_f32(candidates, compared_stored);
                const float32x4_t merged =
                    vbslq_f32(store_mask, candidates, stored);

                vst1q_f32(destination_row + column, merged);
            }
            column += 4u;
        }

        for (; column < block_width; ++column) {
            const float stored_depth = destination_row[column];

            if ((row_mask & (UINT32_C(1) << column)) != 0u) {
                const float candidate_depth =
                    clamp_plane_depth_neon_tail(
                        fmaf(depth_step_x, (float)column, row_depth)
                    );
                if (candidate_depth > stored_depth) {
                    destination_row[column] = candidate_depth;
                }
            }
        }
    }
}

static void reduce_hiz_row_scalar(
    const float* top,
    const float* bottom,
    uint32_t source_width,
    float* destination,
    uint32_t destination_begin,
    uint32_t destination_width
)
{
    uint32_t destination_x;

    for (destination_x = destination_begin;
         destination_x < destination_width;
         ++destination_x) {
        const uint32_t source_x = destination_x * 2u;
        float reduced = top[source_x];

        if (source_x + 1u < source_width) {
            reduced = reduce_depth_scalar(reduced, top[source_x + 1u]);
        }
        if (bottom != NULL) {
            reduced = reduce_depth_scalar(reduced, bottom[source_x]);
            if (source_x + 1u < source_width) {
                reduced = reduce_depth_scalar(
                    reduced,
                    bottom[source_x + 1u]
                );
            }
        }
        destination[destination_x] = reduced;
    }
}

static void reduce_hiz_level_f32_neon(
    const float* source,
    uint32_t source_width,
    uint32_t source_height,
    float* destination
)
{
    const uint32_t destination_width =
        source_width / 2u + source_width % 2u;
    const uint32_t paired_row_count = source_height / 2u;
    uint32_t destination_y;

    for (destination_y = 0u;
         destination_y < paired_row_count;
         ++destination_y) {
        const float* top =
            source + (size_t)(destination_y * 2u) * source_width;
        const float* bottom = top + source_width;
        float* destination_row =
            destination + (size_t)destination_y * destination_width;
        uint32_t source_x = 0u;
        uint32_t destination_x = 0u;

        while (source_width - source_x >= 8u) {
            const float32x4x2_t top_pairs = vld2q_f32(top + source_x);
            const float32x4x2_t bottom_pairs =
                vld2q_f32(bottom + source_x);
            float32x4_t reduced = top_pairs.val[0];

            reduced = reduce_depth_neon(
                reduced,
                top_pairs.val[1]
            );
            reduced = reduce_depth_neon(
                reduced,
                bottom_pairs.val[0]
            );
            reduced = reduce_depth_neon(
                reduced,
                bottom_pairs.val[1]
            );
            vst1q_f32(destination_row + destination_x, reduced);
            source_x += 8u;
            destination_x += 4u;
        }

        reduce_hiz_row_scalar(
            top,
            bottom,
            source_width,
            destination_row,
            destination_x,
            destination_width
        );
    }

    if ((source_height & 1u) != 0u) {
        const float* top =
            source + (size_t)(source_height - 1u) * source_width;
        float* destination_row =
            destination + (size_t)paired_row_count * destination_width;

        reduce_hiz_row_scalar(
            top,
            NULL,
            source_width,
            destination_row,
            0u,
            destination_width
        );
    }
}

static SOC_NEON_FORCE_INLINE uint8_t compute_clip_outcode_f32_neon(
    float32x4_t clip,
    soc_clip_depth_range depth_range
)
{
    const float x = vgetq_lane_f32(clip, 0);
    const float y = vgetq_lane_f32(clip, 1);
    const float z = vgetq_lane_f32(clip, 2);
    const float w = vgetq_lane_f32(clip, 3);
    uint8_t outcode = 0u;

    if (x + w < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 0u));
    }
    if (w - x < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 1u));
    }
    if (y + w < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 2u));
    }
    if (w - y < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 3u));
    }
    if ((depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE ? z : z + w) < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 4u));
    }
    if (w - z < 0.0f) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 5u));
    }
    return outcode;
}

void soc_kernel_transform_triangle_f32_neon(
    const soc_kernel_mat4_f32* clip_from_object,
    const float* position0_xyz,
    const float* position1_xyz,
    const float* position2_xyz,
    soc_clip_depth_range depth_range,
    soc_kernel_clip_vertex out_clip[3],
    soc_kernel_clip_metadata* out_metadata
)
{
    const float* positions[3] = {
        position0_xyz,
        position1_xyz,
        position2_xyz,
    };
    const float32x4_t column0 =
        vld1q_f32(&clip_from_object->columns[0][0]);
    const float32x4_t column1 =
        vld1q_f32(&clip_from_object->columns[1][0]);
    const float32x4_t column2 =
        vld1q_f32(&clip_from_object->columns[2][0]);
    const float32x4_t column3 =
        vld1q_f32(&clip_from_object->columns[3][0]);
    uint8_t active_planes = 0u;
    uint8_t common_planes = UINT8_C(0x3f);
    size_t index;

    for (index = 0u; index < 3u; ++index) {
        float32x4_t clip = column3;
        uint8_t outcode;

        clip = vfmaq_n_f32(clip, column0, positions[index][0]);
        clip = vfmaq_n_f32(clip, column1, positions[index][1]);
        clip = vfmaq_n_f32(clip, column2, positions[index][2]);
        vst1q_f32(&out_clip[index].x, clip);
        outcode = compute_clip_outcode_f32_neon(clip, depth_range);
        active_planes = (uint8_t)(active_planes | outcode);
        common_planes = (uint8_t)(common_planes & outcode);
    }
    out_metadata->active_planes = active_planes;
    out_metadata->common_planes = common_planes;
}

static const soc_kernel_table neon_kernels = {
    .backend = SOC_KERNEL_BACKEND_NEON,
    .clear_f32 = soc_kernel_clear_f32_scalar,
    .merge_depth_planes_f32 = merge_depth_planes_f32_neon,
    .store_constant_depth_block_f32 =
        store_constant_depth_block_f32_neon,
    .store_depth_plane_block_f32 =
        store_depth_plane_block_f32_neon,
    .reduce_hiz_level_f32 = reduce_hiz_level_f32_neon,
    .test_aabbs = soc_occlusion_test_aabbs_neon,
};

#endif

const soc_kernel_table* soc_kernel_table_neon(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return &neon_kernels;
#else
    return NULL;
#endif
}
