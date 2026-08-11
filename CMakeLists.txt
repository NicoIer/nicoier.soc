cmake_minimum_required(VERSION 3.21)

# Native top-level macOS builds also require an explicit SDK. Include the
# repository's Apple wrapper here even when CMake is already using it as a
# toolchain: older build directories created before the wrapper was attached
# to the presets do not reload a newly assigned CMAKE_TOOLCHAIN_FILE.
get_filename_component(
    _soc_apple_toolchain_file
    "${CMAKE_CURRENT_LIST_DIR}/cmake/toolchains/apple-sdk.cmake"
    REALPATH
)
set(_soc_load_apple_toolchain OFF)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
        AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
        set(SOC_APPLE_SDK_PLATFORM macosx)
        set(_soc_load_apple_toolchain ON)
    else()
        get_filename_component(
            _soc_requested_toolchain_file
            "${CMAKE_TOOLCHAIN_FILE}"
            REALPATH
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        if(_soc_requested_toolchain_file STREQUAL _soc_apple_toolchain_file)
            set(_soc_load_apple_toolchain ON)
        endif()
    endif()
endif()
if(_soc_load_apple_toolchain)
    include("${_soc_apple_toolchain_file}")
endif()

project(
    soc
    VERSION 3.1.0
    DESCRIPTION "CPU software rasterization and occlusion culling library"
    LANGUAGES C
)

# CMake's unused-command-line tracking does not count reads performed only by
# a toolchain file on every reconfigure. Touch the preset's platform contract
# in project scope as well so a correctly consumed input stays warning-free.
if(DEFINED SOC_APPLE_SDK_PLATFORM)
    set(_soc_apple_sdk_platform "${SOC_APPLE_SDK_PLATFORM}")
endif()
if(DEFINED SOC_APPLE_SDK_PATH)
    set(_soc_apple_sdk_path "${SOC_APPLE_SDK_PATH}")
endif()
if(DEFINED SOC_ANDROID_NDK_PATH)
    set(_soc_android_ndk_path "${SOC_ANDROID_NDK_PATH}")
endif()

set(SOC_BUILD_SHARED_DEFAULT ON)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(SOC_BUILD_SHARED_DEFAULT OFF)
endif()

set(SOC_BUILD_HOST_TARGETS_DEFAULT "${PROJECT_IS_TOP_LEVEL}")
if(CMAKE_CROSSCOMPILING)
    set(SOC_BUILD_HOST_TARGETS_DEFAULT OFF)
endif()

option(
    SOC_BUILD_SHARED
    "Build soc as a shared library"
    "${SOC_BUILD_SHARED_DEFAULT}"
)
option(
    SOC_BUILD_TESTS
    "Build soc tests"
    "${SOC_BUILD_HOST_TARGETS_DEFAULT}"
)
option(
    SOC_BUILD_TOOLS
    "Build soc debugging tools"
    "${SOC_BUILD_HOST_TARGETS_DEFAULT}"
)
option(SOC_BUILD_BENCHMARKS "Build soc performance benchmarks" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SocWarnings)
include(GNUInstallDirs)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Keep DLLs and the executables which load them in the same directory. This
# also makes the native MSVC test configuration work without editing PATH.
if(WIN32
        AND PROJECT_IS_TOP_LEVEL
        AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

add_subdirectory(src)

if(SOC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

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
