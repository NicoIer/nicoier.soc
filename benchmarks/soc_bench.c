#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <soc/soc.h>

#include <errno.h>
#include <fenv.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SOC_BENCH_HAVE_GENERATED_METADATA)
#include "soc_bench_git_metadata.h"
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>
#include <time.h>
#endif

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define DEFAULT_SAMPLES 15u
#define DEFAULT_SAMPLE_MS 200u
#define DEFAULT_SEED UINT64_C(0x534F4301)

#if !defined(SOC_BENCH_GIT_REVISION)
#define SOC_BENCH_GIT_REVISION "unknown"
#endif
#if !defined(SOC_BENCH_GIT_DIRTY)
#define SOC_BENCH_GIT_DIRTY "unknown"
#endif
#if !defined(SOC_BENCH_COMPILER_FLAGS)
#define SOC_BENCH_COMPILER_FLAGS "unknown"
#endif
#if !defined(SOC_BENCH_IPO)
#define SOC_BENCH_IPO "unknown"
#endif
#if !defined(SOC_BENCH_BUILD_TYPE)
#if defined(NDEBUG)
#define SOC_BENCH_BUILD_TYPE "release"
#else
#define SOC_BENCH_BUILD_TYPE "debug"
#endif
#endif
#if !defined(SOC_BENCH_LINKAGE)
#if defined(SOC_STATIC)
#define SOC_BENCH_LINKAGE "static"
#else
#define SOC_BENCH_LINKAGE "shared"
#endif
#endif

#if !defined(SOC_BENCH_COMPILER_ID)
#if defined(__clang__)
#define SOC_BENCH_COMPILER_ID "clang"
#elif defined(_MSC_VER)
#define SOC_BENCH_COMPILER_ID "msvc"
#elif defined(__GNUC__)
#define SOC_BENCH_COMPILER_ID "gcc"
#else
#define SOC_BENCH_COMPILER_ID "unknown"
#endif
#endif

#if !defined(SOC_BENCH_COMPILER_VERSION)
#if defined(__clang_version__)
#define SOC_BENCH_COMPILER_VERSION __clang_version__
#elif defined(_MSC_FULL_VER)
#define SOC_BENCH_COMPILER_VERSION STRINGIFY(_MSC_FULL_VER)
#elif defined(__VERSION__)
#define SOC_BENCH_COMPILER_VERSION __VERSION__
#else
#define SOC_BENCH_COMPILER_VERSION "unknown"
#endif
#endif

typedef enum bench_kind {
    BENCH_CLEAR,
    BENCH_HIZ,
    BENCH_GEOMETRY,
    BENCH_FILL,
    BENCH_OVERDRAW,
    BENCH_INSTANCE,
    BENCH_QUERY,
    BENCH_E2E,
    BENCH_CONTEXT_CREATE,
    BENCH_CONTEXT_RESIZE,
    BENCH_MESH_CREATE,
    BENCH_READBACK
} bench_kind;

typedef enum geometry_pattern {
    GEOMETRY_INSIDE,
    GEOMETRY_NEAR_CLIP,
    GEOMETRY_BACKFACE,
    GEOMETRY_DEGENERATE,
    GEOMETRY_OUTSIDE,
    GEOMETRY_SHARED_GRID
} geometry_pattern;

typedef enum query_pattern {
    QUERY_OCCLUDED,
    QUERY_VISIBLE,
    QUERY_MIXED,
    QUERY_MIXED_PERSPECTIVE
} query_pattern;

typedef struct bench_case {
    const char* name;
    const char* description;
    bench_kind kind;
    unsigned tier;
    uint32_t width;
    uint32_t height;
    uint32_t triangle_count;
    uint32_t instance_count;
    uint32_t query_count;
    uint32_t query_batch_size;
    soc_clip_depth_range clip_depth_range;
    geometry_pattern geometry_pattern;
    query_pattern query_pattern;
    soc_bool large_queries;
    soc_bool reverse_order;
    soc_index_type index_type;
    uint32_t vertex_stride;
    uint32_t position_offset;
    uint32_t readback_level;
    uint32_t repeat_count;
} bench_case;

typedef struct options {
    const char* suite;
    const char* filter;
    const char* output;
    uint32_t samples;
    uint32_t sample_ms;
    uint32_t worker_count;
    uint64_t seed;
    unsigned suite_tier;
    soc_bool list;
    soc_bool validate_only;
} options;

typedef struct bench_result {
    const bench_case* definition;
    uint64_t* samples_ns;
    uint64_t* iterations;
    uint64_t median_ns;
    uint64_t p95_ns;
    uint64_t mad_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    soc_bool noisy;
    soc_build_stats build_stats;
    soc_query_stats query_stats;
    uint64_t visible;
    uint64_t occluded;
    uint64_t unknown;
    uint64_t checksum;
} bench_result;

typedef struct workload {
    const bench_case* definition;
    uint64_t seed;
    uint32_t worker_count;
    soc_context* context;
    soc_mesh* mesh;
    soc_snapshot* snapshot;
    soc_frame_desc frame_desc;
    soc_mat4* transforms;
    soc_aabb* bounds;
    soc_visibility* visibility;
    float* depth;
    uint64_t depth_count;
    uint64_t readback_count;
    unsigned char* mesh_vertices;
    void* mesh_indices;
    uint32_t mesh_vertex_count;
    uint32_t mesh_index_count;
    soc_build_stats build_stats;
    soc_query_stats query_stats;
    uint64_t visible;
    uint64_t occluded;
    uint64_t unknown;
    uint64_t checksum;
    soc_bool capture_results;
} workload;

