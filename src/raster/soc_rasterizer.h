#ifndef SOC_RASTERIZER_H_INCLUDED
#define SOC_RASTERIZER_H_INCLUDED

#include <soc/soc.h>

typedef struct soc_rasterizer {
    uint32_t width;
    uint32_t height;
} soc_rasterizer;

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height
);

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer);

#endif
