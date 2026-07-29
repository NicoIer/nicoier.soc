#include "raster/soc_rasterizer.h"

#include <stddef.h>

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
)
{
    if (rasterizer == NULL || width == 0u || height == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    rasterizer->width = width;
    rasterizer->height = height;
    return SOC_RESULT_OK;
}

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer)
{
    if (rasterizer == NULL) {
        return;
    }

    rasterizer->width = 0u;
    rasterizer->height = 0u;
}
