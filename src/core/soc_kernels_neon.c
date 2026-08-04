#include "core/soc_kernels.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#include "occlusion/soc_visibility.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

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

                    /*
                     * Uncovered lanes did not participate in the old Scalar
                     * path. Compare the candidate with itself in those lanes
                     * so an uncovered stored NaN cannot affect FP status.
                     */
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
    const soc_kernel_mat4_f64* object_to_world,
    const soc_kernel_mat4_f64* clip_from_world,
    const float* position0_xyz,
    const float* position1_xyz,
    const float* position2_xyz,
    soc_bool positions_all_finite,
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
    const float64x2_t object_xy0 =
        vld1q_f64(&object_to_world->columns[0][0]);
    const float64x2_t object_xy1 =
        vld1q_f64(&object_to_world->columns[1][0]);
    const float64x2_t object_xy2 =
        vld1q_f64(&object_to_world->columns[2][0]);
    const float64x2_t object_xy3 =
        vld1q_f64(&object_to_world->columns[3][0]);
    const float64x2_t object_zw0 =
        vld1q_f64(&object_to_world->columns[0][2]);
    const float64x2_t object_zw1 =
        vld1q_f64(&object_to_world->columns[1][2]);
    const float64x2_t object_zw2 =
        vld1q_f64(&object_to_world->columns[2][2]);
    const float64x2_t object_zw3 =
        vld1q_f64(&object_to_world->columns[3][2]);
    const float64x2_t clip_xy0 =
        vld1q_f64(&clip_from_world->columns[0][0]);
    const float64x2_t clip_xy1 =
        vld1q_f64(&clip_from_world->columns[1][0]);
    const float64x2_t clip_xy2 =
        vld1q_f64(&clip_from_world->columns[2][0]);
    const float64x2_t clip_xy3 =
        vld1q_f64(&clip_from_world->columns[3][0]);
    const float64x2_t clip_zw0 =
        vld1q_f64(&clip_from_world->columns[0][2]);
    const float64x2_t clip_zw1 =
        vld1q_f64(&clip_from_world->columns[1][2]);
    const float64x2_t clip_zw2 =
        vld1q_f64(&clip_from_world->columns[2][2]);
    const float64x2_t clip_zw3 =
        vld1q_f64(&clip_from_world->columns[3][2]);
    size_t index;

    if (object_to_world->all_finite != UINT64_C(1) ||
        clip_from_world->all_finite != UINT64_C(1) ||
        positions_all_finite != SOC_TRUE) {
        soc_kernel_transform_triangle_f64_scalar(
            object_to_world,
            clip_from_world,
            position0_xyz,
            position1_xyz,
            position2_xyz,
            positions_all_finite,
            depth_range,
            out_clip,
            out_metadata
        );
        return;
    }

    out_metadata->active_planes = 0u;
    out_metadata->common_planes = UINT8_C(0x3f);
    out_metadata->all_finite = SOC_TRUE;

    for (index = 0u; index < 3u; ++index) {
        const double x = positions[index][0];
        const double y = positions[index][1];
        const double z = positions[index][2];
        const float64x2_t world_xy = transform_pair_f64_neon(
            object_xy0,
            object_xy1,
            object_xy2,
            object_xy3,
            x,
            y,
            z,
            1.0
        );
        const float64x2_t world_zw = transform_pair_f64_neon(
            object_zw0,
            object_zw1,
            object_zw2,
            object_zw3,
            x,
            y,
            z,
            1.0
        );
        const double world_x = vgetq_lane_f64(world_xy, 0);
        const double world_y = vgetq_lane_f64(world_xy, 1);
        const double world_z = vgetq_lane_f64(world_zw, 0);
        const double world_w = vgetq_lane_f64(world_zw, 1);
        const float64x2_t clip_xy = transform_pair_f64_neon(
            clip_xy0,
            clip_xy1,
            clip_xy2,
            clip_xy3,
            world_x,
            world_y,
            world_z,
            world_w
        );
        const float64x2_t clip_zw = transform_pair_f64_neon(
            clip_zw0,
            clip_zw1,
            clip_zw2,
            clip_zw3,
            world_x,
            world_y,
            world_z,
            world_w
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
    .reduce_hiz_level_f32 = reduce_hiz_level_f32_neon,
    .transform_triangle_f64 = transform_triangle_f64_neon,
    .test_aabbs = soc_occlusion_test_aabbs,
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
