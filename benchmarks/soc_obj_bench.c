#include "soc_cli_obj.h"

#include <soc/soc.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

#define DEFAULT_SAMPLE_COUNT 15u
#define DEFAULT_SAMPLE_MS 200u
#define WARMUP_TARGET_NS UINT64_C(250000000)
#define METADATA_LINE_CAPACITY 1024u

#define METADATA_MAGIC_SEEN (1u << 0u)
#define METADATA_WIDTH_SEEN (1u << 1u)
#define METADATA_HEIGHT_SEEN (1u << 2u)
#define METADATA_CLIP_RANGE_SEEN (1u << 3u)
#define METADATA_FRONT_FACE_SEEN (1u << 4u)
#define METADATA_TWO_SIDED_SEEN (1u << 5u)
#define METADATA_MATRIX_SEEN (1u << 6u)
#define METADATA_REQUIRED_MASK ((1u << 7u) - 1u)

typedef struct obj_metadata {
    uint32_t width;
    uint32_t height;
    soc_clip_depth_range clip_depth_range;
    soc_front_face front_face;
    uint32_t mesh_flags;
    soc_mat4 clip_from_world;
} obj_metadata;

typedef struct options {
    const char* input_path;
    uint32_t sample_count;
    uint32_t sample_ms;
    uint32_t worker_count;
} options;

typedef struct validation_result {
    soc_build_stats stats;
    uint64_t drawn_pixel_count;
    uint64_t checksum;
} validation_result;

static void print_usage(FILE* stream, const char* executable)
{
    (void)fprintf(
        stream,
        "Usage: %s --input benchmark.obj [--samples N] [--sample-ms N] "
        "[--workers N]\n"
        "\n"
        "Omitting --workers uses the online logical CPU count; 1 is serial.\n"
        "The worker count includes the thread which calls the build.\n"
        "The OBJ must contain the '# SOC benchmark OBJ v2' metadata header.\n"
        "Its camera matrix must use reversed Z (near = 1; far = 0 for ZO\n"
        "or -1 for negative-one-to-one clip depth).\n"
        "Each timed operation builds a complete immutable snapshot, including\n"
        "Level 0 clear, rasterization, and Hi-Z construction. Parsing, mesh and\n"
        "context creation, readback, validation, and destruction are outside timing.\n",
        executable
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
    if (end == text || errno == ERANGE || value == 0u || value > UINT32_MAX) {
        return 0;
    }
    while (*end != '\0' && isspace((unsigned char)*end) != 0) {
        ++end;
    }
    if (*end != '\0') {
        return 0;
    }
    *out_value = (uint32_t)value;
    return 1;
}

static int parse_options(
    int argc,
    char** argv,
    options* out_options
)
{
    int argument;

    memset(out_options, 0, sizeof(*out_options));
    out_options->sample_count = DEFAULT_SAMPLE_COUNT;
    out_options->sample_ms = DEFAULT_SAMPLE_MS;
    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "-h") == 0 ||
            strcmp(argv[argument], "--help") == 0) {
            return 2;
        }
        if (strcmp(argv[argument], "--input") == 0) {
            if (++argument >= argc) {
                return 0;
            }
            out_options->input_path = argv[argument];
        } else if (strcmp(argv[argument], "--samples") == 0) {
            if (++argument >= argc ||
                !parse_uint32(argv[argument], &out_options->sample_count)) {
                return 0;
            }
        } else if (strcmp(argv[argument], "--sample-ms") == 0) {
            if (++argument >= argc ||
                !parse_uint32(argv[argument], &out_options->sample_ms)) {
                return 0;
            }
        } else if (strcmp(argv[argument], "--workers") == 0) {
            if (++argument >= argc ||
                !parse_uint32(argv[argument], &out_options->worker_count) ||
                out_options->worker_count > SOC_MAX_WORKER_COUNT) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return out_options->input_path != NULL ? 1 : 0;
}

static const char* metadata_value(
    const char* line,
    const char* prefix
)
{
    const size_t prefix_length = strlen(prefix);

    return strncmp(line, prefix, prefix_length) == 0
        ? line + prefix_length
        : NULL;
}

