#include "occlusion/soc_hiz.h"
#include "occlusion/soc_visibility.h"

#include "core/soc_kernels.h"
#include "platform/soc_thread_pool.h"

#include <float.h>
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

#define CHECK_RESULT(expression, expected) \
    do { \
        const soc_result actual_result = (expression); \
        const soc_result expected_result = (expected); \
        if (actual_result != expected_result) { \
            fprintf( \
                stderr, \
                "%s:%d: result was %d, expected %d: %s\n", \
                __FILE__, \
                __LINE__, \
                (int)actual_result, \
                (int)expected_result, \
                #expression \
            ); \
            return 1; \
        } \
    } while (0)

static int check_values(
    const float* actual,
    const float* expected,
    size_t count,
    const char* label
)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (actual[index] != expected[index]) {
            fprintf(
                stderr,
                "%s[%zu] was %.9g, expected %.9g\n",
                label,
                index,
                (double)actual[index],
                (double)expected[index]
            );
            return 1;
        }
    }
    return 0;
}

static int check_level(
    soc_hiz* hiz,
    uint32_t level,
    uint32_t width,
    uint32_t height,
    const float* expected,
    size_t expected_count
)
{
    const soc_hiz_level* metadata;
    const float* data;

    CHECK(hiz != NULL);
    CHECK(level < hiz->level_count);
    metadata = &hiz->levels[level];
    CHECK(metadata->width == width);
    CHECK(metadata->height == height);
    CHECK(metadata->element_count == expected_count);
    CHECK(expected_count == (size_t)width * height);
    data = soc_hiz_level_data(hiz, level);
    CHECK(data != NULL);
    return check_values(data, expected, expected_count, "Hi-Z level");
}

static int check_all_values(
    const soc_hiz* hiz,
    float expected,
    const char* label
)
{
    size_t index;

    CHECK(hiz != NULL);
    CHECK(hiz->data != NULL);
    for (index = 0u; index < hiz->element_count; ++index) {
        if (hiz->data[index] != expected) {
            fprintf(
                stderr,
                "%s[%zu] was %.9g, expected %.9g\n",
                label,
                index,
                (double)hiz->data[index],
                (double)expected
            );
            return 1;
        }
    }
    return 0;
}

static int set_level_zero(
    soc_hiz* hiz,
    const float* values,
    size_t value_count
)
{
    float* destination = soc_hiz_level_data(hiz, 0u);

    CHECK(destination != NULL);
    CHECK(hiz->levels[0].element_count == value_count);
    memcpy(destination, values, value_count * sizeof(*values));
    return 0;
}

