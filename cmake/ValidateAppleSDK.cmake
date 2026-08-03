cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/SocAppleSDK.cmake")
soc_validate_apple_sdk("${SOC_APPLE_SDK_PATH}" "${SOC_APPLE_SDK_PLATFORM}")
