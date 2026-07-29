add_library(
    soc_core
    OBJECT
        core/soc_context.c
        raster/soc_rasterizer.c
)

target_include_directories(
    soc_core
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(soc_core PRIVATE c_std_17)
set_target_properties(
    soc_core
    PROPERTIES
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON
)
soc_enable_warnings(soc_core)

if(SOC_BUILD_SHARED)
    add_library(
        soc
        SHARED
            abi/soc_api.c
            $<TARGET_OBJECTS:soc_core>
    )
    target_compile_definitions(soc PRIVATE SOC_BUILDING_LIBRARY)
else()
    add_library(
        soc
        STATIC
            abi/soc_api.c
            $<TARGET_OBJECTS:soc_core>
    )
    target_compile_definitions(soc PUBLIC SOC_STATIC)
endif()

add_library(soc::soc ALIAS soc)

target_include_directories(
    soc
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(soc PUBLIC c_std_17)
soc_enable_warnings(soc)

set_target_properties(
    soc
    PROPERTIES
        C_VISIBILITY_PRESET hidden
        OUTPUT_NAME soc
)

if(SOC_BUILD_SHARED)
    set_target_properties(
        soc
        PROPERTIES
            VERSION "${PROJECT_VERSION}"
            SOVERSION "${PROJECT_VERSION_MAJOR}"
    )
endif()
