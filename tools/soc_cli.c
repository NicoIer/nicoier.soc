#include "soc_cli_obj.h"
#include "soc_cli_png.h"

#include <soc/soc.h>

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_CLI_DEFAULT_WIDTH 800u
#define SOC_CLI_DEFAULT_HEIGHT 600u
#define SOC_CLI_DEFAULT_FOV_DEGREES 60.0
#define SOC_CLI_ERROR_CAPACITY 512u
#define SOC_CLI_MAX_PIXEL_COUNT 67108864u
#define SOC_CLI_PI 3.14159265358979323846

typedef struct soc_cli_vector3d {
    double x;
    double y;
    double z;
} soc_cli_vector3d;

typedef struct soc_cli_matrix4d {
    double values[16];
} soc_cli_matrix4d;

typedef struct soc_cli_options {
    const char* input_path;
    const char* output_path;
    uint32_t width;
    uint32_t height;
    double fov_degrees;
    soc_cli_vector3d eye;
    soc_cli_vector3d target;
    soc_cli_vector3d up;
    double near_plane;
    double far_plane;
    int has_eye;
    int has_target;
    int has_near;
    int has_far;
    int two_sided;
    int reversed_z;
    soc_front_face front_face;
} soc_cli_options;

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

static void print_usage(FILE* stream, const char* executable)
{
    (void)fprintf(
        stream,
        "Usage:\n"
        "  %s --input model.obj --output depth.png [options]\n"
        "\n"
        "Required:\n"
        "  --input PATH              Input Wavefront OBJ file\n"
        "  --output PATH             Output 8-bit grayscale PNG file\n"
        "\n"
        "Image and camera:\n"
        "  --width N                 Image width (default: %u)\n"
        "  --height N                Image height (default: %u)\n"
        "  --fov DEGREES             Vertical field of view (default: %.0f)\n"
        "  --eye X Y Z               Camera position\n"
        "  --target X Y Z            Camera look-at target\n"
        "  --up X Y Z                Camera up vector (default: 0 1 0)\n"
        "  --near N                  Positive near plane\n"
        "  --far N                   Far plane, greater than near\n"
        "\n"
        "Rasterization:\n"
        "  --two-sided               Disable face culling\n"
        "  --reversed-z              Use reversed-Z depth\n"
        "  --front-face ccw|cw       Front-face winding (default: ccw)\n"
        "  -h, --help                Show this help\n"
        "\n"
        "When camera or clip-plane values are omitted, they are derived from\n"
        "the OBJ bounding box. OBJ v/f records, negative indices, and polygon\n"
        "fan triangulation are supported.\n",
        executable,
        SOC_CLI_DEFAULT_WIDTH,
        SOC_CLI_DEFAULT_HEIGHT,
        SOC_CLI_DEFAULT_FOV_DEGREES
    );
}

static int parse_uint32(const char* text, uint32_t* out_value)
{
    char* end;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return 0;
    }

    errno = 0;
    value = strtoull(text, &end, 10);
    if (end == text ||
        *end != '\0' ||
        errno == ERANGE ||
        value == 0u ||
        value > UINT32_MAX) {
        return 0;
    }

    *out_value = (uint32_t)value;
    return 1;
}

static int parse_double_value(const char* text, double* out_value)
{
    char* end;
    double value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    errno = 0;
    value = strtod(text, &end);
    if (end == text ||
        *end != '\0' ||
        errno == ERANGE ||
        isfinite(value) == 0) {
        return 0;
    }

    *out_value = value;
    return 1;
}

static int parse_vector3(
    char* const* values,
    soc_cli_vector3d* out_vector
)
{
    return parse_double_value(values[0], &out_vector->x) &&
        parse_double_value(values[1], &out_vector->y) &&
        parse_double_value(values[2], &out_vector->z);
}

