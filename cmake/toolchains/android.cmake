# Delegate to the Android NDK's supported CMake toolchain while allowing the
# NDK location to come only from CMake or an explicitly set environment
# variable. No default path or filesystem search is used.

set(_soc_android_ndk "")
set(_soc_android_ndk_cmake "")
set(_soc_android_ndk_environment "")

if(DEFINED SOC_ANDROID_NDK_PATH AND NOT SOC_ANDROID_NDK_PATH STREQUAL "")
    set(_soc_android_ndk_cmake "${SOC_ANDROID_NDK_PATH}")
endif()

# -D entries are cache variables. Capture the current invocation's value and
# remove it immediately, before validation or project code can fail. Restore a
# normal variable so repeated toolchain loads and try_compile keep working in
# this configure only. The former SOC_ANDROID_NDK cache input is also removed
# so build directories created by older versions cannot become a fallback.
unset(SOC_ANDROID_NDK_PATH CACHE)
unset(SOC_ANDROID_NDK CACHE)
if(NOT _soc_android_ndk_cmake STREQUAL "")
    set(SOC_ANDROID_NDK_PATH "${_soc_android_ndk_cmake}")
endif()

if(DEFINED ENV{SOC_ANDROID_NDK}
        AND NOT "$ENV{SOC_ANDROID_NDK}" STREQUAL "")
    set(_soc_android_ndk_environment "$ENV{SOC_ANDROID_NDK}")
endif()

if(NOT _soc_android_ndk_cmake STREQUAL ""
        AND NOT _soc_android_ndk_environment STREQUAL ""
        AND NOT _soc_android_ndk_cmake STREQUAL _soc_android_ndk_environment)
    message(FATAL_ERROR
        "Conflicting Android NDK paths were provided through the "
        "SOC_ANDROID_NDK_PATH CMake variable and SOC_ANDROID_NDK "
        "environment variable.")
endif()

if(NOT _soc_android_ndk_cmake STREQUAL "")
    set(_soc_android_ndk "${_soc_android_ndk_cmake}")
elseif(NOT _soc_android_ndk_environment STREQUAL "")
    set(_soc_android_ndk "${_soc_android_ndk_environment}")
endif()

if(_soc_android_ndk STREQUAL "")
    message(FATAL_ERROR
        "Android NDK path is required. Set the SOC_ANDROID_NDK_PATH CMake "
        "variable or SOC_ANDROID_NDK environment variable explicitly.")
endif()

file(TO_CMAKE_PATH "${_soc_android_ndk}" _soc_android_ndk)
if(NOT IS_ABSOLUTE "${_soc_android_ndk}")
    message(FATAL_ERROR
        "Android NDK path must be absolute: ${_soc_android_ndk}")
endif()
set(_soc_android_toolchain
    "${_soc_android_ndk}/build/cmake/android.toolchain.cmake")

if(NOT EXISTS "${_soc_android_toolchain}")
    message(FATAL_ERROR
        "Android NDK toolchain does not exist: ${_soc_android_toolchain}")
endif()

set(CMAKE_ANDROID_NDK "${_soc_android_ndk}" CACHE PATH "Android NDK root" FORCE)
set(ANDROID_NDK "${_soc_android_ndk}" CACHE PATH "Android NDK root" FORCE)
include("${_soc_android_toolchain}")

# CMake reloads this wrapper inside compiler-detection try_compile projects.
# The NDK toolchain replaces CMAKE_TRY_COMPILE_PLATFORM_VARIABLES, so append
# our supported CMake entry points after including it. Otherwise a path passed
# as -DSOC_ANDROID_NDK_PATH is available only to the outer configure and the
# nested configure fails before it can detect the compiler.
list(
    APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
        SOC_ANDROID_NDK_PATH
)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
