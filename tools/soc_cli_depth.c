#include "soc_cli_depth.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

uint64_t soc_cli_depth_to_gray8(
    const float* depth,
    size_t pixel_count,
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
        double reciprocal_distance;
        double view_distance;
        double logarithmic_depth;

        if (value <= 0.0) {
            pixels[pixel] = 255u;
            continue;
        }
        ++drawn_pixel_count;

        reciprocal_distance =
            value / near_plane + (1.0 - value) / far_plane;
        view_distance = reciprocal_distance > 0.0
            ? 1.0 / reciprocal_distance
            : far_plane;
        logarithmic_depth =
            log(view_distance / near_plane) / logarithmic_range;
        if (logarithmic_depth < 0.0) {
            logarithmic_depth = 0.0;
        } else if (logarithmic_depth > 1.0) {
            logarithmic_depth = 1.0;
        }

        /* Reserve black and white for the covered and uncovered endpoints. */
        pixels[pixel] = (unsigned char)(
            1.0 + floor(logarithmic_depth * 253.0 + 0.5)
        );
    }

    return drawn_pixel_count;
}
