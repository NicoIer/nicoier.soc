#include "soc_cli_obj.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(
    char* error,
    size_t error_capacity,
    const char* format,
    ...
)
{
    va_list arguments;

    if (error == NULL || error_capacity == 0u) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_capacity, format, arguments);
    va_end(arguments);
}

static int checked_size_multiply(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || (right != 0u && left > SIZE_MAX / right)) {
        return 0;
    }

    *out_result = left * right;
    return 1;
}

static int read_line(
    FILE* file,
    char** buffer,
    size_t* capacity,
    size_t* out_length
)
{
    size_t length = 0u;
    int character;

    if (file == NULL ||
        buffer == NULL ||
        capacity == NULL ||
        out_length == NULL) {
        return -1;
    }

    for (;;) {
        character = fgetc(file);
        if (character == EOF || character == '\n') {
            break;
        }
        if (character == '\0') {
            return -2;
        }

        if (length + 1u >= *capacity) {
            size_t new_capacity = *capacity == 0u ? 256u : *capacity * 2u;
            char* replacement;

            if (new_capacity <= *capacity) {
                return -1;
            }

            replacement = realloc(*buffer, new_capacity);
            if (replacement == NULL) {
                return -1;
            }
            *buffer = replacement;
            *capacity = new_capacity;
        }

        (*buffer)[length] = (char)character;
        ++length;
    }

    if (character == EOF && length == 0u) {
        return 0;
    }

    if (length + 1u >= *capacity) {
        const size_t new_capacity = length + 2u;
        char* replacement = realloc(*buffer, new_capacity);

        if (replacement == NULL) {
            return -1;
        }
        *buffer = replacement;
        *capacity = new_capacity;
    }

    (*buffer)[length] = '\0';
    *out_length = length;
    return 1;
}

static char* skip_space(char* cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor) != 0) {
        ++cursor;
    }
    return cursor;
}

static int reserve_positions(
    soc_cli_obj* mesh,
    size_t* capacity,
    size_t required
)
{
    size_t new_capacity;
    size_t element_count;
    size_t byte_count;
    float* replacement;

    if (required <= *capacity) {
        return 1;
    }

    new_capacity = *capacity == 0u ? 256u : *capacity;
    while (new_capacity < required) {
        if (new_capacity > (size_t)UINT32_MAX / 2u) {
            new_capacity = (size_t)UINT32_MAX;
            break;
        }
        new_capacity *= 2u;
    }

    if (new_capacity < required ||
        !checked_size_multiply(new_capacity, 3u, &element_count) ||
        !checked_size_multiply(element_count, sizeof(float), &byte_count)) {
        return 0;
    }

    replacement = realloc(mesh->positions, byte_count);
    if (replacement == NULL) {
        return 0;
    }

    mesh->positions = replacement;
    *capacity = new_capacity;
    return 1;
}

static int reserve_indices(
    uint32_t** indices,
    size_t* capacity,
    size_t required
)
{
    size_t new_capacity;
    size_t byte_count;
    uint32_t* replacement;

    if (required <= *capacity) {
        return 1;
    }

    new_capacity = *capacity == 0u ? 256u : *capacity;
    while (new_capacity < required) {
        if (new_capacity > (size_t)UINT32_MAX / 2u) {
            new_capacity = (size_t)UINT32_MAX;
            break;
        }
        new_capacity *= 2u;
    }

    if (new_capacity < required ||
        !checked_size_multiply(
            new_capacity,
            sizeof(uint32_t),
            &byte_count
        )) {
        return 0;
    }

    replacement = realloc(*indices, byte_count);
    if (replacement == NULL) {
        return 0;
    }

    *indices = replacement;
    *capacity = new_capacity;
    return 1;
}

