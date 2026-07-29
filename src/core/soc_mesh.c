#include "core/soc_mesh.h"

#include "core/soc_context.h"

#include <stddef.h>
#include <stdlib.h>

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

    if (desc->position_offset > desc->vertex_stride ||
        desc->vertex_stride - desc->position_offset <
            3u * (uint32_t)sizeof(float)) {
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

soc_result soc_mesh_create_internal(
    soc_context* context,
    const soc_mesh_desc* desc,
    soc_mesh** out_mesh
)
{
    soc_mesh* mesh;
    soc_result result;

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

    mesh = calloc(1u, sizeof(*mesh));
    if (mesh == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }

    /*
     * Framework only: vertex and index payloads will be copied when the
     * rasterizer implementation is added.
     */
    mesh->owner = context;
    mesh->flags = desc->flags;
    mesh->vertex_count = desc->vertex_count;
    mesh->index_count = desc->index_count;
    mesh->index_type = desc->index_type;
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
    free(mesh);
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
        free(mesh);
        mesh = next;
    }
    context->meshes = NULL;
}
