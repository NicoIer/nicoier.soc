#ifndef SOC_HIZ_H_INCLUDED
#define SOC_HIZ_H_INCLUDED

#include <soc/soc.h>

#include <stddef.h>

struct soc_kernel_table;

#define SOC_HIZ_MAX_LEVEL_COUNT 33u

typedef struct soc_hiz_level {
    uint32_t width;
    uint32_t height;
    size_t offset;
    size_t element_count;
} soc_hiz_level;

typedef struct soc_hiz {
    uint32_t level_count;
    size_t element_count;
    float* data;
    soc_bool initialized;
    soc_hiz_level levels[SOC_HIZ_MAX_LEVEL_COUNT];
} soc_hiz;

soc_result soc_hiz_initialize(
    soc_hiz* hiz,
    uint32_t width,
    uint32_t height
);

void soc_hiz_shutdown(soc_hiz* hiz);

float* soc_hiz_level_data(soc_hiz* hiz, uint32_t level);

soc_result soc_hiz_clear_level_zero(
    soc_hiz* hiz,
    soc_depth_direction depth_direction
);

soc_result soc_hiz_build(
    soc_hiz* hiz,
    soc_depth_direction depth_direction
);

soc_result soc_hiz_build_with_kernels(
    soc_hiz* hiz,
    soc_depth_direction depth_direction,
    const struct soc_kernel_table* kernels
);

void soc_hiz_reduce_level_scalar(
    const float* source,
    uint32_t source_width,
    uint32_t source_height,
    float* destination,
    soc_depth_direction depth_direction
);

soc_result soc_hiz_query(
    const soc_hiz* hiz,
    uint32_t level,
    soc_hiz_level_info* out_info,
    float* out_depth,
    uint64_t out_depth_count
);

#endif
