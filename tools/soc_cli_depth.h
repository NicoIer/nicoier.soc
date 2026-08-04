#ifndef SOC_CLI_DEPTH_H_INCLUDED
#define SOC_CLI_DEPTH_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

/*
 * Converts Level 0 depth to a diagnostic grayscale image.
 *
 * 0:     non-finite/invalid depth
 * 1-254: valid covered depth, logarithmic in view distance
 * 255:   clear/uncovered depth
 */
uint64_t soc_cli_depth_to_gray8(
    const float* depth,
    size_t pixel_count,
    int reversed_z,
    double near_plane,
    double far_plane,
    unsigned char* pixels
);

#endif
