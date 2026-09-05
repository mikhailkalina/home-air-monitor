// SPDX-License-Identifier: Apache-2.0
//
// gfx.c primitives are pinned down here at the pixel level, using a tiny
// synthetic font rather than the real DejaVu tables so that a change to the
// generated glyph data cannot break these tests. The generated fonts get
// their own coverage in tests/golden/.

#include <stdbool.h>
#include <stddef.h>

#include "hac_test.h"
#include "ui/framebuffer.h"
#include "ui/gfx.h"

#define FG 9u
#define BG 0u

static void make_fb(framebuffer_t *fb, uint8_t *storage, size_t storage_size, uint16_t width,
                    uint16_t height)
{
    framebuffer_init(fb, storage, storage_size, width, height, PIXFMT_GRAY4);
    framebuffer_clear(fb, BG);
    framebuffer_reset_dirty(fb);  // exercise gfx.c's own dirty tracking, not clear()'s
}

// --- hline / vline -----------------------------------------------------

static void hline_draws_length_pixels_starting_at_x(void)
{
    uint8_t storage[10] = {0};  // 20x1 GRAY4: 10 bytes
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 20, 1);

    gfx_hline(&fb, 3, 0, 4, FG);

    for (int32_t x = 0; x < 20; ++x) {
        const uint8_t expected = (x >= 3 && x < 7) ? FG : BG;
        HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, 0), expected);
    }
}

static void a_negative_length_draws_to_the_left_of_the_start_point(void)
{
    uint8_t storage[10] = {0};
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 20, 1);

    gfx_hline(&fb, 5, 0, -3, FG);  // equivalent to gfx_hline(&fb, 3, 0, 3, FG)

    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 2, 0), BG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 3, 0), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 4, 0), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 5, 0), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 6, 0), BG);
}

static void vline_draws_length_pixels_starting_at_y(void)
{
    uint8_t storage[5] = {0};  // 2x5 GRAY4: 1 byte/row * 5 rows
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 2, 5);

    gfx_vline(&fb, 0, 1, 3, FG);

    for (int32_t y = 0; y < 5; ++y) {
        const uint8_t expected = (y >= 1 && y < 4) ? FG : BG;
        HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, y), expected);
    }
}

static void lines_clip_at_the_buffer_edge_instead_of_crashing(void)
{
    uint8_t storage[10] = {0};
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 20, 1);

    gfx_hline(&fb, 18, 0, 10, FG);  // runs 8 pixels past the right edge

    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 17, 0), BG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 18, 0), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 19, 0), FG);
}

// --- rect / fill_rect ------------------------------------------------------

static void rect_draws_the_outline_only(void)
{
    uint8_t storage[18] = {0};  // 6x6 GRAY4: 3 bytes/row * 6 rows
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 6, 6);

    gfx_rect(&fb, 1, 1, 4, 3, FG);  // spans x in [1,5), y in [1,4)

    for (int32_t y = 0; y < 6; ++y) {
        for (int32_t x = 0; x < 6; ++x) {
            const bool on_border =
                (x >= 1 && x < 5 && (y == 1 || y == 3)) || (y >= 1 && y < 4 && (x == 1 || x == 4));
            const uint8_t expected = on_border ? FG : BG;
            HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, y), expected);
        }
    }
}

static void fill_rect_draws_every_pixel_in_the_area(void)
{
    uint8_t storage[18] = {0};  // 6x6 GRAY4
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 6, 6);

    gfx_fill_rect(&fb, 1, 1, 3, 2, FG);  // x in [1,4), y in [1,3)

    for (int32_t y = 0; y < 6; ++y) {
        for (int32_t x = 0; x < 6; ++x) {
            const bool inside = (x >= 1 && x < 4 && y >= 1 && y < 3);
            HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, y), inside ? FG : BG);
        }
    }
}

static void a_non_positive_width_or_height_draws_nothing(void)
{
    uint8_t storage[18] = {0};  // 6x6 GRAY4
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 6, 6);

    gfx_rect(&fb, 1, 1, 0, 3, FG);
    gfx_rect(&fb, 1, 1, 3, -1, FG);
    gfx_fill_rect(&fb, 1, 1, 0, 3, FG);
    gfx_fill_rect(&fb, 1, 1, 3, 0, FG);

    HAC_CHECK(!fb.dirty);
}

// --- blit_glyph1bpp ----------------------------------------------------

static void blit_paints_set_bits_and_leaves_clear_bits_untouched(void)
{
    // Two rows, 5 columns wide (not byte-aligned): row bytes are padded, and
    // the padding bits past column 4 must never be read.
    static const uint8_t bits[] = {
        0xB0,  // 0b10110xxx -> columns 0,2,3 set
        0x40,  // 0b01000xxx -> column 1 set
    };

    uint8_t storage[8] = {0};  // 8x2 GRAY4: 4 bytes/row * 2 rows
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 8, 2);

    gfx_blit_glyph1bpp(&fb, 0, 0, bits, 5, 2, FG);

    const uint8_t row0_expected[8] = {FG, BG, FG, FG, BG, BG, BG, BG};
    const uint8_t row1_expected[8] = {BG, FG, BG, BG, BG, BG, BG, BG};
    for (int32_t x = 0; x < 8; ++x) {
        HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, 0), row0_expected[x]);
        HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, 1), row1_expected[x]);
    }
}