static const bench_case g_cases[] = {
    {.name = "frame.clear.64x64", .description = "Build an empty smoke snapshot",
     .kind = BENCH_CLEAR, .tier = 0u, .width = 64u, .height = 64u},
    {.name = "hiz.build.64x64", .description = "Build an empty smoke snapshot and Hi-Z",
     .kind = BENCH_HIZ, .tier = 0u, .width = 64u, .height = 64u},
    {.name = "geometry.inside.32", .description = "Build a snapshot from smoke in-frustum geometry",
     .kind = BENCH_GEOMETRY, .tier = 0u, .width = 64u, .height = 64u,
     .triangle_count = 32u, .instance_count = 1u},
    {.name = "query.batch.smoke.64", .description = "Query one small occluded batch",
     .kind = BENCH_QUERY, .tier = 0u, .width = 64u, .height = 64u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 64u,
     .query_batch_size = 64u},
    {.name = "pipeline.e2e.64x64", .description = "Smoke complete frame pipeline",
     .kind = BENCH_E2E, .tier = 0u, .width = 64u, .height = 64u,
     .triangle_count = 64u, .instance_count = 1u, .query_count = 64u,
     .query_batch_size = 64u, .query_pattern = QUERY_MIXED},

    {.name = "frame.clear.320x180", .description = "Build an empty 180p snapshot",
     .kind = BENCH_CLEAR, .tier = 1u, .width = 320u, .height = 180u},
    {.name = "frame.clear.640x360", .description = "Build an empty 360p snapshot",
     .kind = BENCH_CLEAR, .tier = 1u, .width = 640u, .height = 360u},
    {.name = "frame.clear.1280x720", .description = "Build an empty 720p snapshot",
     .kind = BENCH_CLEAR, .tier = 1u, .width = 1280u, .height = 720u},
    {.name = "frame.clear.npot.1279x719", .description = "Build an empty odd NPOT snapshot",
     .kind = BENCH_CLEAR, .tier = 1u, .width = 1279u, .height = 719u},
    {.name = "hiz.build.320x180", .description = "Build an empty 180p snapshot and Hi-Z",
     .kind = BENCH_HIZ, .tier = 1u, .width = 320u, .height = 180u},
    {.name = "hiz.build.640x360", .description = "Build an empty 360p snapshot and Hi-Z",
     .kind = BENCH_HIZ, .tier = 1u, .width = 640u, .height = 360u},
    {.name = "hiz.build.1280x720", .description = "Build an empty 720p snapshot and Hi-Z",
     .kind = BENCH_HIZ, .tier = 1u, .width = 1280u, .height = 720u},
    {.name = "hiz.build.npot.1279x719",
     .description = "Build an empty odd NPOT snapshot and Hi-Z",
     .kind = BENCH_HIZ, .tier = 1u, .width = 1279u, .height = 719u},
    {.name = "geometry.inside.16384",
     .description = "Build a snapshot from 16384 in-frustum triangles",
     .kind = BENCH_GEOMETRY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u},
    {.name = "geometry.outside.16384",
     .description = "Reject 16384 triangles outside the right clip plane",
     .kind = BENCH_GEOMETRY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u,
     .geometry_pattern = GEOMETRY_OUTSIDE},
    {.name = "geometry.shared_grid.u16.16384",
     .description = "Build a shared 16384-triangle grid with uint16 indices",
     .kind = BENCH_GEOMETRY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u,
     .geometry_pattern = GEOMETRY_SHARED_GRID,
     .index_type = SOC_INDEX_UINT16},
    {.name = "geometry.shared_grid.u32.16384",
     .description = "Build a shared 16384-triangle grid with uint32 indices",
     .kind = BENCH_GEOMETRY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u,
     .geometry_pattern = GEOMETRY_SHARED_GRID,
     .index_type = SOC_INDEX_UINT32},
    {.name = "geometry.near_clip.16384",
     .description = "Clip 16384 triangles crossing the near plane",
     .kind = BENCH_GEOMETRY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u,
     .geometry_pattern = GEOMETRY_NEAR_CLIP},
    {.name = "geometry.backface.16384",
     .description = "Cull 16384 back-facing triangles",
     .kind = BENCH_GEOMETRY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u,
     .geometry_pattern = GEOMETRY_BACKFACE},
    {.name = "geometry.degenerate.16384",
     .description = "Reject 16384 degenerate triangles",
     .kind = BENCH_GEOMETRY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u,
     .geometry_pattern = GEOMETRY_DEGENERATE},
    {.name = "geometry.msoc.1280x720.4096",
     .description = "Build 4096 triangles through high-resolution MSOC",
     .kind = BENCH_GEOMETRY, .tier = 2u, .width = 1280u, .height = 720u,
     .triangle_count = 4096u, .instance_count = 1u},
    {.name = "geometry.msoc.1280x720.8192",
     .description = "Build 8192 triangles through high-resolution MSOC",
     .kind = BENCH_GEOMETRY, .tier = 2u, .width = 1280u, .height = 720u,
     .triangle_count = 8192u, .instance_count = 1u},
    {.name = "geometry.msoc.1280x720.16384",
     .description = "Build 16384 triangles through high-resolution MSOC",
     .kind = BENCH_GEOMETRY, .tier = 2u, .width = 1280u, .height = 720u,
     .triangle_count = 16384u, .instance_count = 1u},
    {.name = "geometry.msoc.1280x720.32768",
     .description = "Build 32768 triangles through high-resolution MSOC",
     .kind = BENCH_GEOMETRY, .tier = 2u, .width = 1280u, .height = 720u,
     .triangle_count = 32768u, .instance_count = 1u},

    {.name = "raster.fill.fullscreen", .description = "Rasterize one full-screen triangle",
     .kind = BENCH_FILL, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u},
    {.name = "raster.overdraw.1x.front_to_back",
     .description = "Rasterize one full-screen layer",
     .kind = BENCH_OVERDRAW, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u},
    {.name = "raster.overdraw.4x.front_to_back",
     .description = "Rasterize four near-to-far layers",
     .kind = BENCH_OVERDRAW, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 4u},
    {.name = "raster.overdraw.4x.back_to_front",
     .description = "Rasterize four far-to-near layers",
     .kind = BENCH_OVERDRAW, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 4u, .reverse_order = SOC_TRUE},
    {.name = "raster.overdraw.16x.front_to_back",
     .description = "Rasterize sixteen near-to-far layers",
     .kind = BENCH_OVERDRAW, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 16u},
    {.name = "raster.overdraw.16x.back_to_front",
     .description = "Rasterize sixteen far-to-near layers",
     .kind = BENCH_OVERDRAW, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 16u, .reverse_order = SOC_TRUE},

    {.name = "snapshot.instances.1", .description = "Build a snapshot with one small instance",
     .kind = BENCH_INSTANCE, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 128u, .instance_count = 1u},
    {.name = "snapshot.instances.16", .description = "Build a snapshot with sixteen small instances",
     .kind = BENCH_INSTANCE, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 128u, .instance_count = 16u},
    {.name = "snapshot.instances.256", .description = "Build a snapshot with 256 small instances",
     .kind = BENCH_INSTANCE, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 128u, .instance_count = 256u},

    {.name = "query.batch.1", .description = "Query 65536 AABBs one at a time",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 1u},
    {.name = "query.batch.64", .description = "Query 65536 AABBs in batches of 64",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 64u},
    {.name = "query.batch.4096", .description = "Query 65536 AABBs in batches of 4096",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 4096u},
    {.name = "query.batch.65536", .description = "Query one 65536-AABB batch",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 65536u},
    {.name = "query.outcomes.visible.65536",
     .description = "Query a fully visible AABB distribution",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 65536u, .query_pattern = QUERY_VISIBLE},
    {.name = "query.outcomes.mixed.small.65536",
     .description = "Query mixed small projected AABBs",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 65536u, .query_pattern = QUERY_MIXED},
    {.name = "query.perspective.mixed.small.65536",
     .description = "Query mixed small AABBs through perspective projection",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 65536u,
     .query_pattern = QUERY_MIXED_PERSPECTIVE},
    {.name = "query.outcomes.mixed.large.65536",
     .description = "Query mixed large projected AABBs",
     .kind = BENCH_QUERY, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 1u, .instance_count = 1u, .query_count = 65536u,
     .query_batch_size = 65536u, .query_pattern = QUERY_MIXED,
     .large_queries = SOC_TRUE},

    {.name = "pipeline.e2e.320x180", .description = "Representative small frame",
     .kind = BENCH_E2E, .tier = 1u, .width = 320u, .height = 180u,
     .triangle_count = 4096u, .instance_count = 1u, .query_count = 10000u,
     .query_batch_size = 10000u, .query_pattern = QUERY_MIXED},
    {.name = "pipeline.e2e.640x360", .description = "Representative medium frame",
     .kind = BENCH_E2E, .tier = 1u, .width = 640u, .height = 360u,
     .triangle_count = 16384u, .instance_count = 1u, .query_count = 50000u,
     .query_batch_size = 50000u, .query_pattern = QUERY_MIXED},
    {.name = "pipeline.e2e.1280x720", .description = "Representative large frame",
     .kind = BENCH_E2E, .tier = 1u, .width = 1280u, .height = 720u,
     .triangle_count = 32768u, .instance_count = 1u, .query_count = 100000u,
     .query_batch_size = 100000u, .query_pattern = QUERY_MIXED},

    {.name = "pipeline.convention.zo.reversed",
     .description = "Medium frame with zero-to-one reversed Z",
     .kind = BENCH_E2E, .tier = 2u, .width = 640u, .height = 360u,
     .triangle_count = 4096u, .instance_count = 1u, .query_count = 10000u,
     .query_batch_size = 10000u, .query_pattern = QUERY_MIXED},
    {.name = "pipeline.convention.no.reversed",
     .description = "Medium frame with negative-one-to-one reversed Z",
     .kind = BENCH_E2E, .tier = 2u, .width = 640u, .height = 360u,
     .triangle_count = 4096u, .instance_count = 1u, .query_count = 10000u,
     .query_batch_size = 10000u, .query_pattern = QUERY_MIXED,
     .clip_depth_range = SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE},
    {.name = "lifecycle.context.create.1920x1080",
     .description = "Create and destroy a 1080p context",
     .kind = BENCH_CONTEXT_CREATE, .tier = 2u,
     .width = 1920u, .height = 1080u},
    {.name = "lifecycle.context.resize.1280x720",
     .description = "Resize a context from 320x180 to 1280x720",
     .kind = BENCH_CONTEXT_RESIZE, .tier = 2u,
     .width = 1280u, .height = 720u},
    {.name = "lifecycle.mesh.u16.stride12",
     .description = "Create a uint16 tightly packed mesh",
     .kind = BENCH_MESH_CREATE, .tier = 2u, .width = 64u, .height = 64u,
     .triangle_count = 16384u, .index_type = SOC_INDEX_UINT16,
     .vertex_stride = 12u},
    {.name = "lifecycle.mesh.u32.stride32",
     .description = "Create a uint32 interleaved mesh",
     .kind = BENCH_MESH_CREATE, .tier = 2u, .width = 64u, .height = 64u,
     .triangle_count = 16384u, .index_type = SOC_INDEX_UINT32,
     .vertex_stride = 32u, .position_offset = 8u},
    {.name = "readback.level0.1280x720",
     .description = "Copy a complete 720p Level 0 image",
     .kind = BENCH_READBACK, .tier = 2u, .width = 1280u, .height = 720u,
     .triangle_count = 512u, .instance_count = 1u},
    {.name = "readback.top.1280x720",
     .description = "Copy the top 720p Hi-Z level",
     .kind = BENCH_READBACK, .tier = 2u, .width = 1280u, .height = 720u,
     .triangle_count = 512u, .instance_count = 1u,
     .readback_level = UINT32_MAX, .repeat_count = 1024u}
};

static int timer_initialize(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (!QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) {
        return 1;
    }
#elif defined(__APPLE__)
    mach_timebase_info_data_t timebase;

    if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
        timebase.denom == 0u) {
        return 1;
    }
#else
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 1;
    }
#endif
    return 0;
}

static uint64_t timer_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64_t seconds;
    uint64_t remainder;

    if (frequency.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0) {
            return UINT64_MAX;
        }
    }
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0) {
        return UINT64_MAX;
    }
    seconds = (uint64_t)counter.QuadPart / (uint64_t)frequency.QuadPart;
    remainder = (uint64_t)counter.QuadPart % (uint64_t)frequency.QuadPart;
    return seconds * UINT64_C(1000000000) +
        (remainder * UINT64_C(1000000000)) /
        (uint64_t)frequency.QuadPart;
#elif defined(__APPLE__)
    static mach_timebase_info_data_t timebase;
    const uint64_t ticks = mach_absolute_time();
    uint64_t quotient;
    uint64_t remainder;

    if (timebase.denom == 0u) {
        if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
            timebase.denom == 0u) {
            return UINT64_MAX;
        }
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

static soc_mat4 identity_matrix(void)
{
    const soc_mat4 matrix = {
        .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
        .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
        .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
        .col3 = {0.0f, 0.0f, 0.0f, 1.0f}
    };
    return matrix;
}

static soc_frame_desc default_frame_desc(void)
{
    const soc_frame_desc desc = {
        .struct_size = sizeof(soc_frame_desc),
        .clip_from_world = {
            .col0 = {1.0f, 0.0f, 0.0f, 0.0f},
            .col1 = {0.0f, 1.0f, 0.0f, 0.0f},
            .col2 = {0.0f, 0.0f, 1.0f, 0.0f},
            .col3 = {0.0f, 0.0f, 0.0f, 1.0f}
        },
        .clip_depth_range = SOC_CLIP_DEPTH_ZERO_TO_ONE,
        .front_face = SOC_FRONT_FACE_CCW,
        .flags = SOC_FRAME_FLAG_NONE
    };
    return desc;
}

static soc_bool uses_perspective_queries(const bench_case* definition)
{
    return definition->query_pattern == QUERY_MIXED_PERSPECTIVE
        ? SOC_TRUE
        : SOC_FALSE;
}

static void configure_perspective_frame(soc_frame_desc* desc)
{
    /*
     * w = world z and clip z = 1. The near plane is z = 1, and normalized
     * reversed depth decreases toward zero as world z increases.
     */
    desc->clip_from_world.col2.z = 0.0f;
    desc->clip_from_world.col2.w = 1.0f;
    desc->clip_from_world.col3.z = 1.0f;
    desc->clip_from_world.col3.w = 0.0f;
}

static uint32_t rng_next(uint64_t* state)
{
    uint64_t value = *state;

    if (value == 0u) {
        value = UINT64_C(0x9E3779B97F4A7C15);
    }
    value ^= value >> 12u;
    value ^= value << 25u;
    value ^= value >> 27u;
    *state = value;
    return (uint32_t)((value * UINT64_C(2685821657736338717)) >> 32u);
}

static float rng_unit(uint64_t* state)
{
    return (float)(rng_next(state) >> 8u) * (1.0f / 16777216.0f);
}

static soc_bool checked_size_multiply(
    size_t left,
    size_t right,
    size_t* out_value
)
{
    if (out_value == NULL ||
        (right != 0u && left > SIZE_MAX / right)) {
        return SOC_FALSE;
    }
    *out_value = left * right;
    return SOC_TRUE;
}

static float case_occluder_depth(const bench_case* definition)
{
    if (uses_perspective_queries(definition) == SOC_TRUE) {
        return 2.0f;
    }
    if (definition->clip_depth_range ==
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
        return 0.25f;
    }
    return 0.75f;
}

static float case_visible_depth(const bench_case* definition)
{
    if (uses_perspective_queries(definition) == SOC_TRUE) {
        return 1.35f;
    }
    if (definition->clip_depth_range ==
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
        return 0.80f;
    }
    return 0.95f;
}

static float case_occluded_depth(const bench_case* definition)
{
    if (uses_perspective_queries(definition) == SOC_TRUE) {
        return 4.0f;
    }
    if (definition->clip_depth_range ==
        SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE) {
        return -0.40f;
    }
    return 0.35f;
}

static soc_result create_context(
    uint32_t width,
    uint32_t height,
    uint32_t worker_count,
    soc_context** out_context
)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = width,
        .height = height,
        .worker_count = worker_count,
        .flags = SOC_CONFIG_FLAG_NONE
    };
    return soc_context_create(&config, out_context);
}

