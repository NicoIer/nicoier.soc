#ifndef SOC_CLI_PNG_H_INCLUDED
#define SOC_CLI_PNG_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#define SOC_CLI_PNG_MAX_DIMENSION 0x7fffffffu

int soc_cli_png_write_gray8(
    const char* path,
    uint32_t width,
    uint32_t height,
    const unsigned char* pixels,
    char* error,
    size_t error_capacity
);

#endif
