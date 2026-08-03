option(
    SOC_WARNINGS_AS_ERRORS
    "Treat compiler warnings as errors for soc targets"
    OFF
)

function(soc_enable_warnings target_name)
    if(MSVC)
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
