#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "core/soc_cpu_features.h"
#include "core/soc_kernels.h"
#include "occlusion/soc_hiz.h"
#include "occlusion/soc_visibility.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

#define DEFAULT_SAMPLE_COUNT 15u
#define DEFAULT_SAMPLE_MS 200u
#define WARMUP_TARGET_NS UINT64_C(250000000)

#define CLEAR_ELEMENT_COUNT ((size_t)1920u * 1080u)
#define HIZ_SOURCE_WIDTH 1920u
#define HIZ_SOURCE_HEIGHT 1080u
#define RASTER_WIDTH 1920u
#define RASTER_HEIGHT 1080u
#define RASTER_PARTIAL_MASK UINT64_C(0x55aa55aa55aa55aa)
#define TRANSFORM_TRIANGLES_PER_OPERATION 16384u
#define TRANSFORM_POSITION_SET_COUNT 64u
#define QUERY_AABB_COUNT 65536u
#define QUERY_WIDTH 640u
#define QUERY_HEIGHT 360u

#if !defined(SOC_KERNEL_BENCH_BUILD_TYPE)
#define SOC_KERNEL_BENCH_BUILD_TYPE "unknown"
#endif

#if !defined(SOC_KERNEL_BENCH_LINKAGE)
#define SOC_KERNEL_BENCH_LINKAGE "unknown"
#endif

#define REQUIRED_GATE_NONE UINT32_C(0)
#define REQUIRED_GATE_CLEAR_F32 (UINT32_C(1) << 0u)
#define REQUIRED_GATE_HIZ_REDUCE_LEVEL_F32 (UINT32_C(1) << 1u)
#define REQUIRED_GATE_RASTER_DEPTH_BLOCK_F32 (UINT32_C(1) << 2u)
#define REQUIRED_GATE_TRANSFORM_TRIANGLE_F32 (UINT32_C(1) << 3u)
#define REQUIRED_GATE_AABB_QUERY (UINT32_C(1) << 4u)
#define REQUIRED_GATE_ALL \
    (REQUIRED_GATE_CLEAR_F32 | REQUIRED_GATE_HIZ_REDUCE_LEVEL_F32 | \
        REQUIRED_GATE_RASTER_DEPTH_BLOCK_F32 | \
        REQUIRED_GATE_TRANSFORM_TRIANGLE_F32 | REQUIRED_GATE_AABB_QUERY)

typedef enum benchmark_kind {
    BENCHMARK_CLEAR_F32 = 0,
    BENCHMARK_HIZ_REDUCE_LEVEL_F32,
    BENCHMARK_RASTER_DEPTH_BLOCK_FULL_F32,
    BENCHMARK_RASTER_DEPTH_BLOCK_PARTIAL_F32,
} benchmark_kind;

typedef struct options {
    uint32_t sample_count;
    uint32_t sample_ms;
    uint32_t required_gates;
    soc_bool validate_only;
} options;

typedef struct kernel_workload {
    benchmark_kind kind;
    const float* source;
    float* destination;
    size_t destination_count;
    uint32_t source_width;
    uint32_t source_height;
    uint64_t invocation;
} kernel_workload;

typedef struct backend_result {
    uint64_t* samples_ns;
    uint64_t median_ns;
    uint64_t mad_ns;
    uint64_t checksum;
} backend_result;

typedef struct transform_workload {
    soc_kernel_mat4_f32 clip_from_object;
    float positions[TRANSFORM_POSITION_SET_COUNT][9];
    soc_kernel_clip_vertex outputs[TRANSFORM_POSITION_SET_COUNT][3];
    soc_kernel_clip_metadata metadata[TRANSFORM_POSITION_SET_COUNT];
    uint64_t invocation;
} transform_workload;

typedef void (*transform_triangle_fn)(
    const soc_kernel_mat4_f32* clip_from_object,
    const float* position0_xyz,
    const float* position1_xyz,
    const float* position2_xyz,
    soc_clip_depth_range depth_range,
    soc_kernel_clip_vertex out_clip[3],
    soc_kernel_clip_metadata* out_metadata
);

typedef struct query_workload {
    soc_hiz hiz;
    soc_aabb_query_context query;
    soc_aabb* bounds;
    soc_visibility* visibility;
    soc_occlusion_query_counts counts;
    uint64_t invocation;
} query_workload;

static volatile uint64_t benchmark_sink;

static soc_bool build_architecture_matches_neon(uint32_t architecture)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return architecture == SOC_CPU_ARCHITECTURE_ARM64
        ? SOC_TRUE : SOC_FALSE;
#elif (defined(__arm__) || defined(_M_ARM)) && \
    defined(SOC_BUILD_AARCH32_NEON_FMA)
    return architecture == SOC_CPU_ARCHITECTURE_ARM32
        ? SOC_TRUE : SOC_FALSE;
#else
    (void)architecture;
    return SOC_FALSE;
#endif
}

static void observe_memory(const void* pointer)
{
#if defined(_MSC_VER)
    (void)pointer;
    _ReadWriteBarrier();
#elif defined(__clang__) || defined(__GNUC__)
    __asm__ __volatile__("" : : "r"(pointer) : "memory");
#else
    benchmark_sink ^= (uint64_t)(uintptr_t)pointer;
#endif
}