static soc_result create_mesh(
    soc_context* context,
    const float* vertices,
    uint32_t vertex_count,
    const void* indices,
    uint32_t index_count,
    soc_index_type index_type,
    uint32_t flags,
    soc_mesh** out_mesh
)
{
    const soc_mesh_desc desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = flags,
        .vertices = vertices,
        .indices = indices,
        .vertex_count = vertex_count,
        .vertex_stride = 3u * (uint32_t)sizeof(float),
        .position_offset = 0u,
        .index_count = index_count,
        .index_type = index_type
    };
    return soc_mesh_create(context, &desc, out_mesh);
}

static int create_surface_mesh(
    workload* work,
    uint32_t triangle_count,
    soc_bool oversized
)
{
    float* vertices;
    uint32_t* indices;
    uint32_t triangle;
    uint64_t rng = work->seed ^ UINT64_C(0xA0761D6478BD642F);
    soc_result result;
    size_t vertex_bytes;
    size_t index_bytes;

    if (triangle_count == 0u ||
        triangle_count > UINT32_MAX / 3u ||
        !checked_size_multiply(
            (size_t)triangle_count,
            9u * sizeof(float),
            &vertex_bytes
        ) ||
        !checked_size_multiply(
            (size_t)triangle_count,
            3u * sizeof(uint32_t),
            &index_bytes
        )) {
        return 1;
    }
    vertices = (float*)malloc(vertex_bytes);
    indices = (uint32_t*)malloc(index_bytes);
    if (vertices == NULL || indices == NULL) {
        free(vertices);
        free(indices);
        return 1;
    }

    for (triangle = 0u; triangle < triangle_count; ++triangle) {
        float* v = &vertices[(size_t)triangle * 9u];
        const float depth_delta = 0.02f * rng_unit(&rng);
        const float depth =
            case_occluder_depth(work->definition) - depth_delta;

        if (oversized) {
            const float perspective_scale =
                uses_perspective_queries(work->definition) == SOC_TRUE
                ? depth : 1.0f;

            v[0] = -perspective_scale;
            v[1] = -perspective_scale;
            v[2] = depth;
            v[3] = 3.0f * perspective_scale;
            v[4] = -perspective_scale;
            v[5] = depth;
            v[6] = -perspective_scale;
            v[7] = 3.0f * perspective_scale;
            v[8] = depth;
        } else {
            uint32_t side = 1u;
            const uint32_t cell = triangle / 2u;
            uint32_t x;
            uint32_t y;
            float step;
            float x0;
            float y0;
            float x1;
            float y1;

            while ((uint64_t)side * (uint64_t)side * 2u <
                triangle_count) {
                ++side;
            }
            x = cell % side;
            y = (cell / side) % side;
            step = 2.0f / (float)side;
            x0 = -1.0f + (float)x * step;
            y0 = -1.0f + (float)y * step;
            x1 = x0 + step * 1.02f;
            y1 = y0 + step * 1.02f;

            if ((triangle & 1u) == 0u) {
                v[0] = x0; v[1] = y0; v[2] = depth;
                v[3] = x1; v[4] = y0; v[5] = depth;
                v[6] = x1; v[7] = y1; v[8] = depth;
            } else {
                v[0] = x0; v[1] = y0; v[2] = depth;
                v[3] = x1; v[4] = y1; v[5] = depth;
                v[6] = x0; v[7] = y1; v[8] = depth;
            }
        }
        indices[(size_t)triangle * 3u + 0u] = triangle * 3u + 0u;
        indices[(size_t)triangle * 3u + 1u] = triangle * 3u + 1u;
        indices[(size_t)triangle * 3u + 2u] = triangle * 3u + 2u;
    }
    result = create_mesh(
        work->context,
        vertices,
        triangle_count * 3u,
        indices,
        triangle_count * 3u,
        SOC_INDEX_UINT32,
        SOC_MESH_FLAG_TWO_SIDED,
        &work->mesh
    );
    free(vertices);
    free(indices);
    return result == SOC_RESULT_OK ? 0 : 1;
}

static void write_mesh_index(
    void* indices,
    soc_index_type index_type,
    uint32_t offset,
    uint32_t value
)
{
    if (index_type == SOC_INDEX_UINT16) {
        ((uint16_t*)indices)[offset] = (uint16_t)value;
    } else {
        ((uint32_t*)indices)[offset] = value;
    }
}

static int create_shared_grid_mesh(workload* work)
{
    const bench_case* definition = work->definition;
    const uint32_t triangle_count = definition->triangle_count;
    const size_t index_size =
        definition->index_type == SOC_INDEX_UINT16
        ? sizeof(uint16_t) : sizeof(uint32_t);
    uint32_t quad_count;
    uint32_t rows = 1u;
    uint32_t columns;
    uint32_t divisor;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t row;
    uint32_t column;
    float* vertices;
    void* indices;
    size_t vertex_bytes;
    size_t index_bytes;
    uint64_t vertex_count_u64;
    soc_result result;

    if (triangle_count == 0u || (triangle_count & 1u) != 0u ||
        triangle_count > UINT32_MAX / 3u ||
        (definition->index_type != SOC_INDEX_UINT16 &&
         definition->index_type != SOC_INDEX_UINT32)) {
        return 1;
    }
    quad_count = triangle_count / 2u;
    for (divisor = 1u;
         (uint64_t)divisor * divisor <= quad_count;
         ++divisor) {
        if (quad_count % divisor == 0u) {
            rows = divisor;
        }
    }
    columns = quad_count / rows;
    vertex_count_u64 =
        ((uint64_t)columns + 1u) * ((uint64_t)rows + 1u);
    if (vertex_count_u64 > UINT32_MAX ||
        (definition->index_type == SOC_INDEX_UINT16 &&
         vertex_count_u64 > UINT16_MAX)) {
        return 1;
    }
    vertex_count = (uint32_t)vertex_count_u64;
    index_count = triangle_count * 3u;
    if (!checked_size_multiply(
            (size_t)vertex_count,
            3u * sizeof(float),
            &vertex_bytes
        ) ||
        !checked_size_multiply(
            (size_t)index_count,
            index_size,
            &index_bytes
        )) {
        return 1;
    }
    vertices = (float*)malloc(vertex_bytes);
    indices = malloc(index_bytes);
    if (vertices == NULL || indices == NULL) {
        free(vertices);
        free(indices);
        return 1;
    }

    for (row = 0u; row <= rows; ++row) {
        for (column = 0u; column <= columns; ++column) {
            const uint32_t vertex = row * (columns + 1u) + column;
            float* position = &vertices[(size_t)vertex * 3u];

            position[0] = -0.90f +
                1.80f * (float)column / (float)columns;
            position[1] = -0.90f +
                1.80f * (float)row / (float)rows;
            position[2] = case_occluder_depth(definition);
        }
    }
    for (row = 0u; row < rows; ++row) {
        for (column = 0u; column < columns; ++column) {
            const uint32_t cell = row * columns + column;
            const uint32_t first_index = cell * 6u;
            const uint32_t top_left = row * (columns + 1u) + column;
            const uint32_t top_right = top_left + 1u;
            const uint32_t bottom_left = top_left + columns + 1u;
            const uint32_t bottom_right = bottom_left + 1u;

            write_mesh_index(indices, definition->index_type,
                first_index + 0u, top_left);
            write_mesh_index(indices, definition->index_type,
                first_index + 1u, top_right);
            write_mesh_index(indices, definition->index_type,
                first_index + 2u, bottom_right);
            write_mesh_index(indices, definition->index_type,
                first_index + 3u, top_left);
            write_mesh_index(indices, definition->index_type,
                first_index + 4u, bottom_right);
            write_mesh_index(indices, definition->index_type,
                first_index + 5u, bottom_left);
        }
    }
    result = create_mesh(
        work->context,
        vertices,
        vertex_count,
        indices,
        index_count,
        definition->index_type,
        SOC_MESH_FLAG_NONE,
        &work->mesh
    );
    free(vertices);
    free(indices);
    return result == SOC_RESULT_OK ? 0 : 1;
}

static int create_geometry_mesh(workload* work)
{
    const bench_case* definition = work->definition;
    const uint32_t triangle_count = definition->triangle_count;
    float* vertices;
    uint32_t* indices;
    uint32_t side = 1u;
    uint32_t triangle;
    soc_result result;
    size_t vertex_bytes;
    size_t index_bytes;

    if (definition->geometry_pattern == GEOMETRY_SHARED_GRID) {
        return create_shared_grid_mesh(work);
    }
    if (triangle_count == 0u ||
        triangle_count > UINT32_MAX / 3u ||
        !checked_size_multiply(
            (size_t)triangle_count,
            9u * sizeof(float),
            &vertex_bytes
        ) ||
        !checked_size_multiply(
            (size_t)triangle_count,
            3u * sizeof(uint32_t),
            &index_bytes
        )) {
        return 1;
    }
    vertices = (float*)malloc(vertex_bytes);
    indices = (uint32_t*)malloc(index_bytes);
    if (vertices == NULL || indices == NULL) {
        free(vertices);
        free(indices);
        return 1;
    }
    while ((uint64_t)side * side < triangle_count) {
        ++side;
    }

    for (triangle = 0u; triangle < triangle_count; ++triangle) {
        float* v = &vertices[(size_t)triangle * 9u];
        const uint32_t x = triangle % side;
        const uint32_t y = triangle / side;
        const float step = 1.80f / (float)side;
        const float x0 = -0.90f + ((float)x + 0.15f) * step;
        const float y0 = -0.90f + ((float)y + 0.15f) * step;
        const float x1 = x0 + 0.65f * step;
        const float y1 = y0 + 0.65f * step;
        const float depth = case_occluder_depth(definition);
        const float near_outside = 1.05f;
        const float near_inside = 0.95f;

        v[0] = x0; v[1] = y0; v[2] = depth;
        v[3] = x1; v[4] = y0; v[5] = depth;
        v[6] = x0; v[7] = y1; v[8] = depth;
        if (definition->geometry_pattern == GEOMETRY_NEAR_CLIP) {
            v[2] = near_outside;
            v[5] = near_inside;
            v[8] = near_inside;
        } else if (definition->geometry_pattern == GEOMETRY_BACKFACE) {
            v[3] = x0;
            v[4] = y1;
            v[6] = x1;
            v[7] = y0;
        } else if (definition->geometry_pattern == GEOMETRY_DEGENERATE) {
            v[3] = (x0 + x1) * 0.5f;
            v[4] = y0;
            v[6] = x1;
            v[7] = y0;
        } else if (definition->geometry_pattern == GEOMETRY_OUTSIDE) {
            v[0] += 2.0f;
            v[3] += 2.0f;
            v[6] += 2.0f;
        }
        indices[(size_t)triangle * 3u + 0u] = triangle * 3u + 0u;
        indices[(size_t)triangle * 3u + 1u] = triangle * 3u + 1u;
        indices[(size_t)triangle * 3u + 2u] = triangle * 3u + 2u;
    }
    result = create_mesh(
        work->context,
        vertices,
        triangle_count * 3u,
        indices,
        triangle_count * 3u,
        SOC_INDEX_UINT32,
        SOC_MESH_FLAG_NONE,
        &work->mesh
    );
    free(vertices);
    free(indices);
    return result == SOC_RESULT_OK ? 0 : 1;
}

