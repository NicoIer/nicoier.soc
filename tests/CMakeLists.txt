add_executable(soc_test_abi test_abi.c)
target_link_libraries(soc_test_abi PRIVATE soc::soc)
target_compile_features(soc_test_abi PRIVATE c_std_17)
soc_enable_warnings(soc_test_abi)

add_test(NAME soc.abi COMMAND soc_test_abi)

add_executable(soc_test_types test_types.c)
target_link_libraries(soc_test_types PRIVATE soc::soc)
target_compile_features(soc_test_types PRIVATE c_std_17)
soc_enable_warnings(soc_test_types)

add_test(NAME soc.types COMMAND soc_test_types)

if(TARGET soc_cli_depth)
    add_executable(soc_test_cli_depth test_cli_depth.c)
    target_link_libraries(soc_test_cli_depth PRIVATE soc_cli_depth)
    target_compile_features(soc_test_cli_depth PRIVATE c_std_17)
    soc_enable_warnings(soc_test_cli_depth)

    add_test(NAME soc.cli_depth COMMAND soc_test_cli_depth)
endif()

add_executable(soc_test_cpu_features test_cpu_features.c)
target_link_libraries(soc_test_cpu_features PRIVATE soc_core)
target_include_directories(
    soc_test_cpu_features
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(soc_test_cpu_features PRIVATE c_std_17)
soc_enable_warnings(soc_test_cpu_features)

add_test(NAME soc.cpu_features COMMAND soc_test_cpu_features)

add_executable(soc_test_kernels test_kernels.c)
target_link_libraries(soc_test_kernels PRIVATE soc_core)
target_include_directories(
    soc_test_kernels
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(soc_test_kernels PRIVATE c_std_17)
soc_enable_warnings(soc_test_kernels)

add_test(NAME soc.kernels COMMAND soc_test_kernels)

add_executable(
    soc_test_simd_differential
    test_simd_differential.c
)
target_link_libraries(soc_test_simd_differential PRIVATE soc_core)
target_include_directories(
    soc_test_simd_differential
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(soc_test_simd_differential PRIVATE c_std_17)
soc_enable_warnings(soc_test_simd_differential)

add_test(NAME soc.simd_differential COMMAND soc_test_simd_differential)

add_executable(soc_test_simd_pipeline test_simd_pipeline.c)
target_link_libraries(soc_test_simd_pipeline PRIVATE soc_core)
target_include_directories(
    soc_test_simd_pipeline
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(soc_test_simd_pipeline PRIVATE c_std_17)
soc_enable_warnings(soc_test_simd_pipeline)

add_test(NAME soc.simd_pipeline COMMAND soc_test_simd_pipeline)

add_executable(soc_test_pipeline test_pipeline.c)
target_link_libraries(soc_test_pipeline PRIVATE soc::soc)
target_compile_features(soc_test_pipeline PRIVATE c_std_17)
soc_enable_warnings(soc_test_pipeline)

add_test(NAME soc.pipeline COMMAND soc_test_pipeline)

add_executable(soc_test_mesh test_mesh.c)
target_link_libraries(soc_test_mesh PRIVATE soc::soc)
target_include_directories(
    soc_test_mesh
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(soc_test_mesh PRIVATE c_std_17)
soc_enable_warnings(soc_test_mesh)

add_test(NAME soc.mesh COMMAND soc_test_mesh)

add_executable(soc_test_depth test_depth.c)
target_link_libraries(soc_test_depth PRIVATE soc::soc)
target_compile_features(soc_test_depth PRIVATE c_std_17)
soc_enable_warnings(soc_test_depth)

add_test(NAME soc.depth COMMAND soc_test_depth)

add_executable(soc_test_visibility test_visibility.c)
target_link_libraries(soc_test_visibility PRIVATE soc::soc)
target_compile_features(soc_test_visibility PRIVATE c_std_17)
soc_enable_warnings(soc_test_visibility)

add_test(NAME soc.visibility COMMAND soc_test_visibility)

add_executable(soc_test_hiz test_hiz.c)
target_link_libraries(soc_test_hiz PRIVATE soc_core)
target_include_directories(
    soc_test_hiz
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(soc_test_hiz PRIVATE c_std_17)
soc_enable_warnings(soc_test_hiz)

add_test(NAME soc.hiz COMMAND soc_test_hiz)

add_executable(soc_test_rasterizer test_rasterizer.c)
target_link_libraries(soc_test_rasterizer PRIVATE soc_core)
target_include_directories(
    soc_test_rasterizer
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(soc_test_rasterizer PRIVATE c_std_17)
soc_enable_warnings(soc_test_rasterizer)

add_test(NAME soc.rasterizer COMMAND soc_test_rasterizer)

if(TARGET soc_bench)
    add_test(
        NAME soc.bench.validate
        COMMAND soc_bench --suite full --validate-only
    )
    set_tests_properties(soc.bench.validate PROPERTIES LABELS validation)
endif()

if(TARGET soc_kernel_bench)
    add_test(
        NAME soc.kernel_bench.validate
        COMMAND soc_kernel_bench --validate-only
    )
    set_tests_properties(
        soc.kernel_bench.validate
        PROPERTIES LABELS validation
    )
endif()

if(TARGET soc_cli)
    add_test(
        NAME soc.cli
        COMMAND
            "${CMAKE_COMMAND}"
            "-DSOC_CLI=$<TARGET_FILE:soc_cli>"
            "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/soc-cli-test-$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.cmake"
    )
endif()
