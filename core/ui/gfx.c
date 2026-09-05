// SPDX-License-Identifier: Apache-2.0

#include "ui/gfx.h"

#include <stddef.h>

void gfx_hline(framebuffer_t *fb, int32_t x, int32_t y, int32_t length, uint8_t value)
{
    if (length < 0) {
        x += length + 1;
        length = -length;
    }
    for (int32_t i = 0; i < length; ++i) {
        framebuffer_set_pixel(fb, x + i, y, value);
    }
}

void gfx_vline(framebuffer_t *fb, int32_t x, int32_t y, int32_t length, uint8_t value)
{
    if (length < 0) {
        y += length + 1;
        length = -length;
    }
    for (int32_t i = 0; i < length; ++i) {
        framebuffer_set_pixel(fb, x, y + i, value);
    }
}

void gfx_rect(framebuffer_t *fb, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t value)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    gfx_hline(fb, x, y, w, value);
    gfx_hline(fb, x, y + h - 1, w, value);
    gfx_vline(fb, x, y, h, value);
    gfx_vline(fb, x + w - 1, y, h, value);
}

void gfx_fill_rect(framebuffer_t *fb, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t value)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int32_t row = 0; row < h; ++row) {
        gfx_hline(fb, x, y + row, w, value);
    }
}

void gfx_blit_glyph1bpp(framebuffer_t *fb, int32_t x, int32_t y, const uint8_t *bits, uint8_t width,
                        uint8_t height, uint8_t value)
{
    if (bits == NULL) {
        return;
    }

    const size_t row_bytes = ((size_t)width + 7u) / 8u;

    for (uint8_t row = 0; row < height; ++row) {
        const uint8_t *row_bits = bits + row_bytes * (size_t)row;
        for (uint8_t col = 0; col < width; ++col) {
            const uint8_t byte = row_bits[(size_t)col >> 3];
            const uint8_t bit = (uint8_t)(0x80u >> (col & 7));
            if ((byte & bit) != 0) {
                framebuffer_set_pixel(fb, x + (int32_t)col, y + (int32_t)row, value);
            }
        }
    }
}

// NULL when `c` falls outside the font's covered codepoint range.
static const font_glyph_t *find_glyph(const font_t *font, unsigned char c)
{
    const unsigned int first = font->first_codepoint;
    const unsigned int code = c;

    if (code < first) {
        return NULL;
    }
    const unsigned int index = code - first;
    if (index >= (unsigned int)font->glyph_count) {
        return NULL;
    }
    return &font->glyphs[index];
}

int32_t gfx_draw_text(framebuffer_t *fb, int32_t x, int32_t y, const font_t *font, const char *text,
                      uint8_t value)
{
    if (font == NULL || text == NULL) {
        return x;
    }

    int32_t cursor = x;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        const font_glyph_t *g = find_glyph(font, *p);
        if (g == NULL) {
            continue;  // unsupported codepoint: skip, do not advance
        }
        const uint8_t *bits = font->bitmap_data + g->bitmap_offset;
        gfx_blit_glyph1bpp(fb, cursor + g->x_offset, y + g->y_offset, bits, g->width, g->height,
                           value);
        cursor += g->x_advance;
    }
    return cursor;
}

int32_t gfx_measure_text(const font_t *font, const char *text)
{
    if (font == NULL || text == NULL) {
        return 0;
    }

    int32_t width = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        const font_glyph_t *g = find_glyph(font, *p);
        if (g == NULL) {
            continue;
        }
        width += g->x_advance;
    }
    return width;
}
