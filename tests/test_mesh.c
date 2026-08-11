#include <soc/soc.h>

#include "core/soc_context.h"
#include "core/soc_mesh.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf( \
                stderr, \
                "%s:%d: check failed: %s\n", \
                __FILE__, \
                __LINE__, \
                #condition \
            ); \
            return 1; \
        } \
    } while (0)

#define CHECK_RESULT(expression, expected) \
    do { \
        const soc_result actual_result = (expression); \
        const soc_result expected_result = (expected); \
        if (actual_result != expected_result) { \
            fprintf( \
                stderr, \
                "%s:%d: result was %d, expected %d: %s\n", \
                __FILE__, \
                __LINE__, \
                (int)actual_result, \
                (int)expected_result, \
                #expression \
            ); \
            return 1; \
        } \
    } while (0)

static soc_context* create_context(void)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 64u,
        .height = 64u,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    soc_context* context = NULL;

    if (soc_context_create(&config, &context) != SOC_RESULT_OK) {
        return NULL;
    }
    return context;
}

static soc_mesh_desc make_mesh_desc(
    const void* vertices,
    const void* indices,
    uint32_t vertex_count,
    uint32_t vertex_stride,
    uint32_t position_offset,
    uint32_t index_count,
    soc_index_type index_type
)
{
    const soc_mesh_desc desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_NONE,
        .vertices = vertices,
        .indices = indices,
        .vertex_count = vertex_count,
        .vertex_stride = vertex_stride,
        .position_offset = position_offset,
        .index_count = index_count,
        .index_type = index_type,
    };
    return desc;
}

static int floats_equal(
    const float* actual,
    const float* expected,
    size_t count
)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (actual[index] != expected[index]) {
            return 0;
        }
    }
    return 1;
}

static int test_uint16_storage_and_explicit_destroy(void)
{
    const float expected_positions[] = {
        -1.0f, -1.0f, 0.25f,
         1.0f, -1.0f, 0.50f,
         0.0f,  1.0f, 0.75f,
    };
    const uint16_t expected_indices[] = {
        0u, 1u, 2u,
        2u, 1u, 0u,
    };
    float source_positions[9];
    uint16_t source_indices[6];
    soc_context* context;
    soc_mesh_desc desc;
    soc_mesh* mesh = NULL;

    memcpy(source_positions, expected_positions, sizeof(source_positions));
    memcpy(source_indices, expected_indices, sizeof(source_indices));
    desc = make_mesh_desc(
        source_positions,
        source_indices,
        3u,
        3u * (uint32_t)sizeof(float),
        0u,
        6u,
        SOC_INDEX_UINT16
    );

    context = create_context();
    CHECK(context != NULL);
    CHECK_RESULT(soc_mesh_create(context, &desc, &mesh), SOC_RESULT_OK);
    CHECK(mesh != NULL);
    CHECK(context->meshes == mesh);
    CHECK(mesh->owner == context);
    CHECK(mesh->next == NULL);
    CHECK(mesh->vertex_count == 3u);
    CHECK(mesh->index_count == 6u);
    CHECK(mesh->index_type == SOC_INDEX_UINT16);
    CHECK(mesh->positions_xyz != NULL);
    CHECK(mesh->indices != NULL);
    CHECK(mesh->positions_xyz != source_positions);
    CHECK(mesh->indices != source_indices);
    CHECK(floats_equal(mesh->positions_xyz, expected_positions, 9u));
    CHECK(memcmp(
        mesh->indices,
        expected_indices,
        sizeof(expected_indices)
    ) == 0);

    memset(source_positions, 0, sizeof(source_positions));
    memset(source_indices, 0, sizeof(source_indices));
    CHECK(floats_equal(mesh->positions_xyz, expected_positions, 9u));
    CHECK(memcmp(
        mesh->indices,
        expected_indices,
        sizeof(expected_indices)
    ) == 0);

    CHECK_RESULT(soc_mesh_destroy(mesh), SOC_RESULT_OK);
    CHECK(context->meshes == NULL);
    soc_context_destroy(context);
    return 0;
}

