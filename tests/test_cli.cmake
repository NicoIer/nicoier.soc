if(NOT DEFINED SOC_CLI OR SOC_CLI STREQUAL "")
    message(FATAL_ERROR "SOC_CLI must name the soc_cli executable")
endif()

if(NOT DEFINED TEST_WORK_DIR OR TEST_WORK_DIR STREQUAL "")
    message(FATAL_ERROR "TEST_WORK_DIR must name a writable test directory")
endif()

set(obj_path "${TEST_WORK_DIR}/triangle.obj")
set(png_path "${TEST_WORK_DIR}/triangle.png")
set(oblique_png_path "${TEST_WORK_DIR}/triangle-oblique.png")
set(tiny_obj_path "${TEST_WORK_DIR}/triangle-tiny.obj")
set(tiny_png_path "${TEST_WORK_DIR}/triangle-tiny.png")
set(invalid_obj_path "${TEST_WORK_DIR}/invalid.obj")
set(invalid_png_path "${TEST_WORK_DIR}/invalid.png")

file(MAKE_DIRECTORY "${TEST_WORK_DIR}")
file(REMOVE "${png_path}")
file(REMOVE "${oblique_png_path}")
file(REMOVE "${tiny_png_path}")
file(REMOVE "${invalid_png_path}")
file(
    WRITE
    "${obj_path}"
    "v -2.0 -2.0 0.0 2.0\n"
    "v 2.0 -2.0 0.0 2.0\n"
    "v 0.0 2.0 0.0 2.0\n"
    "vt 0.0 0.0\n"
    "vt 1.0 0.0\n"
    "vt 0.5 1.0\n"
    "vn 0.0 0.0 1.0\n"
    "f -3/1/1 -2/2/1 -1/3/1\n"
)
file(
    WRITE
    "${tiny_obj_path}"
    "v -0.0000001 -0.0000001 0\n"
    "v 0.0000001 -0.0000001 0\n"
    "v 0 0.0000001 0\n"
    "f 1 2 3\n"
)
file(WRITE "${invalid_obj_path}" "f 1 2 3\n")

execute_process(
    COMMAND "${SOC_CLI}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_stdout
    ERROR_VARIABLE help_stderr
    TIMEOUT 30
)
if(NOT "${help_result}" STREQUAL "0" OR
        NOT "${help_stdout}" MATCHES "Usage:")
    message(
        FATAL_ERROR
        "soc_cli --help failed\n"
        "result: ${help_result}\n"
        "stdout:\n${help_stdout}\n"
        "stderr:\n${help_stderr}"
    )
endif()
if("${help_stdout}" MATCHES "--reversed-z")
    message(FATAL_ERROR "soc_cli still advertises the removed --reversed-z option")
endif()

execute_process(
    COMMAND
        "${SOC_CLI}"
        --input "${obj_path}"
        --output "${png_path}"
        --width 0
    RESULT_VARIABLE invalid_option_result
    OUTPUT_VARIABLE invalid_option_stdout
    ERROR_VARIABLE invalid_option_stderr
    TIMEOUT 30
)
if(NOT "${invalid_option_result}" STREQUAL "2" OR
        NOT "${invalid_option_stderr}" MATCHES
            "--width requires a positive uint32")
    message(
        FATAL_ERROR
        "soc_cli did not reject an invalid width\n"
        "result: ${invalid_option_result}\n"
        "stdout:\n${invalid_option_stdout}\n"
        "stderr:\n${invalid_option_stderr}"
    )
endif()

execute_process(
    COMMAND
        "${SOC_CLI}"
        --input "${invalid_obj_path}"
        --output "${invalid_png_path}"
        --width 8
        --height 8
    RESULT_VARIABLE invalid_obj_result
    OUTPUT_VARIABLE invalid_obj_stdout
    ERROR_VARIABLE invalid_obj_stderr
    TIMEOUT 30
)
if("${invalid_obj_result}" STREQUAL "0" OR EXISTS "${invalid_png_path}")
    message(
        FATAL_ERROR
        "soc_cli accepted a malformed OBJ\n"
        "result: ${invalid_obj_result}\n"
        "stdout:\n${invalid_obj_stdout}\n"
        "stderr:\n${invalid_obj_stderr}"
    )
endif()

