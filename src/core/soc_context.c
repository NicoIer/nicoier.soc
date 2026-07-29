#include "core/soc_context.h"

#include "core/soc_mesh.h"

#include <stddef.h>
#include <stdlib.h>

soc_result soc_context_create_internal(
    const soc_config* config,
    soc_context** out_context
)
{
    if (out_context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *out_context = NULL;

    if (config == NULL ||
        config->struct_size < SOC_CONFIG_SIZE_V1 ||
        config->width == 0u ||
        config->height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    if (config->worker_count > 1u || config->flags != SOC_CONFIG_FLAG_NONE) {
        return SOC_RESULT_UNSUPPORTED;
    }

    soc_context *context = calloc(1u, sizeof(*context));
    if (context == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    soc_result result = soc_rasterizer_initialize(
        &context->rasterizer,
        config->width,
        config->height
    );
    if (result != SOC_RESULT_OK) {
        free(context);
        return result;
    }

    context->state = SOC_CONTEXT_STATE_IDLE;
    context->stats.struct_size = sizeof(context->stats);
    *out_context = context;
    return SOC_RESULT_OK;
}

void soc_context_destroy_internal(soc_context* context)
{
    if (context == NULL) {
        return;
    }

    soc_mesh_destroy_all_internal(context);
    soc_rasterizer_shutdown(&context->rasterizer);
    free(context);
}

soc_result soc_context_resize_internal(
    soc_context* context,
    uint32_t width,
    uint32_t height
)
{
    if (context == NULL || width == 0u || height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_IDLE) {
        return SOC_RESULT_INVALID_STATE;
    }

    return soc_rasterizer_resize(&context->rasterizer, width, height);
}

soc_result soc_context_get_stats_internal(
    const soc_context* context,
    soc_stats* out_stats
)
{
    if (context == NULL ||
        out_stats == NULL ||
        out_stats->struct_size < SOC_STATS_SIZE_V1) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    out_stats->hiz_level_count = context->stats.hiz_level_count;
    out_stats->input_triangle_count = context->stats.input_triangle_count;
    out_stats->clipped_triangle_count = context->stats.clipped_triangle_count;
    out_stats->rasterized_triangle_count =
        context->stats.rasterized_triangle_count;
    out_stats->tested_aabb_count = context->stats.tested_aabb_count;
    out_stats->occluded_aabb_count = context->stats.occluded_aabb_count;
    return SOC_RESULT_OK;
}