static int allocate_transforms(workload* work)
{
    const bench_case* definition = work->definition;
    uint32_t index;
    uint64_t rng = work->seed ^ UINT64_C(0xE7037ED1A0B428DB);
    size_t transform_bytes;

    if (definition->instance_count == 0u) {
        return 0;
    }
    if (!checked_size_multiply(
            (size_t)definition->instance_count,
            sizeof(soc_mat4),
            &transform_bytes
        )) {
        return 1;
    }
    work->transforms = (soc_mat4*)malloc(transform_bytes);
    if (work->transforms == NULL) {
        return 1;
    }
    for (index = 0u; index < definition->instance_count; ++index) {
        soc_mat4 transform = identity_matrix();

        if (definition->kind == BENCH_OVERDRAW) {
            const float fraction = definition->instance_count > 1u
                ? (float)index / (float)(definition->instance_count - 1u)
                : 0.0f;
            const float front = case_visible_depth(definition);
            const float back = case_occluded_depth(definition);
            const float target = definition->reverse_order
                ? back + (front - back) * fraction
                : front + (back - front) * fraction;

            transform.col3.z =
                target - case_occluder_depth(definition);
        } else {
            const uint32_t side = 32u;
            const float x = ((float)(index % side) + 0.5f) / 16.0f - 1.0f;
            const float y = ((float)((index / side) % side) + 0.5f) /
                16.0f - 1.0f;
            transform.col0.x = 0.028f;
            transform.col1.y = 0.028f;
            transform.col3.x = x + (rng_unit(&rng) - 0.5f) * 0.004f;
            transform.col3.y = y + (rng_unit(&rng) - 0.5f) * 0.004f;
            transform.col3.z =
                case_visible_depth(definition) +
                (case_occluded_depth(definition) -
                 case_visible_depth(definition)) * rng_unit(&rng) -
                case_occluder_depth(definition);
        }
        work->transforms[index] = transform;
    }
    return 0;
}

static int allocate_queries(workload* work)
{
    const bench_case* definition = work->definition;
    const uint32_t count = definition->query_count;
    const soc_bool perspective = uses_perspective_queries(definition);
    uint32_t index;
    uint64_t rng = work->seed ^ UINT64_C(0x8EBC6AF09C88C6E3);
    size_t bounds_bytes;
    size_t visibility_bytes;

    if (count == 0u) {
        return 0;
    }
    if (!checked_size_multiply(
            (size_t)count,
            sizeof(soc_aabb),
            &bounds_bytes
        ) ||
        !checked_size_multiply(
            (size_t)count,
            sizeof(soc_visibility),
            &visibility_bytes
        )) {
        return 1;
    }
    work->bounds = (soc_aabb*)malloc(bounds_bytes);
    work->visibility = (soc_visibility*)malloc(visibility_bytes);
    if (work->bounds == NULL || work->visibility == NULL) {
        return 1;
    }
    for (index = 0u; index < count; ++index) {
        const float coordinate_range =
            definition->large_queries ? 0.55f : 0.85f;
        float x = -coordinate_range +
            2.0f * coordinate_range * rng_unit(&rng);
        float y = -coordinate_range +
            2.0f * coordinate_range * rng_unit(&rng);
        float radius = definition->large_queries
            ? 0.14f + 0.08f * rng_unit(&rng)
            : 0.006f + 0.018f * rng_unit(&rng);
        float half_depth = perspective == SOC_TRUE
            ? 0.05f
            : (definition->large_queries ? 0.04f : 0.01f);
        float center_z = case_occluded_depth(definition);
        float minimum_z;
        float maximum_z;

        if (definition->query_pattern == QUERY_VISIBLE) {
            center_z = case_visible_depth(definition);
        } else if (definition->query_pattern == QUERY_MIXED ||
                   definition->query_pattern == QUERY_MIXED_PERSPECTIVE) {
            const uint32_t bucket = index % 20u;

            if (bucket < 12u) {
                center_z = case_occluded_depth(definition);
            } else if (bucket < 17u) {
                center_z = case_visible_depth(definition);
            } else if (bucket < 19u) {
                x = 1.30f;
                center_z = case_occluded_depth(definition);
            } else {
                if (perspective == SOC_TRUE) {
                    center_z = 1.0f;
                    half_depth = 0.10f;
                } else {
                    minimum_z = 0.95f;
                    maximum_z = 1.05f;
                }
                if (perspective != SOC_TRUE) {
                    work->bounds[index].min.x = x - radius;
                    work->bounds[index].min.y = y - radius;
                    work->bounds[index].min.z = minimum_z;
                    work->bounds[index].max.x = x + radius;
                    work->bounds[index].max.y = y + radius;
                    work->bounds[index].max.z = maximum_z;
                    continue;
                }
            }
        }
        if (perspective == SOC_TRUE) {
            x *= center_z;
            y *= center_z;
            radius *= center_z;
        }
        minimum_z = center_z - half_depth;
        maximum_z = center_z + half_depth;
        work->bounds[index].min.x = x - radius;
        work->bounds[index].min.y = y - radius;
        work->bounds[index].min.z = minimum_z;
        work->bounds[index].max.x = x + radius;
        work->bounds[index].max.y = y + radius;
        work->bounds[index].max.z = maximum_z;
    }
    return 0;
}

static int allocate_mesh_create_inputs(workload* work)
{
    const bench_case* definition = work->definition;
    const uint32_t vertex_count = definition->triangle_count * 3u;
    const uint32_t index_count = vertex_count;
    const size_t index_size =
        definition->index_type == SOC_INDEX_UINT16
        ? sizeof(uint16_t) : sizeof(uint32_t);
    size_t vertex_bytes;
    size_t index_bytes;
    uint32_t index;

    if (definition->triangle_count == 0u ||
        definition->triangle_count > UINT32_MAX / 3u ||
        definition->vertex_stride < 3u * sizeof(float) ||
        definition->position_offset > definition->vertex_stride ||
        definition->vertex_stride - definition->position_offset <
            3u * sizeof(float) ||
        (definition->index_type == SOC_INDEX_UINT16 &&
         vertex_count > UINT16_MAX) ||
        (size_t)vertex_count > SIZE_MAX / definition->vertex_stride ||
        (size_t)index_count > SIZE_MAX / index_size) {
        return 1;
    }

    vertex_bytes = (size_t)vertex_count * definition->vertex_stride;
    index_bytes = (size_t)index_count * index_size;
    work->mesh_vertices = (unsigned char*)malloc(vertex_bytes);
    work->mesh_indices = malloc(index_bytes);
    if (work->mesh_vertices == NULL || work->mesh_indices == NULL) {
        return 1;
    }
    memset(work->mesh_vertices, 0xA5, vertex_bytes);
    for (index = 0u; index < vertex_count; ++index) {
        const float position[3] = {
            (float)(index % 257u) * (1.0f / 256.0f) - 0.5f,
            (float)((index / 257u) % 257u) * (1.0f / 256.0f) - 0.5f,
            case_occluder_depth(definition)
        };

        memcpy(
            work->mesh_vertices +
                (size_t)index * definition->vertex_stride +
                definition->position_offset,
            position,
            sizeof(position)
        );
        if (definition->index_type == SOC_INDEX_UINT16) {
            ((uint16_t*)work->mesh_indices)[index] = (uint16_t)index;
        } else {
            ((uint32_t*)work->mesh_indices)[index] = index;
        }
    }
    work->mesh_vertex_count = vertex_count;
    work->mesh_index_count = index_count;
    return 0;
}

static soc_result create_benchmark_mesh(workload* work)
{
    const bench_case* definition = work->definition;
    const soc_mesh_desc desc = {
        .struct_size = sizeof(soc_mesh_desc),
        .flags = SOC_MESH_FLAG_NONE,
        .vertices = work->mesh_vertices,
        .indices = work->mesh_indices,
        .vertex_count = work->mesh_vertex_count,
        .vertex_stride = definition->vertex_stride,
        .position_offset = definition->position_offset,
        .index_count = work->mesh_index_count,
        .index_type = definition->index_type
    };

    return soc_mesh_create(work->context, &desc, &work->mesh);
}

static soc_result workload_build_snapshot(workload* work)
{
    const soc_mat4 identity = identity_matrix();
    soc_occluder_group group;
    soc_occlusion_build_desc desc;

    if (work == NULL || work->context == NULL || work->snapshot != NULL) {
        return SOC_RESULT_INVALID_STATE;
    }

    memset(&group, 0, sizeof(group));
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.flags = SOC_OCCLUSION_BUILD_FLAG_NONE;
    desc.frame = &work->frame_desc;
    desc.group_stride = sizeof(group);

    if (work->mesh != NULL) {
        group.mesh = work->mesh;
        if (work->definition->kind == BENCH_OVERDRAW ||
            work->definition->kind == BENCH_INSTANCE) {
            group.object_to_world = work->transforms;
            group.instance_count = work->definition->instance_count;
        } else {
            group.object_to_world = &identity;
            group.instance_count = 1u;
        }
        group.flags = SOC_OCCLUDER_GROUP_FLAG_NONE;
        desc.groups = &group;
        desc.group_count = 1u;
    }

    return soc_occlusion_build(work->context, &desc, &work->snapshot);
}

static int workload_initialize(
    workload* work,
    const bench_case* definition,
    uint64_t seed,
    uint32_t worker_count
)
{
    memset(work, 0, sizeof(*work));
    work->definition = definition;
    work->seed = seed;
    work->worker_count = worker_count;
    work->frame_desc = default_frame_desc();
    work->frame_desc.clip_depth_range = definition->clip_depth_range;
    if (uses_perspective_queries(definition) == SOC_TRUE) {
        configure_perspective_frame(&work->frame_desc);
    }
    work->build_stats.struct_size = sizeof(work->build_stats);
    work->query_stats.struct_size = sizeof(work->query_stats);

    if (definition->kind == BENCH_CONTEXT_CREATE) {
        return 0;
    }
    if (create_context(
        definition->kind == BENCH_CONTEXT_RESIZE ? 320u : definition->width,
        definition->kind == BENCH_CONTEXT_RESIZE ? 180u : definition->height,
        worker_count,
        &work->context
    ) != SOC_RESULT_OK) {
        fprintf(stderr, "%s: context creation failed\n", definition->name);
        return 1;
    }

    switch (definition->kind) {
    case BENCH_MESH_CREATE:
        if (allocate_mesh_create_inputs(work) != 0) {
            return 1;
        }
        break;
    case BENCH_GEOMETRY:
        if (create_geometry_mesh(work) != 0) {
            return 1;
        }
        break;
    case BENCH_FILL:
        if (create_surface_mesh(
                work,
                definition->triangle_count,
                definition->triangle_count == 1u ? SOC_TRUE : SOC_FALSE
            ) != 0) {
            return 1;
        }
        break;
    case BENCH_E2E:
    case BENCH_READBACK:
        if (create_surface_mesh(work, definition->triangle_count, SOC_FALSE) !=
            0) {
            return 1;
        }
        break;
    case BENCH_OVERDRAW:
    case BENCH_QUERY:
        if (create_surface_mesh(work, 1u, SOC_TRUE) != 0) {
            return 1;
        }
        break;
    case BENCH_INSTANCE:
        if (create_surface_mesh(
                work,
                definition->triangle_count,
                SOC_FALSE
            ) != 0) {
            return 1;
        }
        break;
    default:
        break;
    }

    if (allocate_transforms(work) != 0 || allocate_queries(work) != 0) {
        return 1;
    }
    if (definition->kind != BENCH_CONTEXT_CREATE &&
        definition->kind != BENCH_CONTEXT_RESIZE &&
        definition->kind != BENCH_MESH_CREATE &&
        definition->kind != BENCH_QUERY &&
        definition->kind != BENCH_E2E) {
        const uint64_t count = (uint64_t)definition->width *
            (uint64_t)definition->height;

        if (count > SIZE_MAX / sizeof(float)) {
            return 1;
        }
        work->depth = (float*)malloc((size_t)count * sizeof(float));
        if (work->depth == NULL) {
            return 1;
        }
        work->depth_count = count;
    }
    return 0;
}