static int parse_vertex(
    char* cursor,
    size_t line_number,
    soc_cli_obj* mesh,
    size_t* position_capacity,
    char* error,
    size_t error_capacity
)
{
    double components[3];
    double homogeneous_w = 1.0;
    size_t component;
    size_t base;

    for (component = 0u; component < 3u; ++component) {
        char* end;

        cursor = skip_space(cursor);
        errno = 0;
        components[component] = strtod(cursor, &end);
        if (end == cursor || errno == ERANGE) {
            set_error(
                error,
                error_capacity,
                "line %zu: invalid vertex component %zu",
                line_number,
                component + 1u
            );
            return 0;
        }
        cursor = end;
    }

    cursor = skip_space(cursor);
    if (*cursor != '\0' && *cursor != '#') {
        char* end;

        errno = 0;
        homogeneous_w = strtod(cursor, &end);
        if (end == cursor ||
            errno == ERANGE ||
            homogeneous_w == 0.0) {
            set_error(
                error,
                error_capacity,
                "line %zu: homogeneous vertex w must be non-zero",
                line_number
            );
            return 0;
        }

        cursor = skip_space(end);
        if (*cursor != '\0' && *cursor != '#') {
            set_error(
                error,
                error_capacity,
                "line %zu: vertex has more than x y z [w]",
                line_number
            );
            return 0;
        }
    }

    for (component = 0u; component < 3u; ++component) {
        components[component] /= homogeneous_w;
    }

    if (mesh->vertex_count == UINT32_MAX ||
        !reserve_positions(
            mesh,
            position_capacity,
            (size_t)mesh->vertex_count + 1u
        )) {
        set_error(
            error,
            error_capacity,
            "line %zu: too many vertices or out of memory",
            line_number
        );
        return 0;
    }

    base = (size_t)mesh->vertex_count * 3u;
    for (component = 0u; component < 3u; ++component) {
        const float value = (float)components[component];

        mesh->positions[base + component] = value;
        if (mesh->vertex_count == 0u) {
            mesh->bounds_min[component] = (double)value;
            mesh->bounds_max[component] = (double)value;
        } else {
            if ((double)value < mesh->bounds_min[component]) {
                mesh->bounds_min[component] = (double)value;
            }
            if ((double)value > mesh->bounds_max[component]) {
                mesh->bounds_max[component] = (double)value;
            }
        }
    }

    ++mesh->vertex_count;
    return 1;
}

static int parse_integer_component(
    const char* begin,
    const char* end,
    long long* out_value
)
{
    char* number_end;
    long long value;

    if (begin == end) {
        return 0;
    }

    errno = 0;
    value = strtoll(begin, &number_end, 10);
    if (number_end == begin ||
        number_end != end ||
        errno == ERANGE ||
        value == 0) {
        return 0;
    }

    *out_value = value;
    return 1;
}

static int parse_face_index(
    const char* token,
    const char* token_end,
    uint32_t vertex_count,
    uint32_t* out_index
)
{
    const char* first_slash = token;
    const char* second_slash = NULL;
    const char* cursor;
    long long position_index;

    while (first_slash < token_end && *first_slash != '/') {
        ++first_slash;
    }
    if (!parse_integer_component(
            token,
            first_slash,
            &position_index
        )) {
        return 0;
    }

    if (first_slash < token_end) {
        cursor = first_slash + 1;
        while (cursor < token_end && *cursor != '/') {
            ++cursor;
        }
        if (cursor < token_end) {
            long long ignored_index;

            second_slash = cursor;
            if (second_slash != first_slash + 1 &&
                !parse_integer_component(
                    first_slash + 1,
                    second_slash,
                    &ignored_index
                )) {
                return 0;
            }
            if (!parse_integer_component(
                    second_slash + 1,
                    token_end,
                    &ignored_index
                )) {
                return 0;
            }
            for (cursor = second_slash + 1;
                 cursor < token_end;
                 ++cursor) {
                if (*cursor == '/') {
                    return 0;
                }
            }
        } else {
            long long ignored_index;

            if (!parse_integer_component(
                    first_slash + 1,
                    token_end,
                    &ignored_index
                )) {
                return 0;
            }
        }
    }

    if (position_index > 0) {
        if ((unsigned long long)position_index >
            (unsigned long long)vertex_count) {
            return 0;
        }
        *out_index = (uint32_t)(position_index - 1);
        return 1;
    }

    if (position_index < -(long long)vertex_count) {
        return 0;
    }
    *out_index = (uint32_t)(
        (long long)vertex_count + position_index
    );
    return 1;
}

