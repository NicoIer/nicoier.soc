function(soc_enable_warnings target_name)
    if(MSVC)
        target_compile_options("${target_name}" PRIVATE /W4)
    else()
        target_compile_options(
            "${target_name}"
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()
endfunction()
