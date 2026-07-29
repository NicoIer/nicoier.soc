#include "core/soc_context.h"

#include <stddef.h>
#include <stdlib.h>

soc_result soc_context_create_internal(
    const soc_config* config,
    soc_context** out_context
)
{
    soc_context* context;
    soc_result result;

    if (out_context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *out_context = NULL;

    if (config == NULL ||
        config->struct_size < sizeof(soc_config) ||
        config->width == 0u ||
        config->height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    if (config->flags != 0u) {
        return SOC_RESULT_UNSUPPORTED;
    }

    context = calloc(1u, sizeof(*context));
    if (context == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    result = soc_rasterizer_initialize(
        &context->rasterizer,
        config->width,
        config->height
    );
    if (result != SOC_RESULT_OK) {
        free(context);
        return result;
    }

    *out_context = context;
    return SOC_RESULT_OK;
}

void soc_context_destroy_internal(soc_context* context)
{
    if (context == NULL) {
        return;
    }

    soc_rasterizer_shutdown(&context->rasterizer);
    free(context);
}
