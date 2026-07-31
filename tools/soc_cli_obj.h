#ifndef SOC_CLI_OBJ_H_INCLUDED
#define SOC_CLI_OBJ_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

typedef struct soc_cli_obj {
    float* positions;
    uint32_t* indices;
    uint32_t vertex_count;
    uint32_t index_count;
    double bounds_min[3];
    double bounds_max[3];
} soc_cli_obj;

int soc_cli_obj_load(
    const char* path,
    soc_cli_obj* out_mesh,
    char* error,
    size_t error_capacity
);

void soc_cli_obj_destroy(soc_cli_obj* mesh);

#endif
