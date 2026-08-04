#ifndef SOC_CONTEXT_H_INCLUDED
#define SOC_CONTEXT_H_INCLUDED

#include <soc/soc.h>

#include "core/soc_cpu_features.h"

struct soc_context {
    uint32_t width;
    uint32_t height;
    uint32_t worker_count;
    soc_cpu_features cpu_features;
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

#endif
