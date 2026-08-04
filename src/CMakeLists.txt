add_library(
    soc_core
    OBJECT
        core/soc_context.c
        core/soc_cpu_features.c
        core/soc_kernels.c
        core/soc_kernels_neon.c
        core/soc_kernels_transform.c
        core/soc_mesh.c
        core/soc_pipeline.c
        core/soc_snapshot.c
        occlusion/soc_hiz.c
        occlusion/soc_visibility.c
        occlusion/soc_visibility_neon.c
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

if(MSVC)
    set_source_files_properties(
        core/soc_kernels_neon.c
        core/soc_kernels_transform.c
        PROPERTIES COMPILE_OPTIONS /fp:strict
    )
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        core/soc_kernels_neon.c
        core/soc_kernels_transform.c
        PROPERTIES COMPILE_OPTIONS -ffp-contract=off
    )
endif()

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
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
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

if(WIN32)
    # Keep Windows artifact names consistent with the libsoc names used on
    # every other platform, including MSVC where CMake omits "lib" by default.
    set_target_properties(soc PROPERTIES PREFIX "lib")
    if(SOC_BUILD_SHARED)
        set_target_properties(soc PROPERTIES IMPORT_PREFIX "lib")
    endif()
endif()

if(SOC_BUILD_SHARED)
    set_target_properties(
        soc
        PROPERTIES
            VERSION "${PROJECT_VERSION}"
            SOVERSION "${PROJECT_VERSION_MAJOR}"
    )
endif()