static void workload_destroy(workload* work)
{
    soc_snapshot_destroy(work->snapshot);
    if (work->mesh != NULL) {
        (void)soc_mesh_destroy(work->mesh);
    }
    soc_context_destroy(work->context);
    free(work->transforms);
    free(work->bounds);
    free(work->visibility);
    free(work->depth);
    free(work->mesh_vertices);
    free(work->mesh_indices);
    memset(work, 0, sizeof(*work));
}

static int workload_prepare(workload* work)
{
    soc_result result;
    const bench_kind kind = work->definition->kind;

    work->visible = 0u;
    work->occluded = 0u;
    work->unknown = 0u;
    work->checksum = 0u;
    memset(&work->build_stats, 0, sizeof(work->build_stats));
    work->build_stats.struct_size = sizeof(work->build_stats);
    memset(&work->query_stats, 0, sizeof(work->query_stats));
    work->query_stats.struct_size = sizeof(work->query_stats);

    if (kind != BENCH_QUERY && kind != BENCH_READBACK) {
        return 0;
    }

    result = workload_build_snapshot(work);
    if (result != SOC_RESULT_OK) {
        fprintf(stderr, "%s: snapshot setup failed (%d)\n",
            work->definition->name, (int)result);
        return 1;
    }
    result = soc_snapshot_get_build_stats(
        work->snapshot,
        &work->build_stats
    );
    if (result != SOC_RESULT_OK) {
        fprintf(stderr, "%s: snapshot stats failed (%d)\n",
            work->definition->name, (int)result);
        return 1;
    }
    return 0;
}

static soc_result run_query_batches(workload* work)
{
    const uint32_t count = work->definition->query_count;
    const uint32_t configured_batch =
        work->definition->query_batch_size;
    const uint32_t batch_size =
        configured_batch == 0u ? count : configured_batch;
    uint32_t offset = 0u;

    if (count == 0u || batch_size == 0u) {
        return SOC_RESULT_OK;
    }
    while (offset < count) {
        const uint32_t remaining = count - offset;
        const uint32_t batch =
            remaining < batch_size ? remaining : batch_size;
        soc_query_stats batch_stats = {
            .struct_size = sizeof(soc_query_stats)
        };
        const soc_result result = soc_snapshot_test_aabbs(
            work->snapshot,
            work->bounds + offset,
            batch,
            work->visibility + offset,
            &batch_stats
        );

        if (result != SOC_RESULT_OK) {
            return result;
        }
        work->query_stats.tested_aabb_count +=
            batch_stats.tested_aabb_count;
        work->query_stats.visible_aabb_count +=
            batch_stats.visible_aabb_count;
        work->query_stats.occluded_aabb_count +=
            batch_stats.occluded_aabb_count;
        work->query_stats.unknown_aabb_count +=
            batch_stats.unknown_aabb_count;
        offset += batch;
    }
    return SOC_RESULT_OK;
}

static int workload_run_timed(workload* work)
{
    const bench_case* definition = work->definition;
    soc_result result = SOC_RESULT_OK;

    switch (definition->kind) {
    case BENCH_CLEAR:
    case BENCH_HIZ:
    case BENCH_GEOMETRY:
    case BENCH_FILL:
    case BENCH_OVERDRAW:
    case BENCH_INSTANCE:
        result = workload_build_snapshot(work);
        break;
    case BENCH_QUERY:
        result = run_query_batches(work);
        break;
    case BENCH_E2E:
        result = workload_build_snapshot(work);
        if (result == SOC_RESULT_OK) {
            result = run_query_batches(work);
        }
        break;
    case BENCH_CONTEXT_CREATE: {
        soc_context* temporary = NULL;

        result = create_context(
            definition->width,
            definition->height,
            work->worker_count,
            &temporary
        );
        if (result == SOC_RESULT_OK) {
            soc_context_destroy(temporary);
        }
        break;
    }
    case BENCH_CONTEXT_RESIZE:
        result = soc_context_resize(
            work->context,
            definition->width,
            definition->height
        );
        break;
    case BENCH_MESH_CREATE:
        result = create_benchmark_mesh(work);
        break;
    case BENCH_READBACK: {
        const uint32_t level =
            definition->readback_level == UINT32_MAX
            ? work->build_stats.hiz_level_count - 1u
            : definition->readback_level;
        const uint32_t repeat_count =
            definition->repeat_count == 0u ? 1u : definition->repeat_count;
        uint32_t repeat;

        for (repeat = 0u; repeat < repeat_count; ++repeat) {
            soc_hiz_level_info info = {
                .struct_size = sizeof(soc_hiz_level_info)
            };

            result = soc_snapshot_hiz_level_query(
                work->snapshot,
                level,
                &info,
                work->depth,
                work->depth_count
            );
            if (result != SOC_RESULT_OK) {
                break;
            }
            work->readback_count = info.required_element_count;
        }
        break;
    }
    }

    if (result != SOC_RESULT_OK) {
        fprintf(stderr, "%s: timed operation failed (%d)\n",
            definition->name, (int)result);
        return 1;
    }
    return 0;
}

static void checksum_visibility(workload* work)
{
    uint32_t index;
    uint64_t hash = UINT64_C(14695981039346656037);

    for (index = 0u; index < work->definition->query_count; ++index) {
        const soc_visibility value = work->visibility[index];

        if (value == SOC_VISIBILITY_VISIBLE) {
            ++work->visible;
        } else if (value == SOC_VISIBILITY_OCCLUDED) {
            ++work->occluded;
        } else {
            ++work->unknown;
        }
        hash ^= (uint64_t)value;
        hash *= UINT64_C(1099511628211);
    }
    work->checksum = hash;
}

static void checksum_depth(workload* work, uint64_t count)
{
    uint64_t index;
    uint64_t hash = UINT64_C(14695981039346656037);

    for (index = 0u; index < count; ++index) {
        uint32_t bits;

        memcpy(&bits, &work->depth[index], sizeof(bits));
        hash ^= (uint64_t)bits;
        hash *= UINT64_C(1099511628211);
    }
    work->checksum = hash;
}

static soc_result capture_depth_pyramid(workload* work)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t level;

    for (level = 0u; level < work->build_stats.hiz_level_count; ++level) {
        soc_hiz_level_info info = {
            .struct_size = sizeof(soc_hiz_level_info)
        };
        soc_result result;
        uint64_t index;

        result = soc_snapshot_hiz_level_query(
            work->snapshot,
            level,
            &info,
            work->depth,
            work->depth_count
        );
        if (result != SOC_RESULT_OK) {
            return result;
        }
        if (info.level != level ||
            info.width == 0u ||
            info.height == 0u ||
            info.required_element_count == 0u ||
            info.required_element_count > work->depth_count) {
            return SOC_RESULT_INTERNAL_ERROR;
        }
        if (level == 0u &&
            (info.width != work->definition->width ||
             info.height != work->definition->height ||
             info.required_element_count != work->depth_count)) {
            return SOC_RESULT_INTERNAL_ERROR;
        }
        if (level + 1u == work->build_stats.hiz_level_count &&
            (info.width != 1u || info.height != 1u)) {
            return SOC_RESULT_INTERNAL_ERROR;
        }

        hash ^= info.level;
        hash *= UINT64_C(1099511628211);
        hash ^= info.width;
        hash *= UINT64_C(1099511628211);
        hash ^= info.height;
        hash *= UINT64_C(1099511628211);
        for (index = 0u; index < info.required_element_count; ++index) {
            uint32_t bits;

            memcpy(&bits, &work->depth[index], sizeof(bits));
            hash ^= (uint64_t)bits;
            hash *= UINT64_C(1099511628211);
        }
    }
    work->checksum = hash;
    return SOC_RESULT_OK;
}

static int workload_finish(workload* work)
{
    const bench_kind kind = work->definition->kind;
    soc_result result = SOC_RESULT_OK;

    if (kind == BENCH_CONTEXT_CREATE) {
        work->checksum = ((uint64_t)work->definition->width << 32u) |
            work->definition->height;
        return 0;
    }
    if (kind == BENCH_CONTEXT_RESIZE) {
        result = soc_context_resize(work->context, 320u, 180u);
        work->checksum =
            (((uint64_t)work->definition->width << 32u) |
             work->definition->height) ^
            UINT64_C(0x52535A45);
        if (result != SOC_RESULT_OK) {
            fprintf(stderr, "%s: resize reset failed (%d)\n",
                work->definition->name, (int)result);
            return 1;
        }
        return 0;
    }
    if (kind == BENCH_MESH_CREATE) {
        result = soc_mesh_destroy(work->mesh);
        work->mesh = NULL;
        work->checksum =
            ((uint64_t)work->definition->triangle_count << 32u) ^
            ((uint64_t)work->definition->vertex_stride << 16u) ^
            work->definition->index_type;
        if (result != SOC_RESULT_OK) {
            fprintf(stderr, "%s: mesh cleanup failed (%d)\n",
                work->definition->name, (int)result);
            return 1;
        }
        return 0;
    }

    if (result == SOC_RESULT_OK) {
        result = soc_snapshot_get_build_stats(
            work->snapshot,
            &work->build_stats
        );
    }
    if (result == SOC_RESULT_OK && work->capture_results &&
        (kind == BENCH_QUERY || kind == BENCH_E2E)) {
        checksum_visibility(work);
    } else if (result == SOC_RESULT_OK && work->capture_results &&
        kind == BENCH_READBACK) {
        checksum_depth(work, work->readback_count);
    } else if (result == SOC_RESULT_OK && work->capture_results &&
        work->depth != NULL) {
        result = capture_depth_pyramid(work);
    }
    soc_snapshot_destroy(work->snapshot);
    work->snapshot = NULL;
    if (result != SOC_RESULT_OK) {
        fprintf(stderr, "%s: snapshot cleanup failed (%d)\n",
            work->definition->name, (int)result);
        return 1;
    }
    return 0;
}