execute_process(
    COMMAND
        "${SOC_CLI}"
        --input "${obj_path}"
        --output "${TEST_WORK_DIR}"
        --width 8
        --height 8
        --two-sided
    RESULT_VARIABLE output_failure_result
    OUTPUT_VARIABLE output_failure_stdout
    ERROR_VARIABLE output_failure_stderr
    TIMEOUT 30
)
if("${output_failure_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "soc_cli reported success when its output path was a directory\n"
        "stdout:\n${output_failure_stdout}\n"
        "stderr:\n${output_failure_stderr}"
    )
endif()

execute_process(
    COMMAND
        "${SOC_CLI}"
        --input "${obj_path}"
        --output "${png_path}"
        --width 32
        --height 32
        --fov 60
        --eye 0 0 3
        --target 0 0 0
        --near 0.1
        --far 10
    RESULT_VARIABLE cli_result
    OUTPUT_VARIABLE cli_stdout
    ERROR_VARIABLE cli_stderr
    TIMEOUT 30
)

if(NOT "${cli_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "soc_cli failed with exit code ${cli_result}\n"
        "stdout:\n${cli_stdout}\n"
        "stderr:\n${cli_stderr}"
    )
endif()

if(NOT "${cli_stdout}" MATCHES "drawn_pixels=([1-9][0-9]*)")
    message(
        FATAL_ERROR
        "soc_cli did not report a positive drawn_pixels count\n"
        "stdout:\n${cli_stdout}\n"
        "stderr:\n${cli_stderr}"
    )
endif()

if(NOT EXISTS "${png_path}")
    message(FATAL_ERROR "soc_cli did not create ${png_path}")
endif()

file(SIZE "${png_path}" png_size)
if(png_size LESS 24)
    message(FATAL_ERROR "PNG is too small to contain an IHDR: ${png_size} bytes")
endif()

file(READ "${png_path}" png_header OFFSET 0 LIMIT 24 HEX)
string(TOLOWER "${png_header}" png_header)
set(
    expected_header
    "89504e470d0a1a0a0000000d494844520000002000000020"
)

if(NOT "${png_header}" STREQUAL "${expected_header}")
    message(
        FATAL_ERROR
        "unexpected PNG signature or dimensions\n"
        "expected: ${expected_header}\n"
        "actual:   ${png_header}"
    )
endif()

execute_process(
    COMMAND
        "${SOC_CLI}"
        --input "${obj_path}"
        --output "${oblique_png_path}"
        --width 256
        --height 256
        --fov 90
        --eye 0 0 10
        --target 0.669130606 0 9.256855175
        --two-sided
    RESULT_VARIABLE oblique_result
    OUTPUT_VARIABLE oblique_stdout
    ERROR_VARIABLE oblique_stderr
    TIMEOUT 30
)

if(NOT "${oblique_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "soc_cli oblique camera failed with exit code ${oblique_result}\n"
        "stdout:\n${oblique_stdout}\n"
        "stderr:\n${oblique_stderr}"
    )
endif()

if(NOT "${oblique_stdout}" MATCHES "drawn_pixels=([1-9][0-9]*)")
    message(
        FATAL_ERROR
        "automatic clip planes removed the oblique camera result\n"
        "stdout:\n${oblique_stdout}\n"
        "stderr:\n${oblique_stderr}"
    )
endif()

execute_process(
    COMMAND
        "${SOC_CLI}"
        --input "${tiny_obj_path}"
        --output "${tiny_png_path}"
        --width 64
        --height 64
        --two-sided
    RESULT_VARIABLE tiny_result
    OUTPUT_VARIABLE tiny_stdout
    ERROR_VARIABLE tiny_stderr
    TIMEOUT 30
)

if(NOT "${tiny_result}" STREQUAL "0")
    message(
        FATAL_ERROR
        "soc_cli tiny auto-fit failed with exit code ${tiny_result}\n"
        "stdout:\n${tiny_stdout}\n"
        "stderr:\n${tiny_stderr}"
    )
endif()

if(NOT "${tiny_stdout}" MATCHES "drawn_pixels=([1-9][0-9]*)")
    message(
        FATAL_ERROR
        "automatic camera is not scale-independent for the tiny OBJ\n"
        "stdout:\n${tiny_stdout}\n"
        "stderr:\n${tiny_stderr}"
    )
endif()