static int test_min_reduction(void)
{
    const float level_zero[] = {
        0.10f, 0.70f, 0.20f, 0.30f,
        0.60f, 0.40f, 0.90f, 0.80f,
        0.05f, 0.15f, 0.25f, 0.35f,
        0.45f, 0.55f, 0.65f, 0.75f,
    };
    const float level_one[] = {
        0.10f, 0.20f,
        0.05f, 0.25f,
    };
    const float level_two[] = {0.05f};
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 4u, 4u), SOC_RESULT_OK);
    CHECK(set_level_zero(&hiz, level_zero, ARRAY_COUNT(level_zero)) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        0u,
        4u,
        4u,
        level_zero,
        ARRAY_COUNT(level_zero)
    ) == 0);
    CHECK(check_level(
        &hiz,
        1u,
        2u,
        2u,
        level_one,
        ARRAY_COUNT(level_one)
    ) == 0);
    CHECK(check_level(
        &hiz,
        2u,
        1u,
        1u,
        level_two,
        ARRAY_COUNT(level_two)
    ) == 0);
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_odd_5_by_3_edges(void)
{
    const float level_zero[] = {
         1.0f,  2.0f,  3.0f,  4.0f,  5.0f,
         6.0f,  7.0f,  8.0f,  9.0f, 10.0f,
        11.0f, 12.0f, 13.0f, 14.0f, 15.0f,
    };
    const float level_one[] = {
         1.0f,  3.0f,  5.0f,
        11.0f, 13.0f, 15.0f,
    };
    const float level_two[] = {1.0f, 5.0f};
    const float level_three[] = {1.0f};
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 5u, 3u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 4u);
    CHECK(set_level_zero(&hiz, level_zero, ARRAY_COUNT(level_zero)) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        1u,
        3u,
        2u,
        level_one,
        ARRAY_COUNT(level_one)
    ) == 0);
    CHECK(check_level(
        &hiz,
        2u,
        2u,
        1u,
        level_two,
        ARRAY_COUNT(level_two)
    ) == 0);
    CHECK(check_level(
        &hiz,
        3u,
        1u,
        1u,
        level_three,
        ARRAY_COUNT(level_three)
    ) == 0);
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_single_axis_and_single_pixel_shapes(void)
{
    const float vertical_zero[] = {0.20f, 0.90f, 0.40f, 0.70f, 0.80f};
    const float vertical_one[] = {0.20f, 0.40f, 0.80f};
    const float vertical_two[] = {0.20f, 0.80f};
    const float vertical_three[] = {0.20f};
    const float horizontal_zero[] = {0.20f, 0.90f, 0.40f, 0.70f, 0.80f};
    const float horizontal_one[] = {0.20f, 0.40f, 0.80f};
    const float horizontal_two[] = {0.20f, 0.80f};
    const float horizontal_three[] = {0.20f};
    const float single[] = {0.42f};
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 1u, 5u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 4u);
    CHECK(set_level_zero(
        &hiz,
        vertical_zero,
        ARRAY_COUNT(vertical_zero)
    ) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        1u,
        1u,
        3u,
        vertical_one,
        ARRAY_COUNT(vertical_one)
    ) == 0);
    CHECK(check_level(
        &hiz,
        2u,
        1u,
        2u,
        vertical_two,
        ARRAY_COUNT(vertical_two)
    ) == 0);
    CHECK(check_level(
        &hiz,
        3u,
        1u,
        1u,
        vertical_three,
        ARRAY_COUNT(vertical_three)
    ) == 0);
    soc_hiz_shutdown(&hiz);

    CHECK_RESULT(soc_hiz_initialize(&hiz, 5u, 1u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 4u);
    CHECK(set_level_zero(
        &hiz,
        horizontal_zero,
        ARRAY_COUNT(horizontal_zero)
    ) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        1u,
        3u,
        1u,
        horizontal_one,
        ARRAY_COUNT(horizontal_one)
    ) == 0);
    CHECK(check_level(
        &hiz,
        2u,
        2u,
        1u,
        horizontal_two,
        ARRAY_COUNT(horizontal_two)
    ) == 0);
    CHECK(check_level(
        &hiz,
        3u,
        1u,
        1u,
        horizontal_three,
        ARRAY_COUNT(horizontal_three)
    ) == 0);
    soc_hiz_shutdown(&hiz);

    CHECK_RESULT(soc_hiz_initialize(&hiz, 1u, 1u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 1u);
    CHECK(set_level_zero(&hiz, single, ARRAY_COUNT(single)) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        0u,
        1u,
        1u,
        single,
        ARRAY_COUNT(single)
    ) == 0);
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_query_metadata_and_buffer_size(void)
{
    const float level_zero[] = {
         1.0f,  2.0f,  3.0f,  4.0f,  5.0f,
         6.0f,  7.0f,  8.0f,  9.0f, 10.0f,
        11.0f, 12.0f, 13.0f, 14.0f, 15.0f,
    };
    const float expected[] = {
         1.0f,  3.0f,  5.0f,
        11.0f, 13.0f, 15.0f,
    };
    const float sentinel = -123.0f;
    soc_hiz_level_info info = {
        .struct_size = SOC_HIZ_LEVEL_INFO_SIZE_V1,
    };
    float output[7];
    soc_hiz hiz = {0};
    size_t index;

    CHECK_RESULT(soc_hiz_initialize(&hiz, 5u, 3u), SOC_RESULT_OK);
    CHECK(set_level_zero(&hiz, level_zero, ARRAY_COUNT(level_zero)) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);

    CHECK_RESULT(
        soc_hiz_query(&hiz, 1u, &info, NULL, 0u),
        SOC_RESULT_OK
    );
    CHECK(info.struct_size == SOC_HIZ_LEVEL_INFO_SIZE_V1);
    CHECK(info.level == 1u);
    CHECK(info.width == 3u);
    CHECK(info.height == 2u);
    CHECK(info.required_element_count == 6u);

    for (index = 0u; index < ARRAY_COUNT(output); ++index) {
        output[index] = sentinel;
    }
    CHECK_RESULT(
        soc_hiz_query(&hiz, 1u, &info, output, 5u),
        SOC_RESULT_BUFFER_TOO_SMALL
    );
    CHECK(info.level == 1u);
    CHECK(info.width == 3u);
    CHECK(info.height == 2u);
    CHECK(info.required_element_count == 6u);
    for (index = 0u; index < ARRAY_COUNT(output); ++index) {
        CHECK(output[index] == sentinel);
    }

    CHECK_RESULT(
        soc_hiz_query(&hiz, 1u, &info, output, 6u),
        SOC_RESULT_OK
    );
    CHECK(check_values(output, expected, ARRAY_COUNT(expected), "query") == 0);
    CHECK(output[6] == sentinel);
    CHECK_RESULT(
        soc_hiz_query(&hiz, 1u, &info, NULL, 6u),
        SOC_RESULT_INVALID_ARGUMENT
    );

    info.struct_size = SOC_HIZ_LEVEL_INFO_SIZE_V1 - 1u;
    CHECK_RESULT(
        soc_hiz_query(&hiz, 1u, &info, NULL, 0u),
        SOC_RESULT_INVALID_ARGUMENT
    );
    info.struct_size = SOC_HIZ_LEVEL_INFO_SIZE_V1;
    CHECK_RESULT(
        soc_hiz_query(&hiz, hiz.level_count, &info, NULL, 0u),
        SOC_RESULT_INVALID_ARGUMENT
    );
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_layout_reinitialization(void)
{
    const float original_zero[] = {0.10f, 0.20f, 0.30f, 0.40f};
    const float original_one[] = {0.10f};
    const float resized_zero[] = {
         1.0f,  2.0f,  3.0f,  4.0f,  5.0f,
         6.0f,  7.0f,  8.0f,  9.0f, 10.0f,
        11.0f, 12.0f, 13.0f, 14.0f, 15.0f,
    };
    const float resized_top[] = {1.0f};
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 2u, 2u), SOC_RESULT_OK);
    CHECK(set_level_zero(
        &hiz,
        original_zero,
        ARRAY_COUNT(original_zero)
    ) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        1u,
        1u,
        1u,
        original_one,
        ARRAY_COUNT(original_one)
    ) == 0);
    soc_hiz_shutdown(&hiz);

    CHECK_RESULT(soc_hiz_initialize(&hiz, 5u, 3u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 4u);
    CHECK(hiz.element_count == 24u);
    CHECK(hiz.levels[0].offset == 0u);
    CHECK(hiz.levels[1].offset == 15u);
    CHECK(hiz.levels[2].offset == 21u);
    CHECK(hiz.levels[3].offset == 23u);
    CHECK(set_level_zero(
        &hiz,
        resized_zero,
        ARRAY_COUNT(resized_zero)
    ) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        3u,
        1u,
        1u,
        resized_top,
        ARRAY_COUNT(resized_top)
    ) == 0);
    soc_hiz_shutdown(&hiz);

    CHECK_RESULT(soc_hiz_initialize(&hiz, 3u, 2u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 3u);
    CHECK(hiz.element_count == 9u);
    CHECK_RESULT(soc_hiz_clear_level_zero(&hiz), SOC_RESULT_OK);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_all_values(&hiz, 0.0f, "resized clear") == 0);
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_masked_layout_clear_and_build(void)
{
    const float level_zero[] = {
        0.90f, 0.80f, 0.70f,
        0.60f, 0.50f, 0.40f,
        0.30f, 0.20f, 0.10f,
    };
    const float level_one[] = {
        0.50f, 0.40f,
        0.20f, 0.10f,
    };
    const float level_two[] = {0.10f};
    soc_projected_aabb projected = {
        .minimum_ndc_x = 0.10f,
        .maximum_ndc_x = 0.40f,
        .minimum_ndc_y = -0.40f,
        .maximum_ndc_y = -0.20f,
        .nearest_depth = 0.50f,
    };
    soc_hiz hiz = {0};
    size_t index;

    CHECK_RESULT(
        soc_hiz_initialize_masked(&hiz, 17u, 9u),
        SOC_RESULT_OK
    );
    CHECK(hiz.masked == SOC_TRUE);
    CHECK(hiz.pixel_width == 17u);
    CHECK(hiz.pixel_height == 9u);
    CHECK(hiz.levels[0].width == 3u);
    CHECK(hiz.levels[0].height == 3u);
    CHECK(hiz.working_depth != NULL);
    CHECK(hiz.layer_masks != NULL);
    CHECK_RESULT(soc_hiz_clear_level_zero(&hiz), SOC_RESULT_OK);
    for (index = 0u; index < hiz.levels[0].element_count; ++index) {
        CHECK(hiz.data[index] == -1.0f);
        CHECK(hiz.working_depth[index] == FLT_MAX);
        CHECK(hiz.layer_masks[index] == 0u);
    }

    /* Pixels 9-11 by 5-6 map only to masked block (1, 1). */
    hiz.data[4] = 0.80f;
    CHECK(
        soc_test_projected_aabb_scalar(&hiz, &projected) ==
            SOC_VISIBILITY_OCCLUDED
    );
    projected.minimum_ndc_x = -0.20f;
    CHECK(
        soc_test_projected_aabb_scalar(&hiz, &projected) ==
            SOC_VISIBILITY_VISIBLE
    );

    CHECK(set_level_zero(&hiz, level_zero, ARRAY_COUNT(level_zero)) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        1u,
        2u,
        2u,
        level_one,
        ARRAY_COUNT(level_one)
    ) == 0);
    CHECK(check_level(
        &hiz,
        2u,
        1u,
        1u,
        level_two,
        ARRAY_COUNT(level_two)
    ) == 0);

    soc_hiz_shutdown(&hiz);
    CHECK(hiz.data == NULL);
    CHECK(hiz.working_depth == NULL);
    CHECK(hiz.layer_masks == NULL);
    return 0;
}

