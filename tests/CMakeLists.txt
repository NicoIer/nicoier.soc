add_executable(soc_test_abi test_abi.c)
target_link_libraries(soc_test_abi PRIVATE soc::soc)
target_compile_features(soc_test_abi PRIVATE c_std_17)
soc_enable_warnings(soc_test_abi)

add_test(NAME soc.abi COMMAND soc_test_abi)
