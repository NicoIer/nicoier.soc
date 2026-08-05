#include "occlusion/soc_hiz.h"

#include "core/soc_kernels.h"
#include "platform/soc_thread_pool.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(SOC_HIZ_PARALLEL_TARGET_ELEMENTS_PER_WORKER)
    #define SOC_HIZ_PARALLEL_TARGET_ELEMENTS_PER_WORKER ((size_t)393216u)
#endif

#if !defined(SOC_HIZ_PARALLEL_MAX_WORKER_COUNT)
    #define SOC_HIZ_PARALLEL_MAX_WORKER_COUNT UINT32_C(8)
#endif

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

void soc_hiz_reduce_level_scalar(
    const float* source,
    uint32_t source_width,
    uint32_t source_height,
    float* destination,
    soc_depth_direction depth_direction
)
{
    const uint32_t destination_width = halve_ceil(source_width);
    const uint32_t destination_height = halve_ceil(source_height);
    uint32_t destination_y;

    for (destination_y = 0u;
         destination_y < destination_height;
         ++destination_y) {
        const uint32_t source_y = destination_y * 2u;
        uint32_t destination_x;

        for (destination_x = 0u;
             destination_x < destination_width;
             ++destination_x) {
            const uint32_t source_x = destination_x * 2u;
            const size_t source_index =
                (size_t)source_y * source_width + source_x;
            float reduced = source[source_index];

            if (source_x + 1u < source_width) {
                reduced = reduce_depth(
                    reduced,
                    source[source_index + 1u],
                    depth_direction
                );
            }
            if (source_y + 1u < source_height) {
                const size_t next_row_index = source_index + source_width;

                reduced = reduce_depth(
                    reduced,
                    source[next_row_index],
                    depth_direction
                );
                if (source_x + 1u < source_width) {
                    reduced = reduce_depth(
                        reduced,
                        source[next_row_index + 1u],
                        depth_direction
                    );
                }
            }

            destination[
                (size_t)destination_y * destination_width + destination_x
            ] = reduced;
        }
    }
}

soc_result soc_hiz_build_with_kernels(
    soc_hiz* hiz,
    soc_depth_direction depth_direction,
    const struct soc_kernel_table* kernels
)
{
    uint32_t level;

    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        kernels == NULL ||
        kernels->reduce_hiz_level_f32 == NULL ||
        (depth_direction != SOC_DEPTH_FORWARD &&
            depth_direction != SOC_DEPTH_REVERSED)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (level = 1u; level < hiz->level_count; ++level) {
        const soc_hiz_level* source_level = &hiz->levels[level - 1u];
        const soc_hiz_level* destination_level = &hiz->levels[level];

        kernels->reduce_hiz_level_f32(
            hiz->data + source_level->offset,
            source_level->width,
            source_level->height,
            hiz->data + destination_level->offset,
            depth_direction
        );
    }

    return SOC_RESULT_OK;
}

typedef struct soc_hiz_parallel_build_state {
    soc_hiz* hiz;
    const struct soc_kernel_table* kernels;
    soc_depth_direction depth_direction;
    uint32_t band_count;
} soc_hiz_parallel_build_state;

static soc_bool validate_hiz_build_arguments(
    const soc_hiz* hiz,
    soc_depth_direction depth_direction,
    const struct soc_kernel_table* kernels
)
{
    return hiz != NULL &&
        hiz->initialized == SOC_TRUE &&
        hiz->data != NULL &&
        hiz->level_count != 0u &&
        hiz->level_count <= SOC_HIZ_MAX_LEVEL_COUNT &&
        hiz->levels[0].width != 0u &&
        hiz->levels[0].height != 0u &&
        kernels != NULL &&
        kernels->reduce_hiz_level_f32 != NULL &&
        (depth_direction == SOC_DEPTH_FORWARD ||
            depth_direction == SOC_DEPTH_REVERSED);
}

static uint32_t lower_band_count_unchecked(const soc_hiz* hiz)
{
    return hiz->levels[0].height / SOC_HIZ_LOWER_BAND_HEIGHT +
        (hiz->levels[0].height % SOC_HIZ_LOWER_BAND_HEIGHT != 0u ?
            1u : 0u);
}

void soc_hiz_build_lower_band_unchecked_with_kernels(
    soc_hiz* hiz,
    soc_depth_direction depth_direction,
    const struct soc_kernel_table* kernels,
    uint32_t band_index
)
{
    uint32_t source_row_begin =
        band_index * SOC_HIZ_LOWER_BAND_HEIGHT;
    const uint32_t remaining_source_rows =
        hiz->levels[0].height - source_row_begin;
    uint32_t source_row_end = source_row_begin +
        (remaining_source_rows < SOC_HIZ_LOWER_BAND_HEIGHT ?
            remaining_source_rows : SOC_HIZ_LOWER_BAND_HEIGHT);
    uint32_t level;

    for (level = 1u;
         level <= SOC_HIZ_LOWER_LEVEL_COUNT && level < hiz->level_count;
         ++level) {
        const soc_hiz_level* source_level = &hiz->levels[level - 1u];
        const soc_hiz_level* destination_level = &hiz->levels[level];
        const uint32_t destination_row_begin = source_row_begin / 2u;
        const uint32_t destination_row_end = halve_ceil(source_row_end);

        /*
         * A 16-row Level 0 boundary stays even through the first four
         * reductions (16 -> 8 -> 4 -> 2). Each local reduction therefore
         * has the same row pairing as the full-level kernel invocation.
         */
        kernels->reduce_hiz_level_f32(
            hiz->data + source_level->offset +
                (size_t)source_row_begin * source_level->width,
            source_level->width,
            source_row_end - source_row_begin,
            hiz->data + destination_level->offset +
                (size_t)destination_row_begin * destination_level->width,
            depth_direction
        );

        source_row_begin = destination_row_begin;
        source_row_end = destination_row_end;
    }
}

