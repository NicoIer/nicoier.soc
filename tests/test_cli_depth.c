#include "soc_cli_depth.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf( \
                stderr, \
                "%s:%d: check failed: %s\n", \
                __FILE__, \
                __LINE__, \
                #condition \
            ); \
            return 1; \
        } \
    } while (0)

static int test_reserved_values(void)
{
    const float forward_depth[] = {
        1.0f,
        0.0f,
        0.655646145f,
        nextafterf(1.0f, 0.0f),
    };
    unsigned char pixels[sizeof(forward_depth) / sizeof(forward_depth[0])];
    uint64_t drawn_count;

    drawn_count = soc_cli_depth_to_gray8(
        forward_depth,
        sizeof(forward_depth) / sizeof(forward_depth[0]),
        0,
        0.1,
        100.0,
        pixels
    );

    CHECK(drawn_count == 3u);
    CHECK(pixels[0] == 255u);
    CHECK(pixels[1] == 1u);
    CHECK(pixels[2] > 1u && pixels[2] < 254u);
    CHECK(pixels[3] == 254u);
    return 0;
}

static int test_reversed_z_matches_forward_z(void)
{
    const float forward_depth[] = {0.0f, 0.25f, 0.5f, 0.75f};
    const float reversed_depth[] = {1.0f, 0.75f, 0.5f, 0.25f};
    unsigned char forward_pixels[4];
    unsigned char reversed_pixels[4];
    size_t pixel;

    CHECK(
        soc_cli_depth_to_gray8(
            forward_depth,
            4u,
            0,
            0.1,
            100.0,
            forward_pixels
        ) == 4u
    );
    CHECK(
        soc_cli_depth_to_gray8(
            reversed_depth,
            4u,
            1,
            0.1,
            100.0,
            reversed_pixels
        ) == 4u
    );
    for (pixel = 0u; pixel < 4u; ++pixel) {
        CHECK(forward_pixels[pixel] == reversed_pixels[pixel]);
        CHECK(forward_pixels[pixel] > 0u);
        CHECK(forward_pixels[pixel] < 255u);
    }
    return 0;
}

int main(void)
{
    if (test_reserved_values() != 0) {
        return 1;
    }
    if (test_reversed_z_matches_forward_z() != 0) {
        return 1;
    }
    return 0;
}