static void print_usage(FILE* stream, const char* executable)
{
    (void)fprintf(
        stream,
        "Usage: %s [--samples N] [--sample-ms N] [--validate-only]\n"
        "  --samples N    Samples per backend and case (default: %u)\n"
        "  --sample-ms N  Minimum milliseconds per sample (default: %u)\n"
        "  --validate-only  Check Scalar/NEON outputs without timing\n"
        "  --require-gate clear|hiz|raster|transform|query|all\n"
        "                  Require Release, default sampling, distinct kernel,\n"
        "                  and fail unless selected performance gate(s) pass\n"
        "  --help         Show this help\n",
        executable,
        DEFAULT_SAMPLE_COUNT,
        DEFAULT_SAMPLE_MS
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
    if (end == text || errno == ERANGE || value == 0u ||
        value > UINT32_MAX) {
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

static int parse_option_value(
    int argc,
    char** argv,
    int* index,
    const char* argument,
    const char* option,
    const char** out_value
)
{
    const size_t option_length = strlen(option);

    if (strncmp(argument, option, option_length) != 0) {
        return 0;
    }
    if (argument[option_length] == '=') {
        *out_value = argument + option_length + 1u;
        return 1;
    }
    if (argument[option_length] != '\0') {
        return 0;
    }
    if (*index + 1 >= argc) {
        return -1;
    }
    ++*index;
    *out_value = argv[*index];
    return 1;
}

static int parse_options(int argc, char** argv, options* out_options)
{
    int index;

    out_options->sample_count = DEFAULT_SAMPLE_COUNT;
    out_options->sample_ms = DEFAULT_SAMPLE_MS;
    out_options->required_gates = REQUIRED_GATE_NONE;
    out_options->validate_only = SOC_FALSE;

    for (index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        const char* value = NULL;
        int matched;

        if (strcmp(argument, "--help") == 0 ||
            strcmp(argument, "-h") == 0) {
            print_usage(stdout, argv[0]);
            return 2;
        }
        if (strcmp(argument, "--validate-only") == 0) {
            out_options->validate_only = SOC_TRUE;
            continue;
        }

        matched = parse_option_value(
            argc,
            argv,
            &index,
            argument,
            "--samples",
            &value
        );
        if (matched != 0) {
            if (matched < 0 || !parse_uint32(value, &out_options->sample_count) ||
                out_options->sample_count > 10000u) {
                (void)fprintf(stderr, "invalid sample count\n");
                return 1;
            }
            continue;
        }

        matched = parse_option_value(
            argc,
            argv,
            &index,
            argument,
            "--sample-ms",
            &value
        );
        if (matched != 0) {
            if (matched < 0 || !parse_uint32(value, &out_options->sample_ms) ||
                out_options->sample_ms > 600000u) {
                (void)fprintf(stderr, "invalid sample duration\n");
                return 1;
            }
            continue;
        }

        matched = parse_option_value(
            argc,
            argv,
            &index,
            argument,
            "--require-gate",
            &value
        );
        if (matched != 0) {
            if (matched < 0 || value == NULL) {
                (void)fprintf(stderr, "missing required gate\n");
                return 1;
            }
            if (strcmp(value, "clear") == 0) {
                out_options->required_gates |= REQUIRED_GATE_CLEAR_F32;
            } else if (strcmp(value, "hiz") == 0) {
                out_options->required_gates |=
                    REQUIRED_GATE_HIZ_REDUCE_LEVEL_F32;
            } else if (strcmp(value, "raster") == 0) {
                out_options->required_gates |=
                    REQUIRED_GATE_RASTER_DEPTH_BLOCK_F32;
            } else if (strcmp(value, "transform") == 0) {
                out_options->required_gates |=
                    REQUIRED_GATE_TRANSFORM_TRIANGLE_F32;
            } else if (strcmp(value, "query") == 0) {
                out_options->required_gates |= REQUIRED_GATE_AABB_QUERY;
            } else if (strcmp(value, "all") == 0) {
                out_options->required_gates = REQUIRED_GATE_ALL;
            } else {
                (void)fprintf(stderr, "invalid required gate: %s\n", value);
                return 1;
            }
            continue;
        }

        (void)fprintf(stderr, "unknown option: %s\n", argument);
        return 1;
    }
    return 0;
}

static int timer_initialize(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    return QueryPerformanceFrequency(&frequency) != 0 &&
        frequency.QuadPart > 0 &&
        QueryPerformanceCounter(&counter) != 0;
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
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64_t seconds;
    uint64_t remainder;

    if (frequency.QuadPart == 0 &&
        (QueryPerformanceFrequency(&frequency) == 0 ||
            frequency.QuadPart <= 0)) {
        return UINT64_MAX;
    }
    if (QueryPerformanceCounter(&counter) == 0 || counter.QuadPart < 0) {
        return UINT64_MAX;
    }
    seconds = (uint64_t)counter.QuadPart / (uint64_t)frequency.QuadPart;
    remainder = (uint64_t)counter.QuadPart % (uint64_t)frequency.QuadPart;
    return seconds * UINT64_C(1000000000) +
        remainder * UINT64_C(1000000000) /
            (uint64_t)frequency.QuadPart;
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
    if (quotient > UINT64_MAX / (uint64_t)timebase.numer) {
        return UINT64_MAX;
    }
    return quotient * (uint64_t)timebase.numer +
        remainder * (uint64_t)timebase.numer /
            (uint64_t)timebase.denom;
#else
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
#endif
}

static const char* timer_name(void)
{
#if defined(_WIN32)
    return "QueryPerformanceCounter";
#elif defined(__APPLE__)
    return "mach_absolute_time";
#else
    return "clock_gettime(CLOCK_MONOTONIC)";
#endif
}

static int compare_u64(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;

    return (a > b) - (a < b);
}

static uint64_t median_sorted(const uint64_t* values, uint32_t count)
{
    const uint64_t lower = values[(count - 1u) / 2u];
    const uint64_t upper = values[count / 2u];

    return lower / 2u + upper / 2u +
        ((lower & 1u) + (upper & 1u) + 1u) / 2u;
}

static int summarize_result(backend_result* result, uint32_t sample_count)
{
    uint64_t* sorted;
    uint64_t* deviations;
    uint32_t index;

    sorted = (uint64_t*)malloc((size_t)sample_count * sizeof(*sorted));
    deviations = (uint64_t*)malloc(
        (size_t)sample_count * sizeof(*deviations)
    );
    if (sorted == NULL || deviations == NULL) {
        free(sorted);
        free(deviations);
        return 0;
    }

    memcpy(
        sorted,
        result->samples_ns,
        (size_t)sample_count * sizeof(*sorted)
    );
    qsort(sorted, sample_count, sizeof(*sorted), compare_u64);
    result->median_ns = median_sorted(sorted, sample_count);

    for (index = 0u; index < sample_count; ++index) {
        const uint64_t value = result->samples_ns[index];

        deviations[index] = value >= result->median_ns
            ? value - result->median_ns
            : result->median_ns - value;
    }
    qsort(deviations, sample_count, sizeof(*deviations), compare_u64);
    result->mad_ns = median_sorted(deviations, sample_count);

    free(sorted);
    free(deviations);
    return 1;
}

static uint64_t checksum_f32(const float* values, size_t count)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;

    for (index = 0u; index < count; ++index) {
        uint32_t bits;

        memcpy(&bits, &values[index], sizeof(bits));
        hash ^= bits;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t checksum_transform_outputs(
    const soc_kernel_clip_vertex outputs[TRANSFORM_POSITION_SET_COUNT][3],
    const soc_kernel_clip_metadata metadata[TRANSFORM_POSITION_SET_COUNT]
)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t set;

    for (set = 0u; set < TRANSFORM_POSITION_SET_COUNT; ++set) {
        size_t vertex;

        for (vertex = 0u; vertex < 3u; ++vertex) {
            const float components[4] = {
                outputs[set][vertex].x,
                outputs[set][vertex].y,
                outputs[set][vertex].z,
                outputs[set][vertex].w,
            };
            size_t component;

            for (component = 0u; component < 4u; ++component) {
                uint32_t bits;

                memcpy(&bits, &components[component], sizeof(bits));
                hash ^= bits;
                hash *= UINT64_C(1099511628211);
            }
        }
        hash ^= metadata[set].active_planes;
        hash *= UINT64_C(1099511628211);
        hash ^= metadata[set].common_planes;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int transform_outputs_equivalent(
    const transform_workload* scalar,
    const transform_workload* neon
)
{
    size_t set;

    for (set = 0u; set < TRANSFORM_POSITION_SET_COUNT; ++set) {
        size_t vertex;

        if (scalar->metadata[set].active_planes !=
                neon->metadata[set].active_planes ||
            scalar->metadata[set].common_planes !=
                neon->metadata[set].common_planes) {
            return 0;
        }
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const float scalar_components[4] = {
                scalar->outputs[set][vertex].x,
                scalar->outputs[set][vertex].y,
                scalar->outputs[set][vertex].z,
                scalar->outputs[set][vertex].w,
            };
            const float neon_components[4] = {
                neon->outputs[set][vertex].x,
                neon->outputs[set][vertex].y,
                neon->outputs[set][vertex].z,
                neon->outputs[set][vertex].w,
            };
            size_t component;

            for (component = 0u; component < 4u; ++component) {
                const float scale = fmaxf(
                    fabsf(scalar_components[component]),
                    fabsf(neon_components[component])
                );

                if (fabsf(
                        scalar_components[component] -
                            neon_components[component]
                    ) > 2.0e-5f * (1.0f + scale)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static uint32_t random_u32(uint32_t* state)
{
    uint32_t value = *state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static void initialize_hiz_source(float* source, size_t count)
{
    uint32_t state = UINT32_C(0x534f4301);
    size_t index;

    for (index = 0u; index < count; ++index) {
        source[index] =
            (float)(random_u32(&state) >> 8u) / 16777215.0f;
    }
}

static void initialize_transform_workload(transform_workload* workload)
{
    static const float object_columns[4][4] = {
        {1.125, -0.375, 0.25, 0.0625},
        {0.1875, 0.9375, -0.3125, 0.125},
        {-0.4375, 0.21875, 1.0625, -0.09375},
        {2.25, -1.5, 0.625, 1.0},
    };
    static const float clip_columns[4][4] = {
        {0.8125, 0.15625, -0.28125, 0.09375},
        {-0.125, 1.1875, 0.34375, -0.0625},
        {0.40625, -0.234375, 0.875, 0.15625},
        {-0.75, 0.5, 0.1875, 1.125},
    };
    soc_kernel_mat4_f32 object_to_world;
    soc_kernel_mat4_f32 clip_from_world;
    uint32_t state = UINT32_C(0x5452414e);
    size_t set;

    memset(workload, 0, sizeof(*workload));
    memcpy(
        object_to_world.columns,
        object_columns,
        sizeof(object_columns)
    );
    memcpy(
        clip_from_world.columns,
        clip_columns,
        sizeof(clip_columns)
    );
    soc_kernel_mat4_f32_multiply(
        &clip_from_world,
        &object_to_world,
        &workload->clip_from_object
    );
    for (set = 0u; set < TRANSFORM_POSITION_SET_COUNT; ++set) {
        size_t component;

        for (component = 0u; component < 9u; ++component) {
            const int32_t centered =
                (int32_t)(random_u32(&state) & UINT32_C(0x0000ffff)) -
                INT32_C(32768);

            workload->positions[set][component] =
                (float)centered / 8192.0f;
        }
    }
}

static void execute_transform_operation(
    transform_workload* workload,
    transform_triangle_fn transform_triangle
)
{
    uint32_t triangle;
    uint64_t observed = UINT64_C(14695981039346656037);

    for (triangle = 0u;
         triangle < TRANSFORM_TRIANGLES_PER_OPERATION;
         ++triangle) {
        const size_t set =
            (size_t)((triangle + workload->invocation) &
                (TRANSFORM_POSITION_SET_COUNT - 1u));
        const float* positions = workload->positions[set];

        transform_triangle(
            &workload->clip_from_object,
            positions,
            positions + 3u,
            positions + 6u,
            SOC_CLIP_DEPTH_ZERO_TO_ONE,
            workload->outputs[set],
            &workload->metadata[set]
        );
        {
            uint32_t bits;

            memcpy(&bits, &workload->outputs[set][0].x, sizeof(bits));
            observed = (observed ^ bits) * UINT64_C(1099511628211);
        }
    }
    benchmark_sink =
        (benchmark_sink ^ observed ^ workload->invocation) *
        UINT64_C(1099511628211);
    ++workload->invocation;
}

static int run_transform_timed_sample(
    transform_workload* workload,
    transform_triangle_fn transform_triangle,
    uint64_t target_ns,
    uint64_t* out_average_ns
)
{
    const uint64_t start = timer_now_ns();
    uint64_t end;
    uint64_t elapsed;
    uint64_t iterations = 0u;

    if (start == UINT64_MAX) {
        return 0;
    }
    do {
        execute_transform_operation(workload, transform_triangle);
        if (iterations == UINT64_MAX) {
            return 0;
        }
        ++iterations;
        end = timer_now_ns();
        if (end == UINT64_MAX || end < start) {
            return 0;
        }
        elapsed = end - start;
    } while (elapsed < target_ns);

    *out_average_ns = (elapsed + iterations / 2u) / iterations;
    return 1;
}

static uint64_t checksum_bytes(const void* data, size_t size)
{
    const unsigned char* bytes = (const unsigned char*)data;
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;

    for (index = 0u; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t checksum_query_outputs(const query_workload* workload)
{
    uint64_t hash = checksum_bytes(
        workload->visibility,
        QUERY_AABB_COUNT * sizeof(*workload->visibility)
    );

    hash = (hash ^ workload->counts.visible) * UINT64_C(1099511628211);
    hash = (hash ^ workload->counts.occluded) * UINT64_C(1099511628211);
    hash = (hash ^ workload->counts.unknown) * UINT64_C(1099511628211);
    return hash;
}

static int initialize_query_workload(
    query_workload* workload,
    const soc_kernel_table* scalar_kernels
)
{
    const soc_frame_desc frame = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
            .col3 = {0.0f, 0.0f, 0.0f, 1.0f},
        },
        .clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE,
    };
    size_t data_index;
    uint32_t index;

    memset(workload, 0, sizeof(*workload));
    workload->bounds = (soc_aabb*)malloc(
        QUERY_AABB_COUNT * sizeof(*workload->bounds)
    );
    workload->visibility = (soc_visibility*)malloc(
        QUERY_AABB_COUNT * sizeof(*workload->visibility)
    );
    if (workload->bounds == NULL || workload->visibility == NULL ||
        soc_hiz_initialize(
            &workload->hiz,
            QUERY_WIDTH,
            QUERY_HEIGHT
        ) != SOC_RESULT_OK) {
        soc_hiz_shutdown(&workload->hiz);
        free(workload->visibility);
        free(workload->bounds);
        memset(workload, 0, sizeof(*workload));
        return 0;
    }

    for (data_index = 0u;
         data_index < workload->hiz.levels[0].element_count;
         ++data_index) {
        workload->hiz.data[data_index] = 0.5f;
    }
    if (soc_hiz_build_with_kernels(
            &workload->hiz,
            scalar_kernels
        ) != SOC_RESULT_OK) {
        soc_hiz_shutdown(&workload->hiz);
        free(workload->visibility);
        free(workload->bounds);
        memset(workload, 0, sizeof(*workload));
        return 0;
    }
    soc_aabb_query_context_initialize(&frame, &workload->query);

    for (index = 0u; index < QUERY_AABB_COUNT; ++index) {
        const uint32_t grid_x = index & UINT32_C(0xff);
        const uint32_t grid_y = (index >> 8u) & UINT32_C(0xff);
        const float center_x = -0.75f +
            (float)grid_x * (1.5f / 255.0f);
        const float center_y = -0.75f +
            (float)grid_y * (1.5f / 255.0f);
        const float radius = 0.0005f;
        const float depth = (index & 1u) == 0u ? 0.15f : 0.85f;

        workload->bounds[index].min.x = center_x - radius;
        workload->bounds[index].min.y = center_y - radius;
        workload->bounds[index].min.z = depth;
        workload->bounds[index].max.x = center_x + radius;
        workload->bounds[index].max.y = center_y + radius;
        workload->bounds[index].max.z = depth + 0.002f;
    }
    return 1;
}

static void destroy_query_workload(query_workload* workload)
{
    if (workload == NULL) {
        return;
    }
    soc_hiz_shutdown(&workload->hiz);
    free(workload->visibility);
    free(workload->bounds);
    memset(workload, 0, sizeof(*workload));
}

static int execute_query_operation(
    query_workload* workload,
    const soc_kernel_table* kernels
)
{
    const soc_result result = kernels->test_aabbs(
        &workload->hiz,
        &workload->query,
        workload->bounds,
        QUERY_AABB_COUNT,
        workload->visibility,
        &workload->counts
    );
    const size_t observed_index =
        (size_t)(workload->invocation & (QUERY_AABB_COUNT - 1u));
    uint64_t observed;

    if (result != SOC_RESULT_OK) {
        return 0;
    }
    observe_memory(workload->visibility);
    observed = workload->visibility[observed_index];
    observed ^= workload->counts.visible;
    observed ^= workload->counts.occluded << 17u;
    observed ^= workload->counts.unknown << 33u;
    benchmark_sink =
        (benchmark_sink ^ observed ^ workload->invocation) *
        UINT64_C(1099511628211);
    ++workload->invocation;
    return 1;
}

static int capture_query_checksum(
    query_workload* workload,
    const soc_kernel_table* kernels,
    int fill,
    uint64_t* out_checksum
)
{
    memset(
        workload->visibility,
        fill,
        QUERY_AABB_COUNT * sizeof(*workload->visibility)
    );
    workload->counts.visible = 0u;
    workload->counts.occluded = 0u;
    workload->counts.unknown = 0u;
    workload->invocation = 0u;
    if (!execute_query_operation(workload, kernels)) {
        return 0;
    }
    *out_checksum = checksum_query_outputs(workload);
    return 1;
}

static int run_query_timed_sample(
    query_workload* workload,
    const soc_kernel_table* kernels,
    uint64_t target_ns,
    uint64_t* out_average_ns
)
{
    const uint64_t start = timer_now_ns();
    uint64_t end;
    uint64_t elapsed;
    uint64_t iterations = 0u;

    if (start == UINT64_MAX) {
        return 0;
    }
    do {
        if (!execute_query_operation(workload, kernels) ||
            iterations == UINT64_MAX) {
            return 0;
        }
        ++iterations;
        end = timer_now_ns();
        if (end == UINT64_MAX || end < start) {
            return 0;
        }
        elapsed = end - start;
    } while (elapsed < target_ns);

    *out_average_ns = (elapsed + iterations / 2u) / iterations;
    return 1;
}

static soc_bool benchmark_is_raster_depth_block(benchmark_kind kind)
{
    return kind == BENCHMARK_RASTER_DEPTH_BLOCK_FULL_F32 ||
        kind == BENCHMARK_RASTER_DEPTH_BLOCK_PARTIAL_F32
        ? SOC_TRUE
        : SOC_FALSE;
}

static void execute_raster_depth_blocks(
    kernel_workload* workload,
    const soc_kernel_table* kernels,
    uint64_t invocation
)
{
    const uint64_t cycle_index = invocation & UINT64_C(0xffff);
    const float candidate_depth = 0.25f + (float)cycle_index *
            (0.5f / 65536.0f);
    const uint64_t coverage_mask =
        workload->kind == BENCHMARK_RASTER_DEPTH_BLOCK_FULL_F32
            ? UINT64_MAX
            : RASTER_PARTIAL_MASK;
    uint32_t block_y;

    /* Keep every timed depth test on the passing/store path after wraparound. */
    if (cycle_index == 0u) {
        kernels->clear_f32(
            workload->destination,
            workload->destination_count,
            0.0f
        );
    }

    for (block_y = 0u;
         block_y < RASTER_HEIGHT;
         block_y += SOC_KERNEL_RASTER_BLOCK_SIZE) {
        uint32_t block_x;

        for (block_x = 0u;
             block_x < RASTER_WIDTH;
             block_x += SOC_KERNEL_RASTER_BLOCK_SIZE) {
            kernels->store_constant_depth_block_f32(
                workload->destination +
                    (size_t)block_y * RASTER_WIDTH + block_x,
                RASTER_WIDTH,
                SOC_KERNEL_RASTER_BLOCK_SIZE,
                SOC_KERNEL_RASTER_BLOCK_SIZE,
                coverage_mask,
                candidate_depth
            );
        }
    }
}

static void execute_operation(
    kernel_workload* workload,
    const soc_kernel_table* kernels
)
{
    size_t probe;
    uint32_t bits;

    if (workload->kind == BENCHMARK_CLEAR_F32) {
        static const float clear_values[] = {0.0f, 1.0f, 0.25f, 0.75f};
        const float value = clear_values[workload->invocation & 3u];

        kernels->clear_f32(
            workload->destination,
            workload->destination_count,
            value
        );
    } else if (workload->kind == BENCHMARK_HIZ_REDUCE_LEVEL_F32) {
        kernels->reduce_hiz_level_f32(
            workload->source,
            workload->source_width,
            workload->source_height,
            workload->destination
        );
    } else {
        execute_raster_depth_blocks(
            workload,
            kernels,
            workload->invocation
        );
    }

    probe = (size_t)(workload->invocation % workload->destination_count);
    memcpy(&bits, &workload->destination[probe], sizeof(bits));
    benchmark_sink =
        (benchmark_sink ^ bits ^ workload->invocation) *
        UINT64_C(1099511628211);
    ++workload->invocation;
}

static int run_timed_sample(
    kernel_workload* workload,
    const soc_kernel_table* kernels,
    uint64_t target_ns,
    uint64_t* out_average_ns
)
{
    const uint64_t start = timer_now_ns();
    uint64_t end;
    uint64_t elapsed;
    uint64_t iterations = 0u;

    if (start == UINT64_MAX) {
        return 0;
    }
    do {
        execute_operation(workload, kernels);
        if (iterations == UINT64_MAX) {
            return 0;
        }
        ++iterations;
        end = timer_now_ns();
        if (end == UINT64_MAX || end < start) {
            return 0;
        }
        elapsed = end - start;
    } while (elapsed < target_ns);

    *out_average_ns = (elapsed + iterations / 2u) / iterations;
    return 1;
}

static uint64_t capture_checksum(
    kernel_workload* workload,
    const soc_kernel_table* kernels,
    int fill
)
{
    memset(
        workload->destination,
        fill,
        workload->destination_count * sizeof(*workload->destination)
    );
    if (workload->kind == BENCHMARK_CLEAR_F32) {
        kernels->clear_f32(
            workload->destination,
            workload->destination_count,
            0.625f
        );
    } else if (workload->kind == BENCHMARK_HIZ_REDUCE_LEVEL_F32) {
        kernels->reduce_hiz_level_f32(
            workload->source,
            workload->source_width,
            workload->source_height,
            workload->destination
        );
    } else {
        size_t index;

        for (index = 0u;
             index < workload->destination_count;
             ++index) {
            workload->destination[index] = 0.0f;
        }
        execute_raster_depth_blocks(workload, kernels, 0u);
    }
    return checksum_f32(
        workload->destination,
        workload->destination_count
    );
}

static int allocate_f32(size_t count, float** out_values)
{
    if (count == 0u || count > SIZE_MAX / sizeof(float)) {
        return 0;
    }
    *out_values = (float*)malloc(count * sizeof(float));
    return *out_values != NULL;
}

static int initialize_case(
    benchmark_kind kind,
    kernel_workload* scalar_workload,
    kernel_workload* neon_workload,
    float** out_source
)
{
    size_t destination_count;

    memset(scalar_workload, 0, sizeof(*scalar_workload));
    memset(neon_workload, 0, sizeof(*neon_workload));
    *out_source = NULL;

    scalar_workload->kind = kind;
    neon_workload->kind = kind;
    if (kind == BENCHMARK_CLEAR_F32) {
        destination_count = CLEAR_ELEMENT_COUNT;
    } else if (kind == BENCHMARK_HIZ_REDUCE_LEVEL_F32) {
        const uint32_t destination_width =
            HIZ_SOURCE_WIDTH / 2u + HIZ_SOURCE_WIDTH % 2u;
        const uint32_t destination_height =
            HIZ_SOURCE_HEIGHT / 2u + HIZ_SOURCE_HEIGHT % 2u;
        const size_t source_count =
            (size_t)HIZ_SOURCE_WIDTH * HIZ_SOURCE_HEIGHT;

        destination_count =
            (size_t)destination_width * destination_height;
        if (!allocate_f32(source_count, out_source)) {
            return 0;
        }
        initialize_hiz_source(*out_source, source_count);
        scalar_workload->source = *out_source;
        neon_workload->source = *out_source;
        scalar_workload->source_width = HIZ_SOURCE_WIDTH;
        scalar_workload->source_height = HIZ_SOURCE_HEIGHT;
        neon_workload->source_width = HIZ_SOURCE_WIDTH;
        neon_workload->source_height = HIZ_SOURCE_HEIGHT;
    } else {
        destination_count = (size_t)RASTER_WIDTH * RASTER_HEIGHT;
    }

    scalar_workload->destination_count = destination_count;
    neon_workload->destination_count = destination_count;
    if (!allocate_f32(destination_count, &scalar_workload->destination) ||
        !allocate_f32(destination_count, &neon_workload->destination)) {
        free(scalar_workload->destination);
        free(neon_workload->destination);
        free(*out_source);
        scalar_workload->destination = NULL;
        neon_workload->destination = NULL;
        *out_source = NULL;
        return 0;
    }
    return 1;
}

static void destroy_case(
    kernel_workload* scalar_workload,
    kernel_workload* neon_workload,
    float* source
)
{
    free(scalar_workload->destination);
    free(neon_workload->destination);
    free(source);
}

static void print_backend_result(
    const char* case_name,
    const char* backend_name,
    const backend_result* result
)
{
    (void)printf(
        "case=%s backend=%s median_ns=%" PRIu64
        " mad_ns=%" PRIu64 " checksum=%016" PRIx64 "\n",
        case_name,
        backend_name,
        result->median_ns,
        result->mad_ns,
        result->checksum
    );
}

static soc_bool print_comparison(
    const char* case_name,
    const backend_result* scalar,
    const backend_result* neon
)
{
    const uint64_t maximum_mad = scalar->mad_ns > neon->mad_ns
        ? scalar->mad_ns
        : neon->mad_ns;
    const uint64_t noise_limit = maximum_mad > UINT64_MAX / 3u
        ? UINT64_MAX
        : maximum_mad * 3u;
    const uint64_t improvement_ns = scalar->median_ns > neon->median_ns
        ? scalar->median_ns - neon->median_ns
        : 0u;
    const double speedup = neon->median_ns != 0u
        ? (double)scalar->median_ns / (double)neon->median_ns
        : 0.0;
    const double improvement_percent = scalar->median_ns != 0u
        ? ((double)scalar->median_ns - (double)neon->median_ns) * 100.0 /
            (double)scalar->median_ns
        : 0.0;
    const soc_bool passes_five_percent =
        improvement_percent >= 5.0 ? SOC_TRUE : SOC_FALSE;
    const soc_bool passes_noise = improvement_ns > noise_limit
        ? SOC_TRUE : SOC_FALSE;
    const soc_bool passes =
        passes_five_percent == SOC_TRUE && passes_noise == SOC_TRUE
        ? SOC_TRUE : SOC_FALSE;

    (void)printf(
        "case=%s speedup=%.4fx improvement_percent=%.3f"
        " improvement_ns=%" PRIu64 " noise_limit_ns=%" PRIu64
        " gate_5_percent=%s gate_3x_mad=%s result=%s\n",
        case_name,
        speedup,
        improvement_percent,
        improvement_ns,
        noise_limit,
        passes_five_percent == SOC_TRUE ? "pass" : "fail",
        passes_noise == SOC_TRUE ? "pass" : "fail",
        passes == SOC_TRUE ? "pass" : "fail"
    );
    return passes;
}

static int run_case(
    benchmark_kind kind,
    const char* case_name,
    const options* opts,
    const soc_kernel_table* scalar_kernels,
    const soc_kernel_table* neon_kernels,
    soc_bool* out_gate_passed
)
{
    const soc_bool distinct_implementation =
        (kind == BENCHMARK_CLEAR_F32 &&
            scalar_kernels->clear_f32 != neon_kernels->clear_f32) ||
        (kind == BENCHMARK_HIZ_REDUCE_LEVEL_F32 &&
            scalar_kernels->reduce_hiz_level_f32 !=
                neon_kernels->reduce_hiz_level_f32) ||
        (benchmark_is_raster_depth_block(kind) == SOC_TRUE &&
            scalar_kernels->store_constant_depth_block_f32 !=
                neon_kernels->store_constant_depth_block_f32)
        ? SOC_TRUE
        : SOC_FALSE;
    kernel_workload scalar_workload;
    kernel_workload neon_workload;
    backend_result scalar_result = {0};
    backend_result neon_result = {0};
    float* source = NULL;
    uint64_t source_checksum = 0u;
    uint64_t warmup_average;
    uint64_t target_ns;
    uint64_t validation_scalar;
    uint64_t validation_neon;
    uint32_t sample;
    int success = 0;

    if (!initialize_case(
            kind,
            &scalar_workload,
            &neon_workload,
            &source
        )) {
        (void)fprintf(stderr, "%s: workload allocation failed\n", case_name);
        return 0;
    }
    if (source != NULL) {
        source_checksum = checksum_f32(
            source,
            (size_t)HIZ_SOURCE_WIDTH * HIZ_SOURCE_HEIGHT
        );
    }

    validation_scalar = capture_checksum(
        &scalar_workload,
        scalar_kernels,
        0xa5
    );
    validation_neon = capture_checksum(
        &neon_workload,
        neon_kernels,
        0x5a
    );
    if (validation_scalar != validation_neon) {
        (void)fprintf(stderr, "%s: Scalar/NEON checksum mismatch\n", case_name);
        goto cleanup;
    }
    if (source != NULL && source_checksum != checksum_f32(
            source,
            (size_t)HIZ_SOURCE_WIDTH * HIZ_SOURCE_HEIGHT
        )) {
        (void)fprintf(stderr, "%s: validation modified source data\n", case_name);
        goto cleanup;
    }
    if (opts->validate_only == SOC_TRUE) {
        (void)printf(
            "case=%s validation=pass checksum=%016" PRIx64 "\n",
            case_name,
            validation_scalar
        );
        success = 1;
        goto cleanup;
    }
    if (distinct_implementation != SOC_TRUE) {
        (void)printf(
            "case=%s performance=unavailable reason=shared_implementation\n",
            case_name
        );
        *out_gate_passed = SOC_FALSE;
        success = 1;
        goto cleanup;
    }

    scalar_result.samples_ns = (uint64_t*)calloc(
        opts->sample_count,
        sizeof(*scalar_result.samples_ns)
    );
    neon_result.samples_ns = (uint64_t*)calloc(
        opts->sample_count,
        sizeof(*neon_result.samples_ns)
    );
    if (scalar_result.samples_ns == NULL || neon_result.samples_ns == NULL) {
        (void)fprintf(stderr, "%s: sample allocation failed\n", case_name);
        goto cleanup;
    }

    if (!run_timed_sample(
            &scalar_workload,
            scalar_kernels,
            WARMUP_TARGET_NS,
            &warmup_average
        ) ||
        !run_timed_sample(
            &neon_workload,
            neon_kernels,
            WARMUP_TARGET_NS,
            &warmup_average
        )) {
        (void)fprintf(stderr, "%s: warmup failed\n", case_name);
        goto cleanup;
    }

    target_ns = (uint64_t)opts->sample_ms * UINT64_C(1000000);
    for (sample = 0u; sample < opts->sample_count; ++sample) {
        backend_result* first_result = (sample & 1u) == 0u
            ? &scalar_result : &neon_result;
        backend_result* second_result = (sample & 1u) == 0u
            ? &neon_result : &scalar_result;
        kernel_workload* first_workload = (sample & 1u) == 0u
            ? &scalar_workload : &neon_workload;
        kernel_workload* second_workload = (sample & 1u) == 0u
            ? &neon_workload : &scalar_workload;
        const soc_kernel_table* first_kernels = (sample & 1u) == 0u
            ? scalar_kernels : neon_kernels;
        const soc_kernel_table* second_kernels = (sample & 1u) == 0u
            ? neon_kernels : scalar_kernels;

        if (!run_timed_sample(
                first_workload,
                first_kernels,
                target_ns,
                &first_result->samples_ns[sample]
            ) ||
            !run_timed_sample(
                second_workload,
                second_kernels,
                target_ns,
                &second_result->samples_ns[sample]
            )) {
            (void)fprintf(stderr, "%s: timed sample failed\n", case_name);
            goto cleanup;
        }
    }

    scalar_result.checksum = capture_checksum(
        &scalar_workload,
        scalar_kernels,
        0xa5
    );
    neon_result.checksum = capture_checksum(
        &neon_workload,
        neon_kernels,
        0x5a
    );
    if (scalar_result.checksum != validation_scalar ||
        neon_result.checksum != validation_neon ||
        scalar_result.checksum != neon_result.checksum ||
        (source != NULL && source_checksum != checksum_f32(
            source,
            (size_t)HIZ_SOURCE_WIDTH * HIZ_SOURCE_HEIGHT
        ))) {
        (void)fprintf(stderr, "%s: post-sampling validation changed\n", case_name);
        goto cleanup;
    }

    if (!summarize_result(&scalar_result, opts->sample_count) ||
        !summarize_result(&neon_result, opts->sample_count)) {
        (void)fprintf(stderr, "%s: result summary failed\n", case_name);
        goto cleanup;
    }

    print_backend_result(case_name, "scalar", &scalar_result);
    print_backend_result(case_name, "neon", &neon_result);
    *out_gate_passed = print_comparison(
        case_name,
        &scalar_result,
        &neon_result
    );
    success = 1;

cleanup:
    free(scalar_result.samples_ns);
    free(neon_result.samples_ns);
    destroy_case(&scalar_workload, &neon_workload, source);
    return success;
}

static uint64_t capture_transform_checksum(
    transform_workload* workload,
    transform_triangle_fn transform_triangle,
    int fill
)
{
    memset(workload->outputs, fill, sizeof(workload->outputs));
    memset(workload->metadata, fill, sizeof(workload->metadata));
    workload->invocation = 0u;
    execute_transform_operation(workload, transform_triangle);
    return checksum_transform_outputs(
        workload->outputs,
        workload->metadata
    );
}

static int run_transform_case(
    const char* case_name,
    const options* opts,
    soc_bool* out_gate_passed
)
{
    transform_workload scalar_workload;
    transform_workload neon_workload;
    backend_result scalar_result = {0};
    backend_result neon_result = {0};
    uint64_t validation_scalar;
    uint64_t validation_neon;
    uint64_t warmup_average;
    uint64_t target_ns;
    uint32_t sample;
    int success = 0;
    const transform_triangle_fn scalar_transform =
        soc_kernel_transform_triangle_f32_scalar;
    const transform_triangle_fn neon_transform =
        soc_kernel_transform_triangle_f32_neon;

    initialize_transform_workload(&scalar_workload);
    initialize_transform_workload(&neon_workload);
    validation_scalar = capture_transform_checksum(
        &scalar_workload,
        scalar_transform,
        0xa5
    );
    validation_neon = capture_transform_checksum(
        &neon_workload,
        neon_transform,
        0x5a
    );
    if (!transform_outputs_equivalent(
            &scalar_workload,
            &neon_workload
        ) ||
        memcmp(
            scalar_workload.positions,
            neon_workload.positions,
            sizeof(scalar_workload.positions)
        ) != 0 ||
        memcmp(
            &scalar_workload.clip_from_object,
            &neon_workload.clip_from_object,
            sizeof(scalar_workload.clip_from_object)
        ) != 0) {
        (void)fprintf(stderr, "%s: Scalar/NEON validation mismatch\n", case_name);
        goto cleanup;
    }
    if (opts->validate_only == SOC_TRUE) {
        (void)printf(
            "case=%s validation=pass checksum=%016" PRIx64 "\n",
            case_name,
            validation_scalar
        );
        success = 1;
        goto cleanup;
    }
    scalar_result.samples_ns = (uint64_t*)calloc(
        opts->sample_count,
        sizeof(*scalar_result.samples_ns)
    );
    neon_result.samples_ns = (uint64_t*)calloc(
        opts->sample_count,
        sizeof(*neon_result.samples_ns)
    );
    if (scalar_result.samples_ns == NULL || neon_result.samples_ns == NULL) {
        (void)fprintf(stderr, "%s: sample allocation failed\n", case_name);
        goto cleanup;
    }

    if (!run_transform_timed_sample(
            &scalar_workload,
            scalar_transform,
            WARMUP_TARGET_NS,
            &warmup_average
        ) ||
        !run_transform_timed_sample(
            &neon_workload,
            neon_transform,
            WARMUP_TARGET_NS,
            &warmup_average
        )) {
        (void)fprintf(stderr, "%s: warmup failed\n", case_name);
        goto cleanup;
    }

    target_ns = (uint64_t)opts->sample_ms * UINT64_C(1000000);
    for (sample = 0u; sample < opts->sample_count; ++sample) {
        backend_result* first_result = (sample & 1u) == 0u
            ? &scalar_result : &neon_result;
        backend_result* second_result = (sample & 1u) == 0u
            ? &neon_result : &scalar_result;
        transform_workload* first_workload = (sample & 1u) == 0u
            ? &scalar_workload : &neon_workload;
        transform_workload* second_workload = (sample & 1u) == 0u
            ? &neon_workload : &scalar_workload;
        const transform_triangle_fn first_transform =
            (sample & 1u) == 0u ? scalar_transform : neon_transform;
        const transform_triangle_fn second_transform =
            (sample & 1u) == 0u ? neon_transform : scalar_transform;

        if (!run_transform_timed_sample(
                first_workload,
                first_transform,
                target_ns,
                &first_result->samples_ns[sample]
            ) ||
            !run_transform_timed_sample(
                second_workload,
                second_transform,
                target_ns,
                &second_result->samples_ns[sample]
            )) {
            (void)fprintf(stderr, "%s: timed sample failed\n", case_name);
            goto cleanup;
        }
    }

    scalar_result.checksum = capture_transform_checksum(
        &scalar_workload,
        scalar_transform,
        0xa5
    );
    neon_result.checksum = capture_transform_checksum(
        &neon_workload,
        neon_transform,
        0x5a
    );
    if (scalar_result.checksum != validation_scalar ||
        neon_result.checksum != validation_neon) {
        (void)fprintf(stderr, "%s: post-sampling validation changed\n", case_name);
        goto cleanup;
    }
    if (!summarize_result(&scalar_result, opts->sample_count) ||
        !summarize_result(&neon_result, opts->sample_count)) {
        (void)fprintf(stderr, "%s: result summary failed\n", case_name);
        goto cleanup;
    }

    print_backend_result(case_name, "scalar", &scalar_result);
    print_backend_result(case_name, "neon", &neon_result);
    *out_gate_passed = print_comparison(
        case_name,
        &scalar_result,
        &neon_result
    );
    success = 1;

cleanup:
    free(scalar_result.samples_ns);
    free(neon_result.samples_ns);
    return success;
}

static int run_query_case(
    const char* case_name,
    const options* opts,
    const soc_kernel_table* scalar_kernels,
    const soc_kernel_table* neon_kernels,
    soc_bool* out_gate_passed
)
{
    query_workload scalar_workload;
    query_workload neon_workload;
    backend_result scalar_result = {0};
    backend_result neon_result = {0};
    uint64_t validation_scalar = 0u;
    uint64_t validation_neon = 0u;
    uint64_t bounds_checksum;
    uint64_t hiz_checksum;
    uint64_t warmup_average;
    uint64_t target_ns;
    uint32_t sample;
    int success = 0;

    memset(&scalar_workload, 0, sizeof(scalar_workload));
    memset(&neon_workload, 0, sizeof(neon_workload));
    if (!initialize_query_workload(&scalar_workload, scalar_kernels) ||
        !initialize_query_workload(&neon_workload, scalar_kernels)) {
        (void)fprintf(stderr, "%s: workload initialization failed\n", case_name);
        goto cleanup;
    }
    bounds_checksum = checksum_bytes(
        scalar_workload.bounds,
        QUERY_AABB_COUNT * sizeof(*scalar_workload.bounds)
    );
    hiz_checksum = checksum_f32(
        scalar_workload.hiz.data,
        scalar_workload.hiz.element_count
    );
    if (bounds_checksum != checksum_bytes(
            neon_workload.bounds,
            QUERY_AABB_COUNT * sizeof(*neon_workload.bounds)
        ) ||
        hiz_checksum != checksum_f32(
            neon_workload.hiz.data,
            neon_workload.hiz.element_count
        ) ||
        !capture_query_checksum(
            &scalar_workload,
            scalar_kernels,
            0xa5,
            &validation_scalar
        ) ||
        !capture_query_checksum(
            &neon_workload,
            neon_kernels,
            0x5a,
            &validation_neon
        ) ||
        validation_scalar != validation_neon ||
        memcmp(
            scalar_workload.visibility,
            neon_workload.visibility,
            QUERY_AABB_COUNT * sizeof(*scalar_workload.visibility)
        ) != 0 ||
        memcmp(
            &scalar_workload.counts,
            &neon_workload.counts,
            sizeof(scalar_workload.counts)
        ) != 0 ||
        scalar_workload.counts.visible != QUERY_AABB_COUNT / 2u ||
        scalar_workload.counts.occluded != QUERY_AABB_COUNT / 2u ||
        scalar_workload.counts.unknown != 0u ||
        bounds_checksum != checksum_bytes(
            scalar_workload.bounds,
            QUERY_AABB_COUNT * sizeof(*scalar_workload.bounds)
        ) ||
        bounds_checksum != checksum_bytes(
            neon_workload.bounds,
            QUERY_AABB_COUNT * sizeof(*neon_workload.bounds)
        ) ||
        hiz_checksum != checksum_f32(
            scalar_workload.hiz.data,
            scalar_workload.hiz.element_count
        ) ||
        hiz_checksum != checksum_f32(
            neon_workload.hiz.data,
            neon_workload.hiz.element_count
        )) {
        (void)fprintf(stderr, "%s: Scalar/NEON validation mismatch\n", case_name);
        goto cleanup;
    }
    if (opts->validate_only == SOC_TRUE) {
        (void)printf(
            "case=%s validation=pass checksum=%016" PRIx64 "\n",
            case_name,
            validation_scalar
        );
        success = 1;
        goto cleanup;
    }
    if (scalar_kernels->test_aabbs == neon_kernels->test_aabbs) {
        (void)printf(
            "case=%s performance=unavailable reason=shared_implementation\n",
            case_name
        );
        *out_gate_passed = SOC_FALSE;
        success = 1;
        goto cleanup;
    }

    scalar_result.samples_ns = (uint64_t*)calloc(
        opts->sample_count,
        sizeof(*scalar_result.samples_ns)
    );
    neon_result.samples_ns = (uint64_t*)calloc(
        opts->sample_count,
        sizeof(*neon_result.samples_ns)
    );
    if (scalar_result.samples_ns == NULL || neon_result.samples_ns == NULL) {
        (void)fprintf(stderr, "%s: sample allocation failed\n", case_name);
        goto cleanup;
    }
    if (!run_query_timed_sample(
            &scalar_workload,
            scalar_kernels,
            WARMUP_TARGET_NS,
            &warmup_average
        ) ||
        !run_query_timed_sample(
            &neon_workload,
            neon_kernels,
            WARMUP_TARGET_NS,
            &warmup_average
        )) {
        (void)fprintf(stderr, "%s: warmup failed\n", case_name);
        goto cleanup;
    }

    target_ns = (uint64_t)opts->sample_ms * UINT64_C(1000000);
    for (sample = 0u; sample < opts->sample_count; ++sample) {
        backend_result* first_result = (sample & 1u) == 0u
            ? &scalar_result : &neon_result;
        backend_result* second_result = (sample & 1u) == 0u
            ? &neon_result : &scalar_result;
        query_workload* first_workload = (sample & 1u) == 0u
            ? &scalar_workload : &neon_workload;
        query_workload* second_workload = (sample & 1u) == 0u
            ? &neon_workload : &scalar_workload;
        const soc_kernel_table* first_kernels = (sample & 1u) == 0u
            ? scalar_kernels : neon_kernels;
        const soc_kernel_table* second_kernels = (sample & 1u) == 0u
            ? neon_kernels : scalar_kernels;

        if (!run_query_timed_sample(
                first_workload,
                first_kernels,
                target_ns,
                &first_result->samples_ns[sample]
            ) ||
            !run_query_timed_sample(
                second_workload,
                second_kernels,
                target_ns,
                &second_result->samples_ns[sample]
            )) {
            (void)fprintf(stderr, "%s: timed sample failed\n", case_name);
            goto cleanup;
        }
    }

    if (!capture_query_checksum(
            &scalar_workload,
            scalar_kernels,
            0xa5,
            &scalar_result.checksum
        ) ||
        !capture_query_checksum(
            &neon_workload,
            neon_kernels,
            0x5a,
            &neon_result.checksum
        ) ||
        scalar_result.checksum != validation_scalar ||
        neon_result.checksum != validation_neon ||
        scalar_result.checksum != neon_result.checksum ||
        bounds_checksum != checksum_bytes(
            scalar_workload.bounds,
            QUERY_AABB_COUNT * sizeof(*scalar_workload.bounds)
        ) ||
        bounds_checksum != checksum_bytes(
            neon_workload.bounds,
            QUERY_AABB_COUNT * sizeof(*neon_workload.bounds)
        ) ||
        hiz_checksum != checksum_f32(
            scalar_workload.hiz.data,
            scalar_workload.hiz.element_count
        ) ||
        hiz_checksum != checksum_f32(
            neon_workload.hiz.data,
            neon_workload.hiz.element_count
        )) {
        (void)fprintf(stderr, "%s: post-sampling validation changed\n", case_name);
        goto cleanup;
    }
    if (!summarize_result(&scalar_result, opts->sample_count) ||
        !summarize_result(&neon_result, opts->sample_count)) {
        (void)fprintf(stderr, "%s: result summary failed\n", case_name);
        goto cleanup;
    }

    print_backend_result(case_name, "scalar", &scalar_result);
    print_backend_result(case_name, "neon", &neon_result);
    *out_gate_passed = print_comparison(
        case_name,
        &scalar_result,
        &neon_result
    );
    success = 1;

cleanup:
    free(scalar_result.samples_ns);
    free(neon_result.samples_ns);
    destroy_query_workload(&scalar_workload);
    destroy_query_workload(&neon_workload);
    return success;
}

int main(int argc, char** argv)
{
    options opts;
    soc_cpu_features features;
    const soc_kernel_table* scalar_kernels;
    const soc_kernel_table* neon_kernels;
    soc_bool clear_gate = SOC_FALSE;
    soc_bool hiz_gate = SOC_FALSE;
    soc_bool raster_full_gate = SOC_FALSE;
    soc_bool raster_partial_gate = SOC_FALSE;
    soc_bool transform_gate = SOC_FALSE;
    soc_bool query_gate = SOC_FALSE;
    soc_bool required_gates_passed = SOC_TRUE;
    int parse_result;

    parse_result = parse_options(argc, argv, &opts);
    if (parse_result == 2) {
        return EXIT_SUCCESS;
    }
    if (parse_result != 0) {
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (opts.validate_only == SOC_TRUE &&
        opts.required_gates != REQUIRED_GATE_NONE) {
        (void)fprintf(
            stderr,
            "--validate-only cannot be combined with --require-gate\n"
        );
        return 2;
    }
    if (opts.required_gates != REQUIRED_GATE_NONE &&
        (opts.sample_count < DEFAULT_SAMPLE_COUNT ||
            opts.sample_ms < DEFAULT_SAMPLE_MS)) {
        (void)fprintf(
            stderr,
            "--require-gate needs at least %u samples and %u ms per sample\n",
            DEFAULT_SAMPLE_COUNT,
            DEFAULT_SAMPLE_MS
        );
        return 2;
    }
    if (opts.required_gates != REQUIRED_GATE_NONE &&
        strcmp(SOC_KERNEL_BENCH_BUILD_TYPE, "Release") != 0) {
        (void)fprintf(
            stderr,
            "--require-gate needs a Release build (current: %s)\n",
            SOC_KERNEL_BENCH_BUILD_TYPE
        );
        return 2;
    }
    scalar_kernels = soc_kernel_table_scalar();
    neon_kernels = soc_kernel_table_neon();
    features = soc_cpu_features_detect();
    if (scalar_kernels == NULL || scalar_kernels->clear_f32 == NULL ||
        scalar_kernels->store_constant_depth_block_f32 == NULL ||
        scalar_kernels->reduce_hiz_level_f32 == NULL ||
        scalar_kernels->test_aabbs == NULL) {
        (void)fprintf(stderr, "soc_kernel_bench: Scalar kernels unavailable\n");
        return EXIT_FAILURE;
    }
    if (neon_kernels == NULL ||
        neon_kernels->clear_f32 == NULL ||
        neon_kernels->store_constant_depth_block_f32 == NULL ||
        neon_kernels->reduce_hiz_level_f32 == NULL ||
        neon_kernels->test_aabbs == NULL ||
        build_architecture_matches_neon(features.architecture) != SOC_TRUE ||
        !soc_cpu_features_has(&features, SOC_CPU_FEATURE_NEON)) {
        (void)printf(
            "soc_kernel_bench status=skip reason=neon_unavailable"
            " architecture=%" PRIu32 " feature_flags=0x%08" PRIx32
            " required_gate=%s\n",
            features.architecture,
            features.flags,
            opts.required_gates == REQUIRED_GATE_NONE
                ? "not_requested"
                : "unavailable"
        );
        return opts.required_gates == REQUIRED_GATE_NONE
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }
    if (opts.validate_only != SOC_TRUE && !timer_initialize()) {
        (void)fprintf(stderr, "soc_kernel_bench: timer initialization failed\n");
        return EXIT_FAILURE;
    }

    if (opts.validate_only == SOC_TRUE) {
        (void)printf(
            "soc_kernel_bench mode=validate-only build_type=%s linkage=%s\n",
            SOC_KERNEL_BENCH_BUILD_TYPE,
            SOC_KERNEL_BENCH_LINKAGE
        );
    } else {
        (void)printf(
            "soc_kernel_bench samples=%" PRIu32 " sample_ms=%" PRIu32
            " warmup_ms=%" PRIu64 " timer=%s build_type=%s linkage=%s\n",
            opts.sample_count,
            opts.sample_ms,
            WARMUP_TARGET_NS / UINT64_C(1000000),
            timer_name(),
            SOC_KERNEL_BENCH_BUILD_TYPE,
            SOC_KERNEL_BENCH_LINKAGE
        );
    }

    if (!run_case(
            BENCHMARK_CLEAR_F32,
            "clear_f32.1920x1080",
            &opts,
            scalar_kernels,
            neon_kernels,
            &clear_gate
        ) ||
        !run_case(
            BENCHMARK_HIZ_REDUCE_LEVEL_F32,
            "hiz_reduce_level_f32.1920x1080",
            &opts,
            scalar_kernels,
            neon_kernels,
            &hiz_gate
        ) ||
        !run_case(
            BENCHMARK_RASTER_DEPTH_BLOCK_FULL_F32,
            "raster_depth_block_f32.full.1920x1080",
            &opts,
            scalar_kernels,
            neon_kernels,
            &raster_full_gate
        ) ||
        !run_case(
            BENCHMARK_RASTER_DEPTH_BLOCK_PARTIAL_F32,
            "raster_depth_block_f32.partial.1920x1080",
            &opts,
            scalar_kernels,
            neon_kernels,
            &raster_partial_gate
        ) ||
        !run_transform_case(
            "transform_triangle_f32.16384",
            &opts,
            &transform_gate
        ) ||
        !run_query_case(
            "aabb_query.common.65536",
            &opts,
            scalar_kernels,
            neon_kernels,
            &query_gate
        )) {
        return EXIT_FAILURE;
    }
    if (opts.validate_only == SOC_TRUE) {
        (void)printf("soc_kernel_bench status=validated\n");
        return EXIT_SUCCESS;
    }

    if ((opts.required_gates & REQUIRED_GATE_CLEAR_F32) != 0u &&
        clear_gate != SOC_TRUE) {
        required_gates_passed = SOC_FALSE;
    }
    if ((opts.required_gates & REQUIRED_GATE_HIZ_REDUCE_LEVEL_F32) != 0u &&
        hiz_gate != SOC_TRUE) {
        required_gates_passed = SOC_FALSE;
    }
    if ((opts.required_gates & REQUIRED_GATE_RASTER_DEPTH_BLOCK_F32) != 0u &&
        (raster_full_gate != SOC_TRUE ||
            raster_partial_gate != SOC_TRUE)) {
        required_gates_passed = SOC_FALSE;
    }
    if ((opts.required_gates & REQUIRED_GATE_TRANSFORM_TRIANGLE_F32) != 0u &&
        transform_gate != SOC_TRUE) {
        required_gates_passed = SOC_FALSE;
    }
    if ((opts.required_gates & REQUIRED_GATE_AABB_QUERY) != 0u &&
        query_gate != SOC_TRUE) {
        required_gates_passed = SOC_FALSE;
    }

    (void)printf(
        "soc_kernel_bench status=complete performance_gate=%s"
        " required_gate=%s sink=%016" PRIx64 "\n",
        clear_gate == SOC_TRUE && hiz_gate == SOC_TRUE &&
            raster_full_gate == SOC_TRUE &&
            raster_partial_gate == SOC_TRUE &&
            transform_gate == SOC_TRUE && query_gate == SOC_TRUE
            ? "pass"
            : "fail",
        opts.required_gates == REQUIRED_GATE_NONE
            ? "not_requested"
            : (required_gates_passed == SOC_TRUE ? "pass" : "fail"),
        (uint64_t)benchmark_sink
    );
    return required_gates_passed == SOC_TRUE ? EXIT_SUCCESS : EXIT_FAILURE;
}
