#include "soc_cli_png.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(
    char* error,
    size_t error_capacity,
    const char* format,
    ...
)
{
    va_list arguments;

    if (error == NULL || error_capacity == 0u) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_capacity, format, arguments);
    va_end(arguments);
}

static int checked_size_add(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || left > SIZE_MAX - right) {
        return 0;
    }

    *out_result = left + right;
    return 1;
}

static int checked_size_multiply(
    size_t left,
    size_t right,
    size_t* out_result
)
{
    if (out_result == NULL || (right != 0u && left > SIZE_MAX / right)) {
        return 0;
    }

    *out_result = left * right;
    return 1;
}

static void encode_u32_be(unsigned char* output, uint32_t value)
{
    output[0] = (unsigned char)(value >> 24u);
    output[1] = (unsigned char)(value >> 16u);
    output[2] = (unsigned char)(value >> 8u);
    output[3] = (unsigned char)value;
}

static uint32_t crc32_update(
    uint32_t crc,
    const unsigned char* data,
    size_t size
)
{
    size_t index;

    for (index = 0u; index < size; ++index) {
        uint32_t value = crc ^ data[index];
        uint32_t bit;

        for (bit = 0u; bit < 8u; ++bit) {
            value = (value >> 1u) ^
                (0xedb88320u & (uint32_t)(-(int32_t)(value & 1u)));
        }
        crc = value;
    }

    return crc;
}

static uint32_t adler32(const unsigned char* data, size_t size)
{
    uint32_t sum1 = 1u;
    uint32_t sum2 = 0u;
    size_t index;

    for (index = 0u; index < size; ++index) {
        sum1 = (sum1 + data[index]) % 65521u;
        sum2 = (sum2 + sum1) % 65521u;
    }

    return (sum2 << 16u) | sum1;
}

static int write_bytes(
    FILE* file,
    const void* data,
    size_t size
)
{
    return size == 0u || fwrite(data, 1u, size, file) == size;
}

static int write_chunk(
    FILE* file,
    const unsigned char type[4],
    const unsigned char* data,
    uint32_t size
)
{
    unsigned char encoded_size[4];
    unsigned char encoded_crc[4];
    uint32_t crc = 0xffffffffu;

    encode_u32_be(encoded_size, size);
    crc = crc32_update(crc, type, 4u);
    crc = crc32_update(crc, data, size);
    crc ^= 0xffffffffu;
    encode_u32_be(encoded_crc, crc);

    return write_bytes(file, encoded_size, sizeof(encoded_size)) &&
        write_bytes(file, type, 4u) &&
        write_bytes(file, data, size) &&
        write_bytes(file, encoded_crc, sizeof(encoded_crc));
}

static int make_filtered_image(
    uint32_t width,
    uint32_t height,
    const unsigned char* pixels,
    unsigned char** out_data,
    size_t* out_size
)
{
    size_t row_size;
    size_t total_size;
    unsigned char* data;
    uint32_t row;

    if (!checked_size_add((size_t)width, 1u, &row_size) ||
        !checked_size_multiply(row_size, (size_t)height, &total_size)) {
        return 0;
    }

    data = malloc(total_size);
    if (data == NULL) {
        return 0;
    }

    for (row = 0u; row < height; ++row) {
        const size_t target_offset = (size_t)row * row_size;
        const size_t source_offset = (size_t)row * (size_t)width;

        data[target_offset] = 0u;
        memcpy(
            data + target_offset + 1u,
            pixels + source_offset,
            width
        );
    }

    *out_data = data;
    *out_size = total_size;
    return 1;
}

