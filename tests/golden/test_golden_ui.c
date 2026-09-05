// SPDX-License-Identifier: Apache-2.0
//
// Renders a test card exercising every core/ui primitive into a GRAY4 and a
// MONO1 framebuffer, then compares the raw bytes against tests/golden/*.bin.
// The byte comparison is the whole assertion: it is exact and needs no image
// library. On a mismatch, the actual and expected buffers are dumped as PNG
// under the build directory (HAC_DUMP_DIR) for a human to look at.
//
// Run with UPDATE_GOLDEN=1 to (re)write the references after an intentional
// rendering change, e.g.:
//   UPDATE_GOLDEN=1 ./build/host-debug/tests/test_golden_ui

#ifndef HAC_GOLDEN_DIR
#error "HAC_GOLDEN_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif
#ifndef HAC_DUMP_DIR
#error "HAC_DUMP_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hac_test.h"
#include "png_write.h"
#include "ui/fonts/font_dejavu_sans.h"
#include "ui/framebuffer.h"
#include "ui/gfx.h"

#define CARD_WIDTH 200
#define CARD_HEIGHT 110

static void draw_test_card(framebuffer_t *fb)
{
    const uint8_t fg = (fb->format == PIXFMT_GRAY4) ? 15u : 1u;
    const uint8_t mid = (fb->format == PIXFMT_GRAY4) ? 7u : 1u;
    const uint8_t bg = 0u;

    framebuffer_clear(fb, bg);

    gfx_rect(fb, 0, 0, (int32_t)fb->width, (int32_t)fb->height, fg);

    gfx_fill_rect(fb, 4, 4, 30, 16, mid);
    gfx_hline(fb, 4, 24, 60, fg);
    gfx_vline(fb, 4, 24, 16, fg);

    // Deliberately runs past the bottom-right corner, to exercise clipping.
    gfx_fill_rect(fb, (int32_t)fb->width - 12, (int32_t)fb->height - 12, 24, 24, fg);

    // A hand-drawn "X", blitted directly rather than through a font, to
    // exercise gfx_blit_glyph1bpp() on its own.
    static const uint8_t x_glyph[] = {
        0xC3, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0xC3,
    };
    gfx_blit_glyph1bpp(fb, 70, 4, x_glyph, 8, 7, fg);

    gfx_draw_text(fb, 4, 55, &font_dejavu_sans_16, "CO2 738 ppm", fg);

    const char *big = "23.4";
    const int32_t big_width = gfx_measure_text(&font_dejavu_sans_32, big);
    gfx_draw_text(fb, (int32_t)fb->width - big_width - 4, 96, &font_dejavu_sans_32, big, fg);
}

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

static void dump_mismatch_png(const framebuffer_t *fb, const char *label)
{
    uint8_t *gray8 = malloc((size_t)fb->width * (size_t)fb->height);
    if (gray8 == NULL) {
        return;
    }
    framebuffer_to_gray8(fb, gray8);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", HAC_DUMP_DIR, label);
    if (png_write_gray8(path, gray8, fb->width, fb->height)) {
        fprintf(stderr, "  wrote %s\n", path);
    }
    free(gray8);
}

// Renders `fb` and checks it against tests/golden/<name>.bin. Returns true on
// a match (or after (re)writing the golden file under UPDATE_GOLDEN=1).
static bool check_against_golden(framebuffer_t *fb, const char *name)
{
    draw_test_card(fb);

    char golden_path[512];
    snprintf(golden_path, sizeof(golden_path), "%s/%s.bin", HAC_GOLDEN_DIR, name);

    const size_t actual_size = fb->stride * (size_t)fb->height;

    if (getenv("UPDATE_GOLDEN") != NULL) {
        FILE *out = fopen(golden_path, "wb");
        if (out == NULL || fwrite(fb->pixels, 1, actual_size, out) != actual_size) {
            fprintf(stderr, "failed to write %s\n", golden_path);
            if (out != NULL) {
                fclose(out);
            }
            return false;
        }
        fclose(out);
        printf("updated %s (%zu bytes)\n", golden_path, actual_size);
        return true;
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

        char actual_label[64];
        char expected_label[64];
        snprintf(actual_label, sizeof(actual_label), "%s.actual", name);
        snprintf(expected_label, sizeof(expected_label), "%s.expected", name);
        dump_mismatch_png(fb, actual_label);

        if (read <= actual_size) {
            framebuffer_t expected_fb;
            framebuffer_init(&expected_fb, expected, actual_size, fb->width, fb->height,
                             fb->format);
            dump_mismatch_png(&expected_fb, expected_label);
        }
    }

    free(expected);
    return matches;
}

static void gray4_test_card_matches_golden(void)
{
    static uint8_t storage[100 * CARD_HEIGHT];  // GRAY4: 100 bytes/row (ceil(200/2))
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), CARD_WIDTH, CARD_HEIGHT, PIXFMT_GRAY4);

    HAC_CHECK(check_against_golden(&fb, "test_card_gray4"));
}

static void mono1_test_card_matches_golden(void)
{
    static uint8_t storage[25 * CARD_HEIGHT];  // MONO1: 25 bytes/row (ceil(200/8))
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), CARD_WIDTH, CARD_HEIGHT, PIXFMT_MONO1);

    HAC_CHECK(check_against_golden(&fb, "test_card_mono1"));
}

int main(void)
{
    HAC_RUN(gray4_test_card_matches_golden);
    HAC_RUN(mono1_test_card_matches_golden);
    return hac_test_summary();
}