static int workload_validate(const workload* work)
{
    const bench_case* definition = work->definition;

    if (definition->kind != BENCH_CONTEXT_CREATE &&
        definition->kind != BENCH_CONTEXT_RESIZE &&
        definition->kind != BENCH_MESH_CREATE &&
        work->build_stats.hiz_level_count == 0u) {
        fprintf(stderr, "%s: validation failed: no Hi-Z levels\n",
            definition->name);
        return 1;
    }
    switch (definition->kind) {
    case BENCH_CLEAR:
    case BENCH_HIZ:
        if (work->build_stats.input_triangle_count != 0u) {
            return 1;
        }
        break;
    case BENCH_GEOMETRY:
        if (work->build_stats.input_triangle_count !=
            definition->triangle_count) {
            fprintf(stderr, "%s: geometry counters failed validation\n",
                definition->name);
            return 1;
        }
        if (definition->geometry_pattern == GEOMETRY_NEAR_CLIP &&
            (work->build_stats.clipped_triangle_count == 0u ||
             work->build_stats.rasterized_triangle_count == 0u)) {
            fprintf(stderr, "%s: near clipping was not exercised\n",
                definition->name);
            return 1;
        }
        if ((definition->geometry_pattern == GEOMETRY_BACKFACE ||
             definition->geometry_pattern == GEOMETRY_DEGENERATE) &&
            work->build_stats.rasterized_triangle_count != 0u) {
            fprintf(stderr, "%s: rejected geometry was rasterized\n",
                definition->name);
            return 1;
        }
        if (definition->geometry_pattern == GEOMETRY_OUTSIDE &&
            (work->build_stats.clipped_triangle_count !=
                 definition->triangle_count ||
             work->build_stats.rasterized_triangle_count != 0u)) {
            fprintf(stderr,
                "%s: fully outside geometry counters failed validation\n",
                definition->name);
            return 1;
        }
        if (definition->geometry_pattern == GEOMETRY_SHARED_GRID &&
            (work->build_stats.clipped_triangle_count != 0u ||
             work->build_stats.rasterized_triangle_count !=
                 definition->triangle_count)) {
            fprintf(stderr,
                "%s: shared grid counters failed validation\n",
                definition->name);
            return 1;
        }
        if (definition->geometry_pattern == GEOMETRY_INSIDE &&
            work->build_stats.rasterized_triangle_count == 0u) {
            fprintf(stderr, "%s: in-frustum geometry was not rasterized\n",
                definition->name);
            return 1;
        }
        break;
    case BENCH_FILL:
    case BENCH_E2E:
    case BENCH_READBACK:
        if (work->build_stats.input_triangle_count !=
            definition->triangle_count) {
            fprintf(stderr, "%s: triangle count failed validation\n",
                definition->name);
            return 1;
        }
        break;
    case BENCH_OVERDRAW:
    case BENCH_INSTANCE:
        if (work->build_stats.input_triangle_count !=
            (uint64_t)definition->triangle_count *
                definition->instance_count) {
            fprintf(stderr, "%s: instance count failed validation\n",
                definition->name);
            return 1;
        }
        break;
    case BENCH_QUERY:
        break;
    case BENCH_CONTEXT_CREATE:
    case BENCH_CONTEXT_RESIZE:
    case BENCH_MESH_CREATE:
        if (work->checksum == 0u) {
            return 1;
        }
        break;
    }

    if (definition->query_count != 0u) {
        if (work->visible + work->occluded + work->unknown !=
            definition->query_count ||
            work->query_stats.tested_aabb_count != definition->query_count) {
            fprintf(stderr, "%s: query counters failed validation\n",
                definition->name);
            return 1;
        }
        if (definition->query_pattern == QUERY_OCCLUDED &&
            work->occluded != definition->query_count) {
            fprintf(stderr, "%s: expected fully occluded query batch\n",
                definition->name);
            return 1;
        }
        if (definition->query_pattern == QUERY_VISIBLE &&
            work->visible != definition->query_count) {
            fprintf(stderr, "%s: expected fully visible query batch\n",
                definition->name);
            return 1;
        }
        if ((definition->query_pattern == QUERY_MIXED ||
             definition->query_pattern == QUERY_MIXED_PERSPECTIVE) &&
            (work->visible == 0u || work->occluded == 0u ||
             work->unknown == 0u)) {
            fprintf(stderr, "%s: expected mixed visibility outcomes\n",
                definition->name);
            return 1;
        }
    }
    if (definition->kind == BENCH_READBACK && work->checksum == 0u) {
        return 1;
    }
    if (definition->kind == BENCH_READBACK &&
        definition->readback_level == UINT32_MAX &&
        work->readback_count != 1u) {
        fprintf(stderr, "%s: top-level readback was not 1x1\n",
            definition->name);
        return 1;
    }
    if (work->capture_results && work->checksum == 0u) {
        fprintf(stderr, "%s: validation produced an empty checksum\n",
            definition->name);
        return 1;
    }
    return 0;
}

static int run_timed_operation(workload* work, uint64_t* elapsed_ns)
{
    uint64_t begin;
    uint64_t end;

    begin = timer_now_ns();
    if (workload_run_timed(work) != 0) {
        return 1;
    }
    end = timer_now_ns();
    if (begin == UINT64_MAX || end == UINT64_MAX || end <= begin) {
        fprintf(stderr, "monotonic clock failed to advance\n");
        return 1;
    }
    *elapsed_ns = end - begin;
    return 0;
}

static int run_one_operation(workload* work, uint64_t* elapsed_ns)
{
    if (workload_prepare(work) != 0) {
        return 1;
    }
    if (run_timed_operation(work, elapsed_ns) != 0) {
        return 1;
    }
    if (workload_finish(work) != 0) {
        return 1;
    }
    return 0;
}

static int run_validation_operation(workload* work)
{
    if (workload_prepare(work) != 0) {
        return 1;
    }
    if (workload_run_timed(work) != 0) {
        return 1;
    }
    return workload_finish(work);
}

static int can_reuse_prepared_state(const workload* work)
{
    const bench_kind kind = work->definition->kind;

    /*
     * Queries and depth readbacks are read-only after the Hi-Z pyramid has
     * been built. Reusing that prepared frame keeps setup outside the timed
     * phase without rebuilding the same pyramid for every sub-microsecond
     * operation.
     */
    return kind == BENCH_QUERY || kind == BENCH_READBACK;
}

static int run_operation_series(
    workload* work,
    uint64_t target_ns,
    uint64_t minimum_iterations,
    uint64_t* total_ns,
    uint64_t* iterations
)
{
    const int reuse_prepared_state = can_reuse_prepared_state(work);

    *total_ns = 0u;
    *iterations = 0u;
    if (reuse_prepared_state && workload_prepare(work) != 0) {
        return 1;
    }

    do {
        uint64_t operation_ns;
        int operation_result;

        if (reuse_prepared_state) {
            operation_result = run_timed_operation(work, &operation_ns);
        } else {
            operation_result = run_one_operation(work, &operation_ns);
        }
        if (operation_result != 0) {
            return 1;
        }
        if (*total_ns > UINT64_MAX - operation_ns ||
            *iterations == UINT64_MAX) {
            fprintf(stderr, "%s: iteration accounting overflow\n",
                work->definition->name);
            return 1;
        }
        *total_ns += operation_ns;
        ++*iterations;
    } while (*iterations < minimum_iterations || *total_ns < target_ns);

    if (reuse_prepared_state && workload_finish(work) != 0) {
        return 1;
    }
    return 0;
}

static int compare_u64(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return (a > b) - (a < b);
}

static uint64_t median_sorted(const uint64_t* values, uint32_t count)
{
    uint64_t lower;
    uint64_t upper;

    if ((count & 1u) != 0u) {
        return values[count / 2u];
    }
    lower = values[count / 2u - 1u];
    upper = values[count / 2u];
    return lower / 2u + upper / 2u +
        ((lower & 1u) + (upper & 1u) + 1u) / 2u;
}

static int summarize_result(bench_result* result, uint32_t sample_count)
{
    uint64_t* sorted;
    uint64_t* deviations;
    uint32_t index;
    uint32_t p95_index;

    if (sample_count == 0u) {
        return 0;
    }
    sorted = (uint64_t*)malloc((size_t)sample_count * sizeof(uint64_t));
    deviations = (uint64_t*)malloc((size_t)sample_count * sizeof(uint64_t));
    if (sorted == NULL || deviations == NULL) {
        fprintf(stderr, "out of memory while summarizing samples\n");
        free(sorted);
        free(deviations);
        return 1;
    }
    memcpy(sorted, result->samples_ns,
        (size_t)sample_count * sizeof(uint64_t));
    qsort(sorted, sample_count, sizeof(uint64_t), compare_u64);
    result->median_ns = median_sorted(sorted, sample_count);
    result->min_ns = sorted[0];
    result->max_ns = sorted[sample_count - 1u];
    p95_index = (95u * sample_count + 99u) / 100u;
    if (p95_index == 0u) {
        p95_index = 1u;
    }
    result->p95_ns = sorted[p95_index - 1u];
    for (index = 0u; index < sample_count; ++index) {
        deviations[index] = result->samples_ns[index] > result->median_ns
            ? result->samples_ns[index] - result->median_ns
            : result->median_ns - result->samples_ns[index];
    }
    qsort(deviations, sample_count, sizeof(uint64_t), compare_u64);
    result->mad_ns = median_sorted(deviations, sample_count);
    result->noisy =
        result->median_ns != 0u &&
        result->mad_ns * UINT64_C(100) >
            result->median_ns * UINT64_C(3)
        ? SOC_TRUE : SOC_FALSE;
    free(sorted);
    free(deviations);
    return 0;
}

static int result_matches_workload(
    const bench_result* result,
    const workload* work
)
{
    return result->build_stats.hiz_level_count ==
            work->build_stats.hiz_level_count &&
        result->build_stats.input_triangle_count ==
            work->build_stats.input_triangle_count &&
        result->build_stats.clipped_triangle_count ==
            work->build_stats.clipped_triangle_count &&
        result->build_stats.rasterized_triangle_count ==
            work->build_stats.rasterized_triangle_count &&
        result->query_stats.tested_aabb_count ==
            work->query_stats.tested_aabb_count &&
        result->query_stats.occluded_aabb_count ==
            work->query_stats.occluded_aabb_count &&
        result->visible == work->visible &&
        result->occluded == work->occluded &&
        result->unknown == work->unknown &&
        result->checksum == work->checksum;
}

