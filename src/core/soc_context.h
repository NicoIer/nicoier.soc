#ifndef SOC_CONTEXT_H_INCLUDED
#define SOC_CONTEXT_H_INCLUDED

#include <soc/soc.h>

#include "raster/soc_rasterizer.h"

struct soc_context {
    soc_rasterizer rasterizer;
};

soc_result soc_context_create_internal(
    const soc_config* config,
    soc_context** out_context
);

void soc_context_destroy_internal(soc_context* context);

#endif
