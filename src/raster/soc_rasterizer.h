#ifndef SOC_RASTERIZER_H_INCLUDED
#define SOC_RASTERIZER_H_INCLUDED

#include <soc/soc.h>

#include "core/soc_kernels.h"

#include <stddef.h>

typedef struct soc_rasterizer {
    uint32_t width;
    uint32_t height;
    size_t depth_element_count;
    /* Borrowed Level 0 storage owned by the in-progress snapshot. */
    float* depth;
    const soc_kernel_table* kernels;
    uint64_t clipped_triangle_count;
    uint64_t rasterized_triangle_count;
    soc_bool initialized;
    soc_bool frame_active;
    soc_frame_desc frame;
} soc_rasterizer;

soc_result soc_rasterizer_initialize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count,
    const soc_kernel_table* kernels
);

void soc_rasterizer_shutdown(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_resize(
    soc_rasterizer* rasterizer,
    uint32_t width,
    uint32_t height,
    float* depth,
    size_t depth_element_count
);

soc_result soc_rasterizer_begin_frame(
    soc_rasterizer* rasterizer,
    const soc_frame_desc* desc
);

soc_result soc_rasterizer_submit_occluders(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count
);

soc_result soc_rasterizer_finish_occluders(soc_rasterizer* rasterizer);

soc_result soc_rasterizer_end_frame(soc_rasterizer* rasterizer);

#endif
