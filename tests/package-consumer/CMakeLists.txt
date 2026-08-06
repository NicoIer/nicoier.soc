cmake_minimum_required(VERSION 3.21)

project(soc_package_consumer LANGUAGES C)

find_package(soc 3.0 CONFIG REQUIRED)

add_executable(soc_package_consumer main.c)
target_link_libraries(soc_package_consumer PRIVATE soc::soc)
target_compile_features(soc_package_consumer PRIVATE c_std_17)

# Imported DLL targets are not copied beside consumers automatically. Keeping
# the runtime next to this smoke-test executable makes the installed shared
# package directly runnable on Windows; copying a static .lib is harmless.
if(WIN32)
    add_custom_command(
        TARGET soc_package_consumer
        POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:soc::soc>"
            "$<TARGET_FILE_DIR:soc_package_consumer>"
    )
endif()

enable_testing()
add_test(NAME soc.package.consumer COMMAND soc_package_consumer)