static int token_equals(const char* text, const char* expected)
{
    const size_t length = strlen(expected);

    if (strncmp(text, expected, length) != 0) {
        return 0;
    }
    text += length;
    while (*text != '\0' && isspace((unsigned char)*text) != 0) {
        ++text;
    }
    return *text == '\0';
}

static int parse_flag_value(const char* text, uint32_t* out_value)
{
    char* end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || errno == ERANGE || value > 1u) {
        return 0;
    }
    while (*end != '\0' && isspace((unsigned char)*end) != 0) {
        ++end;
    }
    if (*end != '\0') {
        return 0;
    }
    *out_value = value != 0u ? SOC_MESH_FLAG_TWO_SIDED : SOC_MESH_FLAG_NONE;
    return 1;
}

static int parse_matrix(const char* text, soc_mat4* out_matrix)
{
    float values[16];
    uint32_t index;

    for (index = 0u; index < 16u; ++index) {
        char* end;

        while (*text != '\0' && isspace((unsigned char)*text) != 0) {
            ++text;
        }
        errno = 0;
        values[index] = strtof(text, &end);
        if (end == text || errno == ERANGE) {
            return 0;
        }
        text = end;
    }
    while (*text != '\0' && isspace((unsigned char)*text) != 0) {
        ++text;
    }
    if (*text != '\0') {
        return 0;
    }
    out_matrix->col0 = (soc_vector4){
        values[0], values[1], values[2], values[3]
    };
    out_matrix->col1 = (soc_vector4){
        values[4], values[5], values[6], values[7]
    };
    out_matrix->col2 = (soc_vector4){
        values[8], values[9], values[10], values[11]
    };
    out_matrix->col3 = (soc_vector4){
        values[12], values[13], values[14], values[15]
    };
    return 1;
}

static int starts_with_number(const char* text)
{
    while (*text != '\0' && isspace((unsigned char)*text) != 0) {
        ++text;
    }
    return *text == '+' || *text == '-' || *text == '.' ||
        (*text >= '0' && *text <= '9');
}

static int load_metadata(
    const char* path,
    obj_metadata* out_metadata,
    char* error,
    size_t error_capacity
)
{
    FILE* input;
    char line[METADATA_LINE_CAPACITY];
    uint32_t seen = 0u;
    const char* invalid_field = "unknown";

    memset(out_metadata, 0, sizeof(*out_metadata));
    input = fopen(path, "rb");
    if (input == NULL) {
        (void)snprintf(error, error_capacity, "cannot open '%s'", path);
        return 0;
    }
    while (fgets(line, sizeof(line), input) != NULL) {
        const char* value;

        if (strncmp(line, "# SOC benchmark OBJ v2", 22u) == 0) {
            seen |= METADATA_MAGIC_SEEN;
        } else if ((value = metadata_value(line, "# raster_width ")) != NULL) {
            if (!parse_uint32(value, &out_metadata->width)) {
                invalid_field = "raster_width";
                goto invalid_metadata;
            }
            seen |= METADATA_WIDTH_SEEN;
        } else if ((value = metadata_value(line, "# raster_height ")) != NULL) {
            if (!parse_uint32(value, &out_metadata->height)) {
                invalid_field = "raster_height";
                goto invalid_metadata;
            }
            seen |= METADATA_HEIGHT_SEEN;
        } else if ((value = metadata_value(
                        line,
                        "# soc_clip_depth_range "
                    )) != NULL) {
            if (token_equals(value, "zero_to_one")) {
                out_metadata->clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE;
            } else if (token_equals(value, "negative_one_to_one")) {
                out_metadata->clip_depth_range =
                    SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE;
            } else {
                invalid_field = "soc_clip_depth_range";
                goto invalid_metadata;
            }
            seen |= METADATA_CLIP_RANGE_SEEN;
        } else if ((value = metadata_value(line, "# soc_front_face ")) != NULL) {
            if (token_equals(value, "counter_clockwise") ||
                token_equals(value, "ccw")) {
                out_metadata->front_face = SOC_FRONT_FACE_CCW;
            } else if (token_equals(value, "clockwise") ||
                token_equals(value, "cw")) {
                out_metadata->front_face = SOC_FRONT_FACE_CW;
            } else {
                invalid_field = "soc_front_face";
                goto invalid_metadata;
            }
            seen |= METADATA_FRONT_FACE_SEEN;
        } else if ((value = metadata_value(
                        line,
                        "# soc_mesh_two_sided "
                    )) != NULL) {
            if (!parse_flag_value(value, &out_metadata->mesh_flags)) {
                invalid_field = "soc_mesh_two_sided";
                goto invalid_metadata;
            }
            seen |= METADATA_TWO_SIDED_SEEN;
        } else if ((value = metadata_value(
                        line,
                        "# camera_clip_from_world_col_major "
                    )) != NULL) {
            if (!starts_with_number(value)) {
                continue;
            }
            if (!parse_matrix(value, &out_metadata->clip_from_world)) {
                invalid_field = "camera_clip_from_world_col_major";
                goto invalid_metadata;
            }
            seen |= METADATA_MATRIX_SEEN;
        }
    }
    if (ferror(input) != 0) {
        (void)snprintf(error, error_capacity, "failed to read '%s'", path);
        (void)fclose(input);
        return 0;
    }
    (void)fclose(input);
    if (seen != METADATA_REQUIRED_MASK) {
        (void)snprintf(
            error,
            error_capacity,
            "OBJ benchmark metadata is incomplete (mask 0x%02x)",
            seen
        );
        return 0;
    }
    return 1;

invalid_metadata:
    (void)snprintf(
        error,
        error_capacity,
        "invalid OBJ benchmark metadata field '%s'",
        invalid_field
    );
    (void)fclose(input);
    return 0;
}

