cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED BASELINE OR BASELINE STREQUAL "")
    message(FATAL_ERROR "BASELINE must name a soc-bench-v2 JSON result")
endif()
if(NOT DEFINED CANDIDATE OR CANDIDATE STREQUAL "")
    message(FATAL_ERROR "CANDIDATE must name a soc-bench-v2 JSON result")
endif()
if(NOT EXISTS "${BASELINE}")
    message(FATAL_ERROR "Baseline result does not exist: ${BASELINE}")
endif()
if(NOT EXISTS "${CANDIDATE}")
    message(FATAL_ERROR "Candidate result does not exist: ${CANDIDATE}")
endif()

if(NOT DEFINED THRESHOLD_PERCENT)
    set(THRESHOLD_PERCENT 5)
endif()
if(NOT THRESHOLD_PERCENT MATCHES "^[0-9]+$")
    message(FATAL_ERROR "THRESHOLD_PERCENT must be a non-negative integer")
endif()
if(NOT DEFINED FAIL_ON_REGRESSION)
    set(FAIL_ON_REGRESSION OFF)
endif()
if(NOT DEFINED FAIL_ON_NOISY)
    set(FAIL_ON_NOISY OFF)
endif()

file(READ "${BASELINE}" baseline_json)
file(READ "${CANDIDATE}" candidate_json)

