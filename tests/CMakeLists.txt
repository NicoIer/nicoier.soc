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

add_executable(soc_test_pipeline test_pipeline.c)
target_link_libraries(soc_test_pipeline PRIVATE soc::soc)
target_compile_features(soc_test_pipeline PRIVATE c_std_17)
soc_enable_warnings(soc_test_pipeline)

add_test(NAME soc.pipeline COMMAND soc_test_pipeline)
