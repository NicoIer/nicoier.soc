cmake_minimum_required(VERSION 3.21)

project(
    soc
    VERSION 0.2.0
    DESCRIPTION "CPU software rasterization and occlusion culling library"
    LANGUAGES C
)

option(SOC_BUILD_SHARED "Build soc as a shared library" ON)
option(SOC_BUILD_TESTS "Build soc tests" ${PROJECT_IS_TOP_LEVEL})
option(SOC_BUILD_TOOLS "Build soc debugging tools" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SocWarnings)
include(GNUInstallDirs)

add_subdirectory(src)

if(SOC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(SOC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(CMakePackageConfigHelpers)

set(SOC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/soc")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/socConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/socConfig.cmake"
    INSTALL_DESTINATION "${SOC_CMAKE_INSTALL_DIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/socConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(
    TARGETS soc
    EXPORT socTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(SOC_BUILD_TOOLS)
    install(
        TARGETS soc_cli
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    EXPORT socTargets
    FILE socTargets.cmake
    NAMESPACE soc::
    DESTINATION "${SOC_CMAKE_INSTALL_DIR}"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/socConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/socConfigVersion.cmake"
    DESTINATION "${SOC_CMAKE_INSTALL_DIR}"
)