static int parse_face(
    char* cursor,
    size_t line_number,
    soc_cli_obj* mesh,
    size_t* index_capacity,
    char* error,
    size_t error_capacity
)
{
    uint32_t* face_indices = NULL;
    size_t face_capacity = 0u;
    size_t face_count = 0u;
    size_t triangle;
    int success = 0;

    for (;;) {
        char* token_end;
        uint32_t index;

        cursor = skip_space(cursor);
        if (*cursor == '\0' || *cursor == '#') {
            break;
        }

        token_end = cursor;
        while (*token_end != '\0' &&
               *token_end != '#' &&
               isspace((unsigned char)*token_end) == 0) {
            ++token_end;
        }

        if (!parse_face_index(
                cursor,
                token_end,
                mesh->vertex_count,
                &index
            )) {
            set_error(
                error,
                error_capacity,
                "line %zu: invalid or out-of-range face index",
                line_number
            );
            goto cleanup;
        }

        if (face_count == UINT32_MAX ||
            !reserve_indices(
                &face_indices,
                &face_capacity,
                face_count + 1u
            )) {
            set_error(
                error,
                error_capacity,
                "line %zu: face is too large or out of memory",
                line_number
            );
            goto cleanup;
        }

        face_indices[face_count] = index;
        ++face_count;
        cursor = token_end;
    }

    if (face_count < 3u) {
        set_error(
            error,
            error_capacity,
            "line %zu: a face must contain at least three vertices",
            line_number
        );
        goto cleanup;
    }

    if (face_count - 2u >
        ((size_t)UINT32_MAX - (size_t)mesh->index_count) / 3u) {
        set_error(
            error,
            error_capacity,
            "line %zu: triangulated index count exceeds uint32",
            line_number
        );
        goto cleanup;
    }

    if (!reserve_indices(
            &mesh->indices,
            index_capacity,
            (size_t)mesh->index_count + (face_count - 2u) * 3u
        )) {
        set_error(
            error,
            error_capacity,
            "line %zu: out of memory while triangulating face",
            line_number
        );
        goto cleanup;
    }

    for (triangle = 0u; triangle + 2u < face_count; ++triangle) {
        mesh->indices[mesh->index_count] = face_indices[0];
        mesh->indices[mesh->index_count + 1u] =
            face_indices[triangle + 1u];
        mesh->indices[mesh->index_count + 2u] =
            face_indices[triangle + 2u];
        mesh->index_count += 3u;
    }

    success = 1;

cleanup:
    free(face_indices);
    return success;
}

int soc_cli_obj_load(
    const char* path,
    soc_cli_obj* out_mesh,
    char* error,
    size_t error_capacity
)
{
    FILE* file = NULL;
    soc_cli_obj mesh;
    char* line = NULL;
    size_t line_capacity = 0u;
    size_t line_length = 0u;
    size_t line_number = 0u;
    size_t position_capacity = 0u;
    size_t index_capacity = 0u;
    int status;
    int success = 0;

    if (error != NULL && error_capacity > 0u) {
        error[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || out_mesh == NULL) {
        set_error(error, error_capacity, "OBJ path and output mesh are required");
        return 0;
    }

    memset(out_mesh, 0, sizeof(*out_mesh));
    memset(&mesh, 0, sizeof(mesh));

    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(
            error,
            error_capacity,
            "cannot open OBJ '%s': %s",
            path,
            strerror(errno)
        );
        goto cleanup;
    }

    for (;;) {
        char* cursor;

        status = read_line(
            file,
            &line,
            &line_capacity,
            &line_length
        );
        if (status == 0) {
            break;
        }
        if (status < 0) {
            if (status == -2) {
                set_error(
                    error,
                    error_capacity,
                    "line %zu: embedded NUL is not valid OBJ text",
                    line_number + 1u
                );
            } else {
                set_error(
                    error,
                    error_capacity,
                    "out of memory while reading OBJ"
                );
            }
            goto cleanup;
        }

        (void)line_length;
        ++line_number;
        cursor = skip_space(line);
        if (*cursor == '\0' || *cursor == '#') {
            continue;
        }

        if (cursor[0] == 'v' &&
            isspace((unsigned char)cursor[1]) != 0) {
            if (!parse_vertex(
                    cursor + 1,
                    line_number,
                    &mesh,
                    &position_capacity,
                    error,
                    error_capacity
                )) {
                goto cleanup;
            }
        } else if (cursor[0] == 'f' &&
                   isspace((unsigned char)cursor[1]) != 0) {
            if (!parse_face(
                    cursor + 1,
                    line_number,
                    &mesh,
                    &index_capacity,
                    error,
                    error_capacity
                )) {
                goto cleanup;
            }
        }
    }

    if (ferror(file) != 0) {
        set_error(
            error,
            error_capacity,
            "error while reading OBJ '%s'",
            path
        );
        goto cleanup;
    }
    if (mesh.vertex_count == 0u) {
        set_error(error, error_capacity, "OBJ contains no vertices");
        goto cleanup;
    }
    if (mesh.index_count == 0u) {
        set_error(error, error_capacity, "OBJ contains no polygon faces");
        goto cleanup;
    }

    *out_mesh = mesh;
    memset(&mesh, 0, sizeof(mesh));
    success = 1;

cleanup:
    if (file != NULL && fclose(file) != 0 && success != 0) {
        soc_cli_obj_destroy(out_mesh);
        set_error(
            error,
            error_capacity,
            "cannot close OBJ '%s': %s",
            path,
            strerror(errno)
        );
        success = 0;
    }
    free(line);
    soc_cli_obj_destroy(&mesh);
    return success;
}

void soc_cli_obj_destroy(soc_cli_obj* mesh)
{
    if (mesh == NULL) {
        return;
    }

    free(mesh->positions);
    free(mesh->indices);
    memset(mesh, 0, sizeof(*mesh));
}
