#include "core/soc_context.h"

#include "core/soc_mesh.h"

#include <stddef.h>
#include <stdlib.h>

static soc_bool kernel_table_is_valid(const soc_kernel_table* kernels)
{
    return kernels != NULL &&
        (kernels->backend == SOC_KERNEL_BACKEND_SCALAR ||
            kernels->backend == SOC_KERNEL_BACKEND_NEON) &&
        kernels->clear_f32 != NULL &&
        kernels->store_constant_depth_block_f32 != NULL &&
        kernels->reduce_hiz_level_f32 != NULL &&
        kernels->transform_triangle_f64 != NULL &&
        kernels->test_aabbs != NULL;
}

static soc_result create_context(
    const soc_config* config,
    const soc_kernel_table* forced_kernels,
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
    context->cpu_features = soc_cpu_features_detect();
    context->kernels = forced_kernels != NULL
        ? forced_kernels
        : soc_kernel_table_select(&context->cpu_features);
    if (!kernel_table_is_valid(context->kernels)) {
        free(context);
        return SOC_RESULT_INTERNAL_ERROR;
    }
    *out_context = context;
    return SOC_RESULT_OK;
}

soc_result soc_context_create_internal(
    const soc_config* config,
    soc_context** out_context
)
{
    return create_context(config, NULL, out_context);
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

soc_result soc_context_create_for_backend_for_testing_internal(
    const soc_config* config,
    soc_kernel_backend backend,
    soc_context** out_context
)
{
    const soc_kernel_table* kernels;

    if (out_context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    *out_context = NULL;

    kernels = soc_kernel_table_for_backend(backend);
    if (kernels == NULL) {
        return SOC_RESULT_UNSUPPORTED;
    }
    return create_context(config, kernels, out_context);
}
