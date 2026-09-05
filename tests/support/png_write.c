// SPDX-License-Identifier: Apache-2.0

#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        for (int k = 0; k < 8; ++k) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}

static uint32_t adler32_of(const uint8_t *data, size_t len)
{
    uint32_t a = 1u;
    uint32_t b = 0u;
    for (size_t i = 0; i < len; ++i) {
        a = (a + (uint32_t)data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void put_be32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)(v);
}

static bool write_chunk(FILE *f, const char type[4], const uint8_t *data, size_t len)
{
    uint8_t header[8];
    put_be32(header, (uint32_t)len);
    memcpy(header + 4, type, 4);

    uint32_t crc = crc32_update(0, (const uint8_t *)type, 4);
    if (len > 0) {
        crc = crc32_update(crc, data, len);
    }
    uint8_t crc_bytes[4];
    put_be32(crc_bytes, crc);

    return fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
           (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(crc_bytes, 1, sizeof(crc_bytes), f) == sizeof(crc_bytes);
}

// Wraps `raw` (length `raw_len`) as a zlib stream made of uncompressed
// deflate blocks, so no compressor is needed. Returns a malloc'd buffer via
// `out` and its length as the return value, or 0 on allocation failure.
static size_t zlib_store(const uint8_t *raw, size_t raw_len, uint8_t **out)
{
    const size_t max_block = 65535u;
    const size_t block_count = raw_len == 0 ? 1u : (raw_len + max_block - 1u) / max_block;
    const size_t total = 2u                  // zlib header
                         + block_count * 5u  // per-block: 1 header + 2 LEN + 2 NLEN
                         + raw_len           // stored payload
                         + 4u;               // adler32 trailer

    uint8_t *buf = malloc(total);
    if (buf == NULL) {
        return 0;
    }

    size_t pos = 0;
    buf[pos++] = 0x78;
    buf[pos++] = 0x01;

    size_t remaining = raw_len;
    const uint8_t *src = raw;
    for (size_t b = 0; b < block_count; ++b) {
        const size_t block_len = remaining < max_block ? remaining : max_block;
        const bool is_final = (b + 1u == block_count);

        buf[pos++] = (uint8_t)(is_final ? 1 : 0);
        buf[pos++] = (uint8_t)(block_len & 0xFFu);
        buf[pos++] = (uint8_t)((block_len >> 8) & 0xFFu);
        const uint16_t nlen = (uint16_t)(~block_len & 0xFFFFu);
        buf[pos++] = (uint8_t)(nlen & 0xFF);
        buf[pos++] = (uint8_t)((nlen >> 8) & 0xFF);

        if (block_len > 0) {
            memcpy(buf + pos, src, block_len);
            pos += block_len;
            src += block_len;
            remaining -= block_len;
        }
    }

    put_be32(buf + pos, adler32_of(raw, raw_len));
    pos += 4u;

    *out = buf;
    return pos;
}

bool png_write_gray8(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height)
{
    if (path == NULL || pixels == NULL || width == 0u || height == 0u) {
        return false;
    }

    const size_t row_len = (size_t)width + 1u;  // + filter byte
    const size_t raw_len = row_len * (size_t)height;
    uint8_t *raw = malloc(raw_len);
    if (raw == NULL) {
        return false;
    }

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = raw + (size_t)y * row_len;
        row[0] = 0;  // filter type: None
        memcpy(row + 1, pixels + (size_t)y * width, width);
    }

    uint8_t *zlib_buf = NULL;
    const size_t zlib_len = zlib_store(raw, raw_len, &zlib_buf);
    free(raw);
    if (zlib_len == 0) {
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        free(zlib_buf);
        return false;
    }

    static const uint8_t signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    bool ok = fwrite(signature, 1, sizeof(signature), f) == sizeof(signature);

    uint8_t ihdr[13];
    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 0;   // color type: grayscale
    ihdr[10] = 0;  // compression method
    ihdr[11] = 0;  // filter method
    ihdr[12] = 0;  // interlace method
    ok = ok && write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    ok = ok && write_chunk(f, "IDAT", zlib_buf, zlib_len);
    ok = ok && write_chunk(f, "IEND", NULL, 0);

    free(zlib_buf);
    fclose(f);
    return ok;
}
