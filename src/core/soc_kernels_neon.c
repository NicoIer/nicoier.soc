#include "core/soc_kernels.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#include "occlusion/soc_visibility.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
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

static const soc_kernel_table neon_kernels = {
    .backend = SOC_KERNEL_BACKEND_NEON,
    .clear_f32 = soc_kernel_clear_f32_scalar,
    .reduce_hiz_level_f32 = reduce_hiz_level_f32_neon,
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
