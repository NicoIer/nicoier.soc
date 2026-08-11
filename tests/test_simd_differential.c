#include "core/soc_kernels.h"
#include "occlusion/soc_hiz.h"
#include "occlusion/soc_visibility.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf( \
                stderr, \
                "%s:%d: check failed: %s\n", \
                __FILE__, \
                __LINE__, \
                #condition \
            ); \
            return 1; \
        } \
    } while (0)

#if defined(__aarch64__) || defined(_M_ARM64) || \
    ((defined(__arm__) || defined(_M_ARM)) && \
        defined(SOC_BUILD_AARCH32_NEON_FMA))
#define SOC_TEST_HAS_NEON_KERNELS 1
#else
#define SOC_TEST_HAS_NEON_KERNELS 0
#endif

#if SOC_TEST_HAS_NEON_KERNELS

enum {
    CLEAR_GUARD_COUNT = 8,
    CLEAR_MAX_COUNT = 35,
    CLEAR_STORAGE_COUNT = 64,
    HIZ_GUARD_COUNT = 8,
    HIZ_MAX_WIDTH = 17,
    HIZ_MAX_HEIGHT = 9,
    HIZ_MAX_SOURCE_COUNT = HIZ_MAX_WIDTH * HIZ_MAX_HEIGHT,
    HIZ_MAX_DESTINATION_COUNT =
        ((HIZ_MAX_WIDTH + 1) / 2) * ((HIZ_MAX_HEIGHT + 1) / 2),
    HIZ_SOURCE_STORAGE_COUNT = 192,
    HIZ_DESTINATION_STORAGE_COUNT = 80,
    RASTER_DEPTH_GUARD_COUNT = 8,
    RASTER_DEPTH_STORAGE_COUNT = 160,
    TRANSFORM_POSITION_STORAGE_COUNT = 32,
    TRANSFORM_OUTPUT_GUARD_COUNT = 4,
};

static const uint32_t CANARY_BITS = UINT32_C(0x7fc0cafe);

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float float_from_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int float_is_close(float left, float right)
{
    const float absolute_tolerance = 1.0e-6f;
    const float relative_tolerance = 8.0f * FLT_EPSILON;
    float difference;
    float scale;

    if (left == right) {
        return 1;
    }
    if (!isfinite(left) || !isfinite(right)) {
        return 0;
    }
    difference = fabsf(left - right);
    scale = fmaxf(fabsf(left), fabsf(right));
    return difference <= absolute_tolerance + relative_tolerance * scale;
}

static void fill_with_bits(float* values, size_t count, uint32_t bits)
{
    const float value = float_from_bits(bits);
    size_t index;

    for (index = 0u; index < count; ++index) {
        values[index] = value;
    }
}

