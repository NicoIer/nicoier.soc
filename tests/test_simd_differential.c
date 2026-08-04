#include "core/soc_kernels.h"

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

#if defined(__aarch64__) || defined(_M_ARM64)
#define SOC_TEST_AARCH64 1
#else
#define SOC_TEST_AARCH64 0
#endif

#if SOC_TEST_AARCH64

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

static uint32_t quiet_nan_bits(uint32_t bits)
{
    const uint32_t exponent = bits & UINT32_C(0x7f800000);
    const uint32_t significand = bits & UINT32_C(0x007fffff);

    if (exponent == UINT32_C(0x7f800000) && significand != 0u) {
        bits |= UINT32_C(0x00400000);
    }
    return bits;
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
        UINT32_C(0x7f800000),
        UINT32_C(0xff800000),
        UINT32_C(0x7fc12345),
        UINT32_C(0xffc54321),
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
                const uint32_t bits = quiet_nan_bits(
                    random_u32(&random_state)
                );

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
    uint32_t candidate_bits,
    soc_depth_direction depth_direction
)
{
    static const uint32_t stored_values[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x80000000),
        UINT32_C(0x3e800000),
        UINT32_C(0x3f000000),
        UINT32_C(0x3f800000),
        UINT32_C(0x7f800000),
        UINT32_C(0xff800000),
        UINT32_C(0x7fc12345),
        UINT32_C(0xffc54321),
        UINT32_C(0x00000001),
        UINT32_C(0x80000001),
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
                (depth_direction == SOC_DEPTH_REVERSED
                    ? candidate_depth > *stored
                    : candidate_depth < *stored)) {
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
        candidate_depth,
        depth_direction
    );
    neon->store_constant_depth_block_f32(
        neon_storage + begin,
        row_stride,
        width,
        height,
        coverage_mask,
        candidate_depth,
        depth_direction
    );

    for (index = 0u; index < ARRAY_COUNT(expected); ++index) {
        const uint32_t expected_bits = float_bits(expected[index]);
        const uint32_t scalar_bits = float_bits(scalar_storage[index]);
        const uint32_t neon_bits = float_bits(neon_storage[index]);

        if (scalar_bits != expected_bits || neon_bits != expected_bits) {
            fprintf(
                stderr,
                "raster depth mismatch: %ux%u stride=%zu offset=%zu "
                "mask=%016llx candidate=%08x direction=%u index=%zu "
                "expected=%08x scalar=%08x neon=%08x\n",
                (unsigned)width,
                (unsigned)height,
                row_stride,
                offset,
                (unsigned long long)coverage_mask,
                (unsigned)candidate_bits,
                (unsigned)depth_direction,
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
        UINT32_C(0x80000000),
        UINT32_C(0x3e800000),
        UINT32_C(0x3f000000),
        UINT32_C(0x3f800000),
        UINT32_C(0x7f800000),
        UINT32_C(0xff800000),
        UINT32_C(0x7fc13579),
        UINT32_C(0xffc2468a),
        UINT32_C(0x00000001),
    };
    static const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
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
                            size_t direction_index;

                            for (direction_index = 0u;
                                 direction_index <
                                    ARRAY_COUNT(depth_directions);
                                 ++direction_index) {
                                if (run_raster_depth_block_case(
                                        scalar,
                                        neon,
                                        width,
                                        height,
                                        row_strides[stride_index],
                                        offset,
                                        masks[mask_index],
                                        candidate_values[candidate_index],
                                        depth_directions[direction_index]
                                    ) != 0) {
                                    return 1;
                                }
                            }
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
                            ? UINT32_C(0x3f400000)
                            : UINT32_C(0x7fc12345)
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
                    0.25f,
                    SOC_DEPTH_FORWARD
                );
                neon->store_constant_depth_block_f32(
                    neon_storage + offset,
                    width,
                    width,
                    height,
                    UINT64_MAX,
                    0.25f,
                    SOC_DEPTH_FORWARD
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
    soc_depth_direction depth_direction,
    size_t source_offset,
    size_t destination_offset,
    uint32_t pattern
)
{
    static const uint32_t special_values[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x80000000),
        UINT32_C(0x7f800000),
        UINT32_C(0xff800000),
        UINT32_C(0x7fc12345),
        UINT32_C(0x7fc54321),
        UINT32_C(0xffc23456),
        UINT32_C(0x3f000000),
        UINT32_C(0xbf000000),
        UINT32_C(0x3f800000),
        UINT32_C(0xbf800000),
        UINT32_C(0x00000001),
        UINT32_C(0x80000001),
    };
    uint32_t random_state = UINT32_C(0xa341316c) ^
        width * UINT32_C(0x9e3779b9) ^
        height * UINT32_C(0x85ebca6b) ^
        depth_direction * UINT32_C(0xc2b2ae35) ^
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
            output[index] = quiet_nan_bits(random_u32(&random_state));
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
    soc_depth_direction depth_direction,
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
        depth_direction,
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
        scalar_destination + destination_begin,
        depth_direction
    );
    neon->reduce_hiz_level_f32(
        neon_source + source_begin,
        width,
        height,
        neon_destination + destination_begin,
        depth_direction
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
                    "Hi-Z mismatch: %ux%u direction=%u src_offset=%zu "
                    "dst_offset=%zu pattern=%u index=%zu "
                    "scalar=0x%08x neon=0x%08x\n",
                    (unsigned)width,
                    (unsigned)height,
                    (unsigned)depth_direction,
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
        return 1;
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
    soc_depth_direction depth_direction,
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
        depth_direction,
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
        scalar_destination + offset,
        depth_direction
    );
    neon->reduce_hiz_level_f32(
        neon_source + offset,
        width,
        height,
        neon_destination + offset,
        depth_direction
    );

    if (memcmp(
            scalar_destination,
            neon_destination,
            destination_storage_count * sizeof(*scalar_destination)
        ) != 0 ||
        memcmp(
            scalar_source,
            neon_source,
            source_storage_count * sizeof(*scalar_source)
        ) != 0) {
        fprintf(
            stderr,
            "Hi-Z redzone mismatch: %ux%u direction=%u offset=%zu "
            "pattern=%u\n",
            (unsigned)width,
            (unsigned)height,
            (unsigned)depth_direction,
            offset,
            (unsigned)pattern
        );
        goto cleanup;
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
    static const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
    };
    size_t width_index;

    for (width_index = 0u;
         width_index < ARRAY_COUNT(widths);
         ++width_index) {
        size_t height_index;

        for (height_index = 0u;
             height_index < ARRAY_COUNT(heights);
             ++height_index) {
            size_t direction_index;

            for (direction_index = 0u;
                 direction_index < ARRAY_COUNT(depth_directions);
                 ++direction_index) {
                size_t offset;

                for (offset = 0u; offset < 4u; ++offset) {
                    uint32_t pattern;

                    for (pattern = 0u; pattern < 2u; ++pattern) {
                        if (run_hiz_redzone_case(
                                scalar,
                                neon,
                                widths[width_index],
                                heights[height_index],
                                depth_directions[direction_index],
                                offset,
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

static int test_hiz_differential(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
{
    static const soc_depth_direction depth_directions[] = {
        SOC_DEPTH_FORWARD,
        SOC_DEPTH_REVERSED,
    };
    uint32_t width;

    for (width = 1u; width <= HIZ_MAX_WIDTH; ++width) {
        uint32_t height;

        for (height = 1u; height <= HIZ_MAX_HEIGHT; ++height) {
            size_t direction_index;

            for (direction_index = 0u;
                 direction_index < ARRAY_COUNT(depth_directions);
                 ++direction_index) {
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
                                    depth_directions[direction_index],
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
    }
    return 0;
}

typedef struct transform_output_storage {
    uint64_t before[TRANSFORM_OUTPUT_GUARD_COUNT];
    soc_kernel_clip_vertex vertices[3];
    uint64_t after[TRANSFORM_OUTPUT_GUARD_COUNT];
} transform_output_storage;

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

static soc_bool transform_matrix_is_finite(const soc_mat4* matrix)
{
    const float components[16] = {
        matrix->col0.x, matrix->col0.y, matrix->col0.z, matrix->col0.w,
        matrix->col1.x, matrix->col1.y, matrix->col1.z, matrix->col1.w,
        matrix->col2.x, matrix->col2.y, matrix->col2.z, matrix->col2.w,
        matrix->col3.x, matrix->col3.y, matrix->col3.z, matrix->col3.w,
    };
    size_t index;

    for (index = 0u; index < ARRAY_COUNT(components); ++index) {
        if ((float_bits(components[index]) & UINT32_C(0x7f800000)) ==
            UINT32_C(0x7f800000)) {
            return SOC_FALSE;
        }
    }
    return SOC_TRUE;
}

static int run_transform_case(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon,
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
    soc_kernel_mat4_f64 object_f64;
    soc_kernel_mat4_f64 clip_f64;
    soc_kernel_mat4_f64 object_before;
    soc_kernel_mat4_f64 clip_before;
    transform_output_storage scalar_output;
    transform_output_storage neon_output;
    soc_kernel_clip_metadata scalar_metadata = {0xa5u, 0xa5u, 0xa5u};
    soc_kernel_clip_metadata neon_metadata = {0x5au, 0x5au, 0x5au};
    size_t vertex;
    size_t index;
    soc_bool positions_all_finite = SOC_TRUE;

    fill_with_bits(
        position_storage,
        ARRAY_COUNT(position_storage),
        CANARY_BITS
    );
    for (vertex = 0u; vertex < 3u; ++vertex) {
        for (index = 0u; index < 3u; ++index) {
            position_storage[position_starts[vertex] + offset + index] =
                float_from_bits(position_bits[vertex * 3u + index]);
            if ((position_bits[vertex * 3u + index] &
                    UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) {
                positions_all_finite = SOC_FALSE;
            }
        }
    }
    memcpy(position_before, position_storage, sizeof(position_storage));
    soc_kernel_mat4_f64_from_f32(object_to_world, &object_f64);
    soc_kernel_mat4_f64_from_f32(clip_from_world, &clip_f64);
    if (object_f64.all_finite !=
            (transform_matrix_is_finite(object_to_world) == SOC_TRUE
                ? UINT64_C(1)
                : UINT64_C(0)) ||
        clip_f64.all_finite !=
            (transform_matrix_is_finite(clip_from_world) == SOC_TRUE
                ? UINT64_C(1)
                : UINT64_C(0))) {
        fprintf(stderr, "transform matrix finite cache mismatch: %s\n", label);
        return 1;
    }
    object_before = object_f64;
    clip_before = clip_f64;
    for (index = 0u; index < TRANSFORM_OUTPUT_GUARD_COUNT; ++index) {
        scalar_output.before[index] = guard_value;
        scalar_output.after[index] = guard_value;
        neon_output.before[index] = guard_value;
        neon_output.after[index] = guard_value;
    }
    memset(scalar_output.vertices, 0xa5, sizeof(scalar_output.vertices));
    memset(neon_output.vertices, 0x5a, sizeof(neon_output.vertices));

    scalar->transform_triangle_f64(
        &object_f64,
        &clip_f64,
        position_storage + position_starts[0] + offset,
        position_storage + position_starts[1] + offset,
        position_storage + position_starts[2] + offset,
        positions_all_finite,
        depth_range,
        scalar_output.vertices,
        &scalar_metadata
    );
    if (memcmp(
            position_storage,
            position_before,
            sizeof(position_storage)
        ) != 0 ||
        memcmp(&object_f64, &object_before, sizeof(object_f64)) != 0 ||
        memcmp(&clip_f64, &clip_before, sizeof(clip_f64)) != 0) {
        fprintf(stderr, "transform Scalar modified input: %s\n", label);
        return 1;
    }
    if (expected_metadata != NULL && memcmp(
            &scalar_metadata,
            expected_metadata,
            sizeof(scalar_metadata)
        ) != 0) {
        fprintf(
            stderr,
            "transform metadata mismatch: %s active=%u common=%u finite=%u\n",
            label,
            (unsigned)scalar_metadata.active_planes,
            (unsigned)scalar_metadata.common_planes,
            (unsigned)scalar_metadata.all_finite
        );
        return 1;
    }

    neon->transform_triangle_f64(
        &object_f64,
        &clip_f64,
        position_storage + position_starts[0] + offset,
        position_storage + position_starts[1] + offset,
        position_storage + position_starts[2] + offset,
        positions_all_finite,
        depth_range,
        neon_output.vertices,
        &neon_metadata
    );
    if (memcmp(
            position_storage,
            position_before,
            sizeof(position_storage)
        ) != 0 ||
        memcmp(&object_f64, &object_before, sizeof(object_f64)) != 0 ||
        memcmp(&clip_f64, &clip_before, sizeof(clip_f64)) != 0) {
        fprintf(stderr, "transform NEON modified input: %s\n", label);
        return 1;
    }
    if (memcmp(
            scalar_output.vertices,
            neon_output.vertices,
            sizeof(scalar_output.vertices)
        ) != 0 ||
        memcmp(
            &scalar_metadata,
            &neon_metadata,
            sizeof(scalar_metadata)
        ) != 0) {
        fprintf(
            stderr,
            "transform mismatch: %s offset=%zu\n",
            label,
            offset
        );
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const double scalar_components[4] = {
                scalar_output.vertices[vertex].x,
                scalar_output.vertices[vertex].y,
                scalar_output.vertices[vertex].z,
                scalar_output.vertices[vertex].w,
            };
            const double neon_components[4] = {
                neon_output.vertices[vertex].x,
                neon_output.vertices[vertex].y,
                neon_output.vertices[vertex].z,
                neon_output.vertices[vertex].w,
            };

            for (index = 0u; index < 4u; ++index) {
                uint64_t scalar_bits;
                uint64_t neon_bits;

                memcpy(
                    &scalar_bits,
                    &scalar_components[index],
                    sizeof(scalar_bits)
                );
                memcpy(
                    &neon_bits,
                    &neon_components[index],
                    sizeof(neon_bits)
                );
                if (scalar_bits != neon_bits) {
                    fprintf(
                        stderr,
                        "  vertex=%zu component=%zu scalar=%016llx "
                        "neon=%016llx\n",
                        vertex,
                        index,
                        (unsigned long long)scalar_bits,
                        (unsigned long long)neon_bits
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

static double transform_reference_multiply(double left, double right)
{
    volatile double result = left * right;

    return result;
}

static double transform_reference_add(double left, double right)
{
    volatile double result = left + right;

    return result;
}

static double transform_reference_component(
    double column0,
    double column1,
    double column2,
    double column3,
    const soc_kernel_clip_vertex* vertex
)
{
    double result = transform_reference_multiply(column0, vertex->x);

    result = transform_reference_add(
        result,
        transform_reference_multiply(column1, vertex->y)
    );
    result = transform_reference_add(
        result,
        transform_reference_multiply(column2, vertex->z)
    );
    result = transform_reference_add(
        result,
        transform_reference_multiply(column3, vertex->w)
    );
    return result;
}

static soc_kernel_clip_vertex transform_reference_vertex(
    const soc_mat4* matrix,
    const soc_kernel_clip_vertex* vertex
)
{
    const soc_kernel_clip_vertex result = {
        transform_reference_component(
            matrix->col0.x,
            matrix->col1.x,
            matrix->col2.x,
            matrix->col3.x,
            vertex
        ),
        transform_reference_component(
            matrix->col0.y,
            matrix->col1.y,
            matrix->col2.y,
            matrix->col3.y,
            vertex
        ),
        transform_reference_component(
            matrix->col0.z,
            matrix->col1.z,
            matrix->col2.z,
            matrix->col3.z,
            vertex
        ),
        transform_reference_component(
            matrix->col0.w,
            matrix->col1.w,
            matrix->col2.w,
            matrix->col3.w,
            vertex
        ),
    };
    return result;
}

static int test_transform_reference(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
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
    soc_kernel_mat4_f64 object_f64;
    soc_kernel_mat4_f64 clip_f64;
    soc_kernel_clip_vertex expected[3];
    soc_kernel_clip_vertex scalar_output[3];
    soc_kernel_clip_vertex neon_output[3];
    soc_kernel_clip_metadata scalar_metadata;
    soc_kernel_clip_metadata neon_metadata;
    size_t index;

    soc_kernel_mat4_f64_from_f32(&object, &object_f64);
    soc_kernel_mat4_f64_from_f32(&clip, &clip_f64);
    for (index = 0u; index < 3u; ++index) {
        const soc_kernel_clip_vertex object_position = {
            positions[index * 3u],
            positions[index * 3u + 1u],
            positions[index * 3u + 2u],
            1.0,
        };
        const soc_kernel_clip_vertex world = transform_reference_vertex(
            &object,
            &object_position
        );

        expected[index] = transform_reference_vertex(&clip, &world);
    }

    scalar->transform_triangle_f64(
        &object_f64,
        &clip_f64,
        positions,
        positions + 3u,
        positions + 6u,
        SOC_TRUE,
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        scalar_output,
        &scalar_metadata
    );
    neon->transform_triangle_f64(
        &object_f64,
        &clip_f64,
        positions,
        positions + 3u,
        positions + 6u,
        SOC_TRUE,
        SOC_CLIP_DEPTH_ZERO_TO_ONE,
        neon_output,
        &neon_metadata
    );
    if (memcmp(expected, scalar_output, sizeof(expected)) != 0 ||
        memcmp(expected, neon_output, sizeof(expected)) != 0) {
        fprintf(stderr, "transform two-stage reference mismatch\n");
        return 1;
    }
    return 0;
}

static int test_transform_differential(
    const soc_kernel_table* scalar,
    const soc_kernel_table* neon
)
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
        UINT32_C(0x00000001), UINT32_C(0x80000001),
        UINT32_C(0x007fffff), UINT32_C(0x807fffff),
        UINT32_C(0x3f800000), UINT32_C(0xbf800000),
        UINT32_C(0x7f7fffff), UINT32_C(0xff7fffff),
        UINT32_C(0x7f800000), UINT32_C(0xff800000),
        UINT32_C(0x7fc12345), UINT32_C(0xffc54321),
    };
    soc_mat4 identity = transform_matrix_from_values(identity_values);
    soc_mat4 object = transform_matrix_from_values(object_values);
    soc_mat4 clip = transform_matrix_from_values(clip_values);
    uint32_t positions[9];
    uint32_t state = UINT32_C(0x5452414e);
    size_t offset;
    size_t index;
    size_t iteration;

    if (test_transform_reference(scalar, neon) != 0) {
        return 1;
    }

    for (offset = 0u; offset < 4u; ++offset) {
        const soc_kernel_clip_metadata boundary_metadata = {
            .active_planes = (offset & 1u) == 0u ? UINT8_C(0x10) : 0u,
            .common_planes = 0u,
            .all_finite = SOC_TRUE,
        };

        if (run_transform_case(
                scalar,
                neon,
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
                scalar,
                neon,
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
        float nonfinite_matrix_values[16];
        static const uint32_t rejected_positions[9] = {
            UINT32_C(0x40000000), UINT32_C(0x00000000), UINT32_C(0x00000000),
            UINT32_C(0x40400000), UINT32_C(0x3f000000), UINT32_C(0x3f000000),
            UINT32_C(0x40800000), UINT32_C(0xbf000000), UINT32_C(0x3f800000),
        };
        static const uint32_t nonfinite_positions[9] = {
            UINT32_C(0x7fc12345), UINT32_C(0x00000000), UINT32_C(0x00000000),
            UINT32_C(0x00000000), UINT32_C(0x00000000), UINT32_C(0x00000000),
            UINT32_C(0x3f800000), UINT32_C(0x3f800000), UINT32_C(0x3f800000),
        };
        const soc_kernel_clip_metadata rejected_metadata = {
            .active_planes = UINT8_C(0x02),
            .common_planes = UINT8_C(0x02),
            .all_finite = SOC_TRUE,
        };
        const soc_kernel_clip_metadata nonfinite_metadata = {
            .active_planes = 0u,
            .common_planes = 0u,
            .all_finite = SOC_FALSE,
        };
        soc_mat4 nonfinite_matrix;

        memcpy(
            nonfinite_matrix_values,
            identity_values,
            sizeof(nonfinite_matrix_values)
        );
        nonfinite_matrix_values[5] =
            float_from_bits(UINT32_C(0x7fc54321));
        nonfinite_matrix = transform_matrix_from_values(
            nonfinite_matrix_values
        );

        if (run_transform_case(
                scalar,
                neon,
                &identity,
                &identity,
                rejected_positions,
                0u,
                SOC_CLIP_DEPTH_ZERO_TO_ONE,
                &rejected_metadata,
                "trivial-reject"
            ) != 0 ||
            run_transform_case(
                scalar,
                neon,
                &identity,
                &identity,
                nonfinite_positions,
                0u,
                SOC_CLIP_DEPTH_ZERO_TO_ONE,
                &nonfinite_metadata,
                "nonfinite-fallback"
            ) != 0 ||
            run_transform_case(
                scalar,
                neon,
                &nonfinite_matrix,
                &identity,
                boundary_positions,
                0u,
                SOC_CLIP_DEPTH_ZERO_TO_ONE,
                &nonfinite_metadata,
                "nonfinite-matrix-fallback"
            ) != 0) {
            return 1;
        }
    }
    for (offset = 0u; offset < 4u; ++offset) {
        if (run_transform_case(
                scalar,
                neon,
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
            object_random[index] = float_from_bits(
                random_u32(&state) & ~UINT32_C(0x00800000)
            );
            clip_random[index] = float_from_bits(
                random_u32(&state) & ~UINT32_C(0x00800000)
            );
        }
        for (index = 0u; index < ARRAY_COUNT(positions); ++index) {
            positions[index] =
                random_u32(&state) & ~UINT32_C(0x00800000);
        }
        object = transform_matrix_from_values(object_random);
        clip = transform_matrix_from_values(clip_random);
        if (run_transform_case(
                scalar,
                neon,
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
                scalar,
                neon,
                &object,
                &clip,
                positions,
                iteration & 3u,
                (iteration & 1u) == 0u
                    ? SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE
                    : SOC_CLIP_DEPTH_ZERO_TO_ONE,
                NULL,
                "nonfinite-and-special"
            ) != 0) {
            return 1;
        }
    }
    return 0;
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
    CHECK(scalar->reduce_hiz_level_f32 != NULL);
    CHECK(scalar->transform_triangle_f64 != NULL);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_SCALAR) == scalar);

#if SOC_TEST_AARCH64
    CHECK(neon != NULL);
    CHECK(neon->backend == SOC_KERNEL_BACKEND_NEON);
    CHECK(neon->clear_f32 != NULL);
    CHECK(neon->store_constant_depth_block_f32 != NULL);
    CHECK(neon->reduce_hiz_level_f32 != NULL);
    CHECK(neon->transform_triangle_f64 != NULL);
    CHECK(neon->clear_f32 == scalar->clear_f32);
    CHECK(neon->store_constant_depth_block_f32 !=
        scalar->store_constant_depth_block_f32);
    CHECK(neon->reduce_hiz_level_f32 != scalar->reduce_hiz_level_f32);
    CHECK(neon->transform_triangle_f64 != scalar->transform_triangle_f64);
    CHECK(soc_kernel_table_for_backend(SOC_KERNEL_BACKEND_NEON) == neon);

    if (test_clear_differential(scalar, neon) != 0) {
        return 1;
    }
    if (test_raster_depth_block_differential(scalar, neon) != 0) {
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
    if (test_transform_differential(scalar, neon) != 0) {
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
