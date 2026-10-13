// SPDX-License-Identifier: Apache-2.0

#include "ui/gfx.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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

// One pixel of `img`, read the way framebuffer_get_pixel() reads a buffer.
// Callers have already clipped (sx, sy) to the picture.
static uint8_t image_pixel(const image_t *img, int32_t sx, int32_t sy)
{
    const uint8_t *row = img->pixels + (size_t)sy * img->stride;

    if (img->format == PIXFMT_GRAY4) {
        const uint8_t byte = row[(size_t)sx / 2u];
        return ((sx & 1) == 0) ? (uint8_t)(byte & 0x0F) : (uint8_t)(byte >> 4);
    }
    const uint8_t bit = (uint8_t)(0x80u >> (sx & 7));
    return (uint8_t)((row[(size_t)sx / 8u] & bit) != 0 ? 1 : 0);
}

void gfx_blit_image(framebuffer_t *fb, int32_t x, int32_t y, const image_t *img)
{
    if (img == NULL || img->pixels == NULL || fb->pixels == NULL || img->format != fb->format) {
        return;
    }

    // The visible part, in buffer coordinates: [x0, x1) by [y0, y1).
    const int32_t x0 = (x > 0) ? x : 0;
    const int32_t y0 = (y > 0) ? y : 0;
    int32_t x1 = x + (int32_t)img->width;
    int32_t y1 = y + (int32_t)img->height;
    if (x1 > (int32_t)fb->width) {
        x1 = (int32_t)fb->width;
    }
    if (y1 > (int32_t)fb->height) {
        y1 = (int32_t)fb->height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    // Whole bytes line up only when both the first visible buffer column and
    // the picture column it receives are even -- which, as they differ by x,
    // is the case exactly when both x and x0 are. A row of odd visible width
    // still leaves one pixel to place on its own: copying its whole byte
    // would carry the picture's row padding, or its next column, into the
    // neighbouring buffer pixel.
    const bool whole_bytes = (fb->format == PIXFMT_GRAY4) && ((x & 1) == 0) && ((x0 & 1) == 0);
    const int32_t visible_w = x1 - x0;

    for (int32_t dy = y0; dy < y1; ++dy) {
        const int32_t sy = dy - y;
        int32_t dx = x0;

        if (whole_bytes) {
            const size_t pairs = (size_t)visible_w / 2u;
            uint8_t *dst = fb->pixels + (size_t)dy * fb->stride + (size_t)x0 / 2u;
            const uint8_t *src = img->pixels + (size_t)sy * img->stride + (size_t)(x0 - x) / 2u;
            memcpy(dst, src, pairs);
            dx += (int32_t)(pairs * 2u);
        }
        for (; dx < x1; ++dx) {
            framebuffer_set_pixel(fb, dx, dy, image_pixel(img, dx - x, sy));
        }
    }

    const rect_t drawn = {
        .x = (uint16_t)x0,
        .y = (uint16_t)y0,
        .w = (uint16_t)visible_w,
        .h = (uint16_t)(y1 - y0),
    };
    framebuffer_mark_dirty(fb, &drawn);
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