soc_result soc_hiz_lower_band_count(
    const soc_hiz* hiz,
    uint32_t* out_band_count
)
{
    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        hiz->level_count == 0u ||
        hiz->level_count > SOC_HIZ_MAX_LEVEL_COUNT ||
        hiz->levels[0].width == 0u ||
        hiz->levels[0].height == 0u ||
        out_band_count == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *out_band_count = lower_band_count_unchecked(hiz);
    return SOC_RESULT_OK;
}

soc_result soc_hiz_build_lower_band_with_kernels(
    soc_hiz* hiz,
    soc_depth_direction depth_direction,
    const struct soc_kernel_table* kernels,
    uint32_t band_index
)
{
    if (validate_hiz_build_arguments(hiz, depth_direction, kernels) !=
            SOC_TRUE ||
        band_index >= lower_band_count_unchecked(hiz)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    soc_hiz_build_lower_band_unchecked_with_kernels(
        hiz,
        depth_direction,
        kernels,
        band_index
    );
    return SOC_RESULT_OK;
}

soc_result soc_hiz_build_upper_levels_with_kernels(
    soc_hiz* hiz,
    soc_depth_direction depth_direction,
    const struct soc_kernel_table* kernels
)
{
    uint32_t level;

    if (validate_hiz_build_arguments(hiz, depth_direction, kernels) !=
        SOC_TRUE) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    for (level = SOC_HIZ_LOWER_LEVEL_COUNT + 1u;
         level < hiz->level_count;
         ++level) {
        const soc_hiz_level* source_level = &hiz->levels[level - 1u];
        const soc_hiz_level* destination_level = &hiz->levels[level];

        kernels->reduce_hiz_level_f32(
            hiz->data + source_level->offset,
            source_level->width,
            source_level->height,
            hiz->data + destination_level->offset,
            depth_direction
        );
    }

    return SOC_RESULT_OK;
}

static void build_hiz_bands(
    void* user_data,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    soc_hiz_parallel_build_state* state = user_data;
    soc_hiz* hiz = state->hiz;
    const uint32_t bands_per_worker = state->band_count / worker_count;
    const uint32_t extra_bands = state->band_count % worker_count;
    const uint32_t band_begin =
        bands_per_worker * worker_index +
        (worker_index < extra_bands ? worker_index : extra_bands);
    const uint32_t band_end = band_begin + bands_per_worker +
        (worker_index < extra_bands ? 1u : 0u);
    uint32_t band_index;

    for (band_index = band_begin;
         band_index < band_end;
         ++band_index) {
        soc_hiz_build_lower_band_unchecked_with_kernels(
            hiz,
            state->depth_direction,
            state->kernels,
            band_index
        );
    }
}

soc_result soc_hiz_build_parallel_with_kernels(
    soc_hiz* hiz,
    soc_depth_direction depth_direction,
    const struct soc_kernel_table* kernels,
    struct soc_thread_pool* thread_pool
)
{
    soc_hiz_parallel_build_state state;
    size_t target_worker_count;
    uint32_t configured_worker_count;
    uint32_t active_worker_count;

    if (hiz == NULL ||
        hiz->initialized != SOC_TRUE ||
        hiz->data == NULL ||
        kernels == NULL ||
        kernels->reduce_hiz_level_f32 == NULL ||
        thread_pool == NULL ||
        (depth_direction != SOC_DEPTH_FORWARD &&
            depth_direction != SOC_DEPTH_REVERSED)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    configured_worker_count = soc_thread_pool_worker_count(thread_pool);
    if (configured_worker_count == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    state.hiz = hiz;
    state.kernels = kernels;
    state.depth_direction = depth_direction;
    state.band_count = lower_band_count_unchecked(hiz);

    active_worker_count = configured_worker_count;
    if (active_worker_count > SOC_HIZ_PARALLEL_MAX_WORKER_COUNT) {
        active_worker_count = SOC_HIZ_PARALLEL_MAX_WORKER_COUNT;
    }
    if (state.band_count < active_worker_count) {
        active_worker_count = state.band_count;
    }
    target_worker_count =
        hiz->levels[0].element_count /
            SOC_HIZ_PARALLEL_TARGET_ELEMENTS_PER_WORKER;
    if (hiz->levels[0].element_count %
            SOC_HIZ_PARALLEL_TARGET_ELEMENTS_PER_WORKER != 0u) {
        ++target_worker_count;
    }
    if (target_worker_count < active_worker_count) {
        active_worker_count = (uint32_t)target_worker_count;
    }
    if (active_worker_count <= 1u) {
        return soc_hiz_build_with_kernels(
            hiz,
            depth_direction,
            kernels
        );
    }

    soc_thread_pool_run_active(
        thread_pool,
        active_worker_count,
        build_hiz_bands,
        &state
    );

    return soc_hiz_build_upper_levels_with_kernels(
        hiz,
        depth_direction,
        kernels
    );
}

soc_result soc_hiz_build(
    soc_hiz* hiz,
    soc_depth_direction depth_direction
)
{
    return soc_hiz_build_with_kernels(
        hiz,
        depth_direction,
        soc_kernel_table_scalar()
    );
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
