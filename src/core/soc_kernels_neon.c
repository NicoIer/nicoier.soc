#include "core/soc_kernels.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#include "occlusion/soc_visibility.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

#include <math.h>
#include <string.h>

#if defined(_MSC_VER)
#define SOC_NEON_FORCE_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define SOC_NEON_FORCE_INLINE inline __attribute__((always_inline))
#else
#define SOC_NEON_FORCE_INLINE inline
#endif

static float32x4_t reduce_depth_neon(
    float32x4_t accumulated,
    float32x4_t candidate,
    soc_depth_direction depth_direction
)
{
    const uint32x4_t mask = depth_direction == SOC_DEPTH_REVERSED
        ? vcltq_f32(candidate, accumulated)
        : vcgtq_f32(candidate, accumulated);

    return vbslq_f32(mask, candidate, accumulated);
}

static float reduce_depth_scalar(
    float accumulated,
    float candidate,
    soc_depth_direction depth_direction
)
{
    if (depth_direction == SOC_DEPTH_REVERSED) {
        return candidate < accumulated ? candidate : accumulated;
    }
    return candidate > accumulated ? candidate : accumulated;
}

static SOC_NEON_FORCE_INLINE void store_constant_depth_block_f32_neon_impl(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float candidate_depth,
    soc_bool reversed_depth
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

            if (lane_mask != 0u) {
                const float32x4_t stored =
                    vld1q_f32(destination_row + column);
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
                    reversed_depth == SOC_TRUE
                        ? vcgtq_f32(candidates, compared_stored)
                        : vcltq_f32(candidates, compared_stored);

                vst1q_f32(
                    destination_row + column,
                    vbslq_f32(store_mask, candidates, stored)
                );
            }
            column += 4u;
        }

        for (; column < block_width; ++column) {
            if ((row_mask & (UINT32_C(1) << column)) != 0u) {
                const float stored_depth = destination_row[column];
                const soc_bool passes_depth = reversed_depth == SOC_TRUE
                    ? (candidate_depth > stored_depth
                        ? SOC_TRUE
                        : SOC_FALSE)
                    : (candidate_depth < stored_depth
                        ? SOC_TRUE
                        : SOC_FALSE);

                if (passes_depth == SOC_TRUE) {
                    destination_row[column] = candidate_depth;
                }
            }
        }
    }
}

static void store_constant_depth_block_f32_neon(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float candidate_depth,
    soc_depth_direction depth_direction
)
{
    if (depth_direction == SOC_DEPTH_REVERSED) {
        store_constant_depth_block_f32_neon_impl(
            destination,
            row_stride,
            block_width,
            block_height,
            coverage_mask,
            candidate_depth,
            SOC_TRUE
        );
    } else {
        store_constant_depth_block_f32_neon_impl(
            destination,
            row_stride,
            block_width,
            block_height,
            coverage_mask,
            candidate_depth,
            SOC_FALSE
        );
    }
}

static SOC_NEON_FORCE_INLINE float32x4_t
make_far_biased_plane_depth_neon(
    float32x4_t depth,
    soc_bool reversed_depth
)
{
    const uint32x4_t guard =
        vdupq_n_u32(SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS);
    const uint32x4_t one_bits = vdupq_n_u32(UINT32_C(0x3f800000));
    uint32x4_t bits;

    depth = vmaxq_f32(depth, vdupq_n_f32(0.0f));
    depth = vminq_f32(depth, vdupq_n_f32(1.0f));
    bits = vreinterpretq_u32_f32(depth);
    if (reversed_depth == SOC_TRUE) {
        bits = vqsubq_u32(bits, guard);
    } else {
        bits = vminq_u32(vaddq_u32(bits, guard), one_bits);
    }
    return vreinterpretq_f32_u32(bits);
}

