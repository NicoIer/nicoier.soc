#include "core/soc_mesh.h"

#include "core/soc_context.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static soc_bool checked_size_multiply(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || (right != 0u && left > SIZE_MAX / right)) {
        return SOC_FALSE;
    }

    *out_result = left * right;
    return SOC_TRUE;
}

static soc_bool checked_size_add(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || left > SIZE_MAX - right) {
        return SOC_FALSE;
    }

    *out_result = left + right;
    return SOC_TRUE;
}

static soc_result validate_mesh_desc(const soc_mesh_desc* desc)
{
    if (desc == NULL ||
        desc->struct_size < SOC_MESH_DESC_SIZE_V1 ||
        desc->vertices == NULL ||
        desc->indices == NULL ||
        desc->vertex_count == 0u ||
        desc->index_count < 3u ||
        desc->index_count % 3u != 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    if ((size_t)desc->position_offset > (size_t)desc->vertex_stride ||
        (size_t)desc->vertex_stride - (size_t)desc->position_offset <
            3u * sizeof(float)) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    if (desc->index_type != SOC_INDEX_UINT16 &&
        desc->index_type != SOC_INDEX_UINT32) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    if ((desc->flags & ~SOC_MESH_FLAG_TWO_SIDED) != 0u) {
        return SOC_RESULT_UNSUPPORTED;
    }

    return SOC_RESULT_OK;
}

static soc_result calculate_mesh_storage_sizes(
    const soc_mesh_desc* desc,
    size_t* out_position_bytes,
    size_t* out_index_bytes
)
{
    const size_t position_size = 3u * sizeof(float);
    const size_t index_size = desc->index_type == SOC_INDEX_UINT16
        ? sizeof(uint16_t)
        : sizeof(uint32_t);
    size_t last_vertex_offset;
    size_t last_position_offset;

    if (!checked_size_multiply(
            (size_t)desc->vertex_count,
            position_size,
            out_position_bytes
        ) ||
        !checked_size_multiply(
            (size_t)desc->index_count,
            index_size,
            out_index_bytes
        ) ||
        !checked_size_multiply(
            (size_t)(desc->vertex_count - 1u),
            (size_t)desc->vertex_stride,
            &last_vertex_offset
        ) ||
        !checked_size_add(
            last_vertex_offset,
            (size_t)desc->position_offset,
            &last_position_offset
        ) ||
        last_position_offset > SIZE_MAX - position_size) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    return SOC_RESULT_OK;
}

static uint32_t read_mesh_index(
    const soc_mesh* mesh,
    uint32_t index
)
{
    const unsigned char* source = mesh->indices;

    if (mesh->index_type == SOC_INDEX_UINT16) {
        uint16_t value;
        memcpy(&value, source + (size_t)index * sizeof(value), sizeof(value));
        return value;
    }

    uint32_t value;
    memcpy(&value, source + (size_t)index * sizeof(value), sizeof(value));
    return value;
}

static void free_mesh_storage(soc_mesh* mesh)
{
    if (mesh == NULL) {
        return;
    }

    free(mesh->indices);
    free(mesh->positions_xyz);
    mesh->indices = NULL;
    mesh->positions_xyz = NULL;
}

static void free_mesh(soc_mesh* mesh)
{
    if (mesh == NULL) {
        return;
    }

    free_mesh_storage(mesh);
    free(mesh);
}

soc_result soc_mesh_create_internal(
    soc_context* context,
    const soc_mesh_desc* desc,
    soc_mesh** out_mesh
)
{
    soc_mesh* mesh;
    soc_result result;
    size_t position_bytes;
    size_t index_bytes;
    uint32_t index;

    if (out_mesh == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *out_mesh = NULL;

    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_IDLE) {
        return SOC_RESULT_INVALID_STATE;
    }

    result = validate_mesh_desc(desc);
    if (result != SOC_RESULT_OK) {
        return result;
    }

    result = calculate_mesh_storage_sizes(
        desc,
        &position_bytes,
        &index_bytes
    );
    if (result != SOC_RESULT_OK) {
        return result;
    }

    mesh = calloc(1u, sizeof(*mesh));
    if (mesh == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    mesh->flags = desc->flags;
    mesh->vertex_count = desc->vertex_count;
    mesh->index_count = desc->index_count;
    mesh->index_type = desc->index_type;

    mesh->indices = malloc(index_bytes);
    if (mesh->indices == NULL) {
        free_mesh(mesh);
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    memcpy(mesh->indices, desc->indices, index_bytes);

    for (index = 0u; index < mesh->index_count; ++index) {
        if (read_mesh_index(mesh, index) >= mesh->vertex_count) {
            free_mesh(mesh);
            return SOC_RESULT_INVALID_ARGUMENT;
        }
    }

    mesh->positions_xyz = malloc(position_bytes);
    if (mesh->positions_xyz == NULL) {
        free_mesh(mesh);
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    for (index = 0u; index < mesh->vertex_count; ++index) {
        const unsigned char* source =
            (const unsigned char*)desc->vertices +
            (size_t)index * desc->vertex_stride +
            desc->position_offset;
        const size_t destination = (size_t)index * 3u;
        float position[3];

        memcpy(position, source, sizeof(position));
        mesh->positions_xyz[destination] = position[0];
        mesh->positions_xyz[destination + 1u] = position[1];
        mesh->positions_xyz[destination + 2u] = position[2];
    }

    mesh->owner = context;
    mesh->next = context->meshes;
    context->meshes = mesh;

    *out_mesh = mesh;
    return SOC_RESULT_OK;
}

soc_result soc_mesh_destroy_internal(soc_mesh* mesh)
{
    soc_context* context;
    soc_mesh** link;

    if (mesh == NULL) {
        return SOC_RESULT_OK;
    }

    context = mesh->owner;
    if (context == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }
    if (context->state != SOC_CONTEXT_STATE_IDLE) {
        return SOC_RESULT_INVALID_STATE;
    }

    link = &context->meshes;
    while (*link != NULL && *link != mesh) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    *link = mesh->next;
    mesh->owner = NULL;
    free_mesh(mesh);
    return SOC_RESULT_OK;
}

void soc_mesh_destroy_all_internal(soc_context* context)
{
    soc_mesh* mesh;

    if (context == NULL) {
        return;
    }

    mesh = context->meshes;
    while (mesh != NULL) {
        soc_mesh* next = mesh->next;
        mesh->owner = NULL;
        free_mesh(mesh);
        mesh = next;
    }
    context->meshes = NULL;
}