static int test_uint32_unaligned_storage_and_context_destroy(void)
{
    enum {
        vertex_count = 3,
        vertex_stride = 16,
        position_offset = 1,
    };
    const float expected_positions[] = {
        -2.0f, -3.0f, 0.125f,
         4.0f, -5.0f, 0.250f,
         6.0f,  7.0f, 0.500f,
    };
    const uint32_t expected_indices[] = {2u, 1u, 0u};
    unsigned char vertex_bytes[vertex_count * vertex_stride];
    unsigned char index_bytes[1u + sizeof(expected_indices)];
    const void* unaligned_indices = &index_bytes[1];
    soc_context* context;
    soc_mesh_desc desc;
    soc_mesh* first = NULL;
    soc_mesh* second = NULL;
    uint32_t vertex;

    memset(vertex_bytes, 0xa5, sizeof(vertex_bytes));
    for (vertex = 0u; vertex < vertex_count; ++vertex) {
        memcpy(
            &vertex_bytes[vertex * vertex_stride + position_offset],
            &expected_positions[vertex * 3u],
            3u * sizeof(float)
        );
    }
    memset(index_bytes, 0x5a, sizeof(index_bytes));
    memcpy(&index_bytes[1], expected_indices, sizeof(expected_indices));

    desc = make_mesh_desc(
        vertex_bytes,
        unaligned_indices,
        vertex_count,
        vertex_stride,
        position_offset,
        3u,
        SOC_INDEX_UINT32
    );

    context = create_context();
    CHECK(context != NULL);
    CHECK_RESULT(soc_mesh_create(context, &desc, &first), SOC_RESULT_OK);
    CHECK_RESULT(soc_mesh_create(context, &desc, &second), SOC_RESULT_OK);
    CHECK(first != NULL);
    CHECK(second != NULL);
    CHECK(context->meshes == second);
    CHECK(second->next == first);
    CHECK(first->next == NULL);

    CHECK(first->index_type == SOC_INDEX_UINT32);
    CHECK(first->positions_xyz != NULL);
    CHECK(first->indices != NULL);
    CHECK(first->positions_xyz != (const void*)vertex_bytes);
    CHECK(first->indices != unaligned_indices);
    CHECK(floats_equal(first->positions_xyz, expected_positions, 9u));
    CHECK(memcmp(
        first->indices,
        expected_indices,
        sizeof(expected_indices)
    ) == 0);
    CHECK(floats_equal(second->positions_xyz, expected_positions, 9u));
    CHECK(memcmp(
        second->indices,
        expected_indices,
        sizeof(expected_indices)
    ) == 0);

    memset(vertex_bytes, 0, sizeof(vertex_bytes));
    memset(index_bytes, 0, sizeof(index_bytes));
    CHECK(floats_equal(first->positions_xyz, expected_positions, 9u));
    CHECK(memcmp(
        first->indices,
        expected_indices,
        sizeof(expected_indices)
    ) == 0);
    CHECK(floats_equal(second->positions_xyz, expected_positions, 9u));
    CHECK(memcmp(
        second->indices,
        expected_indices,
        sizeof(expected_indices)
    ) == 0);

    soc_context_destroy(context);
    return 0;
}