static int make_stored_zlib_stream(
    const unsigned char* data,
    size_t data_size,
    unsigned char** out_stream,
    uint32_t* out_size
)
{
    size_t block_count;
    size_t overhead;
    size_t stream_size;
    unsigned char* stream;
    size_t source_offset = 0u;
    size_t target_offset = 0u;

    if (data_size == 0u) {
        return 0;
    }

    block_count = data_size / 65535u;
    if (data_size % 65535u != 0u) {
        ++block_count;
    }

    if (!checked_size_multiply(block_count, 5u, &overhead) ||
        !checked_size_add(overhead, 6u, &overhead) ||
        !checked_size_add(data_size, overhead, &stream_size) ||
        stream_size > SOC_CLI_PNG_MAX_DIMENSION) {
        return 0;
    }

    stream = malloc(stream_size);
    if (stream == NULL) {
        return 0;
    }

    stream[target_offset++] = 0x78u;
    stream[target_offset++] = 0x01u;

    while (source_offset < data_size) {
        const size_t remaining = data_size - source_offset;
        const uint16_t block_size = (uint16_t)(
            remaining > 65535u ? 65535u : remaining
        );
        const uint16_t inverse_size = (uint16_t)~block_size;
        const int is_final =
            source_offset + (size_t)block_size == data_size;

        stream[target_offset++] = is_final != 0 ? 1u : 0u;
        stream[target_offset++] = (unsigned char)block_size;
        stream[target_offset++] = (unsigned char)(block_size >> 8u);
        stream[target_offset++] = (unsigned char)inverse_size;
        stream[target_offset++] = (unsigned char)(inverse_size >> 8u);
        memcpy(
            stream + target_offset,
            data + source_offset,
            block_size
        );
        target_offset += block_size;
        source_offset += block_size;
    }

    encode_u32_be(stream + target_offset, adler32(data, data_size));
    target_offset += 4u;

    if (target_offset != stream_size) {
        free(stream);
        return 0;
    }

    *out_stream = stream;
    *out_size = (uint32_t)stream_size;
    return 1;
}

int soc_cli_png_write_gray8(
    const char* path,
    uint32_t width,
    uint32_t height,
    const unsigned char* pixels,
    char* error,
    size_t error_capacity
)
{
    static const unsigned char signature[8] = {
        0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au,
    };
    static const unsigned char ihdr_type[4] = {'I', 'H', 'D', 'R'};
    static const unsigned char idat_type[4] = {'I', 'D', 'A', 'T'};
    static const unsigned char iend_type[4] = {'I', 'E', 'N', 'D'};
    unsigned char ihdr[13] = {0u};
    unsigned char* filtered = NULL;
    size_t filtered_size = 0u;
    unsigned char* compressed = NULL;
    uint32_t compressed_size = 0u;
    FILE* file = NULL;
    int success = 0;

    if (error != NULL && error_capacity > 0u) {
        error[0] = '\0';
    }
    if (path == NULL ||
        path[0] == '\0' ||
        width == 0u ||
        height == 0u ||
        width > SOC_CLI_PNG_MAX_DIMENSION ||
        height > SOC_CLI_PNG_MAX_DIMENSION ||
        pixels == NULL) {
        set_error(
            error,
            error_capacity,
            "PNG path, dimensions in [1, 2^31-1], and pixels are required"
        );
        return 0;
    }

    if (!make_filtered_image(
            width,
            height,
            pixels,
            &filtered,
            &filtered_size
        ) ||
        !make_stored_zlib_stream(
            filtered,
            filtered_size,
            &compressed,
            &compressed_size
        )) {
        set_error(
            error,
            error_capacity,
            "image is too large or there is not enough memory"
        );
        goto cleanup;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        set_error(
            error,
            error_capacity,
            "cannot open PNG '%s': %s",
            path,
            strerror(errno)
        );
        goto cleanup;
    }

    encode_u32_be(ihdr, width);
    encode_u32_be(ihdr + 4u, height);
    ihdr[8] = 8u;
    ihdr[9] = 0u;
    ihdr[10] = 0u;
    ihdr[11] = 0u;
    ihdr[12] = 0u;

    if (!write_bytes(file, signature, sizeof(signature)) ||
        !write_chunk(file, ihdr_type, ihdr, sizeof(ihdr)) ||
        !write_chunk(file, idat_type, compressed, compressed_size) ||
        !write_chunk(file, iend_type, NULL, 0u)) {
        set_error(
            error,
            error_capacity,
            "failed while writing PNG '%s'",
            path
        );
        goto cleanup;
    }

    if (fclose(file) != 0) {
        file = NULL;
        set_error(
            error,
            error_capacity,
            "cannot close PNG '%s': %s",
            path,
            strerror(errno)
        );
        goto cleanup;
    }
    file = NULL;
    success = 1;

cleanup:
    if (file != NULL) {
        (void)fclose(file);
    }
    free(compressed);
    free(filtered);
    return success;
}
