// SPDX-License-Identifier: Apache-2.0

#include "golden_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_write.h"

// GitHub renders a `.bin` diff as "Binary file not shown", so a layout
// regression in a golden reference is invisible in review without this.
// png_write_framebuffer_gray8() is the same unpacking path every other PNG
// dump in this codebase uses (the golden mismatch dumps below, and the
// headless simulator's frames), so a packing bug shows up here rather than
// being hidden by a second, independent decoder.
static bool write_companion_png(const framebuffer_t *fb, const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);

    if (!png_write_framebuffer_gray8(fb, path)) {
        fprintf(stderr, "failed to write %s\n", path);
        return false;
    }
    printf("updated %s\n", path);
    return true;
}

static void dump_mismatch_png(const framebuffer_t *fb, const char *dump_dir, const char *label)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dump_dir, label);

    if (png_write_framebuffer_gray8(fb, path)) {
        fprintf(stderr, "  wrote %s\n", path);
    }
}

// Writes both tests/golden/<name>.bin (the assertion) and tests/golden/<name>.png
// (documentation for reviewers) from the same buffer, so they can never drift
// apart from each other.
static bool write_golden(const framebuffer_t *fb, const char *golden_dir, const char *name)
{
    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s/%s.bin", golden_dir, name);

    const size_t size = fb->stride * (size_t)fb->height;

    FILE *out = fopen(bin_path, "wb");
    if (out == NULL || fwrite(fb->pixels, 1, size, out) != size) {
        fprintf(stderr, "failed to write %s\n", bin_path);
        if (out != NULL) {
            fclose(out);
        }
        return false;
    }
    fclose(out);
    printf("updated %s (%zu bytes)\n", bin_path, size);

    return write_companion_png(fb, golden_dir, name);
}

bool golden_check_framebuffer(const framebuffer_t *fb, const char *golden_dir, const char *dump_dir,
                              const char *name)
{
    if (getenv("UPDATE_GOLDEN") != NULL) {
        return write_golden(fb, golden_dir, name);
    }

    char golden_path[512];
    snprintf(golden_path, sizeof(golden_path), "%s/%s.bin", golden_dir, name);

    const size_t actual_size = fb->stride * (size_t)fb->height;

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
        dump_mismatch_png(fb, dump_dir, label);

        if (read <= actual_size) {
            framebuffer_t expected_fb;
            framebuffer_init(&expected_fb, expected, actual_size, fb->width, fb->height,
                             fb->format);
            snprintf(label, sizeof(label), "%s.expected", name);
            dump_mismatch_png(&expected_fb, dump_dir, label);
        }
    }

    free(expected);
    return matches;
}
