# Require an explicit Apple SDK path. Do not let CMake or xcrun select the
# active Xcode SDK implicitly.

set(_soc_apple_sdk "")
set(_soc_apple_sdk_cmake "")
set(_soc_apple_sdk_environment "")

if(DEFINED SOC_APPLE_SDK_PATH AND NOT SOC_APPLE_SDK_PATH STREQUAL "")
    set(_soc_apple_sdk_cmake "${SOC_APPLE_SDK_PATH}")
endif()

# Keep the CMake path input scoped to this configure, including when a later
# validation or project step fails. Remove the former cache input as a one-way
# migration so an older build directory cannot silently supply the SDK.
unset(SOC_APPLE_SDK_PATH CACHE)
unset(SOC_APPLE_SDK CACHE)
if(NOT _soc_apple_sdk_cmake STREQUAL "")
    set(SOC_APPLE_SDK_PATH "${_soc_apple_sdk_cmake}")
endif()

if(DEFINED ENV{SOC_APPLE_SDK}
        AND NOT "$ENV{SOC_APPLE_SDK}" STREQUAL "")
    set(_soc_apple_sdk_environment "$ENV{SOC_APPLE_SDK}")
endif()

if(NOT _soc_apple_sdk_cmake STREQUAL ""
        AND NOT _soc_apple_sdk_environment STREQUAL ""
        AND NOT _soc_apple_sdk_cmake STREQUAL _soc_apple_sdk_environment)
    message(FATAL_ERROR
        "Conflicting Apple SDK paths were provided through the "
        "SOC_APPLE_SDK_PATH CMake variable and SOC_APPLE_SDK environment "
        "variable.")
endif()

if(NOT _soc_apple_sdk_cmake STREQUAL "")
    set(_soc_apple_sdk "${_soc_apple_sdk_cmake}")
elseif(NOT _soc_apple_sdk_environment STREQUAL "")
    set(_soc_apple_sdk "${_soc_apple_sdk_environment}")
endif()

if(_soc_apple_sdk STREQUAL "")
    message(FATAL_ERROR
        "Apple SDK path is required. Set the SOC_APPLE_SDK_PATH CMake "
        "variable or SOC_APPLE_SDK environment variable explicitly.")
endif()

file(TO_CMAKE_PATH "${_soc_apple_sdk}" _soc_apple_sdk)
include("${CMAKE_CURRENT_LIST_DIR}/../SocAppleSDK.cmake")
soc_validate_apple_sdk(
    "${_soc_apple_sdk}"
    "${SOC_APPLE_SDK_PLATFORM}"
)

set(CMAKE_OSX_SYSROOT "${_soc_apple_sdk}" CACHE PATH "Apple SDK root" FORCE)

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    SOC_APPLE_SDK_PATH
    SOC_APPLE_SDK_PLATFORM)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
