cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "SOURCE_DIR and OUTPUT_FILE are required")
endif()

set(git_revision unknown)
set(git_dirty unknown)
if(DEFINED GIT_EXECUTABLE AND EXISTS "${GIT_EXECUTABLE}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE revision_result
        OUTPUT_VARIABLE revision_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(revision_result EQUAL 0)
        set(git_revision "${revision_output}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE status_result
        OUTPUT_VARIABLE status_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(status_result EQUAL 0)
        if(status_output STREQUAL "")
            set(git_dirty false)
        else()
            set(git_dirty true)
        endif()
    endif()
endif()

string(REPLACE "\\" "\\\\" git_revision "${git_revision}")
string(REPLACE "\"" "\\\"" git_revision "${git_revision}")
string(REPLACE "\\" "\\\\" git_dirty "${git_dirty}")
string(REPLACE "\"" "\\\"" git_dirty "${git_dirty}")

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
set(temporary_file "${OUTPUT_FILE}.tmp")
file(WRITE "${temporary_file}"
    "#ifndef SOC_BENCH_GIT_METADATA_H_INCLUDED\n"
    "#define SOC_BENCH_GIT_METADATA_H_INCLUDED\n"
    "#define SOC_BENCH_GIT_REVISION \"${git_revision}\"\n"
    "#define SOC_BENCH_GIT_DIRTY \"${git_dirty}\"\n"
    "#endif\n"
)
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "${temporary_file}" "${OUTPUT_FILE}"
    RESULT_VARIABLE copy_result
)
file(REMOVE "${temporary_file}")
if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to refresh ${OUTPUT_FILE}")
endif()