static void blit_composites_over_the_existing_background(void)
{
    static const uint8_t bits[] = {0x80};  // 1x1, the single bit set

    uint8_t storage[1] = {0};
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 2, 1);
    framebuffer_set_pixel(&fb, 1, 0, 7);  // pre-existing pixel next to the glyph

    gfx_blit_glyph1bpp(&fb, 0, 0, bits, 1, 1, FG);

    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 0), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 1, 0), 7);  // untouched by the glyph's clear bits
}

// --- draw_text / measure_text -----------------------------------------

// A minimal font covering only 'A' (a 2x2 block) and 'B' (a 1x1 dot), so
// these tests do not depend on the generated DejaVu tables.
static const uint8_t k_test_bitmap[] = {
    0xC0,
    0xC0,  // 'A': 2x2, both rows = 0b11xxxxxx
    0x80,  // 'B': 1x1, set
};

static const font_glyph_t k_test_glyphs[2] = {
    {.width = 2, .height = 2, .x_offset = 0, .y_offset = -2, .x_advance = 3, .bitmap_offset = 0},
    {.width = 1, .height = 1, .x_offset = 1, .y_offset = -1, .x_advance = 2, .bitmap_offset = 2},
};

static const font_t k_test_font = {
    .name = "test",
    .line_height = 4,
    .first_codepoint = 'A',
    .glyph_count = 2,
    .glyphs = k_test_glyphs,
    .bitmap_data = k_test_bitmap,
};

static void draw_text_places_each_glyph_and_advances_the_cursor(void)
{
    uint8_t storage[30] = {0};  // 10x6 GRAY4: 5 bytes/row * 6 rows
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 10, 6);

    const int32_t end = gfx_draw_text(&fb, 0, 4, &k_test_font, "AB", FG);

    // 'A': 2x2 at (0 + x_offset=0, 4 + y_offset=-2) = (0, 2).
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 2), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 1, 2), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 3), FG);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 1, 3), FG);

    // Cursor after 'A' is 0 + 3 = 3. 'B': 1x1 at (3 + x_offset=1, 4 + y_offset=-1) = (4, 3).
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 4, 3), FG);

    // Final cursor: 3 (after 'A') + 2 (after 'B') = 5.
    HAC_CHECK_EQ_INT(end, 5);
}

static void an_unsupported_codepoint_is_skipped_without_advancing(void)
{
    uint8_t storage[30] = {0};  // 10x6 GRAY4
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 10, 6);

    const int32_t end = gfx_draw_text(&fb, 0, 4, &k_test_font, "AxB", FG);

    // 'x' is outside the font's ['A', 'B'] range and contributes nothing.
    HAC_CHECK_EQ_INT(end, 5);
}

static void measure_text_matches_where_draw_text_leaves_the_cursor(void)
{
    HAC_CHECK_EQ_INT(gfx_measure_text(&k_test_font, "AB"), 5);
    HAC_CHECK_EQ_INT(gfx_measure_text(&k_test_font, "AxB"), 5);
    HAC_CHECK_EQ_INT(gfx_measure_text(&k_test_font, "xyz"), 0);
    HAC_CHECK_EQ_INT(gfx_measure_text(&k_test_font, ""), 0);
}

static void a_null_font_or_text_is_handled_without_crashing(void)
{
    uint8_t storage[30] = {0};  // 10x6 GRAY4
    framebuffer_t fb;
    make_fb(&fb, storage, sizeof(storage), 10, 6);

    HAC_CHECK_EQ_INT(gfx_draw_text(&fb, 7, 0, NULL, "AB", FG), 7);
    HAC_CHECK_EQ_INT(gfx_draw_text(&fb, 7, 0, &k_test_font, NULL, FG), 7);
    HAC_CHECK_EQ_INT(gfx_measure_text(NULL, "AB"), 0);
    HAC_CHECK_EQ_INT(gfx_measure_text(&k_test_font, NULL), 0);
}

int main(void)
{
    HAC_RUN(hline_draws_length_pixels_starting_at_x);
    HAC_RUN(a_negative_length_draws_to_the_left_of_the_start_point);
    HAC_RUN(vline_draws_length_pixels_starting_at_y);
    HAC_RUN(lines_clip_at_the_buffer_edge_instead_of_crashing);

    HAC_RUN(rect_draws_the_outline_only);
    HAC_RUN(fill_rect_draws_every_pixel_in_the_area);
    HAC_RUN(a_non_positive_width_or_height_draws_nothing);

    HAC_RUN(blit_paints_set_bits_and_leaves_clear_bits_untouched);
    HAC_RUN(blit_composites_over_the_existing_background);

    HAC_RUN(draw_text_places_each_glyph_and_advances_the_cursor);
    HAC_RUN(an_unsupported_codepoint_is_skipped_without_advancing);
    HAC_RUN(measure_text_matches_where_draw_text_leaves_the_cursor);
    HAC_RUN(a_null_font_or_text_is_handled_without_crashing);

    return hac_test_summary();
}