static int timer_initialize(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    return QueryPerformanceFrequency(&frequency) != 0 &&
        frequency.QuadPart > 0;
#elif defined(__APPLE__)
    mach_timebase_info_data_t timebase;
    return mach_timebase_info(&timebase) == KERN_SUCCESS &&
        timebase.denom != 0u;
#else
    struct timespec value;
    return clock_gettime(CLOCK_MONOTONIC, &value) == 0;
#endif
}

static uint64_t timer_now_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t quotient;
    uint64_t remainder;

    if (QueryPerformanceCounter(&counter) == 0 ||
        QueryPerformanceFrequency(&frequency) == 0 ||
        counter.QuadPart < 0 || frequency.QuadPart <= 0) {
        return UINT64_MAX;
    }
    quotient = (uint64_t)counter.QuadPart / (uint64_t)frequency.QuadPart;
    remainder = (uint64_t)counter.QuadPart % (uint64_t)frequency.QuadPart;
    return quotient * UINT64_C(1000000000) +
        remainder * UINT64_C(1000000000) / (uint64_t)frequency.QuadPart;
#elif defined(__APPLE__)
    static mach_timebase_info_data_t timebase;
    const uint64_t ticks = mach_absolute_time();
    uint64_t quotient;
    uint64_t remainder;

    if (timebase.denom == 0u &&
        (mach_timebase_info(&timebase) != KERN_SUCCESS ||
         timebase.denom == 0u)) {
        return UINT64_MAX;
    }
    quotient = ticks / (uint64_t)timebase.denom;
    remainder = ticks % (uint64_t)timebase.denom;
    return quotient * (uint64_t)timebase.numer +
        remainder * (uint64_t)timebase.numer / (uint64_t)timebase.denom;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
#endif
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

static soc_result build_snapshot(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_frame_desc* frame_desc,
    const soc_mat4* object_to_world,
    soc_snapshot** out_snapshot
)
{
    const soc_occluder_group group = {
        .mesh = mesh,
        .object_to_world = object_to_world,
        .instance_count = 1u,
        .flags = SOC_OCCLUDER_GROUP_FLAG_NONE,
    };
    const soc_occlusion_build_desc build_desc = {
        .struct_size = sizeof(soc_occlusion_build_desc),
        .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
        .frame = frame_desc,
        .groups = &group,
        .group_count = 1u,
        .group_stride = sizeof(soc_occluder_group),
    };

    return soc_occlusion_build(context, &build_desc, out_snapshot);
}