static int parse_options(
    int argc,
    char** argv,
    soc_cli_options* options,
    char* error,
    size_t error_capacity
)
{
    int argument;

    memset(options, 0, sizeof(*options));
    options->width = SOC_CLI_DEFAULT_WIDTH;
    options->height = SOC_CLI_DEFAULT_HEIGHT;
    options->fov_degrees = SOC_CLI_DEFAULT_FOV_DEGREES;
    options->up.y = 1.0;
    options->front_face = SOC_FRONT_FACE_CCW;

    for (argument = 1; argument < argc; ++argument) {
        const char* name = argv[argument];

        if (strcmp(name, "-h") == 0 || strcmp(name, "--help") == 0) {
            return 2;
        }
        if (strcmp(name, "--input") == 0) {
            if (argument + 1 >= argc) {
                set_error(error, error_capacity, "--input requires a path");
                return 0;
            }
            options->input_path = argv[++argument];
        } else if (strcmp(name, "--output") == 0) {
            if (argument + 1 >= argc) {
                set_error(error, error_capacity, "--output requires a path");
                return 0;
            }
            options->output_path = argv[++argument];
        } else if (strcmp(name, "--width") == 0) {
            if (argument + 1 >= argc ||
                !parse_uint32(argv[argument + 1], &options->width)) {
                set_error(
                    error,
                    error_capacity,
                    "%s requires a positive uint32",
                    name
                );
                return 0;
            }
            ++argument;
        } else if (strcmp(name, "--height") == 0) {
            if (argument + 1 >= argc ||
                !parse_uint32(argv[argument + 1], &options->height)) {
                set_error(
                    error,
                    error_capacity,
                    "--height requires a positive uint32"
                );
                return 0;
            }
            ++argument;
        } else if (strcmp(name, "--fov") == 0) {
            if (argument + 1 >= argc ||
                !parse_double_value(
                    argv[argument + 1],
                    &options->fov_degrees
                )) {
                set_error(
                    error,
                    error_capacity,
                    "--fov requires a finite number"
                );
                return 0;
            }
            ++argument;
        } else if (strcmp(name, "--eye") == 0) {
            if (argument + 3 >= argc ||
                !parse_vector3(argv + argument + 1, &options->eye)) {
                set_error(
                    error,
                    error_capacity,
                    "--eye requires three finite numbers"
                );
                return 0;
            }
            options->has_eye = 1;
            argument += 3;
        } else if (strcmp(name, "--target") == 0) {
            if (argument + 3 >= argc ||
                !parse_vector3(argv + argument + 1, &options->target)) {
                set_error(
                    error,
                    error_capacity,
                    "--target requires three finite numbers"
                );
                return 0;
            }
            options->has_target = 1;
            argument += 3;
        } else if (strcmp(name, "--up") == 0) {
            if (argument + 3 >= argc ||
                !parse_vector3(argv + argument + 1, &options->up)) {
                set_error(
                    error,
                    error_capacity,
                    "--up requires three finite numbers"
                );
                return 0;
            }
            argument += 3;
        } else if (strcmp(name, "--near") == 0) {
            if (argument + 1 >= argc ||
                !parse_double_value(
                    argv[argument + 1],
                    &options->near_plane
                )) {
                set_error(
                    error,
                    error_capacity,
                    "--near requires a finite number"
                );
                return 0;
            }
            options->has_near = 1;
            ++argument;
        } else if (strcmp(name, "--far") == 0) {
            if (argument + 1 >= argc ||
                !parse_double_value(
                    argv[argument + 1],
                    &options->far_plane
                )) {
                set_error(
                    error,
                    error_capacity,
                    "--far requires a finite number"
                );
                return 0;
            }
            options->has_far = 1;
            ++argument;
        } else if (strcmp(name, "--two-sided") == 0) {
            options->two_sided = 1;
        } else if (strcmp(name, "--reversed-z") == 0) {
            options->reversed_z = 1;
        } else if (strcmp(name, "--front-face") == 0) {
            if (argument + 1 >= argc) {
                set_error(
                    error,
                    error_capacity,
                    "--front-face requires ccw or cw"
                );
                return 0;
            }
            ++argument;
            if (strcmp(argv[argument], "ccw") == 0) {
                options->front_face = SOC_FRONT_FACE_CCW;
            } else if (strcmp(argv[argument], "cw") == 0) {
                options->front_face = SOC_FRONT_FACE_CW;
            } else {
                set_error(
                    error,
                    error_capacity,
                    "--front-face requires ccw or cw"
                );
                return 0;
            }
        } else {
            set_error(
                error,
                error_capacity,
                "unknown option '%s'",
                name
            );
            return 0;
        }
    }

    if (options->input_path == NULL || options->input_path[0] == '\0') {
        set_error(error, error_capacity, "--input is required");
        return 0;
    }
    if (options->output_path == NULL || options->output_path[0] == '\0') {
        set_error(error, error_capacity, "--output is required");
        return 0;
    }
    if (strcmp(options->input_path, options->output_path) == 0) {
        set_error(
            error,
            error_capacity,
            "--input and --output must name different files"
        );
        return 0;
    }
    if (options->fov_degrees <= 1.0 ||
        options->fov_degrees >= 179.0) {
        set_error(error, error_capacity, "--fov must be between 1 and 179");
        return 0;
    }
    if (options->width > SOC_CLI_PNG_MAX_DIMENSION ||
        options->height > SOC_CLI_PNG_MAX_DIMENSION) {
        set_error(
            error,
            error_capacity,
            "--width and --height must not exceed 2^31-1"
        );
        return 0;
    }
    if (options->has_near != 0 && options->near_plane <= 0.0) {
        set_error(error, error_capacity, "--near must be positive");
        return 0;
    }
    if (options->has_far != 0 && options->far_plane <= 0.0) {
        set_error(error, error_capacity, "--far must be positive");
        return 0;
    }

    return 1;
}

