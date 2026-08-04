#include "soc_cli_depth.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

uint64_t soc_cli_depth_to_gray8(
    const float* depth,
    size_t pixel_count,
    int reversed_z,
    double near_plane,
    double far_plane,
    unsigned char* pixels
)
{
    const double logarithmic_range = log(far_plane / near_plane);
    uint64_t drawn_pixel_count = 0u;
    size_t pixel;

    for (pixel = 0u; pixel < pixel_count; ++pixel) {
        const double value = depth[pixel];
        double forward_depth;
        double reciprocal_distance;
        double view_distance;
        double logarithmic_depth;

        if (isfinite(value) == 0) {
            pixels[pixel] = 0u;
            continue;
        }
        if ((reversed_z != 0 && value <= 0.0) ||
            (reversed_z == 0 && value >= 1.0)) {
            pixels[pixel] = 255u;
            continue;
        }
        ++drawn_pixel_count;

        forward_depth = reversed_z != 0 ? 1.0 - value : value;
        if (forward_depth < 0.0) {
            forward_depth = 0.0;
        } else if (forward_depth > 1.0) {
            forward_depth = 1.0;
        }

        reciprocal_distance =
            (1.0 - forward_depth) / near_plane +
            forward_depth / far_plane;
        view_distance = reciprocal_distance > 0.0
            ? 1.0 / reciprocal_distance
            : far_plane;
        logarithmic_depth =
            log(view_distance / near_plane) / logarithmic_range;
        if (isfinite(logarithmic_depth) == 0) {
            pixels[pixel] = 0u;
            continue;
        }
        if (logarithmic_depth < 0.0) {
            logarithmic_depth = 0.0;
        } else if (logarithmic_depth > 1.0) {
            logarithmic_depth = 1.0;
        }

        /* Reserve black and white for invalid and uncovered pixels. */
        pixels[pixel] = (unsigned char)(
            1.0 + floor(logarithmic_depth * 253.0 + 0.5)
        );
    }

    return drawn_pixel_count;
}
