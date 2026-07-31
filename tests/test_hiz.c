#include "occlusion/soc_hiz.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

static int test_forward_max_reduction(void)
{
    const float level_zero[] = {
        0.10f, 0.70f, 0.20f, 0.30f,
        0.60f, 0.40f, 0.90f, 0.80f,
        0.05f, 0.15f, 0.25f, 0.35f,
        0.45f, 0.55f, 0.65f, 0.75f,
    };
    const float level_one[] = {
        0.70f, 0.90f,
        0.55f, 0.75f,
    };
    const float level_two[] = {0.90f};
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 4u, 4u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 3u);
    CHECK(set_level_zero(&hiz, level_zero, ARRAY_COUNT(level_zero)) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
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

static int test_reversed_min_reduction(void)
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
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_REVERSED), SOC_RESULT_OK);
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
    const float forward_level_one[] = {
         7.0f,  9.0f, 10.0f,
        12.0f, 14.0f, 15.0f,
    };
    const float forward_level_two[] = {14.0f, 15.0f};
    const float forward_level_three[] = {15.0f};
    const float reversed_level_one[] = {
         1.0f,  3.0f,  5.0f,
        11.0f, 13.0f, 15.0f,
    };
    const float reversed_level_two[] = {1.0f, 5.0f};
    const float reversed_level_three[] = {1.0f};
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 5u, 3u), SOC_RESULT_OK);
    CHECK(hiz.level_count == 4u);
    CHECK(set_level_zero(&hiz, level_zero, ARRAY_COUNT(level_zero)) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        1u,
        3u,
        2u,
        forward_level_one,
        ARRAY_COUNT(forward_level_one)
    ) == 0);
    CHECK(check_level(
        &hiz,
        2u,
        2u,
        1u,
        forward_level_two,
        ARRAY_COUNT(forward_level_two)
    ) == 0);
    CHECK(check_level(
        &hiz,
        3u,
        1u,
        1u,
        forward_level_three,
        ARRAY_COUNT(forward_level_three)
    ) == 0);

    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_REVERSED), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        1u,
        3u,
        2u,
        reversed_level_one,
        ARRAY_COUNT(reversed_level_one)
    ) == 0);
    CHECK(check_level(
        &hiz,
        2u,
        2u,
        1u,
        reversed_level_two,
        ARRAY_COUNT(reversed_level_two)
    ) == 0);
    CHECK(check_level(
        &hiz,
        3u,
        1u,
        1u,
        reversed_level_three,
        ARRAY_COUNT(reversed_level_three)
    ) == 0);
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_single_axis_and_single_pixel_shapes(void)
{
    const float vertical_zero[] = {0.20f, 0.90f, 0.40f, 0.70f, 0.80f};
    const float vertical_one[] = {0.90f, 0.70f, 0.80f};
    const float vertical_two[] = {0.90f, 0.80f};
    const float vertical_three[] = {0.90f};
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
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
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
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_REVERSED), SOC_RESULT_OK);
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
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
    CHECK(check_level(
        &hiz,
        0u,
        1u,
        1u,
        single,
        ARRAY_COUNT(single)
    ) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_REVERSED), SOC_RESULT_OK);
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
         7.0f,  9.0f, 10.0f,
        12.0f, 14.0f, 15.0f,
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
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);

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
    const float original_one[] = {0.40f};
    const float resized_zero[] = {
         1.0f,  2.0f,  3.0f,  4.0f,  5.0f,
         6.0f,  7.0f,  8.0f,  9.0f, 10.0f,
        11.0f, 12.0f, 13.0f, 14.0f, 15.0f,
    };
    const float resized_top[] = {15.0f};
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 2u, 2u), SOC_RESULT_OK);
    CHECK(set_level_zero(
        &hiz,
        original_zero,
        ARRAY_COUNT(original_zero)
    ) == 0);
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
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
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
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
    CHECK_RESULT(
        soc_hiz_clear_level_zero(&hiz, SOC_DEPTH_REVERSED),
        SOC_RESULT_OK
    );
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_REVERSED), SOC_RESULT_OK);
    CHECK(check_all_values(&hiz, 0.0f, "resized clear") == 0);
    soc_hiz_shutdown(&hiz);
    return 0;
}

static int test_depth_direction_rebuild_across_frames(void)
{
    soc_hiz hiz = {0};

    CHECK_RESULT(soc_hiz_initialize(&hiz, 5u, 3u), SOC_RESULT_OK);

    CHECK_RESULT(
        soc_hiz_clear_level_zero(&hiz, SOC_DEPTH_FORWARD),
        SOC_RESULT_OK
    );
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
    CHECK(check_all_values(&hiz, 1.0f, "forward frame") == 0);

    CHECK_RESULT(
        soc_hiz_clear_level_zero(&hiz, SOC_DEPTH_REVERSED),
        SOC_RESULT_OK
    );
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_REVERSED), SOC_RESULT_OK);
    CHECK(check_all_values(&hiz, 0.0f, "reversed frame") == 0);

    CHECK_RESULT(
        soc_hiz_clear_level_zero(&hiz, SOC_DEPTH_FORWARD),
        SOC_RESULT_OK
    );
    CHECK_RESULT(soc_hiz_build(&hiz, SOC_DEPTH_FORWARD), SOC_RESULT_OK);
    CHECK(check_all_values(&hiz, 1.0f, "second forward frame") == 0);

    soc_hiz_shutdown(&hiz);
    return 0;
}

int main(void)
{
    if (test_forward_max_reduction() != 0) {
        return 1;
    }
    if (test_reversed_min_reduction() != 0) {
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
    if (test_depth_direction_rebuild_across_frames() != 0) {
        return 1;
    }
    return 0;
}
