// SPDX-License-Identifier: Apache-2.0
//
// The bitmap font format gfx.c renders. A font is a contiguous run of
// codepoints, each with a 1bpp bitmap (MSB-first, every row padded to a
// byte) plus the metrics needed to place it on the baseline and advance the
// cursor. There is no vector data and no hinting: everything is pre-rasterized
// at the size it was generated for, which is what keeps gfx_draw_text() cheap
// enough to call from update_policy on every redraw.
//
// See apps/tools/font_gen/generate_fonts.py for how the fonts under
// core/ui/fonts/*.generated.c are produced.

#ifndef HAC_CORE_UI_FONTS_FONT_H
#define HAC_CORE_UI_FONTS_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t width;           // bitmap width in pixels; 0 for a glyph with no ink (space)
    uint8_t height;          // bitmap height in pixels
    int8_t x_offset;         // added to the cursor to get the bitmap's left edge
    int8_t y_offset;         // added to the baseline y to get the bitmap's top edge
    uint8_t x_advance;       // distance the cursor moves after this glyph
    uint16_t bitmap_offset;  // byte offset of this glyph's rows in bitmap_data
} font_glyph_t;

// Covers codepoints [first_codepoint, first_codepoint + glyph_count). A
// character outside that range has no glyph; gfx_draw_text() skips it without
// advancing the cursor.
typedef struct {
    const char *name;
    uint8_t line_height;  // recommended distance between successive baselines
    uint8_t first_codepoint;
    uint16_t glyph_count;
    const font_glyph_t *glyphs;  // glyphs[codepoint - first_codepoint]
    const uint8_t *bitmap_data;  // every glyph's rows, back to back
} font_t;

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_FONTS_FONT_H
