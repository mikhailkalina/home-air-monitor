// SPDX-License-Identifier: Apache-2.0

#include "golden_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_write.h"

static void framebuffer_to_gray8(const framebuffer_t *fb, uint8_t *out)
{
    for (uint16_t y = 0; y < fb->height; ++y) {
        for (uint16_t x = 0; x < fb->width; ++x) {
            const uint8_t v = framebuffer_get_pixel(fb, x, y);
            const uint8_t gray8 =
                (fb->format == PIXFMT_GRAY4) ? (uint8_t)(v * 17) : (uint8_t)(v != 0 ? 255 : 0);
            out[(size_t)y * (size_t)fb->width + (size_t)x] = gray8;
        }
    }
}

static void dump_png(const framebuffer_t *fb, const char *dump_dir, const char *label)
{
    uint8_t *gray8 = malloc((size_t)fb->width * (size_t)fb->height);
    if (gray8 == NULL) {
        return;
    }
    framebuffer_to_gray8(fb, gray8);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dump_dir, label);
    if (png_write_gray8(path, gray8, fb->width, fb->height)) {
        fprintf(stderr, "  wrote %s\n", path);
    }
    free(gray8);
}

static bool write_golden(const framebuffer_t *fb, const char *path, size_t size)
{
    FILE *out = fopen(path, "wb");
    if (out == NULL || fwrite(fb->pixels, 1, size, out) != size) {
        fprintf(stderr, "failed to write %s\n", path);
        if (out != NULL) {
            fclose(out);
        }
        return false;
    }
    fclose(out);
    printf("updated %s (%zu bytes)\n", path, size);
    return true;
}

bool golden_check_framebuffer(const framebuffer_t *fb, const char *golden_dir, const char *dump_dir,
                              const char *name)
{
    char golden_path[512];
    snprintf(golden_path, sizeof(golden_path), "%s/%s.bin", golden_dir, name);

    const size_t actual_size = fb->stride * (size_t)fb->height;

    if (getenv("UPDATE_GOLDEN") != NULL) {
        return write_golden(fb, golden_path, actual_size);
    }

    FILE *in = fopen(golden_path, "rb");
    if (in == NULL) {
        fprintf(stderr, "missing golden file %s (run with UPDATE_GOLDEN=1 to create it)\n",
                golden_path);
        return false;
    }

    // +1 to detect a golden file that is too long; zeroed so a short read
    // (a golden file that is too short) never leaves the tail uninitialized.
    uint8_t *expected = calloc(actual_size + 1u, 1);
    if (expected == NULL) {
        fclose(in);
        fprintf(stderr, "out of memory reading %s\n", golden_path);
        return false;
    }
    const size_t read = fread(expected, 1, actual_size + 1u, in);
    fclose(in);

    const bool matches = (read == actual_size) && memcmp(fb->pixels, expected, actual_size) == 0;

    if (!matches) {
        fprintf(stderr, "golden mismatch: %s (expected %zu bytes, read %zu)\n", golden_path,
                actual_size, read);

        char label[128];
        snprintf(label, sizeof(label), "%s.actual", name);
        dump_png(fb, dump_dir, label);

        if (read <= actual_size) {
            framebuffer_t expected_fb;
            framebuffer_init(&expected_fb, expected, actual_size, fb->width, fb->height,
                             fb->format);
            snprintf(label, sizeof(label), "%s.expected", name);
            dump_png(&expected_fb, dump_dir, label);
        }
    }

    free(expected);
    return matches;
}