string(
    JSON baseline_schema
    ERROR_VARIABLE baseline_schema_error
    GET "${baseline_json}" schema
)
string(
    JSON candidate_schema
    ERROR_VARIABLE candidate_schema_error
    GET "${candidate_json}" schema
)
if(NOT baseline_schema_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Invalid baseline JSON schema field: ${baseline_schema_error}")
endif()
if(NOT candidate_schema_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Invalid candidate JSON schema field: ${candidate_schema_error}")
endif()
if(NOT baseline_schema STREQUAL "soc-bench-v2" OR
   NOT candidate_schema STREQUAL "soc-bench-v2")
    message(FATAL_ERROR "Both inputs must use schema \"soc-bench-v2\"")
endif()

string(JSON baseline_seed ERROR_VARIABLE baseline_seed_error
    GET "${baseline_json}" seed)
string(JSON candidate_seed ERROR_VARIABLE candidate_seed_error
    GET "${candidate_json}" seed)
if(NOT baseline_seed_error STREQUAL "NOTFOUND" OR
   NOT candidate_seed_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Both results must contain a root-level seed")
endif()
if(NOT baseline_seed STREQUAL candidate_seed)
    message(FATAL_ERROR
        "Results use different seeds: ${baseline_seed} vs ${candidate_seed}")
endif()

foreach(root_key suite sample_count sample_ms)
    string(JSON baseline_root_value
        ERROR_VARIABLE baseline_root_error
        GET "${baseline_json}" "${root_key}")
    string(JSON candidate_root_value
        ERROR_VARIABLE candidate_root_error
        GET "${candidate_json}" "${root_key}")
    if(NOT baseline_root_error STREQUAL "NOTFOUND" OR
       NOT candidate_root_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "Both results must contain ${root_key}")
    endif()
    if(NOT baseline_root_value STREQUAL candidate_root_value)
        message(FATAL_ERROR
            "Results use different ${root_key}: "
            "\"${baseline_root_value}\" vs \"${candidate_root_value}\"")
    endif()
    set("baseline_${root_key}" "${baseline_root_value}")
    set("candidate_${root_key}" "${candidate_root_value}")
endforeach()

string(JSON baseline_validation_only ERROR_VARIABLE baseline_validation_error
    GET "${baseline_json}" validation_only)
string(JSON candidate_validation_only ERROR_VARIABLE candidate_validation_error
    GET "${candidate_json}" validation_only)
if(NOT baseline_validation_error STREQUAL "NOTFOUND" OR
   NOT candidate_validation_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Both results must contain validation_only")
endif()
if(baseline_validation_only OR candidate_validation_only)
    message(FATAL_ERROR
        "Validation-only results do not contain performance samples")
endif()
if(baseline_sample_count EQUAL 0 OR baseline_sample_ms EQUAL 0)
    message(FATAL_ERROR
        "Performance results must use non-zero sample_count and sample_ms")
endif()

set(environment_keys
    os
    os_version
    architecture
    cpu
    compiler
    compiler_version
    compiler_flags
    ipo
    fast_math
    rounding_mode
    build_type
    linkage
    worker_count
    timer
)
foreach(environment_key IN LISTS environment_keys)
    string(JSON baseline_environment_value
        ERROR_VARIABLE baseline_environment_error
        GET "${baseline_json}" environment "${environment_key}")
    string(JSON candidate_environment_value
        ERROR_VARIABLE candidate_environment_error
        GET "${candidate_json}" environment "${environment_key}")
    if(NOT baseline_environment_error STREQUAL "NOTFOUND" OR
       NOT candidate_environment_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Both results must contain environment.${environment_key}")
    endif()
    if(NOT baseline_environment_value STREQUAL candidate_environment_value)
        message(FATAL_ERROR
            "Incompatible environment.${environment_key}: "
            "\"${baseline_environment_value}\" vs "
            "\"${candidate_environment_value}\"")
    endif()
endforeach()

string(JSON baseline_case_count ERROR_VARIABLE baseline_cases_error
    LENGTH "${baseline_json}" cases)
string(JSON candidate_case_count ERROR_VARIABLE candidate_cases_error
    LENGTH "${candidate_json}" cases)
if(NOT baseline_cases_error STREQUAL "NOTFOUND" OR
   NOT candidate_cases_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Both results must contain a cases array")
endif()
if(baseline_case_count EQUAL 0 OR candidate_case_count EQUAL 0)
    message(FATAL_ERROR "Both results must contain at least one case")
endif()
if(NOT baseline_case_count EQUAL candidate_case_count)
    message(FATAL_ERROR
        "Case counts differ: ${baseline_case_count} vs ${candidate_case_count}")
endif()

foreach(result_name baseline candidate)
    math(EXPR unique_case_last "${${result_name}_case_count} - 1")
    foreach(left_index RANGE 0 ${unique_case_last})
        string(JSON left_name
            GET "${${result_name}_json}" cases ${left_index} name)
        math(EXPR right_start "${left_index} + 1")
        if(right_start LESS ${result_name}_case_count)
            foreach(right_index RANGE ${right_start} ${unique_case_last})
                string(JSON right_name
                    GET "${${result_name}_json}" cases ${right_index} name)
                if(left_name STREQUAL right_name)
                    message(FATAL_ERROR
                        "${result_name} contains duplicate case name "
                        "\"${left_name}\"")
                endif()
            endforeach()
        endif()
    endforeach()
endforeach()

math(EXPR baseline_case_last "${baseline_case_count} - 1")
math(EXPR candidate_case_last "${candidate_case_count} - 1")
set(compared_count 0)
set(regression_count 0)
set(noisy_count 0)

foreach(candidate_index RANGE 0 ${candidate_case_last})
    string(JSON candidate_name ERROR_VARIABLE candidate_name_error
        GET "${candidate_json}" cases ${candidate_index} name)
    if(NOT candidate_name_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Candidate case ${candidate_index} has no valid name")
    endif()

    set(baseline_index -1)
    foreach(search_index RANGE 0 ${baseline_case_last})
        string(JSON search_name ERROR_VARIABLE search_name_error
            GET "${baseline_json}" cases ${search_index} name)
        if(NOT search_name_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR
                "Baseline case ${search_index} has no valid name")
        endif()
        if(search_name STREQUAL candidate_name)
            set(baseline_index ${search_index})
            break()
        endif()
    endforeach()
    if(baseline_index EQUAL -1)
        message(FATAL_ERROR
            "Candidate case \"${candidate_name}\" is missing from baseline")
    endif()

    foreach(metric median_ns p95_ns mad_ns min_ns max_ns)
        string(JSON baseline_${metric} ERROR_VARIABLE baseline_metric_error
            GET "${baseline_json}" cases ${baseline_index} summary ${metric})
        string(JSON candidate_${metric} ERROR_VARIABLE candidate_metric_error
            GET "${candidate_json}" cases ${candidate_index} summary ${metric})
        if(NOT baseline_metric_error STREQUAL "NOTFOUND" OR
           NOT candidate_metric_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR
                "Case \"${candidate_name}\" must contain integer summary.${metric}")
        endif()
        if(NOT baseline_${metric} MATCHES "^[0-9]+$" OR
           NOT candidate_${metric} MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "Case \"${candidate_name}\" has non-integer summary.${metric}")
        endif()
    endforeach()

    string(JSON baseline_noisy ERROR_VARIABLE baseline_noisy_error
        GET "${baseline_json}" cases ${baseline_index} summary noisy)
    string(JSON candidate_noisy ERROR_VARIABLE candidate_noisy_error
        GET "${candidate_json}" cases ${candidate_index} summary noisy)
    if(NOT baseline_noisy_error STREQUAL "NOTFOUND" OR
       NOT candidate_noisy_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Case \"${candidate_name}\" must contain summary.noisy")
    endif()
    set(case_noisy FALSE)
    if(baseline_noisy OR candidate_noisy)
        set(case_noisy TRUE)
        math(EXPR noisy_count "${noisy_count} + 1")
    endif()

    foreach(array_name samples_ns iterations)
        string(JSON baseline_array_count ERROR_VARIABLE baseline_array_error
            LENGTH "${baseline_json}" cases ${baseline_index} ${array_name})
        string(JSON candidate_array_count ERROR_VARIABLE candidate_array_error
            LENGTH "${candidate_json}" cases ${candidate_index} ${array_name})
        if(NOT baseline_array_error STREQUAL "NOTFOUND" OR
           NOT candidate_array_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR
                "Case \"${candidate_name}\" must contain ${array_name}")
        endif()
        if(NOT baseline_array_count EQUAL baseline_sample_count OR
           NOT candidate_array_count EQUAL candidate_sample_count)
            message(FATAL_ERROR
                "Case \"${candidate_name}\" has a ${array_name} length "
                "different from sample_count")
        endif()
    endforeach()

    foreach(parameter
        width
        height
        triangles
        instances
        queries
        query_batch
        clip_depth_range
        geometry_pattern
        query_pattern
        large_queries
        reverse_order
        index_type
        vertex_stride
        position_offset
        readback_level
        repeat_count
    )
        string(JSON baseline_parameter ERROR_VARIABLE baseline_parameter_error
            GET "${baseline_json}" cases ${baseline_index}
                parameters ${parameter})
        string(JSON candidate_parameter ERROR_VARIABLE candidate_parameter_error
            GET "${candidate_json}" cases ${candidate_index}
                parameters ${parameter})
        if(NOT baseline_parameter_error STREQUAL "NOTFOUND" OR
           NOT candidate_parameter_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR
                "Case \"${candidate_name}\" must contain "
                "parameters.${parameter}")
        endif()
        if(NOT baseline_parameter STREQUAL candidate_parameter)
            message(FATAL_ERROR
                "Case \"${candidate_name}\" changed parameters.${parameter}: "
                "${baseline_parameter} vs ${candidate_parameter}")
        endif()
    endforeach()

    # The checksum is diagnostic metadata, not a performance-compatibility
    # contract. Conservative coverage, floating-point evaluation order and
    # boundary rounding may legitimately change exact depth bits without
    # changing the workload or its public outcomes. Keep comparing parameters,
    # public counters and visibility below, but do not gate performance results
    # on bit-identical internal buffers.

    foreach(stat
        hiz_levels
        input_triangles
        clipped_triangles
        rasterized_triangles
        tested_aabbs
        occluded_aabbs
    )
        string(JSON baseline_stat ERROR_VARIABLE baseline_stat_error
            GET "${baseline_json}" cases ${baseline_index} stats ${stat})
        string(JSON candidate_stat ERROR_VARIABLE candidate_stat_error
            GET "${candidate_json}" cases ${candidate_index} stats ${stat})
        if(NOT baseline_stat_error STREQUAL "NOTFOUND" OR
           NOT candidate_stat_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR
                "Case \"${candidate_name}\" must contain stats.${stat}")
        endif()
        if(NOT baseline_stat STREQUAL candidate_stat)
            message(FATAL_ERROR
                "Case \"${candidate_name}\" changed stats.${stat}: "
                "${baseline_stat} vs ${candidate_stat}")
        endif()
    endforeach()

    foreach(outcome visible occluded unknown)
        string(JSON baseline_outcome ERROR_VARIABLE baseline_outcome_error
            GET "${baseline_json}" cases ${baseline_index}
                visibility ${outcome})
        string(JSON candidate_outcome ERROR_VARIABLE candidate_outcome_error
            GET "${candidate_json}" cases ${candidate_index}
                visibility ${outcome})
        if(NOT baseline_outcome_error STREQUAL "NOTFOUND" OR
           NOT candidate_outcome_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR
                "Case \"${candidate_name}\" must contain "
                "visibility.${outcome}")
        endif()
        if(NOT baseline_outcome STREQUAL candidate_outcome)
            message(FATAL_ERROR
                "Case \"${candidate_name}\" changed visibility.${outcome}: "
                "${baseline_outcome} vs ${candidate_outcome}")
        endif()
    endforeach()

    math(EXPR threshold_scaled
        "${baseline_median_ns} * (100 + ${THRESHOLD_PERCENT})")
    math(EXPR candidate_scaled "${candidate_median_ns} * 100")
    math(EXPR median_delta
        "${candidate_median_ns} - ${baseline_median_ns}")

    if(baseline_mad_ns GREATER candidate_mad_ns)
        set(max_mad ${baseline_mad_ns})
    else()
        set(max_mad ${candidate_mad_ns})
    endif()
    math(EXPR noise_limit "${max_mad} * 3")

    set(case_regressed FALSE)
    if(candidate_scaled GREATER threshold_scaled AND
       median_delta GREATER noise_limit)
        set(case_regressed TRUE)
        math(EXPR regression_count "${regression_count} + 1")
    endif()
    math(EXPR compared_count "${compared_count} + 1")

    if(case_regressed)
        if(case_noisy)
            set(case_status "REGRESSION (noisy)")
        else()
            set(case_status "REGRESSION")
        endif()
    elseif(case_noisy)
        set(case_status "NOISY")
    elseif(candidate_median_ns LESS baseline_median_ns)
        set(case_status "improved")
    else()
        set(case_status "stable")
    endif()
    message(STATUS
        "${case_status}: ${candidate_name}: "
        "${baseline_median_ns} ns -> ${candidate_median_ns} ns "
        "(delta ${median_delta} ns, 3*MAD ${noise_limit} ns)")
endforeach()

# The equal case counts and candidate-to-baseline name lookup above still allow
# duplicate names to hide a missing result. Verify the reverse direction too.
foreach(baseline_index RANGE 0 ${baseline_case_last})
    string(JSON baseline_name
        GET "${baseline_json}" cases ${baseline_index} name)
    set(candidate_index -1)
    foreach(search_index RANGE 0 ${candidate_case_last})
        string(JSON search_name
            GET "${candidate_json}" cases ${search_index} name)
        if(search_name STREQUAL baseline_name)
            set(candidate_index ${search_index})
            break()
        endif()
    endforeach()
    if(candidate_index EQUAL -1)
        message(FATAL_ERROR
            "Baseline case \"${baseline_name}\" is missing from candidate")
    endif()
endforeach()

message(STATUS
    "Compared ${compared_count} cases: ${regression_count} regression(s), "
    "${noisy_count} noisy/inconclusive; "
    "rule is >${THRESHOLD_PERCENT}% and >3*max(MAD)")

if(regression_count GREATER 0 AND FAIL_ON_REGRESSION)
    message(FATAL_ERROR
        "Performance comparison failed with ${regression_count} regression(s)")
endif()
if(noisy_count GREATER 0 AND FAIL_ON_NOISY)
    message(FATAL_ERROR
        "Performance comparison has ${noisy_count} noisy case(s); rerun it")
endif()
