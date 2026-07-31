add_executable(
    soc_cli
    soc_cli.c
    soc_cli_obj.c
    soc_cli_png.c
)

target_link_libraries(soc_cli PRIVATE soc::soc)
if(UNIX)
    target_link_libraries(soc_cli PRIVATE m)
endif()

target_compile_features(soc_cli PRIVATE c_std_17)
soc_enable_warnings(soc_cli)

if(APPLE)
    set_target_properties(
        soc_cli
        PROPERTIES
            INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}"
    )
elseif(UNIX)
    set_target_properties(
        soc_cli
        PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
endif()