static int run_snapshot_build(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_frame_desc* frame_desc,
    const soc_mat4* object_to_world,
    uint64_t* out_elapsed_ns
)
{
    soc_snapshot* snapshot = NULL;
    soc_result result;
    uint64_t begin;
    uint64_t end;

    begin = timer_now_ns();
    result = build_snapshot(
        context,
        mesh,
        frame_desc,
        object_to_world,
        &snapshot
    );
    end = timer_now_ns();
    soc_snapshot_destroy(snapshot);
    if (result != SOC_RESULT_OK ||
        begin == UINT64_MAX ||
        end == UINT64_MAX ||
        end <= begin) {
        return 0;
    }
    *out_elapsed_ns = end - begin;
    return 1;
}

static int run_series(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_frame_desc* frame_desc,
    const soc_mat4* object_to_world,
    uint64_t target_ns,
    uint64_t minimum_iterations,
    uint64_t* out_average_ns,
    uint64_t* out_iterations
)
{
    uint64_t total_ns = 0u;
    uint64_t iterations = 0u;

    do {
        uint64_t elapsed_ns;

        if (!run_snapshot_build(
                context,
                mesh,
                frame_desc,
                object_to_world,
                &elapsed_ns
            ) ||
            total_ns > UINT64_MAX - elapsed_ns ||
            iterations == UINT64_MAX) {
            return 0;
        }
        total_ns += elapsed_ns;
        ++iterations;
    } while (iterations < minimum_iterations || total_ns < target_ns);

    *out_average_ns = (total_ns + iterations / 2u) / iterations;
    *out_iterations = iterations;
    return 1;
}

static int checked_pixel_count(
    uint32_t width,
    uint32_t height,
    size_t* out_pixel_count
)
{
    if (height != 0u && (size_t)width > SIZE_MAX / (size_t)height) {
        return 0;
    }
    *out_pixel_count = (size_t)width * (size_t)height;
    return *out_pixel_count <= SIZE_MAX / sizeof(float);
}

static int capture_validation(
    soc_context* context,
    const soc_mesh* mesh,
    const soc_frame_desc* frame_desc,
    const soc_mat4* object_to_world,
    size_t pixel_count,
    validation_result* out_validation
)
{
    soc_hiz_level_info info = {.struct_size = sizeof(info)};
    soc_snapshot* snapshot = NULL;
    float* depth = NULL;
    const float clear_depth = 0.0f;
    soc_result result;
    size_t pixel;

    memset(out_validation, 0, sizeof(*out_validation));
    out_validation->stats.struct_size = sizeof(out_validation->stats);
    out_validation->checksum = UINT64_C(14695981039346656037);
    depth = (float*)malloc(pixel_count * sizeof(*depth));
    if (depth == NULL) {
        return 0;
    }
    result = build_snapshot(
        context,
        mesh,
        frame_desc,
        object_to_world,
        &snapshot
    );
    if (result == SOC_RESULT_OK) {
        result = soc_snapshot_get_build_stats(
            snapshot,
            &out_validation->stats
        );
    }
    if (result == SOC_RESULT_OK) {
        result = soc_snapshot_hiz_level_query(
            snapshot,
            0u,
            &info,
            depth,
            pixel_count
        );
    }
    if (result == SOC_RESULT_OK && info.required_element_count == pixel_count) {
        for (pixel = 0u; pixel < pixel_count; ++pixel) {
            uint32_t bits;

            if (depth[pixel] != clear_depth) {
                ++out_validation->drawn_pixel_count;
            }
            memcpy(&bits, &depth[pixel], sizeof(bits));
            out_validation->checksum ^= bits;
            out_validation->checksum *= UINT64_C(1099511628211);
        }
    } else if (result == SOC_RESULT_OK) {
        result = SOC_RESULT_INTERNAL_ERROR;
    }
    soc_snapshot_destroy(snapshot);
    free(depth);
    return result == SOC_RESULT_OK;
}