static int benchmark_case_run(
    const bench_case* definition,
    const options* opts,
    bench_result* result
)
{
    workload work;
    uint32_t sample;

    memset(result, 0, sizeof(*result));
    result->definition = definition;
    result->build_stats.struct_size = sizeof(result->build_stats);
    result->query_stats.struct_size = sizeof(result->query_stats);

    if (workload_initialize(
            &work,
            definition,
            opts->seed,
            opts->worker_count
        ) != 0) {
        workload_destroy(&work);
        return 1;
    }

    work.capture_results = SOC_TRUE;
    if (run_validation_operation(&work) != 0 ||
        workload_validate(&work) != 0) {
        workload_destroy(&work);
        return 1;
    }
    result->build_stats = work.build_stats;
    result->query_stats = work.query_stats;
    result->visible = work.visible;
    result->occluded = work.occluded;
    result->unknown = work.unknown;
    result->checksum = work.checksum;
    work.capture_results = SOC_FALSE;

    if (!opts->validate_only) {
        uint64_t warmup_ns;
        uint64_t warmup_iterations;

        result->samples_ns = (uint64_t*)calloc(
            opts->samples,
            sizeof(uint64_t)
        );
        result->iterations = (uint64_t*)calloc(
            opts->samples,
            sizeof(uint64_t)
        );
        if (result->samples_ns == NULL || result->iterations == NULL) {
            workload_destroy(&work);
            return 1;
        }

        /* Warm for at least five operations and 250 ms of measured work. */
        if (run_operation_series(
                &work,
                UINT64_C(250000000),
                UINT64_C(5),
                &warmup_ns,
                &warmup_iterations
            ) != 0) {
            workload_destroy(&work);
            return 1;
        }

        for (sample = 0u; sample < opts->samples; ++sample) {
            const uint64_t target_ns =
                (uint64_t)opts->sample_ms * UINT64_C(1000000);
            const uint64_t repeat_count =
                definition->repeat_count == 0u
                ? 1u : definition->repeat_count;
            uint64_t total_ns = 0u;
            uint64_t iterations = 0u;

            if (run_operation_series(
                    &work,
                    target_ns,
                    UINT64_C(1),
                    &total_ns,
                    &iterations
                ) != 0) {
                workload_destroy(&work);
                return 1;
            }

            if (iterations > UINT64_MAX / repeat_count) {
                fprintf(stderr, "%s: operation count overflow\n",
                    definition->name);
                workload_destroy(&work);
                return 1;
            }
            result->iterations[sample] = iterations * repeat_count;
            result->samples_ns[sample] =
                (total_ns + result->iterations[sample] / 2u) /
                result->iterations[sample];
        }
        if (summarize_result(result, opts->samples) != 0) {
            workload_destroy(&work);
            return 1;
        }

        /*
         * Re-run the untimed validation capture after sampling. This catches
         * accidental state drift and ensures the reported checksum and public
         * counters still describe the sampled workload.
         */
        work.capture_results = SOC_TRUE;
        if (run_validation_operation(&work) != 0 ||
            workload_validate(&work) != 0 ||
            !result_matches_workload(result, &work)) {
            fprintf(stderr, "%s: post-sampling validation changed results\n",
                definition->name);
            workload_destroy(&work);
            return 1;
        }
        work.capture_results = SOC_FALSE;

        if (result->noisy) {
            fprintf(stderr,
                "%s: noisy result (MAD exceeds 3%% of median)\n",
                definition->name);
        }
    }
    workload_destroy(&work);
    return 0;
}

static int parse_u32(const char* text, uint32_t minimum, uint32_t* out_value)
{
    char* end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > UINT32_MAX) {
        return 1;
    }
    *out_value = (uint32_t)value;
    return 0;
}

static int parse_u64(const char* text, uint64_t* out_value)
{
    char* end = NULL;
    uint64_t value;

    if (text[0] == '-') {
        return 1;
    }
    errno = 0;
#if defined(_MSC_VER)
    value = _strtoui64(text, &end, 0);
#else
    value = (uint64_t)strtoull(text, &end, 0);
#endif
    if (errno != 0 || end == text || *end != '\0') {
        return 1;
    }
    *out_value = value;
    return 0;
}

static void print_usage(FILE* stream, const char* program)
{
    fprintf(stream,
        "Usage: %s [options]\n"
        "  --suite smoke|core|full  Select benchmark suite (default: core)\n"
        "  --filter TEXT            Run cases whose names contain TEXT\n"
        "  --samples N              Samples per case (default: %u)\n"
        "  --sample-ms N            Timed milliseconds per sample target "
        "(default: %u)\n"
        "  --workers N              Context workers (default: 1)\n"
        "  --seed N                 Synthetic workload seed (default: "
        "0x534F4301)\n"
        "  --output PATH            Write JSON to PATH instead of stdout\n"
        "  --list                   List selected cases without running\n"
        "  --validate-only          Validate workloads without timing\n"
        "  --help                   Show this help\n",
        program, DEFAULT_SAMPLES, DEFAULT_SAMPLE_MS);
}

static int parse_options(int argc, char** argv, options* opts)
{
    int index;

    memset(opts, 0, sizeof(*opts));
    opts->suite = "core";
    opts->suite_tier = 1u;
    opts->samples = DEFAULT_SAMPLES;
    opts->sample_ms = DEFAULT_SAMPLE_MS;
    opts->worker_count = 1u;
    opts->seed = DEFAULT_SEED;

    for (index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        const char* value = NULL;
        const char* equals = strchr(argument, '=');

        if (strcmp(argument, "--help") == 0 ||
            strcmp(argument, "-h") == 0) {
            print_usage(stdout, argv[0]);
            return 2;
        }
        if (strcmp(argument, "--list") == 0) {
            opts->list = SOC_TRUE;
            continue;
        }
        if (strcmp(argument, "--validate-only") == 0) {
            opts->validate_only = SOC_TRUE;
            continue;
        }
        if (equals != NULL) {
            value = equals + 1;
        } else {
            if (index + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", argument);
                return 1;
            }
            value = argv[++index];
        }

        if (strncmp(argument, "--suite", 7u) == 0 &&
            (argument[7] == '\0' || argument[7] == '=')) {
            opts->suite = value;
            if (strcmp(value, "smoke") == 0) {
                opts->suite_tier = 0u;
            } else if (strcmp(value, "core") == 0) {
                opts->suite_tier = 1u;
            } else if (strcmp(value, "full") == 0) {
                opts->suite_tier = 2u;
            } else {
                fprintf(stderr, "invalid suite: %s\n", value);
                return 1;
            }
        } else if (strncmp(argument, "--filter", 8u) == 0 &&
            (argument[8] == '\0' || argument[8] == '=')) {
            opts->filter = value;
        } else if (strncmp(argument, "--samples", 9u) == 0 &&
            (argument[9] == '\0' || argument[9] == '=')) {
            if (parse_u32(value, 1u, &opts->samples) != 0 ||
                opts->samples > 10000u) {
                fprintf(stderr, "invalid sample count: %s\n", value);
                return 1;
            }
        } else if (strncmp(argument, "--sample-ms", 11u) == 0 &&
            (argument[11] == '\0' || argument[11] == '=')) {
            if (parse_u32(value, 1u, &opts->sample_ms) != 0 ||
                opts->sample_ms > 600000u) {
                fprintf(stderr, "invalid sample duration: %s\n", value);
                return 1;
            }
        } else if (strncmp(argument, "--workers", 9u) == 0 &&
            (argument[9] == '\0' || argument[9] == '=')) {
            if (parse_u32(value, 1u, &opts->worker_count) != 0) {
                fprintf(stderr, "invalid worker count: %s\n", value);
                return 1;
            }
        } else if (strncmp(argument, "--seed", 6u) == 0 &&
            (argument[6] == '\0' || argument[6] == '=')) {
            if (parse_u64(value, &opts->seed) != 0) {
                fprintf(stderr, "invalid seed: %s\n", value);
                return 1;
            }
        } else if (strncmp(argument, "--output", 8u) == 0 &&
            (argument[8] == '\0' || argument[8] == '=')) {
            opts->output = value;
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            return 1;
        }
    }
    return 0;
}

static soc_bool case_selected(
    const bench_case* definition,
    const options* opts
)
{
    return definition->tier <= opts->suite_tier &&
        (opts->filter == NULL ||
         strstr(definition->name, opts->filter) != NULL)
        ? SOC_TRUE : SOC_FALSE;
}

static const char* os_name(void)
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "unknown";
#endif
}

static void os_version(char* output, size_t capacity)
{
    if (capacity == 0u) {
        return;
    }
    output[0] = '\0';
#if defined(_WIN32)
    {
        OSVERSIONINFOA info;

        memset(&info, 0, sizeof(info));
        info.dwOSVersionInfoSize = sizeof(info);
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        if (GetVersionExA(&info)) {
            (void)snprintf(
                output,
                capacity,
                "%lu.%lu.%lu",
                (unsigned long)info.dwMajorVersion,
                (unsigned long)info.dwMinorVersion,
                (unsigned long)info.dwBuildNumber
            );
        }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    }
#else
    {
        struct utsname info;

        if (uname(&info) == 0) {
            (void)snprintf(output, capacity, "%s", info.release);
        }
    }
#endif
    if (output[0] == '\0') {
        (void)snprintf(output, capacity, "%s", "unknown");
    }
}

static const char* architecture_name(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#else
    return "unknown";
#endif
}

static const char* fast_math_name(void)
{
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    return "on";
#else
    return "off";
#endif
}

static const char* compiler_name(void)
{
    return SOC_BENCH_COMPILER_ID;
}

#define STRINGIFY_DETAIL(value) #value
#define STRINGIFY(value) STRINGIFY_DETAIL(value)

static const char* compiler_version(void)
{
    return SOC_BENCH_COMPILER_VERSION;
}

static void cpu_name(char* output, size_t capacity)
{
    if (capacity == 0u) {
        return;
    }
    output[0] = '\0';
#if defined(_WIN32)
    {
        WCHAR wide_output[256];
        const DWORD result = GetEnvironmentVariableW(
            L"PROCESSOR_IDENTIFIER",
            wide_output,
            (DWORD)ARRAY_COUNT(wide_output)
        );

        if (result == 0u || result >= ARRAY_COUNT(wide_output) ||
            WideCharToMultiByte(
                CP_UTF8,
                0u,
                wide_output,
                -1,
                output,
                capacity > INT_MAX ? INT_MAX : (int)capacity,
                NULL,
                NULL
            ) == 0) {
            output[0] = '\0';
        }
    }
#elif defined(__APPLE__)
    {
        size_t length = capacity;
        if (sysctlbyname("machdep.cpu.brand_string", output, &length,
            NULL, 0u) != 0) {
            output[0] = '\0';
        }
    }
#elif defined(__linux__)
    {
        FILE* file = fopen("/proc/cpuinfo", "r");
        char line[512];

        if (file != NULL) {
            while (fgets(line, sizeof(line), file) != NULL) {
                const char* labels[] = {"model name", "Hardware", "Processor"};
                size_t label;

                for (label = 0u; label < ARRAY_COUNT(labels); ++label) {
                    const size_t length = strlen(labels[label]);
                    if (strncmp(line, labels[label], length) == 0) {
                        const char* colon = strchr(line, ':');
                        if (colon != NULL) {
                            const char* value = colon + 1;
                            size_t value_length;
                            while (*value == ' ' || *value == '\t') {
                                ++value;
                            }
                            value_length = strcspn(value, "\r\n");
                            if (value_length >= capacity) {
                                value_length = capacity - 1u;
                            }
                            memcpy(output, value, value_length);
                            output[value_length] = '\0';
                        }
                        break;
                    }
                }
                if (output[0] != '\0') {
                    break;
                }
            }
            (void)fclose(file);
        }
    }
#endif
    if (output[0] == '\0') {
        (void)snprintf(output, capacity, "%s", architecture_name());
    }
}

