option(
    SOC_WARNINGS_AS_ERRORS
    "Treat compiler warnings as errors for soc targets"
    OFF
)

function(soc_enable_warnings target_name)
    if(MSVC)
        # The project intentionally uses the portable C runtime APIs instead
        # of Microsoft's non-standard *_s variants.
        target_compile_definitions(
            "${target_name}"
            PRIVATE
                _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options("${target_name}" PRIVATE /W4)
        if(SOC_WARNINGS_AS_ERRORS)
            target_compile_options("${target_name}" PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            "${target_name}"
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
        if(SOC_WARNINGS_AS_ERRORS)
            target_compile_options("${target_name}" PRIVATE -Werror)
        endif()
    endif()
endfunction()
