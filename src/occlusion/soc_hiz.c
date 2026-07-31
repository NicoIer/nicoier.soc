#include "occlusion/soc_hiz.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static soc_bool checked_size_add(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || left > SIZE_MAX - right) {
        return SOC_FALSE;
    }

    *out_result = left + right;
    return SOC_TRUE;
}

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

static uint32_t halve_ceil(uint32_t value)
{
    return value / 2u + value % 2u;
}

static soc_result create_hiz(
    uint32_t width,
    uint32_t height,
    soc_hiz* out_hiz
)
{
    soc_hiz hiz;
    uint32_t level = 0u;
    size_t byte_count;

    if (width == 0u || height == 0u || out_hiz == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    memset(&hiz, 0, sizeof(hiz));
    for (;;) {
        soc_hiz_level* current;
        size_t level_element_count;
        size_t total_element_count;

        if (level >= SOC_HIZ_MAX_LEVEL_COUNT ||
            !checked_size_multiply(
                (size_t)width,
                (size_t)height,
                &level_element_count
            ) ||
            !checked_size_add(
                hiz.element_count,
                level_element_count,
                &total_element_count
            )) {
            return SOC_RESULT_OUT_OF_MEMORY;
        }

        current = &hiz.levels[level];
        current->width = width;
        current->height = height;
        current->offset = hiz.element_count;
        current->element_count = level_element_count;
        hiz.element_count = total_element_count;
        ++level;

        if (width == 1u && height == 1u) {
            break;
        }
        width = halve_ceil(width);
        height = halve_ceil(height);
    }

    if (!checked_size_multiply(
            hiz.element_count,
            sizeof(*hiz.data),
            &byte_count
        )) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    hiz.data = malloc(byte_count);
    if (hiz.data == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    hiz.level_count = level;
    hiz.initialized = SOC_TRUE;
    *out_hiz = hiz;
    return SOC_RESULT_OK;
}

soc_result soc_hiz_initialize(
    soc_hiz* hiz,
    uint32_t width,
    uint32_t height
)
{
    soc_hiz initialized;
    soc_result result;

    if (hiz == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    result = create_hiz(width, height, &initialized);
    if (result != SOC_RESULT_OK) {
        memset(hiz, 0, sizeof(*hiz));
        return result;
    }

    *hiz = initialized;
    return SOC_RESULT_OK;
}

void soc_hiz_shutdown(soc_hiz* hiz)
{
    if (hiz == NULL) {
        return;
    }

    free(hiz->data);
    memset(hiz, 0, sizeof(*hiz));
}

float* soc_hiz_level_data(soc_hiz* hiz, uint32_t level)
{
    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        level >= hiz->level_count) {
        return NULL;
    }

    return hiz->data + hiz->levels[level].offset;
}

soc_result soc_hiz_clear_level_zero(
    soc_hiz* hiz,
    soc_depth_direction depth_direction
)
{
    float clear_depth;
    float* level_zero;
    size_t index;

    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        (depth_direction != SOC_DEPTH_FORWARD &&
            depth_direction != SOC_DEPTH_REVERSED)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    clear_depth = depth_direction == SOC_DEPTH_REVERSED ? 0.0f : 1.0f;
    level_zero = hiz->data + hiz->levels[0].offset;
    for (index = 0u; index < hiz->levels[0].element_count; ++index) {
        level_zero[index] = clear_depth;
    }
    return SOC_RESULT_OK;
}

static float reduce_depth(
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

soc_result soc_hiz_build(
    soc_hiz* hiz,
    soc_depth_direction depth_direction
)
{
    uint32_t level;

    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        (depth_direction != SOC_DEPTH_FORWARD &&
            depth_direction != SOC_DEPTH_REVERSED)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (level = 1u; level < hiz->level_count; ++level) {
        const soc_hiz_level* source_level = &hiz->levels[level - 1u];
        const soc_hiz_level* destination_level = &hiz->levels[level];
        const float* source = hiz->data + source_level->offset;
        float* destination = hiz->data + destination_level->offset;
        uint32_t destination_y;

        for (destination_y = 0u;
             destination_y < destination_level->height;
             ++destination_y) {
            const uint32_t source_y = destination_y * 2u;
            uint32_t destination_x;

            for (destination_x = 0u;
                 destination_x < destination_level->width;
                 ++destination_x) {
                const uint32_t source_x = destination_x * 2u;
                const size_t source_index =
                    (size_t)source_y * source_level->width + source_x;
                float reduced = source[source_index];

                if (source_x + 1u < source_level->width) {
                    reduced = reduce_depth(
                        reduced,
                        source[source_index + 1u],
                        depth_direction
                    );
                }
                if (source_y + 1u < source_level->height) {
                    const size_t next_row_index =
                        source_index + source_level->width;

                    reduced = reduce_depth(
                        reduced,
                        source[next_row_index],
                        depth_direction
                    );
                    if (source_x + 1u < source_level->width) {
                        reduced = reduce_depth(
                            reduced,
                            source[next_row_index + 1u],
                            depth_direction
                        );
                    }
                }

                destination[
                    (size_t)destination_y * destination_level->width +
                        destination_x
                ] = reduced;
            }
        }
    }

    return SOC_RESULT_OK;
}

soc_result soc_hiz_query(
    const soc_hiz* hiz,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
)
{
    const soc_hiz_level* selected_level;

    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        out_info == NULL ||
        out_info->struct_size < SOC_HIZ_LEVEL_INFO_SIZE_V1 ||
        level >= hiz->level_count) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    selected_level = &hiz->levels[level];
    out_info->level = level;
    out_info->width = selected_level->width;
    out_info->height = selected_level->height;
    out_info->required_element_count =
        (uint64_t)selected_level->element_count;

    if (out_depth == NULL) {
        return out_depth_count == 0u
            ? SOC_RESULT_OK
            : SOC_RESULT_INVALID_ARGUMENT;
    }
    if (out_depth_count < (uint64_t)selected_level->element_count) {
        return SOC_RESULT_BUFFER_TOO_SMALL;
    }

    memcpy(
        out_depth,
        hiz->data + selected_level->offset,
        selected_level->element_count * sizeof(*out_depth)
    );
    return SOC_RESULT_OK;
}
