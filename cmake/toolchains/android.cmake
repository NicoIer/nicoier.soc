# Delegate to the Android NDK's supported CMake toolchain while allowing the
# NDK location to come from CMake or one of the conventional environment
# variables. SOC_ANDROID_NDK has the highest priority.

set(_soc_android_ndk "")

foreach(_soc_android_ndk_variable
        SOC_ANDROID_NDK
        CMAKE_ANDROID_NDK
        ANDROID_NDK)
    if(DEFINED "${_soc_android_ndk_variable}"
            AND NOT "${${_soc_android_ndk_variable}}" STREQUAL "")
        set(_soc_android_ndk "${${_soc_android_ndk_variable}}")
        break()
    endif()
endforeach()

if(_soc_android_ndk STREQUAL "")
    foreach(_soc_android_ndk_environment
            ANDROID_NDK_HOME
            ANDROID_NDK_ROOT
            NDK_PATH)
        if(DEFINED ENV{${_soc_android_ndk_environment}}
                AND NOT "$ENV{${_soc_android_ndk_environment}}" STREQUAL "")
            set(_soc_android_ndk "$ENV{${_soc_android_ndk_environment}}")
            break()
        endif()
    endforeach()
endif()

if(_soc_android_ndk STREQUAL "")
    message(FATAL_ERROR
        "Android NDK not found. Set SOC_ANDROID_NDK, ANDROID_NDK_HOME, "
        "ANDROID_NDK_ROOT, or NDK_PATH.")
endif()

file(TO_CMAKE_PATH "${_soc_android_ndk}" _soc_android_ndk)
set(_soc_android_toolchain
    "${_soc_android_ndk}/build/cmake/android.toolchain.cmake")

if(NOT EXISTS "${_soc_android_toolchain}")
    message(FATAL_ERROR
        "Android NDK toolchain does not exist: ${_soc_android_toolchain}")
endif()

set(ANDROID_NDK "${_soc_android_ndk}" CACHE PATH "Android NDK root" FORCE)
include("${_soc_android_toolchain}")