static soc_cli_vector3d vector_subtract(
    soc_cli_vector3d left,
    soc_cli_vector3d right
)
{
    const soc_cli_vector3d result = {
        left.x - right.x,
        left.y - right.y,
        left.z - right.z,
    };
    return result;
}

static double vector_dot(
    soc_cli_vector3d left,
    soc_cli_vector3d right
)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

static soc_cli_vector3d vector_cross(
    soc_cli_vector3d left,
    soc_cli_vector3d right
)
{
    const soc_cli_vector3d result = {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
    return result;
}

static int vector_normalize(
    soc_cli_vector3d input,
    soc_cli_vector3d* out_vector
)
{
    const double squared_length = vector_dot(input, input);
    double inverse_length;

    if (squared_length <= DBL_MIN || isfinite(squared_length) == 0) {
        return 0;
    }

    inverse_length = 1.0 / sqrt(squared_length);
    out_vector->x = input.x * inverse_length;
    out_vector->y = input.y * inverse_length;
    out_vector->z = input.z * inverse_length;
    return isfinite(out_vector->x) != 0 &&
        isfinite(out_vector->y) != 0 &&
        isfinite(out_vector->z) != 0;
}

static double vector_length(soc_cli_vector3d vector)
{
    const double squared_length = vector_dot(vector, vector);

    if (squared_length < 0.0 || isfinite(squared_length) == 0) {
        return HUGE_VAL;
    }
    return sqrt(squared_length);
}

static int make_look_at(
    soc_cli_vector3d eye,
    soc_cli_vector3d target,
    soc_cli_vector3d up,
    soc_cli_matrix4d* out_matrix
)
{
    soc_cli_vector3d forward;
    soc_cli_vector3d side;
    soc_cli_vector3d corrected_up;
    soc_cli_matrix4d matrix = {{0.0}};

    if (!vector_normalize(vector_subtract(target, eye), &forward) ||
        !vector_normalize(vector_cross(forward, up), &side)) {
        return 0;
    }
    corrected_up = vector_cross(side, forward);

    matrix.values[0] = side.x;
    matrix.values[1] = corrected_up.x;
    matrix.values[2] = -forward.x;
    matrix.values[4] = side.y;
    matrix.values[5] = corrected_up.y;
    matrix.values[6] = -forward.y;
    matrix.values[8] = side.z;
    matrix.values[9] = corrected_up.z;
    matrix.values[10] = -forward.z;
    matrix.values[12] = -vector_dot(side, eye);
    matrix.values[13] = -vector_dot(corrected_up, eye);
    matrix.values[14] = vector_dot(forward, eye);
    matrix.values[15] = 1.0;

    *out_matrix = matrix;
    return 1;
}

static int make_perspective(
    double vertical_fov_degrees,
    double aspect,
    double near_plane,
    double far_plane,
    int reversed_z,
    soc_cli_matrix4d* out_matrix
)
{
    const double half_angle =
        vertical_fov_degrees * SOC_CLI_PI / 360.0;
    const double focal_length = 1.0 / tan(half_angle);
    const double depth_range = far_plane - near_plane;
    soc_cli_matrix4d matrix = {{0.0}};

    if (aspect <= 0.0 ||
        near_plane <= 0.0 ||
        far_plane <= near_plane ||
        isfinite(focal_length) == 0 ||
        isfinite(depth_range) == 0) {
        return 0;
    }

    matrix.values[0] = focal_length / aspect;
    matrix.values[5] = focal_length;
    if (reversed_z != 0) {
        matrix.values[10] = near_plane / depth_range;
        matrix.values[14] = near_plane * far_plane / depth_range;
    } else {
        matrix.values[10] = -far_plane / depth_range;
        matrix.values[14] = -near_plane * far_plane / depth_range;
    }
    matrix.values[11] = -1.0;

    *out_matrix = matrix;
    return 1;
}

static soc_cli_matrix4d matrix_multiply(
    const soc_cli_matrix4d* left,
    const soc_cli_matrix4d* right
)
{
    soc_cli_matrix4d result = {{0.0}};
    size_t column;
    size_t row;
    size_t inner;

    for (column = 0u; column < 4u; ++column) {
        for (row = 0u; row < 4u; ++row) {
            for (inner = 0u; inner < 4u; ++inner) {
                result.values[column * 4u + row] +=
                    left->values[inner * 4u + row] *
                    right->values[column * 4u + inner];
            }
        }
    }

    return result;
}

static int matrix_to_soc(
    const soc_cli_matrix4d* source,
    soc_mat4* out_matrix
)
{
    float values[16];
    size_t index;

    for (index = 0u; index < 16u; ++index) {
        if (isfinite(source->values[index]) == 0 ||
            source->values[index] < -(double)FLT_MAX ||
            source->values[index] > (double)FLT_MAX) {
            return 0;
        }
        values[index] = (float)source->values[index];
    }

    out_matrix->col0 = (soc_vector4){
        values[0], values[1], values[2], values[3],
    };
    out_matrix->col1 = (soc_vector4){
        values[4], values[5], values[6], values[7],
    };
    out_matrix->col2 = (soc_vector4){
        values[8], values[9], values[10], values[11],
    };
    out_matrix->col3 = (soc_vector4){
        values[12], values[13], values[14], values[15],
    };
    return 1;
}

static soc_mat4 identity_matrix(void)
{
    const soc_mat4 matrix = {
        .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
        .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
        .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
        .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    return matrix;
}

static int resolve_camera(
    const soc_cli_obj* object,
    soc_cli_options* options,
    soc_mat4* out_clip_from_world,
    char* error,
    size_t error_capacity
)
{
    const soc_cli_vector3d bounds_min = {
        object->bounds_min[0],
        object->bounds_min[1],
        object->bounds_min[2],
    };
    const soc_cli_vector3d bounds_max = {
        object->bounds_max[0],
        object->bounds_max[1],
        object->bounds_max[2],
    };
    const soc_cli_vector3d center = {
        (bounds_min.x + bounds_max.x) * 0.5,
        (bounds_min.y + bounds_max.y) * 0.5,
        (bounds_min.z + bounds_max.z) * 0.5,
    };
    const soc_cli_vector3d half_extent = {
        (bounds_max.x - bounds_min.x) * 0.5,
        (bounds_max.y - bounds_min.y) * 0.5,
        (bounds_max.z - bounds_min.z) * 0.5,
    };
    const double object_radius = vector_length(half_extent);
    const double fit_radius =
        object_radius > 0.0 ? object_radius : 1.0;
    const double aspect = (double)options->width / options->height;
    const double vertical_half_angle =
        options->fov_degrees * SOC_CLI_PI / 360.0;
    const double horizontal_half_angle =
        atan(tan(vertical_half_angle) * aspect);
    const double limiting_half_angle =
        vertical_half_angle < horizontal_half_angle
            ? vertical_half_angle
            : horizontal_half_angle;
    soc_cli_matrix4d view;
    soc_cli_matrix4d projection;
    soc_cli_matrix4d combined;
    soc_cli_vector3d view_forward;
    double center_depth;

    if (isfinite(object_radius) == 0 ||
        isfinite(limiting_half_angle) == 0 ||
        limiting_half_angle <= 0.0) {
        set_error(
            error,
            error_capacity,
            "OBJ bounds cannot be represented by the camera"
        );
        return 0;
    }

    if (options->has_target == 0) {
        options->target = center;
    }
    if (options->has_eye == 0) {
        const double distance =
            fit_radius / sin(limiting_half_angle) * 1.1;

        if (isfinite(distance) == 0) {
            set_error(
                error,
                error_capacity,
                "automatic camera distance is not finite"
            );
            return 0;
        }
        options->eye = options->target;
        options->eye.z += distance;
    }

    if (!vector_normalize(
            vector_subtract(options->target, options->eye),
            &view_forward
        )) {
        set_error(error, error_capacity, "eye and target must differ");
        return 0;
    }
    center_depth = vector_dot(
        vector_subtract(center, options->eye),
        view_forward
    );
    if (isfinite(center_depth) == 0) {
        set_error(
            error,
            error_capacity,
            "camera position is too far from the OBJ"
        );
        return 0;
    }

    if (options->has_near == 0) {
        const double minimum_near = fit_radius * 0.001;
        const double candidate = center_depth - fit_radius * 1.25;

        options->near_plane =
            candidate > minimum_near ? candidate : minimum_near;
    }
    if (options->has_far == 0) {
        options->far_plane = center_depth + fit_radius * 1.25;
    }

    if (options->near_plane <= 0.0 ||
        options->far_plane <= options->near_plane ||
        isfinite(options->near_plane) == 0 ||
        isfinite(options->far_plane) == 0) {
        set_error(
            error,
            error_capacity,
            "resolved clip planes must satisfy 0 < near < far"
        );
        return 0;
    }

    if (!make_look_at(
            options->eye,
            options->target,
            options->up,
            &view
        )) {
        set_error(
            error,
            error_capacity,
            "eye and target must differ, and up must not be parallel"
        );
        return 0;
    }
    if (!make_perspective(
            options->fov_degrees,
            aspect,
            options->near_plane,
            options->far_plane,
            options->reversed_z,
            &projection
        )) {
        set_error(error, error_capacity, "cannot build projection matrix");
        return 0;
    }

    combined = matrix_multiply(&projection, &view);
    if (!matrix_to_soc(&combined, out_clip_from_world)) {
        set_error(
            error,
            error_capacity,
            "camera matrix exceeds the float range used by soc"
        );
        return 0;
    }

    return 1;
}

static int checked_pixel_count(
    uint32_t width,
    uint32_t height,
    size_t* out_count
)
{
    if (height != 0u && (size_t)width > SIZE_MAX / (size_t)height) {
        return 0;
    }

    *out_count = (size_t)width * (size_t)height;
    return 1;
}

static const char* result_name(soc_result result)
{
    switch (result) {
        case SOC_RESULT_OK:
            return "ok";
        case SOC_RESULT_INVALID_ARGUMENT:
            return "invalid argument";
        case SOC_RESULT_OUT_OF_MEMORY:
            return "out of memory";
        case SOC_RESULT_UNSUPPORTED:
            return "unsupported";
        case SOC_RESULT_INTERNAL_ERROR:
            return "internal error";
        case SOC_RESULT_INVALID_STATE:
            return "invalid state";
        case SOC_RESULT_BUFFER_TOO_SMALL:
            return "buffer too small";
        default:
            return "unknown error";
    }
}

static int check_result(
    soc_result result,
    const char* operation,
    char* error,
    size_t error_capacity
)
{
    if (result == SOC_RESULT_OK) {
        return 1;
    }

    set_error(
        error,
        error_capacity,
        "%s failed: %s (%d)",
        operation,
        result_name(result),
        (int)result
    );
    return 0;
}

static void depth_to_grayscale(
    const float* depth,
    size_t pixel_count,
    int reversed_z,
    double near_plane,
    double far_plane,
    unsigned char* pixels,
    uint64_t* out_drawn_pixel_count
)
{
    uint64_t drawn_pixel_count = 0u;
    size_t pixel;

    for (pixel = 0u; pixel < pixel_count; ++pixel) {
        double value = depth[pixel];
        double forward_depth;
        double reciprocal_distance;
        double view_distance;
        double linear_depth;

        if (isfinite(value) == 0) {
            value = reversed_z != 0 ? 0.0 : 1.0;
        }
        if ((reversed_z != 0 && value > 0.0) ||
            (reversed_z == 0 && value < 1.0)) {
            ++drawn_pixel_count;
        }

        forward_depth = reversed_z != 0 ? 1.0 - value : value;
        if (forward_depth < 0.0) {
            forward_depth = 0.0;
        } else if (forward_depth > 1.0) {
            forward_depth = 1.0;
        }

        reciprocal_distance =
            (1.0 - forward_depth) / near_plane +
            forward_depth / far_plane;
        view_distance = reciprocal_distance > 0.0
            ? 1.0 / reciprocal_distance
            : far_plane;
        linear_depth =
            (view_distance - near_plane) / (far_plane - near_plane);
        if (linear_depth < 0.0) {
            linear_depth = 0.0;
        } else if (linear_depth > 1.0) {
            linear_depth = 1.0;
        }
        pixels[pixel] =
            (unsigned char)floor(linear_depth * 255.0 + 0.5);
    }

    *out_drawn_pixel_count = drawn_pixel_count;
}

static int render(
    soc_cli_options* options,
    char* error,
    size_t error_capacity
)
{
    soc_cli_obj object;
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_snapshot* snapshot = NULL;
    float* depth = NULL;
    unsigned char* pixels = NULL;
    size_t pixel_count;
    soc_mat4 clip_from_world;
    soc_config config;
    soc_mesh_desc mesh_desc;
    soc_frame_desc frame_desc;
    soc_occluder_group group;
    soc_occlusion_build_desc build_desc;
    soc_hiz_level_info level_info;
    soc_build_stats stats;
    const soc_mat4 object_to_world = identity_matrix();
    uint64_t drawn_pixel_count = 0u;
    int success = 0;

    memset(&object, 0, sizeof(object));
    if (!checked_pixel_count(
            options->width,
            options->height,
            &pixel_count
        ) ||
        pixel_count > SOC_CLI_MAX_PIXEL_COUNT ||
        pixel_count > SIZE_MAX / sizeof(float)) {
        set_error(
            error,
            error_capacity,
            "image exceeds the 67,108,864-pixel debugging limit"
        );
        goto cleanup;
    }

    if (!soc_cli_obj_load(
            options->input_path,
            &object,
            error,
            error_capacity
        )) {
        goto cleanup;
    }
    if (!resolve_camera(
            &object,
            options,
            &clip_from_world,
            error,
            error_capacity
        )) {
        goto cleanup;
    }

    depth = malloc(pixel_count * sizeof(float));
    pixels = malloc(pixel_count);
    if (depth == NULL || pixels == NULL) {
        set_error(error, error_capacity, "out of memory for output image");
        goto cleanup;
    }

    config = (soc_config){
        .struct_size = sizeof(soc_config),
        .width = options->width,
        .height = options->height,
        .worker_count = 0u,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    if (!check_result(
            soc_context_create(&config, &context),
            "soc_context_create",
            error,
            error_capacity
        )) {
        goto cleanup;
    }

    mesh_desc = (soc_mesh_desc){
        .struct_size = sizeof(soc_mesh_desc),
        .flags = options->two_sided != 0
            ? SOC_MESH_FLAG_TWO_SIDED
            : SOC_MESH_FLAG_NONE,
        .vertices = object.positions,
        .indices = object.indices,
        .vertex_count = object.vertex_count,
        .vertex_stride = (uint32_t)(3u * sizeof(float)),
        .position_offset = 0u,
        .index_count = object.index_count,
        .index_type = SOC_INDEX_UINT32,
    };
    if (!check_result(
            soc_mesh_create(context, &mesh_desc, &mesh),
            "soc_mesh_create",
            error,
            error_capacity
        )) {
        goto cleanup;
    }

    frame_desc = (soc_frame_desc){
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = clip_from_world,
        .clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE,
        .depth_direction = options->reversed_z != 0
            ? SOC_DEPTH_REVERSED
            : SOC_DEPTH_FORWARD,
        .front_face = options->front_face,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    group = (soc_occluder_group){
        .mesh = mesh,
        .object_to_world = &object_to_world,
        .instance_count = 1u,
        .flags = SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    build_desc = (soc_occlusion_build_desc){
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = &frame_desc,
        .groups = &group,
        .group_count = 1u,
        .group_stride = sizeof(soc_occluder_group),
    };
    if (!check_result(
            soc_occlusion_build(context, &build_desc, &snapshot),
            "soc_occlusion_build",
            error,
            error_capacity
        )) {
        goto cleanup;
    }

    memset(&level_info, 0, sizeof(level_info));
    level_info.struct_size = sizeof(level_info);
    if (!check_result(
            soc_snapshot_hiz_level_query(
                snapshot,
                0u,
                &level_info,
                depth,
                (uint64_t)pixel_count
            ),
            "soc_snapshot_hiz_level_query",
            error,
            error_capacity
        )) {
        goto cleanup;
    }

    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    if (!check_result(
            soc_snapshot_get_build_stats(snapshot, &stats),
            "soc_snapshot_get_build_stats",
            error,
            error_capacity
        )) {
        goto cleanup;
    }

    depth_to_grayscale(
        depth,
        pixel_count,
        options->reversed_z,
        options->near_plane,
        options->far_plane,
        pixels,
        &drawn_pixel_count
    );
    if (!soc_cli_png_write_gray8(
            options->output_path,
            options->width,
            options->height,
            pixels,
            error,
            error_capacity
        )) {
        goto cleanup;
    }

    (void)printf(
        "output=%s size=%" PRIu32 "x%" PRIu32
        " vertices=%" PRIu32 " triangles=%" PRIu32
        " drawn_pixels=%" PRIu64
        " rasterized_triangles=%" PRIu64 "\n",
        options->output_path,
        options->width,
        options->height,
        object.vertex_count,
        object.index_count / 3u,
        drawn_pixel_count,
        stats.rasterized_triangle_count
    );
    (void)printf(
        "camera eye=(%.9g, %.9g, %.9g)"
        " target=(%.9g, %.9g, %.9g)"
        " fov=%.9g near=%.9g far=%.9g depth=%s\n",
        options->eye.x,
        options->eye.y,
        options->eye.z,
        options->target.x,
        options->target.y,
        options->target.z,
        options->fov_degrees,
        options->near_plane,
        options->far_plane,
        options->reversed_z != 0 ? "reversed" : "forward"
    );
    success = 1;

cleanup:
    soc_snapshot_destroy(snapshot);
    if (mesh != NULL) {
        (void)soc_mesh_destroy(mesh);
    }
    soc_context_destroy(context);
    free(pixels);
    free(depth);
    soc_cli_obj_destroy(&object);
    return success;
}

int main(int argc, char** argv)
{
    soc_cli_options options;
    char error[SOC_CLI_ERROR_CAPACITY];
    const int parse_result = parse_options(
        argc,
        argv,
        &options,
        error,
        sizeof(error)
    );

    if (parse_result == 2) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (parse_result == 0) {
        (void)fprintf(stderr, "soc_cli: %s\n\n", error);
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (!render(&options, error, sizeof(error))) {
        (void)fprintf(stderr, "soc_cli: %s\n", error);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
