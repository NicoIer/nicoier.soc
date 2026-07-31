add_executable(soc_bench soc_bench.c)
target_link_libraries(soc_bench PRIVATE soc::soc)
target_compile_features(soc_bench PRIVATE c_std_17)
soc_enable_warnings(soc_bench)

if(UNIX)
    target_link_libraries(soc_bench PRIVATE m)
endif()

if(SOC_BUILD_SHARED)
    set(SOC_BENCH_LINKAGE shared)
else()
    set(SOC_BENCH_LINKAGE static)
endif()

function(soc_bench_escape_definition input_value output_variable)
    string(STRIP "${input_value}" escaped_value)
    string(REPLACE "\\" "\\\\" escaped_value "${escaped_value}")
    string(REPLACE "\"" "\\\"" escaped_value "${escaped_value}")
    string(REPLACE ";" "\\;" escaped_value "${escaped_value}")
    set("${output_variable}" "${escaped_value}" PARENT_SCOPE)
endfunction()

soc_bench_escape_definition(
    "${CMAKE_C_FLAGS}"
    SOC_BENCH_COMPILER_FLAGS_BASE
)
set(SOC_BENCH_COMPILER_FLAGS_EXPRESSION
    "${SOC_BENCH_COMPILER_FLAGS_BASE}")
if(SOC_BENCH_COMPILER_FLAGS_EXPRESSION STREQUAL "")
    set(SOC_BENCH_COMPILER_FLAGS_SEPARATOR "")
else()
    set(SOC_BENCH_COMPILER_FLAGS_SEPARATOR " ")
endif()
foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" configuration_upper)
    soc_bench_escape_definition(
        "${CMAKE_C_FLAGS_${configuration_upper}}"
        configuration_flags
    )
    if(NOT configuration_flags STREQUAL "")
        string(APPEND SOC_BENCH_COMPILER_FLAGS_EXPRESSION
            "$<$<CONFIG:${configuration}>:"
            "${SOC_BENCH_COMPILER_FLAGS_SEPARATOR}${configuration_flags}>")
    endif()
endforeach()

if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    set(SOC_BENCH_IPO_EXPRESSION on)
else()
    set(SOC_BENCH_IPO_EXPRESSION off)
endif()
foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" configuration_upper)
    set(ipo_variable
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION_${configuration_upper}")
    if(DEFINED "${ipo_variable}")
        if(${ipo_variable})
            set(configuration_ipo on)
        else()
            set(configuration_ipo off)
        endif()
        set(previous_ipo_expression "${SOC_BENCH_IPO_EXPRESSION}")
        string(CONCAT SOC_BENCH_IPO_EXPRESSION
            "$<IF:$<CONFIG:${configuration}>,"
            "${configuration_ipo},${previous_ipo_expression}>")
    endif()
endforeach()

find_package(Git QUIET)
set(SOC_BENCH_METADATA_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SOC_BENCH_METADATA_HEADER
    "${SOC_BENCH_METADATA_DIR}/soc_bench_git_metadata.h")
add_custom_target(
    soc_bench_metadata
    COMMAND
        "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DOUTPUT_FILE=${SOC_BENCH_METADATA_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/write_git_metadata.cmake"
    BYPRODUCTS "${SOC_BENCH_METADATA_HEADER}"
    COMMENT "Refreshing soc benchmark Git metadata"
    VERBATIM
)
add_dependencies(soc_bench soc_bench_metadata)
target_include_directories(
    soc_bench
    PRIVATE
        "${SOC_BENCH_METADATA_DIR}"
)

target_compile_definitions(
    soc_bench
    PRIVATE
        SOC_BENCH_BUILD_TYPE="$<CONFIG>"
        SOC_BENCH_COMPILER_FLAGS="${SOC_BENCH_COMPILER_FLAGS_EXPRESSION}"
        SOC_BENCH_COMPILER_ID="${CMAKE_C_COMPILER_ID}"
        SOC_BENCH_COMPILER_VERSION="${CMAKE_C_COMPILER_VERSION}"
        SOC_BENCH_HAVE_GENERATED_METADATA=1
        SOC_BENCH_IPO="${SOC_BENCH_IPO_EXPRESSION}"
        SOC_BENCH_LINKAGE="${SOC_BENCH_LINKAGE}"
)
