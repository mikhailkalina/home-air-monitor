// SPDX-License-Identifier: Apache-2.0
//
// Drawing primitives over a framebuffer_t. Every primitive clips silently
// against the buffer's bounds: drawing off the edge, or with a negative
// length, is a no-op for the clipped part rather than undefined behaviour.
//
// `value` is a raw pixel value in the framebuffer's own format (0-15 for
// PIXFMT_GRAY4, 0/nonzero for PIXFMT_MONO1); gfx.c does not know about grey
// levels or colour, only the caller does.

#ifndef HAC_CORE_UI_GFX_H
#define HAC_CORE_UI_GFX_H

#include <stdint.h>

#include "ui/fonts/font.h"
#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// A negative `length` draws to the left of / above (x, y) rather than to the
// right of / below it, so callers never need to pre-compute a start point.
void gfx_hline(framebuffer_t *fb, int32_t x, int32_t y, int32_t length, uint8_t value);
void gfx_vline(framebuffer_t *fb, int32_t x, int32_t y, int32_t length, uint8_t value);

// An outline: the four edges of the w x h rectangle with (x, y) as its
// top-left corner. w <= 0 or h <= 0 draws nothing.
void gfx_rect(framebuffer_t *fb, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t value);

// Every pixel of the w x h rectangle with (x, y) as its top-left corner.
// w <= 0 or h <= 0 draws nothing.
void gfx_fill_rect(framebuffer_t *fb, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t value);

// Draws a 1bpp bitmap (MSB-first, each of the `height` rows padded to a
// whole byte) with its top-left corner at (x, y). A set bit is painted with
// `value`; a clear bit is left untouched, so glyphs composite over whatever
// is already on the buffer.
void gfx_blit_glyph1bpp(framebuffer_t *fb, int32_t x, int32_t y, const uint8_t *bits, uint8_t width,
                        uint8_t height, uint8_t value);

// Draws `text` with (x, y) as the left end of its baseline. A codepoint
// outside the font's covered range is skipped and does not move the cursor.
// Returns the x coordinate one pixel past the last glyph advance, so callers
// can chain further drawing after the text.
int32_t gfx_draw_text(framebuffer_t *fb, int32_t x, int32_t y, const font_t *font, const char *text,
                      uint8_t value);

// The width in pixels gfx_draw_text() would advance the cursor by; does not
// touch `fb`. Used to right-align or centre text before drawing it.
int32_t gfx_measure_text(const font_t *font, const char *text);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_GFX_H
