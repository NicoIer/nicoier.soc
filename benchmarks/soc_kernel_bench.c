#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "core/soc_cpu_features.h"
#include "core/soc_kernels.h"

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

#define CLEAR_ELEMENT_COUNT ((size_t)1920u * 1080u)
#define HIZ_SOURCE_WIDTH 1920u
#define HIZ_SOURCE_HEIGHT 1080u

#if !defined(SOC_KERNEL_BENCH_BUILD_TYPE)
#define SOC_KERNEL_BENCH_BUILD_TYPE "unknown"
#endif

#if !defined(SOC_KERNEL_BENCH_LINKAGE)
#define SOC_KERNEL_BENCH_LINKAGE "unknown"
#endif

#define REQUIRED_GATE_NONE UINT32_C(0)
#define REQUIRED_GATE_CLEAR_F32 (UINT32_C(1) << 0u)
#define REQUIRED_GATE_HIZ_REDUCE_LEVEL_F32 (UINT32_C(1) << 1u)
#define REQUIRED_GATE_ALL \
    (REQUIRED_GATE_CLEAR_F32 | REQUIRED_GATE_HIZ_REDUCE_LEVEL_F32)

typedef enum benchmark_kind {
    BENCHMARK_CLEAR_F32 = 0,
    BENCHMARK_HIZ_REDUCE_LEVEL_F32,
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

static volatile uint64_t benchmark_sink;

static void print_usage(FILE* stream, const char* executable)
{
    (void)fprintf(
        stream,
        "Usage: %s [--samples N] [--sample-ms N] [--validate-only]\n"
        "  --samples N    Samples per backend and case (default: %u)\n"
        "  --sample-ms N  Minimum milliseconds per sample (default: %u)\n"
        "  --validate-only  Check Scalar/NEON outputs without timing\n"
        "  --require-gate clear|hiz|all\n"
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
    } else {
        kernels->reduce_hiz_level_f32(
            workload->source,
            workload->source_width,
            workload->source_height,
            workload->destination,
            SOC_DEPTH_FORWARD
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
    } else {
        kernels->reduce_hiz_level_f32(
            workload->source,
            workload->source_width,
            workload->source_height,
            workload->destination,
            SOC_DEPTH_FORWARD
        );
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
    } else {
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
                neon_kernels->reduce_hiz_level_f32)
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

int main(int argc, char** argv)
{
    options opts;
    soc_cpu_features features;
    const soc_kernel_table* scalar_kernels;
    const soc_kernel_table* neon_kernels;
    soc_bool clear_gate = SOC_FALSE;
    soc_bool hiz_gate = SOC_FALSE;
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
        scalar_kernels->reduce_hiz_level_f32 == NULL) {
        (void)fprintf(stderr, "soc_kernel_bench: Scalar kernels unavailable\n");
        return EXIT_FAILURE;
    }
    if (neon_kernels == NULL ||
        neon_kernels->clear_f32 == NULL ||
        neon_kernels->reduce_hiz_level_f32 == NULL ||
        features.architecture != SOC_CPU_ARCHITECTURE_ARM64 ||
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

    (void)printf(
        "soc_kernel_bench status=complete performance_gate=%s"
        " required_gate=%s sink=%016" PRIx64 "\n",
        clear_gate == SOC_TRUE && hiz_gate == SOC_TRUE ? "pass" : "fail",
        opts.required_gates == REQUIRED_GATE_NONE
            ? "not_requested"
            : (required_gates_passed == SOC_TRUE ? "pass" : "fail"),
        (uint64_t)benchmark_sink
    );
    return required_gates_passed == SOC_TRUE ? EXIT_SUCCESS : EXIT_FAILURE;
}
