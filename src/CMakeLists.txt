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
        platform/soc_memory.c
        platform/soc_thread_pool.c
        raster/soc_rasterizer.c
)

target_include_directories(
    soc_core
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(soc_core PRIVATE c_std_17)
target_link_libraries(soc_core PRIVATE Threads::Threads)
if(UNIX)
    target_link_libraries(soc_core PUBLIC m)
endif()
set_target_properties(
    soc_core
    PROPERTIES
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON
)
soc_enable_warnings(soc_core)

if(MSVC)
    target_compile_options(soc_core PUBLIC /experimental:c11atomics)
    target_compile_options(
        soc_core
        PRIVATE
            $<$<CONFIG:Release>:/fp:fast>
    )
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        soc_core
        PRIVATE
            $<$<CONFIG:Release>:-ffast-math>
            $<$<CONFIG:Release>:-ffp-contract=fast>
    )
endif()

set(_soc_native_apple_arm64 OFF)
if(
    APPLE
    AND NOT CMAKE_CROSSCOMPILING
    AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"
)
    if(
        NOT CMAKE_OSX_ARCHITECTURES
        OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"
    )
        set(_soc_native_apple_arm64 ON)
    endif()
endif()
if(_soc_native_apple_arm64 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        soc_core
        PRIVATE
            $<$<CONFIG:Release>:-mcpu=native>
    )
endif()
unset(_soc_native_apple_arm64)

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
target_link_libraries(soc PUBLIC Threads::Threads)
if(UNIX)
    target_link_libraries(soc PUBLIC m)
endif()
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