static SOC_NEON_FORCE_INLINE float
make_far_biased_plane_depth_neon_tail(
    float depth,
    soc_bool reversed_depth
)
{
    const uint32_t one_bits = UINT32_C(0x3f800000);
    uint32_t bits;

    if (depth < 0.0f) {
        depth = 0.0f;
    } else if (depth > 1.0f) {
        depth = 1.0f;
    }
    memcpy(&bits, &depth, sizeof(bits));
    if (reversed_depth == SOC_TRUE) {
        bits = bits > SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            ? bits - SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            : 0u;
    } else {
        bits = bits < one_bits - SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            ? bits + SOC_KERNEL_DEPTH_PLANE_GUARD_ULPS
            : one_bits;
    }
    memcpy(&depth, &bits, sizeof(depth));
    return depth;
}

static void store_depth_plane_block_f32_neon(
    float* destination,
    size_t row_stride,
    uint32_t block_width,
    uint32_t block_height,
    uint64_t coverage_mask,
    float depth_origin,
    float depth_step_x,
    float depth_step_y,
    soc_depth_direction depth_direction
)
{
    static const uint32_t lane_bits_values[4] = {1u, 2u, 4u, 8u};
    static const float lane_offsets_values[4] = {0.0f, 1.0f, 2.0f, 3.0f};
    const uint32x4_t lane_bits = vld1q_u32(lane_bits_values);
    const float32x4_t lane_offsets = vld1q_f32(lane_offsets_values);
    const soc_bool reversed_depth =
        depth_direction == SOC_DEPTH_REVERSED ? SOC_TRUE : SOC_FALSE;
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
                const float32x4_t stored =
                    vld1q_f32(destination_row + column);
                float32x4_t compared_stored = stored;
                uint32x4_t store_mask;

                candidates = make_far_biased_plane_depth_neon(
                    candidates,
                    reversed_depth
                );
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
                store_mask = reversed_depth == SOC_TRUE
                    ? vcgtq_f32(candidates, compared_stored)
                    : vcltq_f32(candidates, compared_stored);
                vst1q_f32(
                    destination_row + column,
                    vbslq_f32(store_mask, candidates, stored)
                );
            }
            column += 4u;
        }

        for (; column < block_width; ++column) {
            if ((row_mask & (UINT32_C(1) << column)) != 0u) {
                const float candidate_depth =
                    make_far_biased_plane_depth_neon_tail(
                        fmaf(depth_step_x, (float)column, row_depth),
                        reversed_depth
                    );
                const float stored_depth = destination_row[column];
                const soc_bool passes_depth = reversed_depth == SOC_TRUE
                    ? (candidate_depth > stored_depth
                        ? SOC_TRUE
                        : SOC_FALSE)
                    : (candidate_depth < stored_depth
                        ? SOC_TRUE
                        : SOC_FALSE);

                if (passes_depth == SOC_TRUE) {
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
    uint32_t destination_width,
    soc_depth_direction depth_direction
)
{
    uint32_t destination_x;

    for (destination_x = destination_begin;
         destination_x < destination_width;
         ++destination_x) {
        const uint32_t source_x = destination_x * 2u;
        float reduced = top[source_x];

        if (source_x + 1u < source_width) {
            reduced = reduce_depth_scalar(
                reduced,
                top[source_x + 1u],
                depth_direction
            );
        }
        if (bottom != NULL) {
            reduced = reduce_depth_scalar(
                reduced,
                bottom[source_x],
                depth_direction
            );
            if (source_x + 1u < source_width) {
                reduced = reduce_depth_scalar(
                    reduced,
                    bottom[source_x + 1u],
                    depth_direction
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
    float* destination,
    soc_depth_direction depth_direction
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
                top_pairs.val[1],
                depth_direction
            );
            reduced = reduce_depth_neon(
                reduced,
                bottom_pairs.val[0],
                depth_direction
            );
            reduced = reduce_depth_neon(
                reduced,
                bottom_pairs.val[1],
                depth_direction
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
            destination_width,
            depth_direction
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
            destination_width,
            depth_direction
        );
    }
}

static SOC_NEON_FORCE_INLINE float64x2_t transform_pair_f64_neon(
    float64x2_t column0,
    float64x2_t column1,
    float64x2_t column2,
    float64x2_t column3,
    double x,
    double y,
    double z,
    double w
)
{
    float64x2_t result = vmulq_n_f64(column0, x);

    result = vaddq_f64(result, vmulq_n_f64(column1, y));
    result = vaddq_f64(result, vmulq_n_f64(column2, z));
    result = vaddq_f64(result, vmulq_n_f64(column3, w));
    return result;
}

static SOC_NEON_FORCE_INLINE uint8_t compute_clip_outcode_f64_neon(
    double x,
    double y,
    double z,
    double w,
    soc_clip_depth_range depth_range
)
{
    uint8_t outcode = 0u;

    if (x + w < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 0u));
    }
    if (w - x < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 1u));
    }
    if (y + w < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 2u));
    }
    if (w - y < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 3u));
    }
    if ((depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE ? z : z + w) < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 4u));
    }
    if (w - z < 0.0) {
        outcode = (uint8_t)(outcode | (UINT8_C(1) << 5u));
    }
    return outcode;
}

