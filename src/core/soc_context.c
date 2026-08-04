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
        config->height == 0u ||
        config->width > SOC_MAX_RASTER_DIMENSION ||
        config->height > SOC_MAX_RASTER_DIMENSION) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    if (config->worker_count > 1u || config->flags != SOC_CONFIG_FLAG_NONE) {
        return SOC_RESULT_UNSUPPORTED;
    }

    soc_context* context = calloc(1u, sizeof(*context));
    if (context == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    context->width = config->width;
    context->height = config->height;
    context->worker_count = config->worker_count;
    *out_context = context;
    return SOC_RESULT_OK;
}

void soc_context_destroy_internal(soc_context* context)
{
    if (context == NULL) {
        return;
    }

    soc_mesh_destroy_all_internal(context);
    free(context);
}

soc_result soc_context_resize_internal(
    soc_context* context,
    uint32_t width,
    uint32_t height
)
{
    if (context == NULL ||
        width == 0u ||
        height == 0u ||
        width > SOC_MAX_RASTER_DIMENSION ||
        height > SOC_MAX_RASTER_DIMENSION) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    context->width = width;
    context->height = height;
    return SOC_RESULT_OK;
}