static int json_string(FILE* output, const char* text)
{
    const unsigned char* cursor = (const unsigned char*)text;

    if (fputc('"', output) == EOF) {
        return 1;
    }
    while (*cursor != '\0') {
        const unsigned char value = *cursor++;

        switch (value) {
        case '"':
            if (fputs("\\\"", output) == EOF) return 1;
            break;
        case '\\':
            if (fputs("\\\\", output) == EOF) return 1;
            break;
        case '\b':
            if (fputs("\\b", output) == EOF) return 1;
            break;
        case '\f':
            if (fputs("\\f", output) == EOF) return 1;
            break;
        case '\n':
            if (fputs("\\n", output) == EOF) return 1;
            break;
        case '\r':
            if (fputs("\\r", output) == EOF) return 1;
            break;
        case '\t':
            if (fputs("\\t", output) == EOF) return 1;
            break;
        default:
            if (value < 0x20u) {
                if (fprintf(output, "\\u%04x", (unsigned)value) < 0) return 1;
            } else if (fputc((int)value, output) == EOF) {
                return 1;
            }
            break;
        }
    }
    return fputc('"', output) == EOF ? 1 : 0;
}

static int write_json(
    FILE* output,
    const options* opts,
    const bench_result* results,
    size_t result_count
)
{
    char cpu[256];
    char os_release[256];
    size_t index;
    uint32_t sample;

    cpu_name(cpu, sizeof(cpu));
    os_version(os_release, sizeof(os_release));
    if (fputs("{\n  \"schema\":\"soc-bench-v2\",\n"
        "  \"environment\":{\n    \"os\":", output) == EOF ||
        json_string(output, os_name()) != 0 ||
        fputs(",\n    \"os_version\":", output) == EOF ||
        json_string(output, os_release) != 0 ||
        fputs(",\n    \"architecture\":", output) == EOF ||
        json_string(output, architecture_name()) != 0 ||
        fputs(",\n    \"cpu\":", output) == EOF ||
        json_string(output, cpu) != 0 ||
        fputs(",\n    \"compiler\":", output) == EOF ||
        json_string(output, compiler_name()) != 0 ||
        fputs(",\n    \"compiler_version\":", output) == EOF ||
        json_string(output, compiler_version()) != 0 ||
        fputs(",\n    \"compiler_flags\":", output) == EOF ||
        json_string(output, SOC_BENCH_COMPILER_FLAGS) != 0 ||
        fputs(",\n    \"ipo\":", output) == EOF ||
        json_string(output, SOC_BENCH_IPO) != 0 ||
        fputs(",\n    \"fast_math\":", output) == EOF ||
        json_string(output, fast_math_name()) != 0 ||
        fputs(",\n    \"rounding_mode\":\"FE_TONEAREST\"", output) == EOF ||
        fputs(",\n    \"git_revision\":", output) == EOF ||
        json_string(output, SOC_BENCH_GIT_REVISION) != 0 ||
        fputs(",\n    \"git_dirty\":", output) == EOF ||
        json_string(output, SOC_BENCH_GIT_DIRTY) != 0 ||
        fputs(",\n    \"build_type\":", output) == EOF ||
        json_string(output, SOC_BENCH_BUILD_TYPE) != 0 ||
        fputs(",\n    \"linkage\":", output) == EOF ||
        json_string(output, SOC_BENCH_LINKAGE) != 0) {
        return 1;
    }
    if (fprintf(
            output,
            ",\n    \"worker_count\":%u,\n    \"timer\":",
            opts->worker_count
        ) < 0 ||
        json_string(output, timer_name()) != 0 ||
        fputs("\n  },\n  \"suite\":", output) == EOF ||
        json_string(output, opts->suite) != 0 ||
        fprintf(output,
            ",\n  \"seed\":%" PRIu64
            ",\n  \"sample_count\":%u"
            ",\n  \"sample_ms\":%u"
            ",\n  \"validation_only\":%s,\n  \"cases\":[",
            opts->seed,
            opts->validate_only ? 0u : opts->samples,
            opts->validate_only ? 0u : opts->sample_ms,
            opts->validate_only ? "true" : "false") < 0) {
        return 1;
    }

    for (index = 0u; index < result_count; ++index) {
        const bench_result* result = &results[index];
        const bench_case* definition = result->definition;

        if (fprintf(output, "%s\n    {\n      \"name\":",
            index == 0u ? "" : ",") < 0 ||
            json_string(output, definition->name) != 0 ||
            fputs(",\n      \"description\":", output) == EOF ||
            json_string(output, definition->description) != 0 ||
            fprintf(output,
                ",\n      \"parameters\":{\"width\":%u,\"height\":%u,"
                "\"triangles\":%u,\"instances\":%u,\"queries\":%u,"
                "\"query_batch\":%u,\"clip_depth_range\":%u,"
                "\"geometry_pattern\":%u,\"query_pattern\":%u,"
                "\"large_queries\":%s,"
                "\"reverse_order\":%s,\"index_type\":%u,"
                "\"vertex_stride\":%u,\"position_offset\":%u,"
                "\"readback_level\":%u,\"repeat_count\":%u},"
                "\n      \"summary\":{\"median_ns\":%" PRIu64
                ",\"p95_ns\":%" PRIu64 ",\"mad_ns\":%" PRIu64
                ",\"min_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64
                ",\"noisy\":%s},"
                "\n      \"samples_ns\":[",
                definition->width, definition->height,
                definition->triangle_count, definition->instance_count,
                definition->query_count, definition->query_batch_size,
                definition->clip_depth_range, definition->geometry_pattern,
                definition->query_pattern,
                definition->large_queries ? "true" : "false",
                definition->reverse_order ? "true" : "false",
                definition->kind == BENCH_MESH_CREATE ||
                    definition->geometry_pattern == GEOMETRY_SHARED_GRID
                    ? definition->index_type : SOC_INDEX_UINT32,
                definition->kind == BENCH_MESH_CREATE
                    ? definition->vertex_stride : 12u,
                definition->kind == BENCH_MESH_CREATE
                    ? definition->position_offset : 0u,
                definition->readback_level,
                definition->repeat_count == 0u
                    ? 1u : definition->repeat_count,
                result->median_ns,
                result->p95_ns, result->mad_ns, result->min_ns,
                result->max_ns, result->noisy ? "true" : "false") < 0) {
            return 1;
        }
        if (!opts->validate_only) {
            for (sample = 0u; sample < opts->samples; ++sample) {
                if (fprintf(output, "%s%" PRIu64,
                    sample == 0u ? "" : ",",
                    result->samples_ns[sample]) < 0) {
                    return 1;
                }
            }
        }
        if (fputs("],\n      \"iterations\":[", output) == EOF) {
            return 1;
        }
        if (!opts->validate_only) {
            for (sample = 0u; sample < opts->samples; ++sample) {
                if (fprintf(output, "%s%" PRIu64,
                    sample == 0u ? "" : ",",
                    result->iterations[sample]) < 0) {
                    return 1;
                }
            }
        }
        if (fprintf(output,
            "],\n      \"stats\":{\"hiz_levels\":%u,"
            "\"input_triangles\":%" PRIu64 ",\"clipped_triangles\":%" PRIu64
            ",\"rasterized_triangles\":%" PRIu64
            ",\"tested_aabbs\":%" PRIu64 ",\"occluded_aabbs\":%" PRIu64
            "},\n      \"visibility\":{\"visible\":%" PRIu64
            ",\"occluded\":%" PRIu64 ",\"unknown\":%" PRIu64
            "},\n      \"checksum\":\"%016" PRIx64 "\"\n    }",
            result->build_stats.hiz_level_count,
            result->build_stats.input_triangle_count,
            result->build_stats.clipped_triangle_count,
            result->build_stats.rasterized_triangle_count,
            result->query_stats.tested_aabb_count,
            result->query_stats.occluded_aabb_count,
            result->visible, result->occluded, result->unknown,
            result->checksum) < 0) {
            return 1;
        }
    }
    return fputs(result_count == 0u ? "]\n}\n" : "\n  ]\n}\n", output)
        == EOF ? 1 : 0;
}

int main(int argc, char** argv)
{
    options opts;
    bench_result* results;
    size_t selected_count = 0u;
    size_t result_index = 0u;
    size_t index;
    FILE* output = stdout;
    int parse_result;
    int status = 0;

    parse_result = parse_options(argc, argv, &opts);
    if (parse_result == 2) {
        return 0;
    }
    if (parse_result != 0) {
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (soc_get_abi_version() != SOC_ABI_VERSION) {
        fprintf(stderr, "soc ABI mismatch: benchmark=%u, library=%u\n",
            SOC_ABI_VERSION, soc_get_abi_version());
        return 1;
    }

    for (index = 0u; index < ARRAY_COUNT(g_cases); ++index) {
        if (case_selected(&g_cases[index], &opts)) {
            ++selected_count;
            if (opts.list) {
                puts(g_cases[index].name);
            }
        }
    }
    if (opts.list) {
        return selected_count == 0u ? 1 : 0;
    }
    if (selected_count == 0u) {
        fprintf(stderr, "no benchmark cases matched the selection\n");
        return 2;
    }
    if (timer_initialize() != 0) {
        fprintf(stderr, "failed to initialize the monotonic timer\n");
        return 1;
    }
    if (fesetround(FE_TONEAREST) != 0) {
        fprintf(stderr, "failed to set FE_TONEAREST rounding mode\n");
        return 1;
    }

    results = (bench_result*)calloc(selected_count, sizeof(bench_result));
    if (results == NULL) {
        fprintf(stderr, "out of memory allocating benchmark results\n");
        return 1;
    }
    for (index = 0u; index < ARRAY_COUNT(g_cases); ++index) {
        if (!case_selected(&g_cases[index], &opts)) {
            continue;
        }
        fprintf(stderr, "%s %s\n",
            opts.validate_only ? "validating" : "benchmarking",
            g_cases[index].name);
        if (benchmark_case_run(
            &g_cases[index],
            &opts,
            &results[result_index]
        ) != 0) {
            status = 1;
            break;
        }
        ++result_index;
    }

    if (status == 0) {
        if (opts.output != NULL && strcmp(opts.output, "-") != 0) {
            output = fopen(opts.output, "wb");
            if (output == NULL) {
                fprintf(stderr, "cannot open output %s: %s\n",
                    opts.output, strerror(errno));
                status = 1;
            }
        }
        if (status == 0 &&
            write_json(output, &opts, results, result_index) != 0) {
            fprintf(stderr, "failed to write benchmark JSON\n");
            status = 1;
        }
        if (output != NULL && output != stdout && fclose(output) != 0) {
            fprintf(stderr, "failed to close benchmark output\n");
            status = 1;
        } else if (output == stdout && fflush(output) != 0) {
            status = 1;
        }
    }

    for (index = 0u; index < selected_count; ++index) {
        free(results[index].samples_ns);
        free(results[index].iterations);
    }
    free(results);
    return status;
}
