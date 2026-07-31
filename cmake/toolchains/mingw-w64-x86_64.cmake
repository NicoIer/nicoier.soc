set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(
    SOC_MINGW_C_COMPILER
    NAMES x86_64-w64-mingw32-gcc
)
if(NOT SOC_MINGW_C_COMPILER)
    message(FATAL_ERROR "x86_64-w64-mingw32-gcc was not found in PATH")
endif()

find_program(
    SOC_MINGW_RC_COMPILER
    NAMES x86_64-w64-mingw32-windres
)
if(NOT SOC_MINGW_RC_COMPILER)
    message(FATAL_ERROR "x86_64-w64-mingw32-windres was not found in PATH")
endif()

set(
    CMAKE_C_COMPILER
    "${SOC_MINGW_C_COMPILER}"
    CACHE FILEPATH
    "MinGW-w64 C compiler"
)
set(
    CMAKE_RC_COMPILER
    "${SOC_MINGW_RC_COMPILER}"
    CACHE FILEPATH
    "MinGW-w64 resource compiler"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