static int compare_split_band_pyramid(
    uint32_t width,
    uint32_t height
)
{
    soc_hiz serial = {0};
    soc_hiz split = {0};
    soc_hiz unchecked = {0};
    float* serial_level_zero;
    float* split_level_zero;
    float* unchecked_level_zero;
    uint32_t band_count;
    uint32_t band_index;
    size_t byte_count;
    size_t index;

    CHECK_RESULT(soc_hiz_initialize(&serial, width, height), SOC_RESULT_OK);
    CHECK_RESULT(soc_hiz_initialize(&split, width, height), SOC_RESULT_OK);
    CHECK_RESULT(
        soc_hiz_initialize(&unchecked, width, height),
        SOC_RESULT_OK
    );
    CHECK(serial.element_count == split.element_count);
    CHECK(serial.level_count == split.level_count);
    CHECK(serial.element_count == unchecked.element_count);
    CHECK(serial.level_count == unchecked.level_count);

    serial_level_zero = soc_hiz_level_data(&serial, 0u);
    split_level_zero = soc_hiz_level_data(&split, 0u);
    unchecked_level_zero = soc_hiz_level_data(&unchecked, 0u);
    CHECK(serial_level_zero != NULL);
    CHECK(split_level_zero != NULL);
    CHECK(unchecked_level_zero != NULL);
    for (index = 0u; index < serial.levels[0].element_count; ++index) {
        serial_level_zero[index] = (float)(
            (index * 53u + (size_t)width * 19u + height * 7u) % 1021u
        ) / 1020.0f;
    }
    memcpy(
        split_level_zero,
        serial_level_zero,
        serial.levels[0].element_count * sizeof(*serial_level_zero)
    );
    memcpy(
        unchecked_level_zero,
        serial_level_zero,
        serial.levels[0].element_count * sizeof(*serial_level_zero)
    );

    CHECK_RESULT(
        soc_hiz_build_with_kernels(
            &serial,
            soc_kernel_table_scalar()
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_hiz_lower_band_count(&split, &band_count),
        SOC_RESULT_OK
    );
    CHECK(band_count == height / SOC_HIZ_LOWER_BAND_HEIGHT +
        (height % SOC_HIZ_LOWER_BAND_HEIGHT != 0u ? 1u : 0u));

    /* Bands are independent; reverse order catches accidental dependencies. */
    for (band_index = band_count; band_index != 0u; --band_index) {
        CHECK_RESULT(
            soc_hiz_build_lower_band_with_kernels(
                &split,
                soc_kernel_table_scalar(),
                band_index - 1u
            ),
            SOC_RESULT_OK
        );
        soc_hiz_build_lower_band_unchecked_with_kernels(
            &unchecked,
            soc_kernel_table_scalar(),
            band_index - 1u
        );
    }
    CHECK_RESULT(
        soc_hiz_build_upper_levels_with_kernels(
            &split,
            soc_kernel_table_scalar()
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_hiz_build_upper_levels_with_kernels(
            &unchecked,
            soc_kernel_table_scalar()
        ),
        SOC_RESULT_OK
    );

    CHECK(serial.element_count <= SIZE_MAX / sizeof(*serial.data));
    byte_count = serial.element_count * sizeof(*serial.data);
    CHECK(memcmp(serial.data, split.data, byte_count) == 0);
    CHECK(memcmp(split.data, unchecked.data, byte_count) == 0);

    soc_hiz_shutdown(&unchecked);
    soc_hiz_shutdown(&split);
    soc_hiz_shutdown(&serial);
    return 0;
}

static int test_split_band_build_matches_serial(void)
{
    static const struct {
        uint32_t width;
        uint32_t height;
    } shapes[] = {
        {1u, 1u},
        {19u, 15u},
        {19u, 16u},
        {19u, 17u},
        {33u, 33u},
        {7u, 65u},
        {1025u, 1u},
        {641u, 63u},
    };
    size_t shape_index;

    _Static_assert(
        SOC_HIZ_LOWER_BAND_HEIGHT == 16u,
        "unexpected lower Hi-Z band height"
    );
    _Static_assert(
        SOC_HIZ_LOWER_LEVEL_COUNT == 4u,
        "unexpected lower Hi-Z level count"
    );
    for (shape_index = 0u;
         shape_index < ARRAY_COUNT(shapes);
         ++shape_index) {
        CHECK(compare_split_band_pyramid(
            shapes[shape_index].width,
            shapes[shape_index].height
        ) == 0);
    }
    return 0;
}

static int test_split_band_api_validation(void)
{
    const soc_kernel_table* kernels = soc_kernel_table_scalar();
    soc_kernel_table missing_reduce = *kernels;
    soc_hiz uninitialized = {0};
    soc_hiz hiz = {0};
    soc_hiz synthetic = {0};
    float synthetic_data = 0.0f;
    uint32_t band_count = 99u;

    missing_reduce.reduce_hiz_level_f32 = NULL;
    CHECK_RESULT(
        soc_hiz_lower_band_count(NULL, &band_count),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_hiz_lower_band_count(&uninitialized, &band_count),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_hiz_lower_band_count(&uninitialized, NULL),
        SOC_RESULT_INVALID_ARGUMENT
    );

    CHECK_RESULT(soc_hiz_initialize(&hiz, 33u, 17u), SOC_RESULT_OK);
    CHECK_RESULT(soc_hiz_clear_level_zero(&hiz), SOC_RESULT_OK);
    CHECK_RESULT(
        soc_hiz_lower_band_count(&hiz, NULL),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(soc_hiz_lower_band_count(&hiz, &band_count), SOC_RESULT_OK);
    CHECK(band_count == 2u);
    CHECK_RESULT(
        soc_hiz_build_lower_band_with_kernels(
            NULL,
            kernels,
            0u
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_hiz_build_lower_band_with_kernels(
            &hiz,
            NULL,
            0u
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_hiz_build_lower_band_with_kernels(
            &hiz,
            &missing_reduce,
            0u
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_hiz_build_lower_band_with_kernels(
            &hiz,
            kernels,
            band_count
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_hiz_build_upper_levels_with_kernels(
            NULL,
            kernels
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        soc_hiz_build_upper_levels_with_kernels(
            &hiz,
            &missing_reduce
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    soc_hiz_shutdown(&hiz);

    /* The final partial band must not overflow at UINT32_MAX height. */
    synthetic.initialized = SOC_TRUE;
    synthetic.data = &synthetic_data;
    synthetic.level_count = 1u;
    synthetic.levels[0].width = 1u;
    synthetic.levels[0].height = UINT32_MAX;
    CHECK_RESULT(
        soc_hiz_lower_band_count(&synthetic, &band_count),
        SOC_RESULT_OK
    );
    CHECK(band_count == UINT32_MAX / SOC_HIZ_LOWER_BAND_HEIGHT + 1u);
    CHECK_RESULT(
        soc_hiz_build_lower_band_with_kernels(
            &synthetic,
            kernels,
            band_count - 1u
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_hiz_build_lower_band_with_kernels(
            &synthetic,
            kernels,
            band_count
        ),
        SOC_RESULT_INVALID_ARGUMENT
    );
    return 0;
}

static int compare_parallel_pyramid(
    soc_thread_pool* thread_pool,
    uint32_t width,
    uint32_t height
)
{
    soc_hiz serial = {0};
    soc_hiz parallel = {0};
    float* serial_level_zero;
    float* parallel_level_zero;
    size_t byte_count;
    size_t index;

    CHECK_RESULT(soc_hiz_initialize(&serial, width, height), SOC_RESULT_OK);
    CHECK_RESULT(
        soc_hiz_initialize(&parallel, width, height),
        SOC_RESULT_OK
    );
    CHECK(serial.element_count == parallel.element_count);
    CHECK(serial.level_count == parallel.level_count);

    serial_level_zero = soc_hiz_level_data(&serial, 0u);
    parallel_level_zero = soc_hiz_level_data(&parallel, 0u);
    CHECK(serial_level_zero != NULL);
    CHECK(parallel_level_zero != NULL);
    for (index = 0u;
         index < serial.levels[0].element_count;
         ++index) {
        serial_level_zero[index] = (float)(
            (index * 37u + (size_t)width * 13u + height) % 1009u
        ) / 1008.0f;
    }
    memcpy(
        parallel_level_zero,
        serial_level_zero,
        serial.levels[0].element_count * sizeof(*serial_level_zero)
    );

    CHECK_RESULT(
        soc_hiz_build_with_kernels(
            &serial,
            soc_kernel_table_scalar()
        ),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_hiz_build_parallel_with_kernels(
            &parallel,
            soc_kernel_table_scalar(),
            thread_pool
        ),
        SOC_RESULT_OK
    );

    CHECK(serial.element_count <= SIZE_MAX / sizeof(*serial.data));
    byte_count = serial.element_count * sizeof(*serial.data);
    CHECK(memcmp(serial.data, parallel.data, byte_count) == 0);

    soc_hiz_shutdown(&parallel);
    soc_hiz_shutdown(&serial);
    return 0;
}

static int test_parallel_build_matches_serial(void)
{
    static const struct {
        uint32_t width;
        uint32_t height;
    } shapes[] = {
        {640u, 640u},
        {641u, 643u},
        {800u, 513u},
        {1024u, 800u},
        {1280u, 1024u},
        {1u, 393217u},
        {393217u, 1u},
        {127u, 73u},
    };
    soc_thread_pool parallel_pool = {0};
    soc_thread_pool serial_pool = {0};
    size_t shape_index;

    CHECK_RESULT(
        soc_thread_pool_initialize(&parallel_pool, 4u),
        SOC_RESULT_OK
    );
    CHECK_RESULT(
        soc_thread_pool_initialize(&serial_pool, 1u),
        SOC_RESULT_OK
    );

    for (shape_index = 0u;
         shape_index < ARRAY_COUNT(shapes);
         ++shape_index) {
        CHECK(compare_parallel_pyramid(
            &parallel_pool,
            shapes[shape_index].width,
            shapes[shape_index].height
        ) == 0);
    }
    CHECK(compare_parallel_pyramid(
        &serial_pool,
        641u,
        643u
    ) == 0);

    soc_thread_pool_shutdown(&serial_pool);
    soc_thread_pool_shutdown(&parallel_pool);
    return 0;
}

int main(void)
{
    if (test_min_reduction() != 0) {
        return 1;
    }
    if (test_odd_5_by_3_edges() != 0) {
        return 1;
    }
    if (test_single_axis_and_single_pixel_shapes() != 0) {
        return 1;
    }
    if (test_query_metadata_and_buffer_size() != 0) {
        return 1;
    }
    if (test_layout_reinitialization() != 0) {
        return 1;
    }
    if (test_masked_layout_clear_and_build() != 0) {
        return 1;
    }
    if (test_split_band_build_matches_serial() != 0) {
        return 1;
    }
    if (test_split_band_api_validation() != 0) {
        return 1;
    }
    return test_parallel_build_matches_serial();
}