static void transform_triangle_f64_neon(
    const soc_kernel_mat4_f64* clip_from_object,
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
    const float64x2_t clip_xy0 =
        vld1q_f64(&clip_from_object->columns[0][0]);
    const float64x2_t clip_xy1 =
        vld1q_f64(&clip_from_object->columns[1][0]);
    const float64x2_t clip_xy2 =
        vld1q_f64(&clip_from_object->columns[2][0]);
    const float64x2_t clip_xy3 =
        vld1q_f64(&clip_from_object->columns[3][0]);
    const float64x2_t clip_zw0 =
        vld1q_f64(&clip_from_object->columns[0][2]);
    const float64x2_t clip_zw1 =
        vld1q_f64(&clip_from_object->columns[1][2]);
    const float64x2_t clip_zw2 =
        vld1q_f64(&clip_from_object->columns[2][2]);
    const float64x2_t clip_zw3 =
        vld1q_f64(&clip_from_object->columns[3][2]);
    size_t index;

    out_metadata->active_planes = 0u;
    out_metadata->common_planes = UINT8_C(0x3f);

    for (index = 0u; index < 3u; ++index) {
        const double x = positions[index][0];
        const double y = positions[index][1];
        const double z = positions[index][2];
        const float64x2_t clip_xy = transform_pair_f64_neon(
            clip_xy0,
            clip_xy1,
            clip_xy2,
            clip_xy3,
            x,
            y,
            z,
            1.0
        );
        const float64x2_t clip_zw = transform_pair_f64_neon(
            clip_zw0,
            clip_zw1,
            clip_zw2,
            clip_zw3,
            x,
            y,
            z,
            1.0
        );

        out_clip[index].x = vgetq_lane_f64(clip_xy, 0);
        out_clip[index].y = vgetq_lane_f64(clip_xy, 1);
        out_clip[index].z = vgetq_lane_f64(clip_zw, 0);
        out_clip[index].w = vgetq_lane_f64(clip_zw, 1);
        {
            const uint8_t outcode = compute_clip_outcode_f64_neon(
                out_clip[index].x,
                out_clip[index].y,
                out_clip[index].z,
                out_clip[index].w,
                depth_range
            );

            out_metadata->active_planes = (uint8_t)(
                out_metadata->active_planes | outcode
            );
            out_metadata->common_planes = (uint8_t)(
                out_metadata->common_planes & outcode
            );
        }
    }
}

static const soc_kernel_table neon_kernels = {
    .backend = SOC_KERNEL_BACKEND_NEON,
    .clear_f32 = soc_kernel_clear_f32_scalar,
    .store_constant_depth_block_f32 =
        store_constant_depth_block_f32_neon,
    .store_depth_plane_block_f32 =
        store_depth_plane_block_f32_neon,
    .reduce_hiz_level_f32 = reduce_hiz_level_f32_neon,
    .transform_triangle_f64 = transform_triangle_f64_neon,
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