static int compare_u64(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return (a > b) - (a < b);
}

static uint64_t calculate_median(uint64_t* values, uint32_t count)
{
    qsort(values, count, sizeof(values[0]), compare_u64);
    if ((count & 1u) != 0u) {
        return values[count / 2u];
    }
    return values[count / 2u - 1u] / 2u +
        values[count / 2u] / 2u +
        ((values[count / 2u - 1u] & 1u) +
         (values[count / 2u] & 1u)) / 2u;
}

static int validation_matches(
    const validation_result* left,
    const validation_result* right
)
{
    return left->stats.hiz_level_count == right->stats.hiz_level_count &&
        left->stats.input_triangle_count == right->stats.input_triangle_count &&
        left->stats.clipped_triangle_count == right->stats.clipped_triangle_count &&
        left->stats.rasterized_triangle_count ==
            right->stats.rasterized_triangle_count &&
        left->drawn_pixel_count == right->drawn_pixel_count &&
        left->checksum == right->checksum;
}

int main(int argc, char** argv)
{
    options opts;
    obj_metadata metadata;
    soc_cli_obj object;
    soc_context* context = NULL;
    soc_mesh* mesh = NULL;
    soc_config config;
    soc_runtime_info runtime_info;
    soc_mesh_desc mesh_desc;
    soc_frame_desc frame_desc;
    const soc_mat4 object_to_world = identity_matrix();
    validation_result validation_before;
    validation_result validation_after;
    uint64_t* samples = NULL;
    uint64_t* deviations = NULL;
    uint64_t median_ns;
    uint64_t mad_ns;
    uint64_t ignored_iterations;
    uint64_t ignored_average;
    uint64_t sample_target_ns;
    size_t pixel_count;
    char error[512];
    uint32_t sample;
    int parse_result;
    int exit_code = EXIT_FAILURE;

    memset(&object, 0, sizeof(object));
    parse_result = parse_options(argc, argv, &opts);
    if (parse_result == 2) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (parse_result == 0) {
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (!timer_initialize()) {
        fprintf(stderr, "soc_obj_bench: monotonic timer initialization failed\n");
        goto cleanup;
    }
    if (!load_metadata(
            opts.input_path,
            &metadata,
            error,
            sizeof(error)
        )) {
        fprintf(stderr, "soc_obj_bench: %s\n", error);
        goto cleanup;
    }
    if (!checked_pixel_count(metadata.width, metadata.height, &pixel_count)) {
        fprintf(stderr, "soc_obj_bench: raster dimensions overflow\n");
        goto cleanup;
    }
    if (!soc_cli_obj_load(opts.input_path, &object, error, sizeof(error))) {
        fprintf(stderr, "soc_obj_bench: %s\n", error);
        goto cleanup;
    }

    config = (soc_config){
        .struct_size = sizeof(config),
        .width = metadata.width,
        .height = metadata.height,
        .worker_count = opts.worker_count,
        .flags = SOC_CONFIG_FLAG_NONE,
    };
    if (soc_context_create(&config, &context) != SOC_RESULT_OK) {
        fprintf(stderr, "soc_obj_bench: context creation failed\n");
        goto cleanup;
    }
    runtime_info = (soc_runtime_info){
        .struct_size = sizeof(soc_runtime_info),
    };
    if (soc_context_get_runtime_info(context, &runtime_info) != SOC_RESULT_OK) {
        fprintf(stderr, "soc_obj_bench: runtime info query failed\n");
        goto cleanup;
    }
    mesh_desc = (soc_mesh_desc){
        .struct_size = sizeof(mesh_desc),
        .flags = metadata.mesh_flags,
        .vertices = object.positions,
        .indices = object.indices,
        .vertex_count = object.vertex_count,
        .vertex_stride = 3u * sizeof(float),
        .position_offset = 0u,
        .index_count = object.index_count,
        .index_type = SOC_INDEX_UINT32,
    };
    if (soc_mesh_create(context, &mesh_desc, &mesh) != SOC_RESULT_OK) {
        fprintf(stderr, "soc_obj_bench: mesh creation failed\n");
        goto cleanup;
    }
    frame_desc = (soc_frame_desc){
        .struct_size = sizeof(frame_desc),
        .clip_from_world = metadata.clip_from_world,
        .clip_depth_range = metadata.clip_depth_range,
        .front_face = metadata.front_face,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    if (!capture_validation(
            context,
            mesh,
            &frame_desc,
            &object_to_world,
            pixel_count,
            &validation_before
        )) {
        fprintf(stderr, "soc_obj_bench: initial validation failed\n");
        goto cleanup;
    }
    if (!run_series(
            context,
            mesh,
            &frame_desc,
            &object_to_world,
            WARMUP_TARGET_NS,
            5u,
            &ignored_average,
            &ignored_iterations
        )) {
        fprintf(stderr, "soc_obj_bench: warmup failed\n");
        goto cleanup;
    }

    samples = (uint64_t*)calloc(opts.sample_count, sizeof(*samples));
    deviations = (uint64_t*)calloc(opts.sample_count, sizeof(*deviations));
    if (samples == NULL || deviations == NULL) {
        fprintf(stderr, "soc_obj_bench: sample allocation failed\n");
        goto cleanup;
    }
    sample_target_ns = (uint64_t)opts.sample_ms * UINT64_C(1000000);
    for (sample = 0u; sample < opts.sample_count; ++sample) {
        uint64_t iterations;

        if (!run_series(
                context,
                mesh,
                &frame_desc,
                &object_to_world,
                sample_target_ns,
                1u,
                &samples[sample],
                &iterations
            )) {
            fprintf(stderr, "soc_obj_bench: sample failed\n");
            goto cleanup;
        }
        printf(
            "sample_%02" PRIu32 "_ns=%" PRIu64
            " iterations=%" PRIu64 "\n",
            sample + 1u,
            samples[sample],
            iterations
        );
    }
    if (!capture_validation(
            context,
            mesh,
            &frame_desc,
            &object_to_world,
            pixel_count,
            &validation_after
        ) ||
        !validation_matches(&validation_before, &validation_after)) {
        fprintf(stderr, "soc_obj_bench: post-sampling validation changed\n");
        goto cleanup;
    }

    median_ns = calculate_median(samples, opts.sample_count);
    for (sample = 0u; sample < opts.sample_count; ++sample) {
        deviations[sample] = samples[sample] >= median_ns
            ? samples[sample] - median_ns
            : median_ns - samples[sample];
    }
    mad_ns = calculate_median(deviations, opts.sample_count);
    printf(
        "median_ns=%" PRIu64 " p95_ns=%" PRIu64
        " mad_ns=%" PRIu64 " min_ns=%" PRIu64
        " max_ns=%" PRIu64 "\n",
        median_ns,
        samples[((uint64_t)opts.sample_count * 95u + 99u) / 100u - 1u],
        mad_ns,
        samples[0],
        samples[opts.sample_count - 1u]
    );
    printf(
        "workers=%" PRIu32 " backend=%s "
        "size=%" PRIu32 "x%" PRIu32
        " vertices=%" PRIu32 " triangles=%" PRIu32
        " clipped=%" PRIu64 " rasterized=%" PRIu64
        " drawn_pixels=%" PRIu64 " checksum=%016" PRIx64 "\n",
        runtime_info.worker_count,
        runtime_info.execution_backend == SOC_EXECUTION_BACKEND_NEON
            ? "neon"
            : "scalar",
        metadata.width,
        metadata.height,
        object.vertex_count,
        object.index_count / 3u,
        validation_after.stats.clipped_triangle_count,
        validation_after.stats.rasterized_triangle_count,
        validation_after.drawn_pixel_count,
        validation_after.checksum
    );
    exit_code = EXIT_SUCCESS;

cleanup:
    free(deviations);
    free(samples);
    if (mesh != NULL) {
        (void)soc_mesh_destroy(mesh);
    }
    soc_context_destroy(context);
    soc_cli_obj_destroy(&object);
    return exit_code;
}