static uint32_t random_u32(uint32_t* state)
{
    uint32_t value = *state;

    if (value == 0u) {
        value = UINT32_C(0x9e3779b9);
    }
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static float random_transform_value(uint32_t* state)
{
    const int32_t scaled =
        (int32_t)(random_u32(state) & UINT32_C(0xffff)) - INT32_C(32768);

    return (float)scaled * (1.0f / 4096.0f);
}

static int run_clear_case(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon,
    size_t offset,
    size_t count,
    uint32_t value_bits
)
{
    float scalar_storage[CLEAR_STORAGE_COUNT];
    float neon_storage[CLEAR_STORAGE_COUNT];
    const size_t begin = CLEAR_GUARD_COUNT + offset;
    const size_t end = begin + count;
    const float value = float_from_bits(value_bits);
    size_t index;

    fill_with_bits(
        scalar_storage,
        ARRAY_COUNT(scalar_storage),
        CANARY_BITS
    );
    fill_with_bits(
        neon_storage,
        ARRAY_COUNT(neon_storage),
        CANARY_BITS
    );

    scalar->clear_f32(scalar_storage + begin, count, value);
    neon->clear_f32(neon_storage + begin, count, value);

    if (memcmp(
            scalar_storage,
            neon_storage,
            sizeof(scalar_storage)
        ) != 0) {
        fprintf(
            stderr,
            "clear mismatch: offset=%zu count=%zu value=0x%08x\n",
            offset,
            count,
            (unsigned)value_bits
        );
        return 1;
    }

    for (index = 0u; index < ARRAY_COUNT(scalar_storage); ++index) {
        const uint32_t expected = index >= begin && index < end
            ? value_bits
            : CANARY_BITS;
        const uint32_t scalar_bits = float_bits(scalar_storage[index]);
        const uint32_t neon_bits = float_bits(neon_storage[index]);

        if (scalar_bits != expected || neon_bits != expected) {
            fprintf(
                stderr,
                "clear canary/value mismatch: offset=%zu count=%zu "
                "index=%zu expected=0x%08x scalar=0x%08x neon=0x%08x\n",
                offset,
                count,
                index,
                (unsigned)expected,
                (unsigned)scalar_bits,
                (unsigned)neon_bits
            );
            return 1;
        }
    }
    return 0;
}

static int test_clear_differential(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    static const uint32_t special_values[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x80000000),
        UINT32_C(0x3f800000),
        UINT32_C(0xbf000000),
    };
    uint32_t random_state = UINT32_C(0x534f4301);
    size_t offset;

    for (offset = 0u; offset < 4u; ++offset) {
        size_t count;

        for (count = 0u; count <= CLEAR_MAX_COUNT; ++count) {
            size_t value_index;

            for (value_index = 0u;
                 value_index < ARRAY_COUNT(special_values);
                 ++value_index) {
                if (run_clear_case(
                        scalar,
                        neon,
                        offset,
                        count,
                        special_values[value_index]
                    ) != 0) {
                    return 1;
                }
            }
            for (value_index = 0u; value_index < 4u; ++value_index) {
                const uint32_t bits = random_u32(&random_state) &
                    ~UINT32_C(0x00800000);

                if (run_clear_case(
                        scalar,
                        neon,
                        offset,
                        count,
                        bits
                    ) != 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static uint64_t make_full_raster_coverage_mask(
    uint32_t width,
    uint32_t height
)
{
    const uint64_t row_mask = (UINT64_C(1) << width) - UINT64_C(1);
    uint64_t coverage_mask = 0u;
    uint32_t row;

    for (row = 0u; row < height; ++row) {
        coverage_mask |= row_mask <<
            (row * SOC_KERNEL_RASTER_BLOCK_SIZE);
    }
    return coverage_mask;
}

static int run_raster_depth_block_case(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon,
    uint32_t width,
    uint32_t height,
    size_t row_stride,
    size_t offset,
    uint64_t coverage_mask,
    uint32_t candidate_bits
)
{
    static const uint32_t stored_values[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x00800000),
        UINT32_C(0x3e800000),
        UINT32_C(0x3f000000),
        UINT32_C(0x3f800000),
    };
    float expected[RASTER_DEPTH_STORAGE_COUNT];
    float scalar_storage[RASTER_DEPTH_STORAGE_COUNT];
    float neon_storage[RASTER_DEPTH_STORAGE_COUNT];
    const size_t begin = RASTER_DEPTH_GUARD_COUNT + offset;
    const float candidate_depth = float_from_bits(candidate_bits);
    size_t index;
    uint32_t row;

    CHECK(width >= 1u && width <= SOC_KERNEL_RASTER_BLOCK_SIZE);
    CHECK(height >= 1u && height <= SOC_KERNEL_RASTER_BLOCK_SIZE);
    CHECK(row_stride >= width);
    CHECK(begin + (size_t)(height - 1u) * row_stride + width <=
        ARRAY_COUNT(expected));

    for (index = 0u; index < ARRAY_COUNT(expected); ++index) {
        const uint32_t bits = stored_values[
            (index + width * 3u + height * 5u + row_stride * 7u + offset) %
                ARRAY_COUNT(stored_values)
        ];

        expected[index] = float_from_bits(bits);
    }
    memcpy(scalar_storage, expected, sizeof(expected));
    memcpy(neon_storage, expected, sizeof(expected));

    for (row = 0u; row < height; ++row) {
        uint32_t column;

        for (column = 0u; column < width; ++column) {
            const uint32_t bit =
                row * SOC_KERNEL_RASTER_BLOCK_SIZE + column;
            float* stored = expected + begin + (size_t)row * row_stride +
                column;

            if ((coverage_mask & (UINT64_C(1) << bit)) != 0u &&
                candidate_depth > *stored) {
                *stored = candidate_depth;
            }
        }
    }

    scalar->store_constant_depth_block_f32(
        scalar_storage + begin,
        row_stride,
        width,
        height,
        coverage_mask,
        candidate_depth
    );
    neon->store_constant_depth_block_f32(
        neon_storage + begin,
        row_stride,
        width,
        height,
        coverage_mask,
        candidate_depth
    );

    for (index = 0u; index < ARRAY_COUNT(expected); ++index) {
        const uint32_t expected_bits = float_bits(expected[index]);
        const uint32_t scalar_bits = float_bits(scalar_storage[index]);
        const uint32_t neon_bits = float_bits(neon_storage[index]);
        if (scalar_bits != expected_bits || neon_bits != expected_bits) {
            fprintf(
                stderr,
                "raster depth mismatch: %ux%u stride=%zu offset=%zu "
                "mask=%016llx candidate=%08x index=%zu "
                "expected=%08x scalar=%08x neon=%08x\n",
                (unsigned)width,
                (unsigned)height,
                row_stride,
                offset,
                (unsigned long long)coverage_mask,
                (unsigned)candidate_bits,
                index,
                (unsigned)expected_bits,
                (unsigned)scalar_bits,
                (unsigned)neon_bits
            );
            return 1;
        }
    }
    return 0;
}

static int test_raster_depth_block_differential(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    static const size_t row_strides[] = {8u, 9u, 17u};
    static const uint32_t candidate_values[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x00800000),
        UINT32_C(0x3e800000),
        UINT32_C(0x3f000000),
        UINT32_C(0x3f800000),
    };
    uint32_t width;

    for (width = 1u; width <= SOC_KERNEL_RASTER_BLOCK_SIZE; ++width) {
        uint32_t height;

        for (height = 1u; height <= SOC_KERNEL_RASTER_BLOCK_SIZE; ++height) {
            const uint32_t last_bit =
                (height - 1u) * SOC_KERNEL_RASTER_BLOCK_SIZE + width - 1u;
            uint32_t random_state = UINT32_C(0x6d2b79f5) ^
                width * UINT32_C(0x9e3779b9) ^
                height * UINT32_C(0x85ebca6b);
            const uint32_t random_low = random_u32(&random_state);
            const uint32_t random_high = random_u32(&random_state);
            const uint64_t masks[] = {
                UINT64_C(0),
                UINT64_MAX,
                make_full_raster_coverage_mask(width, height),
                UINT64_C(0xaaaaaaaaaaaaaaaa),
                UINT64_C(0x5555555555555555),
                UINT64_C(1) | (UINT64_C(1) << last_bit),
                (uint64_t)random_low | ((uint64_t)random_high << 32u),
            };
            size_t stride_index;

            for (stride_index = 0u;
                 stride_index < ARRAY_COUNT(row_strides);
                 ++stride_index) {
                size_t offset;

                for (offset = 0u; offset < 4u; ++offset) {
                    size_t mask_index;

                    for (mask_index = 0u;
                         mask_index < ARRAY_COUNT(masks);
                         ++mask_index) {
                        size_t candidate_index;

                        for (candidate_index = 0u;
                             candidate_index < ARRAY_COUNT(candidate_values);
                             ++candidate_index) {
                            if (run_raster_depth_block_case(
                                    scalar,
                                    neon,
                                    width,
                                    height,
                                    row_strides[stride_index],
                                    offset,
                                    masks[mask_index],
                                    candidate_values[candidate_index]
                                ) != 0) {
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static int test_raster_depth_plane_block_differential(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    static const uint64_t masks[] = {
        UINT64_C(0),
        UINT64_MAX,
        UINT64_C(0xaaaaaaaaaaaaaaaa),
        UINT64_C(0x0123456789abcdef),
    };
    static const float plane_parameters[][3] = {
        {0.1234567f, 0.0135791f, 0.0213579f},
        {0.8765432f, -0.017531f, -0.009713f},
    };
    uint32_t width;

    for (width = 1u; width <= SOC_KERNEL_RASTER_BLOCK_SIZE; ++width) {
        uint32_t height;

        for (height = 1u;
             height <= SOC_KERNEL_RASTER_BLOCK_SIZE;
             ++height) {
            size_t mask_index;

            for (mask_index = 0u;
                 mask_index < ARRAY_COUNT(masks);
                 ++mask_index) {
                size_t plane_index;

                for (plane_index = 0u;
                     plane_index < ARRAY_COUNT(plane_parameters);
                     ++plane_index) {
                    float scalar_storage[RASTER_DEPTH_STORAGE_COUNT];
                    float neon_storage[RASTER_DEPTH_STORAGE_COUNT];
                    const size_t begin = RASTER_DEPTH_GUARD_COUNT + 3u;
                    size_t index;

                    for (index = 0u;
                         index < ARRAY_COUNT(scalar_storage);
                         ++index) {
                        const uint32_t bits = (index & 1u) != 0u
                            ? UINT32_C(0x3e4ccccd)
                            : UINT32_C(0x3f4ccccd);

                        scalar_storage[index] = float_from_bits(bits);
                        neon_storage[index] = float_from_bits(bits);
                    }
                    scalar->store_depth_plane_block_f32(
                        scalar_storage + begin,
                        9u,
                        width,
                        height,
                        masks[mask_index],
                        plane_parameters[plane_index][0],
                        plane_parameters[plane_index][1],
                        plane_parameters[plane_index][2]
                    );
                    neon->store_depth_plane_block_f32(
                        neon_storage + begin,
                        9u,
                        width,
                        height,
                        masks[mask_index],
                        plane_parameters[plane_index][0],
                        plane_parameters[plane_index][1],
                        plane_parameters[plane_index][2]
                    );
                    for (index = 0u;
                         index < ARRAY_COUNT(scalar_storage);
                         ++index) {
                        if (!float_is_close(
                                scalar_storage[index],
                                neon_storage[index]
                            )) {
                            fprintf(
                                stderr,
                                "raster plane mismatch: %ux%u mask=%zu "
                                "plane=%zu index=%zu\n",
                                (unsigned)width,
                                (unsigned)height,
                                mask_index,
                                plane_index,
                                index
                            );
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static int test_raster_depth_block_redzones(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    static const uint32_t widths[] = {1u, 3u, 4u, 5u, 7u, 8u};
    static const uint32_t heights[] = {1u, 7u, 8u};
    size_t width_index;

    for (width_index = 0u;
         width_index < ARRAY_COUNT(widths);
         ++width_index) {
        size_t height_index;

        for (height_index = 0u;
             height_index < ARRAY_COUNT(heights);
             ++height_index) {
            const uint32_t width = widths[width_index];
            const uint32_t height = heights[height_index];
            const size_t logical_count = (size_t)width * height;
            size_t offset;

            for (offset = 0u; offset < 4u; ++offset) {
                const size_t allocation_count = offset + logical_count;
                float* scalar_storage = (float*)malloc(
                    allocation_count * sizeof(*scalar_storage)
                );
                float* neon_storage = (float*)malloc(
                    allocation_count * sizeof(*neon_storage)
                );
                size_t index;

                if (scalar_storage == NULL || neon_storage == NULL) {
                    free(neon_storage);
                    free(scalar_storage);
                    fprintf(stderr, "raster depth redzone allocation failed\n");
                    return 1;
                }
                for (index = 0u; index < allocation_count; ++index) {
                    scalar_storage[index] = float_from_bits(
                        (index & 1u) == 0u
                            ? UINT32_C(0x3e800000)
                            : UINT32_C(0x3f000000)
                    );
                }
                memcpy(
                    neon_storage,
                    scalar_storage,
                    allocation_count * sizeof(*scalar_storage)
                );

                scalar->store_constant_depth_block_f32(
                    scalar_storage + offset,
                    width,
                    width,
                    height,
                    UINT64_MAX,
                    0.75f
                );
                neon->store_constant_depth_block_f32(
                    neon_storage + offset,
                    width,
                    width,
                    height,
                    UINT64_MAX,
                    0.75f
                );
                if (memcmp(
                        scalar_storage,
                        neon_storage,
                        allocation_count * sizeof(*scalar_storage)
                    ) != 0) {
                    fprintf(
                        stderr,
                        "raster depth redzone mismatch: %ux%u offset=%zu\n",
                        (unsigned)width,
                        (unsigned)height,
                        offset
                    );
                    free(neon_storage);
                    free(scalar_storage);
                    return 1;
                }
                free(neon_storage);
                free(scalar_storage);
            }
        }
    }
    return 0;
}

static void make_hiz_source_bits(
    uint32_t* output,
    size_t count,
    uint32_t width,
    uint32_t height,
    size_t source_offset,
    size_t destination_offset,
    uint32_t pattern
)
{
    static const uint32_t special_values[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x00800000),
        UINT32_C(0x3e000000),
        UINT32_C(0x3e800000),
        UINT32_C(0x3f000000),
        UINT32_C(0x3f400000),
        UINT32_C(0x3f800000),
    };
    uint32_t random_state = UINT32_C(0xa341316c) ^
        width * UINT32_C(0x9e3779b9) ^
        height * UINT32_C(0x85ebca6b) ^
        UINT32_C(0xc2b2ae35) ^
        (uint32_t)source_offset * UINT32_C(0x27d4eb2d) ^
        (uint32_t)destination_offset * UINT32_C(0x165667b1);
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (pattern == 0u) {
            const size_t rotation =
                (size_t)width + (size_t)height * 3u +
                source_offset * 5u + destination_offset * 7u;

            output[index] = special_values[
                (index + rotation) % ARRAY_COUNT(special_values)
            ];
        } else {
            output[index] = UINT32_C(0x3f000000) |
                (random_u32(&random_state) & UINT32_C(0x007fffff));
        }
    }
}

static int check_hiz_source_unchanged(
    const float* storage,
    size_t storage_count,
    size_t begin,
    const uint32_t* expected,
    size_t expected_count,
    const char* backend,
    uint32_t width,
    uint32_t height
)
{
    size_t index;

    for (index = 0u; index < storage_count; ++index) {
        const uint32_t expected_bits =
            index >= begin && index < begin + expected_count
            ? expected[index - begin]
            : CANARY_BITS;
        const uint32_t actual_bits = float_bits(storage[index]);

        if (actual_bits != expected_bits) {
            fprintf(
                stderr,
                "%s Hi-Z source modified: %ux%u index=%zu "
                "expected=0x%08x actual=0x%08x\n",
                backend,
                (unsigned)width,
                (unsigned)height,
                index,
                (unsigned)expected_bits,
                (unsigned)actual_bits
            );
            return 1;
        }
    }
    return 0;
}

static int check_hiz_destination_canary(
    const float* storage,
    size_t storage_count,
    size_t begin,
    size_t output_count,
    const char* backend,
    uint32_t width,
    uint32_t height
)
{
    size_t index;

    for (index = 0u; index < storage_count; ++index) {
        if ((index < begin || index >= begin + output_count) &&
            float_bits(storage[index]) != CANARY_BITS) {
            fprintf(
                stderr,
                "%s Hi-Z destination canary modified: %ux%u index=%zu\n",
                backend,
                (unsigned)width,
                (unsigned)height,
                index
            );
            return 1;
        }
    }
    return 0;
}

static int run_hiz_case(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon,
    uint32_t width,
    uint32_t height,
    size_t source_offset,
    size_t destination_offset,
    uint32_t pattern
)
{
    float scalar_source[HIZ_SOURCE_STORAGE_COUNT];
    float neon_source[HIZ_SOURCE_STORAGE_COUNT];
    float scalar_destination[HIZ_DESTINATION_STORAGE_COUNT];
    float neon_destination[HIZ_DESTINATION_STORAGE_COUNT];
    uint32_t source_bits[HIZ_MAX_SOURCE_COUNT];
    const size_t source_begin = HIZ_GUARD_COUNT + source_offset;
    const size_t destination_begin = HIZ_GUARD_COUNT + destination_offset;
    const size_t source_count = (size_t)width * height;
    const uint32_t destination_width = width / 2u + width % 2u;
    const uint32_t destination_height = height / 2u + height % 2u;
    const size_t destination_count =
        (size_t)destination_width * destination_height;
    size_t index;

    CHECK(source_count <= HIZ_MAX_SOURCE_COUNT);
    CHECK(destination_count <= HIZ_MAX_DESTINATION_COUNT);
    CHECK(source_begin + source_count + HIZ_GUARD_COUNT <=
        ARRAY_COUNT(scalar_source));
    CHECK(destination_begin + destination_count + HIZ_GUARD_COUNT <=
        ARRAY_COUNT(scalar_destination));

    make_hiz_source_bits(
        source_bits,
        source_count,
        width,
        height,
        source_offset,
        destination_offset,
        pattern
    );
    fill_with_bits(
        scalar_source,
        ARRAY_COUNT(scalar_source),
        CANARY_BITS
    );
    fill_with_bits(neon_source, ARRAY_COUNT(neon_source), CANARY_BITS);
    fill_with_bits(
        scalar_destination,
        ARRAY_COUNT(scalar_destination),
        CANARY_BITS
    );
    fill_with_bits(
        neon_destination,
        ARRAY_COUNT(neon_destination),
        CANARY_BITS
    );
    for (index = 0u; index < source_count; ++index) {
        scalar_source[source_begin + index] =
            float_from_bits(source_bits[index]);
        neon_source[source_begin + index] =
            float_from_bits(source_bits[index]);
    }

    scalar->reduce_hiz_level_f32(
        scalar_source + source_begin,
        width,
        height,
        scalar_destination + destination_begin
    );
    neon->reduce_hiz_level_f32(
        neon_source + source_begin,
        width,
        height,
        neon_destination + destination_begin
    );

    if (memcmp(
            scalar_destination,
            neon_destination,
            sizeof(scalar_destination)
        ) != 0) {
        for (index = 0u;
             index < ARRAY_COUNT(scalar_destination);
             ++index) {
            const uint32_t scalar_bits = float_bits(
                scalar_destination[index]
            );
            const uint32_t neon_bits = float_bits(neon_destination[index]);

            if (scalar_bits != neon_bits) {
                fprintf(
                    stderr,
                    "Hi-Z mismatch: %ux%u src_offset=%zu "
                    "dst_offset=%zu pattern=%u index=%zu "
                    "scalar=0x%08x neon=0x%08x\n",
                    (unsigned)width,
                    (unsigned)height,
                    source_offset,
                    destination_offset,
                    (unsigned)pattern,
                    index,
                    (unsigned)scalar_bits,
                    (unsigned)neon_bits
                );
                return 1;
            }
        }
    }

    if (check_hiz_source_unchanged(
            scalar_source,
            ARRAY_COUNT(scalar_source),
            source_begin,
            source_bits,
            source_count,
            "scalar",
            width,
            height
        ) != 0 ||
        check_hiz_source_unchanged(
            neon_source,
            ARRAY_COUNT(neon_source),
            source_begin,
            source_bits,
            source_count,
            "NEON",
            width,
            height
        ) != 0 ||
        check_hiz_destination_canary(
            scalar_destination,
            ARRAY_COUNT(scalar_destination),
            destination_begin,
            destination_count,
            "scalar",
            width,
            height
        ) != 0 ||
        check_hiz_destination_canary(
            neon_destination,
            ARRAY_COUNT(neon_destination),
            destination_begin,
            destination_count,
            "NEON",
            width,
            height
        ) != 0) {
        return 1;
    }
    return 0;
}

static int run_hiz_redzone_case(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon,
    uint32_t width,
    uint32_t height,
    size_t offset,
    uint32_t pattern
)
{
    const size_t source_count = (size_t)width * height;
    const uint32_t destination_width = width / 2u + width % 2u;
    const uint32_t destination_height = height / 2u + height % 2u;
    const size_t destination_count =
        (size_t)destination_width * destination_height;
    const size_t source_storage_count = offset + source_count;
    const size_t destination_storage_count = offset + destination_count;
    uint32_t* source_bits = NULL;
    float* scalar_source = NULL;
    float* neon_source = NULL;
    float* scalar_destination = NULL;
    float* neon_destination = NULL;
    size_t index;
    int result = 1;

    source_bits = (uint32_t*)malloc(source_count * sizeof(*source_bits));
    scalar_source = (float*)malloc(
        source_storage_count * sizeof(*scalar_source)
    );
    neon_source = (float*)malloc(
        source_storage_count * sizeof(*neon_source)
    );
    scalar_destination = (float*)malloc(
        destination_storage_count * sizeof(*scalar_destination)
    );
    neon_destination = (float*)malloc(
        destination_storage_count * sizeof(*neon_destination)
    );
    if (source_bits == NULL || scalar_source == NULL || neon_source == NULL ||
        scalar_destination == NULL || neon_destination == NULL) {
        fprintf(stderr, "Hi-Z redzone allocation failed\n");
        goto cleanup;
    }

    make_hiz_source_bits(
        source_bits,
        source_count,
        width,
        height,
        offset,
        offset,
        pattern
    );
    fill_with_bits(
        scalar_source,
        source_storage_count,
        CANARY_BITS
    );
    fill_with_bits(neon_source, source_storage_count, CANARY_BITS);
    fill_with_bits(
        scalar_destination,
        destination_storage_count,
        CANARY_BITS
    );
    fill_with_bits(
        neon_destination,
        destination_storage_count,
        CANARY_BITS
    );
    for (index = 0u; index < source_count; ++index) {
        scalar_source[offset + index] = float_from_bits(source_bits[index]);
        neon_source[offset + index] = float_from_bits(source_bits[index]);
    }

    /*
     * Both logical ranges end exactly at their heap allocations. ASan therefore
     * catches a vector tail which reads or writes into the following redzone.
     */
    scalar->reduce_hiz_level_f32(
        scalar_source + offset,
        width,
        height,
        scalar_destination + offset
    );
    neon->reduce_hiz_level_f32(
        neon_source + offset,
        width,
        height,
        neon_destination + offset
    );

    if (memcmp(
            scalar_source,
            neon_source,
            source_storage_count * sizeof(*scalar_source)
        ) != 0) {
        fprintf(
            stderr,
            "Hi-Z redzone mismatch: %ux%u offset=%zu "
            "pattern=%u\n",
            (unsigned)width,
            (unsigned)height,
            offset,
            (unsigned)pattern
        );
        goto cleanup;
    }
    for (index = 0u; index < destination_storage_count; ++index) {
        const uint32_t scalar_bits = float_bits(scalar_destination[index]);
        const uint32_t neon_bits = float_bits(neon_destination[index]);

        if (scalar_bits != neon_bits) {
            fprintf(
                stderr,
                "Hi-Z redzone value mismatch: %ux%u offset=%zu "
                "pattern=%u index=%zu\n",
                (unsigned)width,
                (unsigned)height,
                offset,
                (unsigned)pattern,
                index
            );
            goto cleanup;
        }
    }
    for (index = 0u; index < offset; ++index) {
        if (float_bits(scalar_source[index]) != CANARY_BITS ||
            float_bits(neon_source[index]) != CANARY_BITS ||
            float_bits(scalar_destination[index]) != CANARY_BITS ||
            float_bits(neon_destination[index]) != CANARY_BITS) {
            fprintf(
                stderr,
                "Hi-Z redzone prefix modified: %ux%u offset=%zu index=%zu\n",
                (unsigned)width,
                (unsigned)height,
                offset,
                index
            );
            goto cleanup;
        }
    }
    for (index = 0u; index < source_count; ++index) {
        if (float_bits(scalar_source[offset + index]) != source_bits[index] ||
            float_bits(neon_source[offset + index]) != source_bits[index]) {
            fprintf(
                stderr,
                "Hi-Z redzone source modified: %ux%u offset=%zu index=%zu\n",
                (unsigned)width,
                (unsigned)height,
                offset,
                index
            );
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    free(neon_destination);
    free(scalar_destination);
    free(neon_source);
    free(scalar_source);
    free(source_bits);
    return result;
}

static int test_hiz_redzones(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    static const uint32_t widths[] = {
        7u, 8u, 9u,
        31u, 32u, 33u,
        63u, 64u, 65u,
    };
    static const uint32_t heights[] = {1u, 2u, 3u, 8u, 9u};
    size_t width_index;

    for (width_index = 0u;
         width_index < ARRAY_COUNT(widths);
         ++width_index) {
        size_t height_index;

        for (height_index = 0u;
             height_index < ARRAY_COUNT(heights);
             ++height_index) {
            size_t offset;

            for (offset = 0u; offset < 4u; ++offset) {
                uint32_t pattern;

                for (pattern = 0u; pattern < 2u; ++pattern) {
                    if (run_hiz_redzone_case(
                            scalar,
                            neon,
                            widths[width_index],
                            heights[height_index],
                            offset,
                            pattern
                        ) != 0) {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static int test_hiz_differential(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    uint32_t width;

    for (width = 1u; width <= HIZ_MAX_WIDTH; ++width) {
        uint32_t height;

        for (height = 1u; height <= HIZ_MAX_HEIGHT; ++height) {
            size_t source_offset;

            for (source_offset = 0u; source_offset < 4u;
                 ++source_offset) {
                size_t destination_offset;

                for (destination_offset = 0u;
                     destination_offset < 4u;
                     ++destination_offset) {
                    uint32_t pattern;

                    for (pattern = 0u; pattern < 2u; ++pattern) {
                        if (run_hiz_case(
                                scalar,
                                neon,
                                width,
                                height,
                                source_offset,
                                destination_offset,
                                pattern
                            ) != 0) {
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

typedef struct transform_output_storage {
    uint64_t before[TRANSFORM_OUTPUT_GUARD_COUNT];
    soc_kernel_clip_vertex vertices[3];
    uint64_t after[TRANSFORM_OUTPUT_GUARD_COUNT];
} transform_output_storage;

static int transform_vertices_are_close(
    const soc_kernel_clip_vertex left[3],
    const soc_kernel_clip_vertex right[3]
)
{
    size_t vertex;

    for (vertex = 0u; vertex < 3u; ++vertex) {
        if (!float_is_close(left[vertex].x, right[vertex].x) ||
            !float_is_close(left[vertex].y, right[vertex].y) ||
            !float_is_close(left[vertex].z, right[vertex].z) ||
            !float_is_close(left[vertex].w, right[vertex].w)) {
            return 0;
        }
    }
    return 1;
}

static int transform_metadata_is_equal(
    const soc_kernel_clip_metadata* left,
    const soc_kernel_clip_metadata* right
)
{
    return left->active_planes == right->active_planes &&
        left->common_planes == right->common_planes;
}

static soc_mat4 transform_matrix_from_values(const float values[16])
{
    const soc_mat4 matrix = {
        .col0 = {values[0], values[1], values[2], values[3]},
        .col1 = {values[4], values[5], values[6], values[7]},
        .col2 = {values[8], values[9], values[10], values[11]},
        .col3 = {values[12], values[13], values[14], values[15]},
    };
    return matrix;
}

static int run_transform_case(
    const soc_mat4* object_to_world,
    const soc_mat4* clip_from_world,
    const uint32_t position_bits[9],
    size_t offset,
    soc_clip_depth_range depth_range,
    const soc_kernel_clip_metadata* expected_metadata,
    const char* label
)
{
    static const size_t position_starts[3] = {4u, 13u, 22u};
    static const uint64_t guard_value = UINT64_C(0x7ff8cafe12345678);
    float position_storage[TRANSFORM_POSITION_STORAGE_COUNT];
    float position_before[TRANSFORM_POSITION_STORAGE_COUNT];
    soc_kernel_mat4_f32 object_f32;
    soc_kernel_mat4_f32 clip_f32;
    soc_kernel_mat4_f32 clip_from_object_f32;
    soc_kernel_mat4_f32 object_before;
    soc_kernel_mat4_f32 clip_before;
    soc_kernel_mat4_f32 clip_from_object_before;
    transform_output_storage scalar_output;
    transform_output_storage neon_output;
    soc_kernel_clip_metadata scalar_metadata = {0xa5u, 0xa5u};
    soc_kernel_clip_metadata neon_metadata = {0x5au, 0x5au};
    size_t vertex;
    size_t index;

    fill_with_bits(
        position_storage,
        ARRAY_COUNT(position_storage),
        CANARY_BITS
    );
    for (vertex = 0u; vertex < 3u; ++vertex) {
        for (index = 0u; index < 3u; ++index) {
            position_storage[position_starts[vertex] + offset + index] =
                float_from_bits(position_bits[vertex * 3u + index]);
        }
    }
    memcpy(position_before, position_storage, sizeof(position_storage));
    soc_kernel_mat4_f32_from_f32(object_to_world, &object_f32);
    soc_kernel_mat4_f32_from_f32(clip_from_world, &clip_f32);
    soc_kernel_mat4_f32_multiply(
        &clip_f32,
        &object_f32,
        &clip_from_object_f32
    );
    object_before = object_f32;
    clip_before = clip_f32;
    clip_from_object_before = clip_from_object_f32;
    for (index = 0u; index < TRANSFORM_OUTPUT_GUARD_COUNT; ++index) {
        scalar_output.before[index] = guard_value;
        scalar_output.after[index] = guard_value;
        neon_output.before[index] = guard_value;
        neon_output.after[index] = guard_value;
    }
    memset(scalar_output.vertices, 0xa5, sizeof(scalar_output.vertices));
    memset(neon_output.vertices, 0x5a, sizeof(neon_output.vertices));

    soc_kernel_transform_triangle_f32_scalar(
        &clip_from_object_f32,
        position_storage + position_starts[0] + offset,
        position_storage + position_starts[1] + offset,
        position_storage + position_starts[2] + offset,
        depth_range,
        scalar_output.vertices,
        &scalar_metadata
    );
    if (memcmp(
            position_storage,
            position_before,
            sizeof(position_storage)
        ) != 0 ||
        memcmp(&object_f32, &object_before, sizeof(object_f32)) != 0 ||
        memcmp(&clip_f32, &clip_before, sizeof(clip_f32)) != 0 ||
        memcmp(
            &clip_from_object_f32,
            &clip_from_object_before,
            sizeof(clip_from_object_f32)
        ) != 0) {
        fprintf(stderr, "transform Scalar modified input: %s\n", label);
        return 1;
    }
    if (expected_metadata != NULL && !transform_metadata_is_equal(
            &scalar_metadata,
            expected_metadata
        )) {
        fprintf(
            stderr,
            "transform metadata mismatch: %s active=%u common=%u\n",
            label,
            (unsigned)scalar_metadata.active_planes,
            (unsigned)scalar_metadata.common_planes
        );
        return 1;
    }

    soc_kernel_transform_triangle_f32_neon(
        &clip_from_object_f32,
        position_storage + position_starts[0] + offset,
        position_storage + position_starts[1] + offset,
        position_storage + position_starts[2] + offset,
        depth_range,
        neon_output.vertices,
        &neon_metadata
    );
    if (memcmp(
            position_storage,
            position_before,
            sizeof(position_storage)
        ) != 0 ||
        memcmp(&object_f32, &object_before, sizeof(object_f32)) != 0 ||
        memcmp(&clip_f32, &clip_before, sizeof(clip_f32)) != 0 ||
        memcmp(
            &clip_from_object_f32,
            &clip_from_object_before,
            sizeof(clip_from_object_f32)
        ) != 0) {
        fprintf(stderr, "transform NEON modified input: %s\n", label);
        return 1;
    }
    if (!transform_vertices_are_close(
            scalar_output.vertices,
            neon_output.vertices
        ) ||
        !transform_metadata_is_equal(&scalar_metadata, &neon_metadata)) {
        fprintf(
            stderr,
            "transform mismatch: %s offset=%zu\n",
            label,
            offset
        );
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const float scalar_components[4] = {
                scalar_output.vertices[vertex].x,
                scalar_output.vertices[vertex].y,
                scalar_output.vertices[vertex].z,
                scalar_output.vertices[vertex].w,
            };
            const float neon_components[4] = {
                neon_output.vertices[vertex].x,
                neon_output.vertices[vertex].y,
                neon_output.vertices[vertex].z,
                neon_output.vertices[vertex].w,
            };

            for (index = 0u; index < 4u; ++index) {
                if (!float_is_close(
                        scalar_components[index],
                        neon_components[index]
                    )) {
                    fprintf(
                        stderr,
                        "  vertex=%zu component=%zu scalar=%.9g "
                        "neon=%.9g\n",
                        vertex,
                        index,
                        scalar_components[index],
                        neon_components[index]
                    );
                }
            }
        }
        return 1;
    }
    for (index = 0u; index < TRANSFORM_OUTPUT_GUARD_COUNT; ++index) {
        if (scalar_output.before[index] != guard_value ||
            scalar_output.after[index] != guard_value ||
            neon_output.before[index] != guard_value ||
            neon_output.after[index] != guard_value) {
            fprintf(stderr, "transform output canary changed: %s\n", label);
            return 1;
        }
    }
    return 0;
}

static float transform_reference_component(
    float column0,
    float column1,
    float column2,
    float column3,
    const soc_kernel_clip_vertex* vertex
)
{
    float result = column3 * vertex->w;

    result = fmaf(column0, vertex->x, result);
    result = fmaf(column1, vertex->y, result);
    result = fmaf(column2, vertex->z, result);
    return result;
}

static soc_kernel_clip_vertex transform_reference_vertex(
    const soc_kernel_mat4_f32* matrix,
    const soc_kernel_clip_vertex* vertex
)
{
    const soc_kernel_clip_vertex result = {
        transform_reference_component(
            matrix->columns[0][0],
            matrix->columns[1][0],
            matrix->columns[2][0],
            matrix->columns[3][0],
            vertex
        ),
        transform_reference_component(
            matrix->columns[0][1],
            matrix->columns[1][1],
            matrix->columns[2][1],
            matrix->columns[3][1],
            vertex
        ),
        transform_reference_component(
            matrix->columns[0][2],
            matrix->columns[1][2],
            matrix->columns[2][2],
            matrix->columns[3][2],
            vertex
        ),
        transform_reference_component(
            matrix->columns[0][3],
            matrix->columns[1][3],
            matrix->columns[2][3],
            matrix->columns[3][3],
            vertex
        ),
    };
    return result;
}

static soc_kernel_mat4_f32 transform_reference_matrix_multiply(
    const soc_kernel_mat4_f32* left,
    const soc_kernel_mat4_f32* right
)
{
    soc_kernel_mat4_f32 result;
    size_t column;

    for (column = 0u; column < 4u; ++column) {
        const soc_kernel_clip_vertex right_column = {
            right->columns[column][0],
            right->columns[column][1],
            right->columns[column][2],
            right->columns[column][3],
        };
        const soc_kernel_clip_vertex product = transform_reference_vertex(
            left,
            &right_column
        );

        result.columns[column][0] = product.x;
        result.columns[column][1] = product.y;
        result.columns[column][2] = product.z;
        result.columns[column][3] = product.w;
    }
    return result;
}

static int transform_matrices_are_close(
    const soc_kernel_mat4_f32* left,
    const soc_kernel_mat4_f32* right
)
{
    size_t column;
    size_t row;

    for (column = 0u; column < 4u; ++column) {
        for (row = 0u; row < 4u; ++row) {
            if (!float_is_close(
                    left->columns[column][row],
                    right->columns[column][row]
                )) {
                return 0;
            }
        }
    }
    return 1;
}

static int test_transform_reference(void)
{
    static const float object_values[16] = {
        1.234567f, -0.918273f, 0.314159f, 0.271828f,
        -0.456789f, 1.111111f, -0.707107f, 0.0625f,
        0.333333f, -1.414214f, 0.987654f, -0.03125f,
        123.4567f, -0.0078125f, 2.718282f, 0.999999f,
    };
    static const float clip_values[16] = {
        0.765432f, -0.222222f, 1.618034f, 0.015625f,
        -1.234568f, 0.444444f, -0.577215f, -0.046875f,
        0.101011f, 1.732051f, 0.693147f, 0.078125f,
        -3.141593f, 0.125001f, -0.30103f, 1.000001f,
    };
    static const float positions[9] = {
        12345.678f, -0.00012345f, 3.141592f,
        -987.6543f, 0.33333334f, -2.7182817f,
        0.0009765625f, -8191.75f, 1.4142135f,
    };
    const soc_mat4 object = transform_matrix_from_values(object_values);
    const soc_mat4 clip = transform_matrix_from_values(clip_values);
    soc_kernel_mat4_f32 object_f32;
    soc_kernel_mat4_f32 clip_f32;
    soc_kernel_mat4_f32 clip_from_object_f32;
    soc_kernel_mat4_f32 expected_matrix;
    soc_kernel_clip_vertex expected[3];
    soc_kernel_clip_vertex scalar_output[3];
    soc_kernel_clip_vertex neon_output[3];
    soc_kernel_clip_metadata scalar_metadata;
    soc_kernel_clip_metadata neon_metadata;
    size_t index;

    soc_kernel_mat4_f32_from_f32(&object, &object_f32);
    soc_kernel_mat4_f32_from_f32(&clip, &clip_f32);
    soc_kernel_mat4_f32_multiply(
        &clip_f32,
        &object_f32,
        &clip_from_object_f32
    );
    expected_matrix = transform_reference_matrix_multiply(
        &clip_f32,
        &object_f32
    );
    if (!transform_matrices_are_close(
            &expected_matrix,
            &clip_from_object_f32
        )) {
        fprintf(stderr, "transform matrix composition mismatch\n");
        return 1;
    }
    for (index = 0u; index < 3u; ++index) {
        const soc_kernel_clip_vertex object_position = {
            positions[index * 3u],
            positions[index * 3u + 1u],
            positions[index * 3u + 2u],
            1.0f,
        };
        expected[index] = transform_reference_vertex(
            &expected_matrix,
            &object_position
        );
    }

    soc_kernel_transform_triangle_f32_scalar(
        &clip_from_object_f32,
        positions,
        positions + 3u,
        positions + 6u,
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        scalar_output,
        &scalar_metadata
    );
    soc_kernel_transform_triangle_f32_neon(
        &clip_from_object_f32,
        positions,
        positions + 3u,
        positions + 6u,
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        neon_output,
        &neon_metadata
    );
    if (!transform_vertices_are_close(expected, scalar_output) ||
        !transform_vertices_are_close(expected, neon_output) ||
        !transform_metadata_is_equal(&scalar_metadata, &neon_metadata)) {
        fprintf(stderr, "transform composed reference mismatch\n");
        return 1;
    }
    return 0;
}

static int test_transform_differential(void)
{
    static const float identity_values[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    static const float object_values[16] = {
        1.125f, -0.375f, 0.25f, 0.0625f,
        0.1875f, 0.9375f, -0.3125f, 0.125f,
        -0.4375f, 0.21875f, 1.0625f, -0.09375f,
        2.25f, -1.5f, 0.625f, 1.0f,
    };
    static const float clip_values[16] = {
        0.8125f, 0.15625f, -0.28125f, 0.09375f,
        -0.125f, 1.1875f, 0.34375f, -0.0625f,
        0.40625f, -0.234375f, 0.875f, 0.15625f,
        -0.75f, 0.5f, 0.1875f, 1.125f,
    };
    static const uint32_t boundary_positions[9] = {
        UINT32_C(0xbf800000), UINT32_C(0x00000000), UINT32_C(0x00000000),
        UINT32_C(0x3f800000), UINT32_C(0x80000000), UINT32_C(0x3f800000),
        UINT32_C(0x00000000), UINT32_C(0x3f800000), UINT32_C(0xbf800000),
    };
    static const uint32_t special_bits[] = {
        UINT32_C(0x00000000), UINT32_C(0x80000000),
        UINT32_C(0x3a800000), UINT32_C(0xba800000),
        UINT32_C(0x3e800000), UINT32_C(0xbe800000),
        UINT32_C(0x3f800000), UINT32_C(0xbf800000),
        UINT32_C(0x41800000), UINT32_C(0xc1800000),
    };
    soc_mat4 identity = transform_matrix_from_values(identity_values);
    soc_mat4 object = transform_matrix_from_values(object_values);
    soc_mat4 clip = transform_matrix_from_values(clip_values);
    uint32_t positions[9];
    uint32_t state = UINT32_C(0x5452414e);
    size_t offset;
    size_t index;
    size_t iteration;

    if (test_transform_reference() != 0) {
        return 1;
    }

    for (offset = 0u; offset < 4u; ++offset) {
        const soc_kernel_clip_metadata boundary_metadata = {
            .active_planes = (offset & 1u) == 0u ? UINT8_C(0x10) : 0u,
            .common_planes = 0u,
        };

        if (run_transform_case(
                &identity,
                &identity,
                boundary_positions,
                offset,
                (offset & 1u) == 0u
                    ? SOC_CLIP_DEPTH_ZERO_TO_ONE
                    : SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
                &boundary_metadata,
                "clip-boundaries"
            ) != 0 ||
            run_transform_case(
                &object,
                &clip,
                boundary_positions,
                offset,
                (offset & 1u) == 0u
                    ? SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
                    : SOC_CLIP_DEPTH_ZERO_TO_ONE,
                NULL,
                "dense-matrices"
            ) != 0) {
            return 1;
        }
    }

    for (index = 0u; index < ARRAY_COUNT(positions); ++index) {
        positions[index] = special_bits[index % ARRAY_COUNT(special_bits)];
    }

    {
        static const uint32_t rejected_positions[9] = {
            UINT32_C(0x40000000), UINT32_C(0x00000000), UINT32_C(0x00000000),
            UINT32_C(0x40400000), UINT32_C(0x3f000000), UINT32_C(0x3f000000),
            UINT32_C(0x40800000), UINT32_C(0xbf000000), UINT32_C(0x3f800000),
        };
        const soc_kernel_clip_metadata rejected_metadata = {
            .active_planes = UINT8_C(0x02),
            .common_planes = UINT8_C(0x02),
        };

        if (run_transform_case(
                &identity,
                &identity,
                rejected_positions,
                0u,
                SOC_CLIP_DEPTH_ZERO_TO_ONE,
                &rejected_metadata,
                "trivial-reject"
            ) != 0) {
            return 1;
        }
    }
    for (offset = 0u; offset < 4u; ++offset) {
        if (run_transform_case(
                &object,
                &clip,
                positions,
                offset,
                (offset & 1u) == 0u
                    ? SOC_CLIP_DEPTH_ZERO_TO_ONE
                    : SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
                NULL,
                "special-positions"
            ) != 0) {
            return 1;
        }
    }

    for (iteration = 0u; iteration < 1024u; ++iteration) {
        float object_random[16];
        float clip_random[16];

        for (index = 0u; index < ARRAY_COUNT(object_random); ++index) {
            object_random[index] = random_transform_value(&state);
            clip_random[index] = random_transform_value(&state);
        }
        for (index = 0u; index < ARRAY_COUNT(positions); ++index) {
            positions[index] = float_bits(random_transform_value(&state));
        }
        object = transform_matrix_from_values(object_random);
        clip = transform_matrix_from_values(clip_random);
        if (run_transform_case(
                &object,
                &clip,
                positions,
                iteration & 3u,
                (iteration & 1u) == 0u
                    ? SOC_CLIP_DEPTH_ZERO_TO_ONE
                    : SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
                NULL,
                "random-finite"
            ) != 0) {
            return 1;
        }
    }

    for (iteration = 0u; iteration < 256u; ++iteration) {
        float object_special[16];
        float clip_special[16];

        for (index = 0u; index < ARRAY_COUNT(object_special); ++index) {
            object_special[index] = float_from_bits(
                special_bits[(iteration + index) %
                    ARRAY_COUNT(special_bits)]
            );
            clip_special[index] = float_from_bits(
                special_bits[(iteration * 3u + index + 5u) %
                    ARRAY_COUNT(special_bits)]
            );
        }
        for (index = 0u; index < ARRAY_COUNT(positions); ++index) {
            positions[index] = special_bits[
                (iteration * 5u + index + 2u) % ARRAY_COUNT(special_bits)
            ];
        }
        object = transform_matrix_from_values(object_special);
        clip = transform_matrix_from_values(clip_special);
        if (run_transform_case(
                &object,
                &clip,
                positions,
                iteration & 3u,
                (iteration & 1u) == 0u
                    ? SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
                    : SOC_CLIP_DEPTH_ZERO_TO_ONE,
                NULL,
                "finite-specials"
            ) != 0) {
            return 1;
        }
    }
    return 0;
}

static soc_frame_desc make_aabb_test_frame(
    soc_clip_depth_range clip_depth_range,
    soc_bool perspective
)
{
    soc_frame_desc frame = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
            .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
        },
        .clip_depth_range = clip_depth_range,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };

    if (perspective == SOC_TRUE) {
        frame.clip_from_world.col2.w = 1.0f;
        frame.clip_from_world.col3.w = 0.0f;
        if (clip_depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE) {
            frame.clip_from_world.col2.z = 0.0f;
            frame.clip_from_world.col3.z = 1.0f;
        } else {
            frame.clip_from_world.col2.z = -1.0f;
            frame.clip_from_world.col3.z = 2.0f;
        }
    }
    return frame;
}

static soc_aabb make_kernel_aabb(
    float minimum_x,
    float minimum_y,
    float minimum_z,
    float maximum_x,
    float maximum_y,
    float maximum_z
)
{
    const soc_aabb bounds = {
        .min = {minimum_x, minimum_y, minimum_z},
        .max = {maximum_x, maximum_y, maximum_z},
    };
    return bounds;
}

static int initialize_aabb_test_hiz(
    const soc_kernel_table* scalar,
    uint32_t width,
    uint32_t height,
    soc_hiz* out_hiz
)
{
    size_t index;

    if (soc_hiz_initialize(out_hiz, width, height) != SOC_RESULT_OK) {
        return 1;
    }
    for (index = 0u; index < out_hiz->levels[0].element_count; ++index) {
        const float steps[] = {0.20f, 0.40f, 0.60f, 0.80f};

        out_hiz->data[index] =
            1.0f - steps[(index * 5u + 3u) & 3u];
    }
    if (soc_hiz_build_with_kernels(out_hiz, scalar) != SOC_RESULT_OK) {
        soc_hiz_shutdown(out_hiz);
        return 1;
    }
    return 0;
}

static float random_unit_f32(uint32_t* state)
{
    return (float)(random_u32(state) >> 8u) *
        (1.0f / 16777216.0f);
}

static void initialize_aabb_test_bounds(
    soc_aabb* bounds,
    uint32_t count,
    soc_clip_depth_range clip_depth_range,
    soc_bool perspective
)
{
    uint32_t state = UINT32_C(0x41414242) ^
        (uint32_t)clip_depth_range ^ (UINT32_C(1) << 8u) ^
        ((uint32_t)perspective << 16u);
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        const float center_x = (random_unit_f32(&state) * 2.0f - 1.0f) *
            0.70f;
        const float center_y = (random_unit_f32(&state) * 2.0f - 1.0f) *
            0.70f;
        const float radius = 0.002f + random_unit_f32(&state) * 0.08f;

        if (perspective == SOC_TRUE) {
            const float minimum_z = 1.25f +
                random_unit_f32(&state) * 3.50f;
            const float maximum_z = minimum_z + 0.05f + radius;

            bounds[index] = make_kernel_aabb(
                center_x * minimum_z - radius,
                center_y * minimum_z - radius,
                minimum_z,
                center_x * minimum_z + radius,
                center_y * minimum_z + radius,
                maximum_z
            );
        } else {
            const float normalized_depth = 0.10f +
                random_unit_f32(&state) * 0.80f;
            const float world_depth =
                clip_depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
                    ? normalized_depth
                    : normalized_depth * 2.0f - 1.0f;

            bounds[index] = make_kernel_aabb(
                center_x - radius,
                center_y - radius,
                world_depth,
                center_x + radius,
                center_y + radius,
                world_depth + 0.01f
            );
        }

        switch (index % 23u) {
        case 0u:
            bounds[index].min.x = bounds[index].max.x + 1.0f;
            break;
        case 1u:
            bounds[index].max.y = bounds[index].min.y - 1.0f;
            break;
        case 2u:
            bounds[index].min.z = bounds[index].max.z + 1.0f;
            break;
        case 3u:
            bounds[index] = perspective == SOC_TRUE
                ? make_kernel_aabb(2.0f, 0.0f, 2.0f, 2.0f, 0.0f, 2.0f)
                : make_kernel_aabb(1.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.5f);
            break;
        case 4u:
            bounds[index] = perspective == SOC_TRUE
                ? make_kernel_aabb(-0.1f, -0.1f, 0.8f, 0.1f, 0.1f, 1.2f)
                : make_kernel_aabb(-0.1f, -0.1f, -0.1f, 0.1f, 0.1f, 0.1f);
            break;
        case 5u:
            bounds[index] = perspective == SOC_TRUE
                ? make_kernel_aabb(-0.1f, -0.1f, 0.0f, 0.1f, 0.1f, 0.0f)
                : make_kernel_aabb(1.2f, -0.1f, 0.2f, 1.5f, 0.1f, 0.4f);
            break;
        case 6u:
            bounds[index] = perspective == SOC_TRUE
                ? make_kernel_aabb(8.0f, -0.1f, 2.0f, 9.0f, 0.1f, 2.5f)
                : make_kernel_aabb(-1.5f, -0.1f, 0.2f, -1.2f, 0.1f, 0.4f);
            break;
        case 7u:
            bounds[index] = make_kernel_aabb(
                -0.0f,
                0.0f,
                perspective == SOC_TRUE ? 2.0f :
                    0.75f,
                0.0f,
                -0.0f,
                perspective == SOC_TRUE ? 2.0f :
                    0.75f
            );
            break;
        default:
            break;
        }
    }
}

static int run_aabb_kernel_differential_case(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon,
    const soc_hiz* hiz,
    const soc_aabb_query_context* query,
    const soc_aabb* bounds,
    uint32_t count,
    const char* label
)
{
    const soc_visibility canary = UINT8_C(0xa5);
    soc_visibility* scalar_storage = malloc((size_t)count + 2u);
    soc_visibility* neon_storage = malloc((size_t)count + 2u);
    soc_aabb* input_copy = malloc((size_t)count * sizeof(*input_copy));
    soc_occlusion_query_counts scalar_counts = {17u, 19u, 23u};
    soc_occlusion_query_counts neon_counts = {29u, 31u, 37u};
    soc_result scalar_result;
    soc_result neon_result;
    uint64_t visible = 0u;
    uint64_t occluded = 0u;
    uint64_t unknown = 0u;
    uint32_t index;

    if (scalar_storage == NULL || neon_storage == NULL ||
        (count != 0u && input_copy == NULL)) {
        free(input_copy);
        free(neon_storage);
        free(scalar_storage);
        return 1;
    }
    memset(scalar_storage, canary, (size_t)count + 2u);
    memset(neon_storage, canary, (size_t)count + 2u);
    if (count != 0u) {
        memcpy(input_copy, bounds, (size_t)count * sizeof(*input_copy));
    }

    scalar_result = scalar->test_aabbs(
        hiz,
        query,
        bounds,
        count,
        scalar_storage + 1u,
        &scalar_counts
    );
    neon_result = neon->test_aabbs(
        hiz,
        query,
        bounds,
        count,
        neon_storage + 1u,
        &neon_counts
    );
    if (scalar_result != neon_result || scalar_result != SOC_RESULT_OK ||
        memcmp(scalar_storage, neon_storage, (size_t)count + 2u) != 0 ||
        memcmp(&scalar_counts, &neon_counts, sizeof(scalar_counts)) != 0 ||
        (count != 0u && memcmp(
            input_copy,
            bounds,
            (size_t)count * sizeof(*input_copy)
        ) != 0) ||
        scalar_storage[0] != canary ||
        scalar_storage[count + 1u] != canary ||
        neon_storage[0] != canary ||
        neon_storage[count + 1u] != canary) {
        fprintf(stderr, "AABB kernel differential mismatch: %s count=%u\n",
            label, (unsigned)count);
        free(input_copy);
        free(neon_storage);
        free(scalar_storage);
        return 1;
    }

    for (index = 0u; index < count; ++index) {
        if (scalar_storage[index + 1u] == SOC_VISIBILITY_UNKNOWN) {
            ++unknown;
        } else if (scalar_storage[index + 1u] == SOC_VISIBILITY_OCCLUDED) {
            ++occluded;
        } else if (scalar_storage[index + 1u] == SOC_VISIBILITY_VISIBLE) {
            ++visible;
        } else {
            fprintf(stderr, "invalid AABB visibility: %s index=%u\n",
                label, (unsigned)index);
            free(input_copy);
            free(neon_storage);
            free(scalar_storage);
            return 1;
        }
    }
    if (scalar_counts.visible != visible ||
        scalar_counts.occluded != occluded ||
        scalar_counts.unknown != unknown ||
        visible + occluded + unknown != count) {
        fprintf(stderr, "AABB count mismatch: %s count=%u\n",
            label, (unsigned)count);
        free(input_copy);
        free(neon_storage);
        free(scalar_storage);
        return 1;
    }

    free(input_copy);
    free(neon_storage);
    free(scalar_storage);
    return 0;
}

static int test_aabb_noncontiguous_hiz_sampling(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon,
    soc_clip_depth_range clip_depth_range
)
{
    const float normalized_depth = 0.20f;
    const float world_depth =
        clip_depth_range == SOC_CLIP_DEPTH_ZERO_TO_ONE
            ? normalized_depth
            : normalized_depth * 2.0f - 1.0f;
    const soc_frame_desc frame = make_aabb_test_frame(
        clip_depth_range,
        SOC_FALSE
    );
    const soc_aabb footprint = make_kernel_aabb(
        -0.45f,
        0.05f,
        world_depth,
        -0.05f,
        0.45f,
        world_depth
    );
    const soc_aabb control = make_kernel_aabb(
        -0.125f,
        0.125f,
        world_depth,
        -0.125f,
        0.125f,
        world_depth
    );
    const soc_aabb bounds[] = {footprint, control, control, footprint};
    const soc_visibility expected[] = {
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_VISIBLE,
    };
    soc_visibility scalar_visibility[ARRAY_COUNT(bounds)] = {0};
    soc_visibility neon_visibility[ARRAY_COUNT(bounds)] = {0};
    soc_occlusion_query_counts scalar_counts;
    soc_occlusion_query_counts neon_counts;
    soc_aabb_query_context query;
    soc_hiz hiz = {0};
    size_t index;

    soc_aabb_query_context_initialize(&frame, &query);
    if (soc_hiz_initialize(&hiz, 8u, 8u) != SOC_RESULT_OK) {
        return 1;
    }
    for (index = 0u; index < hiz.levels[0].element_count; ++index) {
        hiz.data[index] = 0.60f;
    }
    hiz.data[3u * 8u + 2u] = 0.0f;
    if (soc_hiz_build_with_kernels(&hiz, scalar) !=
        SOC_RESULT_OK ||
        scalar->test_aabbs(
            &hiz,
            &query,
            bounds,
            (uint32_t)ARRAY_COUNT(bounds),
            scalar_visibility,
            &scalar_counts
        ) != SOC_RESULT_OK ||
        neon->test_aabbs(
            &hiz,
            &query,
            bounds,
            (uint32_t)ARRAY_COUNT(bounds),
            neon_visibility,
            &neon_counts
        ) != SOC_RESULT_OK ||
        memcmp(scalar_visibility, expected, sizeof(expected)) != 0 ||
        memcmp(neon_visibility, expected, sizeof(expected)) != 0 ||
        memcmp(&scalar_counts, &neon_counts, sizeof(scalar_counts)) != 0) {
        fprintf(
            stderr,
            "noncontiguous Hi-Z AABB mismatch: range=%u "
            "scalar=%u,%u,%u,%u neon=%u,%u,%u,%u\n",
            (unsigned)clip_depth_range,
            (unsigned)scalar_visibility[0],
            (unsigned)scalar_visibility[1],
            (unsigned)scalar_visibility[2],
            (unsigned)scalar_visibility[3],
            (unsigned)neon_visibility[0],
            (unsigned)neon_visibility[1],
            (unsigned)neon_visibility[2],
            (unsigned)neon_visibility[3]
        );
        soc_hiz_shutdown(&hiz);
        return 1;
    }
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_aabb_tiny_positive_w_fail_open(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    soc_frame_desc frame = make_aabb_test_frame(
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_FALSE
    );
    const soc_aabb point = make_kernel_aabb(
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
    );
    soc_visibility scalar_visibility = SOC_VISIBILITY_OCCLUDED;
    soc_visibility neon_visibility = SOC_VISIBILITY_OCCLUDED;
    soc_occlusion_query_counts scalar_counts;
    soc_occlusion_query_counts neon_counts;
    soc_aabb_query_context query;
    soc_hiz hiz = {0};
    size_t index;

    frame.clip_from_world.col2.z = 0.0f;
    frame.clip_from_world.col3.z = 5.0e-16f;
    frame.clip_from_world.col2.w = 0.0f;
    frame.clip_from_world.col3.w = 1.0e-15f;
    soc_aabb_query_context_initialize(&frame, &query);
    if (soc_hiz_initialize(&hiz, 8u, 8u) != SOC_RESULT_OK) {
        return 1;
    }
    for (index = 0u; index < hiz.element_count; ++index) {
        hiz.data[index] = 0.40f;
    }
    if (scalar->test_aabbs(
            &hiz,
            &query,
            &point,
            1u,
            &scalar_visibility,
            &scalar_counts
        ) != SOC_RESULT_OK ||
        neon->test_aabbs(
            &hiz,
            &query,
            &point,
            1u,
            &neon_visibility,
            &neon_counts
        ) != SOC_RESULT_OK ||
        scalar_visibility != SOC_VISIBILITY_VISIBLE ||
        neon_visibility != SOC_VISIBILITY_VISIBLE ||
        memcmp(&scalar_counts, &neon_counts, sizeof(scalar_counts)) != 0) {
        fprintf(stderr, "tiny-positive-W AABB was not fail-open\n");
        soc_hiz_shutdown(&hiz);
        return 1;
    }
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_aabb_fallback_lane_order(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    const soc_frame_desc frame = make_aabb_test_frame(
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_FALSE
    );
    const soc_aabb invalid = make_kernel_aabb(
        0.1f, -0.1f, 0.2f, -0.1f, 0.1f, 0.3f
    );
    const soc_aabb fast_visible = make_kernel_aabb(
        -0.1f, -0.1f, 0.8f, 0.1f, 0.1f, 0.9f
    );
    const soc_aabb fast_occluded = make_kernel_aabb(
        -0.1f, -0.1f, 0.2f, 0.1f, 0.1f, 0.3f
    );
    const soc_aabb boundary = make_kernel_aabb(
        1.0f, 0.0f, 0.8f, 1.0f, 0.0f, 0.8f
    );
    const soc_aabb bounds[] = {
        invalid,
        fast_visible,
        fast_occluded,
        invalid,
        boundary,
        fast_visible,
        fast_occluded,
        boundary,
    };
    const soc_visibility expected[] = {
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_UNKNOWN,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_VISIBLE,
        SOC_VISIBILITY_OCCLUDED,
        SOC_VISIBILITY_VISIBLE,
    };
    soc_visibility scalar_visibility[ARRAY_COUNT(bounds)] = {0};
    soc_visibility neon_visibility[ARRAY_COUNT(bounds)] = {0};
    soc_occlusion_query_counts scalar_counts;
    soc_occlusion_query_counts neon_counts;
    soc_aabb_query_context query;
    soc_hiz hiz = {0};
    size_t index;

    soc_aabb_query_context_initialize(&frame, &query);
    if (soc_hiz_initialize(&hiz, 8u, 8u) != SOC_RESULT_OK) {
        return 1;
    }
    for (index = 0u; index < hiz.levels[0].element_count; ++index) {
        hiz.data[index] = 0.5f;
    }
    if (soc_hiz_build_with_kernels(&hiz, scalar) !=
        SOC_RESULT_OK ||
        scalar->test_aabbs(
            &hiz,
            &query,
            bounds,
            (uint32_t)ARRAY_COUNT(bounds),
            scalar_visibility,
            &scalar_counts
        ) != SOC_RESULT_OK ||
        neon->test_aabbs(
            &hiz,
            &query,
            bounds,
            (uint32_t)ARRAY_COUNT(bounds),
            neon_visibility,
            &neon_counts
        ) != SOC_RESULT_OK ||
        memcmp(scalar_visibility, expected, sizeof(expected)) != 0 ||
        memcmp(neon_visibility, expected, sizeof(expected)) != 0 ||
        memcmp(&scalar_counts, &neon_counts, sizeof(scalar_counts)) != 0 ||
        scalar_counts.visible != 4u || scalar_counts.occluded != 2u ||
        scalar_counts.unknown != 2u) {
        fprintf(stderr, "AABB fallback lane-order mismatch\n");
        soc_hiz_shutdown(&hiz);
        return 1;
    }
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_aabb_overlap(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    typedef union aabb_overlap_storage {
        max_align_t alignment;
        soc_aabb bounds[4];
    } aabb_overlap_storage;
    soc_frame_desc frame = make_aabb_test_frame(
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_FALSE
    );
    const soc_aabb source[] = {
        {{-0.4f, -0.4f, 0.8f}, {-0.3f, -0.3f, 0.9f}},
        {{-0.1f, -0.1f, 0.2f}, {0.1f, 0.1f, 0.3f}},
        {{0.3f, 0.3f, 0.8f}, {0.4f, 0.4f, 0.9f}},
        {{0.5f, 0.5f, 0.2f}, {0.6f, 0.6f, 0.3f}},
    };
    aabb_overlap_storage scalar_storage;
    aabb_overlap_storage neon_storage;
    soc_occlusion_query_counts scalar_counts;
    soc_occlusion_query_counts neon_counts;
    soc_aabb_query_context query;
    soc_hiz hiz = {0};

    scalar_storage.bounds[0] = source[0];
    scalar_storage.bounds[1] = source[1];
    scalar_storage.bounds[2] = source[2];
    scalar_storage.bounds[3] = source[3];
    neon_storage = scalar_storage;
    soc_aabb_query_context_initialize(&frame, &query);
    if (initialize_aabb_test_hiz(
            scalar,
            8u,
            8u,
            &hiz
        ) != 0) {
        return 1;
    }
    if (scalar->test_aabbs(
            &hiz,
            &query,
            scalar_storage.bounds,
            3u,
            (soc_visibility*)&scalar_storage.bounds[1],
            &scalar_counts
        ) != SOC_RESULT_OK ||
        neon->test_aabbs(
            &hiz,
            &query,
            neon_storage.bounds,
            3u,
            (soc_visibility*)&neon_storage.bounds[1],
            &neon_counts
        ) != SOC_RESULT_OK ||
        memcmp(&scalar_storage, &neon_storage, sizeof(scalar_storage)) != 0 ||
        memcmp(&scalar_counts, &neon_counts, sizeof(scalar_counts)) != 0) {
        fprintf(stderr, "overlapping AABB input/output mismatch\n");
        soc_hiz_shutdown(&hiz);
        return 1;
    }

    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_aabb_query_differential(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    static const uint32_t counts[] = {1u, 2u, 3u, 7u, 257u};
    static const soc_clip_depth_range clip_depth_ranges[] = {
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE,
    };
    soc_aabb bounds_storage[259];
    size_t range_index;
    size_t perspective_index;

    for (range_index = 0u;
         range_index < ARRAY_COUNT(clip_depth_ranges);
         ++range_index) {
        for (perspective_index = 0u; perspective_index < 2u;
             ++perspective_index) {
                const soc_bool perspective = perspective_index != 0u
                    ? SOC_TRUE
                    : SOC_FALSE;
                const soc_frame_desc frame = make_aabb_test_frame(
                    clip_depth_ranges[range_index],
                    perspective
                );
                soc_aabb_query_context query;
                soc_hiz hiz = {0};
                size_t count_index;

                soc_aabb_query_context_initialize(&frame, &query);
                initialize_aabb_test_bounds(
                    bounds_storage,
                    (uint32_t)ARRAY_COUNT(bounds_storage),
                    clip_depth_ranges[range_index],
                    perspective
                );
                if (initialize_aabb_test_hiz(
                        scalar,
                        31u,
                        19u,
                        &hiz
                    ) != 0) {
                    return 1;
                }
                for (count_index = 0u;
                     count_index < ARRAY_COUNT(counts);
                     ++count_index) {
                    const uint32_t offset = (uint32_t)(count_index & 1u);

                    if (run_aabb_kernel_differential_case(
                            scalar,
                            neon,
                            &hiz,
                            &query,
                            bounds_storage + offset,
                            counts[count_index],
                            perspective == SOC_TRUE
                                ? "perspective"
                                : "orthographic"
                        ) != 0) {
                        soc_hiz_shutdown(&hiz);
                        return 1;
                    }
                    if ((counts[count_index] & 1u) != 0u) {
                        const size_t exact_offset = _Alignof(soc_aabb);
                        unsigned char* exact_storage = malloc(
                            (size_t)counts[count_index] *
                                sizeof(soc_aabb) + exact_offset
                        );
                        soc_aabb* exact_bounds;

                        if (exact_storage == NULL) {
                            soc_hiz_shutdown(&hiz);
                            return 1;
                        }
                        exact_bounds = (soc_aabb*)(void*)(
                            exact_storage + exact_offset
                        );
                        memcpy(
                            exact_bounds,
                            bounds_storage + offset,
                            (size_t)counts[count_index] *
                                sizeof(*exact_bounds)
                        );
                        if (run_aabb_kernel_differential_case(
                                scalar,
                                neon,
                                &hiz,
                                &query,
                                exact_bounds,
                                counts[count_index],
                                "exact-odd-tail"
                            ) != 0) {
                            free(exact_storage);
                            soc_hiz_shutdown(&hiz);
                            return 1;
                        }
                        free(exact_storage);
                    }
                }
                soc_hiz_shutdown(&hiz);
        }
        {
                soc_frame_desc dense_frame = make_aabb_test_frame(
                    clip_depth_ranges[range_index],
                    SOC_FALSE
                );
                soc_aabb_query_context dense_query;
                soc_hiz dense_hiz = {0};

                dense_frame.clip_from_world.col0 =
                    (soc_vector4){0.713579f, -0.113271f, 0.047119f, 0.03125f};
                dense_frame.clip_from_world.col1 =
                    (soc_vector4){-0.217381f, 0.821357f, -0.029731f, -0.015625f};
                dense_frame.clip_from_world.col2 =
                    (soc_vector4){0.093173f, 0.151937f, 0.310913f, 0.0625f};
                dense_frame.clip_from_world.col3 = (soc_vector4){
                    0.125731f,
                    -0.200173f,
                    clip_depth_ranges[range_index] ==
                        SOC_CLIP_DEPTH_ZERO_TO_ONE ? 0.8f : 0.0f,
                    2.0f,
                };
                soc_aabb_query_context_initialize(
                    &dense_frame,
                    &dense_query
                );
                initialize_aabb_test_bounds(
                    bounds_storage,
                    257u,
                    clip_depth_ranges[range_index],
                    SOC_FALSE
                );
                if (initialize_aabb_test_hiz(
                        scalar,
                        31u,
                        19u,
                        &dense_hiz
                    ) != 0 ||
                    run_aabb_kernel_differential_case(
                        scalar,
                        neon,
                        &dense_hiz,
                        &dense_query,
                        bounds_storage + 1u,
                        257u,
                        "dense-fma-query"
                    ) != 0) {
                    soc_hiz_shutdown(&dense_hiz);
                    return 1;
                }
                soc_hiz_shutdown(&dense_hiz);
            }
            if (test_aabb_noncontiguous_hiz_sampling(
                    scalar,
                    neon,
                    clip_depth_ranges[range_index]
                ) != 0) {
                return 1;
            }
    }

    {
        const soc_frame_desc frame = make_aabb_test_frame(
            SOC_CLIP_DEPTH_ZERO_TO_ONE,
            SOC_FALSE
        );
        soc_aabb_query_context query;
        soc_hiz hiz = {0};
        soc_occlusion_query_counts scalar_counts = {1u, 2u, 3u};
        soc_occlusion_query_counts neon_counts = {4u, 5u, 6u};

        soc_aabb_query_context_initialize(&frame, &query);
        if (initialize_aabb_test_hiz(
                scalar,
                3u,
                3u,
                &hiz
            ) != 0) {
            return 1;
        }
        if (scalar->test_aabbs(
                &hiz, &query, NULL, 0u, NULL, &scalar_counts
            ) != SOC_RESULT_OK ||
            neon->test_aabbs(
                &hiz, &query, NULL, 0u, NULL, &neon_counts
            ) != SOC_RESULT_OK ||
            memcmp(&scalar_counts, &neon_counts, sizeof(scalar_counts)) != 0 ||
            scalar_counts.visible != 0u || scalar_counts.occluded != 0u ||
            scalar_counts.unknown != 0u) {
            fprintf(stderr, "zero-count AABB differential mismatch\n");
            soc_hiz_shutdown(&hiz);
            return 1;
        }
        soc_hiz_shutdown(&hiz);
    }
    if (test_aabb_tiny_positive_w_fail_open(scalar, neon) != 0) {
        return 1;
    }
    if (test_aabb_fallback_lane_order(scalar, neon) != 0) {
        return 1;
    }
    return test_aabb_overlap(scalar, neon);
}

#endif

int main(void)
{
    const soc_kernel_table* scalar = soc_kernel_table_scalar();
    const soc_kernel_table* neon = soc_kernel_table_neon();

    CHECK(scalar != NULL);
    CHECK(scalar->backend == SOC_KERNEL_BACKEND_SCALAR);
    CHECK(scalar->clear_f32 != NULL);
    CHECK(scalar->store_constant_depth_block_f32 != NULL);
    CHECK(scalar->store_depth_plane_block_f32 != NULL);
    CHECK(scalar->reduce_hiz_level_f32 != NULL);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_SCALAR) == scalar);

#if SOC_TEST_HAS_NEON_KERNELS
    CHECK(neon != NULL);
    CHECK(neon->backend == SOC_KERNEL_BACKEND_NEON);
    CHECK(neon->clear_f32 != NULL);
    CHECK(neon->store_constant_depth_block_f32 != NULL);
    CHECK(neon->store_depth_plane_block_f32 != NULL);
    CHECK(neon->reduce_hiz_level_f32 != NULL);
    CHECK(neon->test_aabbs != NULL);
    CHECK(neon->clear_f32 == scalar->clear_f32);
    CHECK(neon->store_constant_depth_block_f32 !=
        scalar->store_constant_depth_block_f32);
    CHECK(neon->store_depth_plane_block_f32 !=
        scalar->store_depth_plane_block_f32);
    CHECK(neon->reduce_hiz_level_f32 != scalar->reduce_hiz_level_f32);
    CHECK(neon->test_aabbs != scalar->test_aabbs);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_NEON) == neon);

    if (test_clear_differential(scalar, neon) != 0) {
        return 1;
    }
    if (test_raster_depth_block_differential(scalar, neon) != 0) {
        return 1;
    }
    if (test_raster_depth_plane_block_differential(scalar, neon) != 0) {
        return 1;
    }
    if (test_raster_depth_block_redzones(scalar, neon) != 0) {
        return 1;
    }
    if (test_hiz_differential(scalar, neon) != 0) {
        return 1;
    }
    if (test_hiz_redzones(scalar, neon) != 0) {
        return 1;
    }
    if (test_transform_differential() != 0) {
        return 1;
    }
    if (test_aabb_query_differential(scalar, neon) != 0) {
        return 1;
    }
#else
    {
        const soc_cpu_features synthetic_arm64_neon = {
            .architecture = SOC_CPU_ARCHITECTURE_ARM64,
            .flags = SOC_CPU_FEATURE_NEON,
        };

        CHECK(neon == NULL);
        CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_NEON) == NULL);
        CHECK(soc_kernel_table_select(&synthetic_arm64_neon) == scalar);
    }
#endif

    return 0;
}
