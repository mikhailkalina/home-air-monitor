// SPDX-License-Identifier: Apache-2.0
//
// Renders a test card exercising every core/ui primitive into a GRAY4 and a
// MONO1 framebuffer, then compares the raw bytes against tests/golden/*.bin.
// The comparison and the mismatch dumps live in golden_check.c.
//
// Run with UPDATE_GOLDEN=1 to (re)write the references after an intentional
// rendering change, e.g.:
//   UPDATE_GOLDEN=1 ./build/host-debug/tests/golden/test_golden_ui

#ifndef HAC_GOLDEN_DIR
#error "HAC_GOLDEN_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif
#ifndef HAC_DUMP_DIR
#error "HAC_DUMP_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif

#include "golden_check.h"
#include "hac_test.h"
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

static void gray4_test_card_matches_golden(void)
{
    static uint8_t storage[100 * CARD_HEIGHT];  // GRAY4: 100 bytes/row (ceil(200/2))
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), CARD_WIDTH, CARD_HEIGHT, PIXFMT_GRAY4);

    draw_test_card(&fb);
    HAC_CHECK(golden_check_framebuffer(&fb, HAC_GOLDEN_DIR, HAC_DUMP_DIR, "test_card_gray4"));
}

static void mono1_test_card_matches_golden(void)
{
    static uint8_t storage[25 * CARD_HEIGHT];  // MONO1: 25 bytes/row (ceil(200/8))
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), CARD_WIDTH, CARD_HEIGHT, PIXFMT_MONO1);

    draw_test_card(&fb);
    HAC_CHECK(golden_check_framebuffer(&fb, HAC_GOLDEN_DIR, HAC_DUMP_DIR, "test_card_mono1"));
}

int main(void)
{
    HAC_RUN(gray4_test_card_matches_golden);
    HAC_RUN(mono1_test_card_matches_golden);
    return hac_test_summary();
}