static int test_invalid_indices_are_atomic(void)
{
    const float positions[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const uint16_t valid_indices_u16[] = {0u, 1u, 2u};
    const uint16_t invalid_indices_u16[] = {0u, 1u, 3u};
    const uint32_t valid_indices_u32[] = {2u, 1u, 0u};
    const uint32_t invalid_indices_u32[] = {0u, UINT32_MAX, 2u};
    soc_context* context;
    soc_mesh_desc desc;
    soc_mesh* anchor = NULL;
    soc_mesh* recovery = NULL;
    soc_mesh* rejected;

    context = create_context();
    CHECK(context != NULL);

    desc = make_mesh_desc(
        positions,
        valid_indices_u16,
        3u,
        3u * (uint32_t)sizeof(float),
        0u,
        3u,
        SOC_INDEX_UINT16
    );
    CHECK_RESULT(soc_mesh_create(context, &desc, &anchor), SOC_RESULT_OK);
    CHECK(anchor != NULL);
    CHECK(context->meshes == anchor);
    CHECK(anchor->next == NULL);

    desc.indices = invalid_indices_u16;
    rejected = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(context, &desc, &rejected),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(rejected == NULL);
    CHECK(context->meshes == anchor);
    CHECK(anchor->next == NULL);

    desc.indices = invalid_indices_u32;
    desc.index_type = SOC_INDEX_UINT32;
    rejected = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(context, &desc, &rejected),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(rejected == NULL);
    CHECK(context->meshes == anchor);
    CHECK(anchor->next == NULL);

    desc.indices = valid_indices_u32;
    CHECK_RESULT(soc_mesh_create(context, &desc, &recovery), SOC_RESULT_OK);
    CHECK(recovery != NULL);
    CHECK(context->meshes == recovery);
    CHECK(recovery->next == anchor);

    CHECK_RESULT(soc_mesh_destroy(anchor), SOC_RESULT_OK);
    CHECK(context->meshes == recovery);
    CHECK(recovery->next == NULL);
    CHECK_RESULT(soc_mesh_destroy(recovery), SOC_RESULT_OK);
    CHECK(context->meshes == NULL);
    soc_context_destroy(context);
    return 0;
}

static int test_post_transform_cache_locality_selection(void)
{
    float positions[64u * 3u];
    uint16_t high_locality_indices[96];
    uint16_t low_locality_indices[96];
    uint32_t high_locality_indices32[96];
    uint32_t low_locality_indices32[96];
    soc_context* context;
    soc_mesh_desc desc;
    uint32_t index_type;
    uint32_t index;

    for (index = 0u; index < 64u; ++index) {
        positions[index * 3u] = (float)(index & 7u) * 0.125f;
        positions[index * 3u + 1u] = (float)(index >> 3u) * 0.125f;
        positions[index * 3u + 2u] = 0.5f;
    }
    for (index = 0u; index < 32u; ++index) {
        high_locality_indices[index * 3u] = 0u;
        high_locality_indices[index * 3u + 1u] = 1u;
        high_locality_indices[index * 3u + 2u] = 2u;
        high_locality_indices32[index * 3u] = 0u;
        high_locality_indices32[index * 3u + 1u] = 1u;
        high_locality_indices32[index * 3u + 2u] = 2u;
    }
    for (index = 0u; index < 96u; ++index) {
        low_locality_indices[index] = (uint16_t)(index & 63u);
        low_locality_indices32[index] = index & 63u;
    }

    context = create_context();
    CHECK(context != NULL);
    for (index_type = 0u; index_type < 2u; ++index_type) {
        soc_mesh* high_locality = NULL;
        soc_mesh* low_locality = NULL;

        desc = make_mesh_desc(
            positions,
            index_type == 0u
                ? (const void*)high_locality_indices
                : (const void*)high_locality_indices32,
            64u,
            3u * (uint32_t)sizeof(float),
            0u,
            96u,
            index_type == 0u ? SOC_INDEX_UINT16 : SOC_INDEX_UINT32
        );
        CHECK_RESULT(
            soc_mesh_create(context, &desc, &high_locality),
            SOC_RESULT_OK
        );
        CHECK(high_locality != NULL);
        CHECK(high_locality->use_post_transform_cache == SOC_TRUE);

        desc.indices = index_type == 0u
            ? (const void*)low_locality_indices
            : (const void*)low_locality_indices32;
        CHECK_RESULT(
            soc_mesh_create(context, &desc, &low_locality),
            SOC_RESULT_OK
        );
        CHECK(low_locality != NULL);
        CHECK(low_locality->use_post_transform_cache == SOC_FALSE);

        CHECK_RESULT(soc_mesh_destroy(high_locality), SOC_RESULT_OK);
        CHECK_RESULT(soc_mesh_destroy(low_locality), SOC_RESULT_OK);
    }
    soc_context_destroy(context);
    return 0;
}

static int test_invalid_layout_is_atomic(void)
{
    const float positions[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const uint16_t indices[] = {0u, 1u, 2u};
    soc_context* context;
    soc_mesh_desc desc;
    soc_mesh* mesh;

    context = create_context();
    CHECK(context != NULL);
    desc = make_mesh_desc(
        positions,
        indices,
        3u,
        3u * (uint32_t)sizeof(float),
        0u,
        3u,
        SOC_INDEX_UINT16
    );

    desc.position_offset = desc.vertex_stride + 1u;
    mesh = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(context, &desc, &mesh),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(mesh == NULL);
    CHECK(context->meshes == NULL);

    desc.position_offset = 1u;
    mesh = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(context, &desc, &mesh),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(mesh == NULL);
    CHECK(context->meshes == NULL);

    soc_context_destroy(context);
    return 0;
}

static int test_invalid_arguments_and_flags_are_atomic(void)
{
    const float positions[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const uint16_t indices[] = {0u, 1u, 2u};
    soc_context* context;
    soc_mesh_desc desc = make_mesh_desc(
        positions,
        indices,
        3u,
        3u * (uint32_t)sizeof(float),
        0u,
        3u,
        SOC_INDEX_UINT16
    );
    soc_mesh* mesh;

    context = create_context();
    CHECK(context != NULL);

    mesh = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(NULL, &desc, &mesh),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(mesh == NULL);

    mesh = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(context, NULL, &mesh),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(mesh == NULL);
    CHECK_RESULT(
        soc_mesh_create(context, &desc, NULL),
        SOC_RESULT_INVALID_ARGUMENT
    );

    desc.struct_size = SOC_MESH_DESC_SIZE_V1 - 1u;
    mesh = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(context, &desc, &mesh),
        SOC_RESULT_INVALID_ARGUMENT
    );
    CHECK(mesh == NULL);

    desc.struct_size = sizeof(desc);
    desc.flags = SOC_MESH_FLAG_TWO_SIDED | (1u << 1u);
    mesh = (soc_mesh*)(uintptr_t)1u;
    CHECK_RESULT(
        soc_mesh_create(context, &desc, &mesh),
        SOC_RESULT_UNSUPPORTED
    );
    CHECK(mesh == NULL);
    CHECK(context->meshes == NULL);
    CHECK_RESULT(soc_mesh_destroy(NULL), SOC_RESULT_OK);

    soc_context_destroy(context);
    return 0;
}

int main(void)
{
    if (test_uint16_storage_and_explicit_destroy() != 0) {
        return 1;
    }
    if (test_uint32_unaligned_storage_and_context_destroy() != 0) {
        return 1;
    }
    if (test_invalid_indices_are_atomic() != 0) {
        return 1;
    }
    if (test_post_transform_cache_locality_selection() != 0) {
        return 1;
    }
    if (test_invalid_layout_is_atomic() != 0) {
        return 1;
    }
    if (test_invalid_arguments_and_flags_are_atomic() != 0) {
        return 1;
    }
    return 0;
}
