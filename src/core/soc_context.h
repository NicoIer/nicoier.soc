#ifndef SOC_CONTEXT_H_INCLUDED
#define SOC_CONTEXT_H_INCLUDED

#include <soc/soc.h>

#include "raster/soc_rasterizer.h"

typedef enum soc_context_state {
    SOC_CONTEXT_STATE_IDLE = 0,
    SOC_CONTEXT_STATE_RECORDING_OCCLUDERS,
    SOC_CONTEXT_STATE_QUERY_READY,
} soc_context_state;

struct soc_context {
    soc_rasterizer rasterizer;
    soc_mesh* meshes;
    soc_context_state state;
    soc_stats stats;
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

soc_result soc_context_get_stats_internal(
    const soc_context* context,
    soc_stats* out_stats
);

#endif
