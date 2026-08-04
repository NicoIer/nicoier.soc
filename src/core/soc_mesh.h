#ifndef SOC_MESH_H_INCLUDED
#define SOC_MESH_H_INCLUDED

#include <soc/soc.h>

struct soc_mesh {
    soc_context* owner;
    soc_mesh* next;
    uint32_t flags;
    uint32_t vertex_count;
    uint32_t index_count;
    soc_index_type index_type;
    soc_bool positions_all_finite;
    float* positions_xyz;
    void* indices;
};

soc_result soc_mesh_create_internal(
    soc_context* context,
    const soc_mesh_desc* desc,
    soc_mesh** out_mesh
);

soc_result soc_mesh_destroy_internal(soc_mesh* mesh);

void soc_mesh_destroy_all_internal(soc_context* context);

#endif
