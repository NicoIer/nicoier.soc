#ifndef SOC_CONTEXT_H_INCLUDED
#define SOC_CONTEXT_H_INCLUDED

#include <soc/soc.h>

#include "core/soc_cpu_features.h"
#include "core/soc_kernels.h"
#include "platform/soc_thread_pool.h"

struct soc_context {
    uint32_t width;
    uint32_t height;
    uint32_t worker_count;
    soc_cpu_features cpu_features;
    const soc_kernel_table* kernels;
    soc_thread_pool thread_pool;
    soc_mesh* meshes;
};

soc_result soc_context_create_internal(
    const soc_config* config,
    soc_context** out_context
);

void soc_context_destroy_internal(soc_context* context);

soc_result soc_context_resize_internal(
    soc_context* context,
    uint32_t width,
    uint32_t height
);

soc_result soc_context_get_runtime_info_internal(
    const soc_context* context,
    soc_runtime_info* out_info
);

/* Internal differential-test constructor; never exposed through the C ABI. */
soc_result soc_context_create_for_backend_for_testing_internal(
    const soc_config* config,
    soc_kernel_backend backend,
    soc_context** out_context
);

#endif
