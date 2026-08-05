#include "core/soc_context.h"

#include "core/soc_mesh.h"

#include <stddef.h>
#include <stdlib.h>

#if defined(_WIN32)
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #if !defined(_WIN32_WINNT)
        #define _WIN32_WINNT 0x0601
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

static uint32_t automatic_worker_count(void)
{
#if defined(_WIN32)
    const DWORD detected = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (detected == 0u) {
        return 1u;
    }
    if (detected > SOC_MAX_WORKER_COUNT) {
        return SOC_MAX_WORKER_COUNT;
    }
    return (uint32_t)detected;
#else
    const long detected = sysconf(_SC_NPROCESSORS_ONLN);
    if (detected < 1) {
        return 1u;
    }
    if ((unsigned long)detected > (unsigned long)SOC_MAX_WORKER_COUNT) {
        return SOC_MAX_WORKER_COUNT;
    }
    return (uint32_t)detected;
#endif
}

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
    soc_result result;

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

    if (config->worker_count > SOC_MAX_WORKER_COUNT ||
        config->flags != SOC_CONFIG_FLAG_NONE) {
        return SOC_RESULT_UNSUPPORTED;
    }

    soc_context* context = calloc(1u, sizeof(*context));
    if (context == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    context->width = config->width;
    context->height = config->height;
    context->worker_count = config->worker_count != 0u
        ? config->worker_count
        : automatic_worker_count();
    context->cpu_features = soc_cpu_features_detect();
    context->kernels = forced_kernels != NULL
        ? forced_kernels
        : soc_kernel_table_select(&context->cpu_features);
    if (!kernel_table_is_valid(context->kernels)) {
        free(context);
        return SOC_RESULT_INTERNAL_ERROR;
    }

    result = soc_thread_pool_initialize(
        &context->thread_pool,
        context->worker_count
    );
    if (result != SOC_RESULT_OK) {
        free(context);
        return result;
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

    soc_thread_pool_shutdown(&context->thread_pool);
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

soc_result soc_context_get_runtime_info_internal(
    const soc_context* context,
    soc_runtime_info* out_info
)
{
    soc_execution_backend execution_backend;

    if (context == NULL ||
        out_info == NULL ||
        out_info->struct_size < SOC_RUNTIME_INFO_SIZE_V1) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->kernels == NULL) {
        return SOC_RESULT_INTERNAL_ERROR;
    }

    if (context->kernels->backend == SOC_KERNEL_BACKEND_SCALAR) {
        execution_backend = SOC_EXECUTION_BACKEND_SCALAR;
    } else if (context->kernels->backend == SOC_KERNEL_BACKEND_NEON) {
        execution_backend = SOC_EXECUTION_BACKEND_NEON;
    } else {
        return SOC_RESULT_INTERNAL_ERROR;
    }

    out_info->cpu_architecture = context->cpu_features.architecture;
    out_info->cpu_features = context->cpu_features.flags;
    out_info->execution_backend = execution_backend;
    out_info->worker_count = context->worker_count;
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
