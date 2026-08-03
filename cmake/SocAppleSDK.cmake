function(soc_validate_apple_sdk sdk_path expected_platform)
    if(sdk_path STREQUAL "")
        message(FATAL_ERROR "Apple SDK path is required.")
    endif()
    if(NOT IS_ABSOLUTE "${sdk_path}")
        message(FATAL_ERROR "Apple SDK path must be absolute: ${sdk_path}")
    endif()
    if(NOT IS_DIRECTORY "${sdk_path}")
        message(FATAL_ERROR
            "Apple SDK directory does not exist: ${sdk_path}")
    endif()

    if(expected_platform STREQUAL "")
        message(FATAL_ERROR
            "Expected Apple SDK platform is required. Set "
            "SOC_APPLE_SDK_PLATFORM to macosx, iphoneos, or "
            "iphonesimulator.")
    endif()
    if(NOT expected_platform MATCHES "^(macosx|iphoneos|iphonesimulator)$")
        message(FATAL_ERROR
            "Unsupported Apple SDK platform: ${expected_platform}")
    endif()

    set(_soc_sdk_settings "${sdk_path}/SDKSettings.json")
    if(NOT EXISTS "${_soc_sdk_settings}")
        message(FATAL_ERROR
            "Apple SDK metadata does not exist: ${_soc_sdk_settings}")
    endif()

    file(READ "${_soc_sdk_settings}" _soc_sdk_metadata)
    string(JSON _soc_actual_platform
        ERROR_VARIABLE _soc_metadata_error
        GET "${_soc_sdk_metadata}" DefaultProperties PLATFORM_NAME)
    if(NOT _soc_metadata_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Could not read PLATFORM_NAME from ${_soc_sdk_settings}: "
            "${_soc_metadata_error}")
    endif()
    if(NOT _soc_actual_platform STREQUAL expected_platform)
        message(FATAL_ERROR
            "Apple SDK platform mismatch: expected ${expected_platform}, but "
            "${sdk_path} is ${_soc_actual_platform}.")
    endif()
endfunction()
