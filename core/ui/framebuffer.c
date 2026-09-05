// SPDX-License-Identifier: Apache-2.0

#include "ui/framebuffer.h"

#include <string.h>

static size_t stride_for(uint16_t width, pixel_format_t format)
{
    if (format == PIXFMT_GRAY4) {
        return ((size_t)width + 1u) / 2u;
    }
    return ((size_t)width + 7u) / 8u;
}

size_t framebuffer_required_size(uint16_t width, uint16_t height, pixel_format_t format)
{
    return stride_for(width, format) * (size_t)height;
}

void framebuffer_init(framebuffer_t *fb, uint8_t *pixels, size_t pixels_size, uint16_t width,
                      uint16_t height, pixel_format_t format)
{
    memset(fb, 0, sizeof(*fb));

    const size_t stride = stride_for(width, format);
    const size_t required = stride * (size_t)height;

    if (pixels == NULL || pixels_size < required) {
        return;  // left zero-sized: every later call clips to nothing
    }

    fb->pixels = pixels;
    fb->pixels_size = pixels_size;
    fb->width = width;
    fb->height = height;
    fb->stride = stride;
    fb->format = format;
}

void framebuffer_clear(framebuffer_t *fb, uint8_t value)
{
    if (fb->pixels == NULL || fb->width == 0 || fb->height == 0) {
        return;
    }

    uint8_t fill_byte;
    if (fb->format == PIXFMT_GRAY4) {
        const uint8_t nibble = value & 0x0F;
        fill_byte = (uint8_t)((uint8_t)(nibble << 4) | nibble);
    } else {
        fill_byte = (uint8_t)((value != 0) ? 0xFF : 0x00);
    }

    memset(fb->pixels, fill_byte, fb->stride * (size_t)fb->height);

    fb->dirty = true;
    fb->dirty_rect.x = 0;
    fb->dirty_rect.y = 0;
    fb->dirty_rect.w = fb->width;
    fb->dirty_rect.h = fb->height;
}

static void mark_dirty_pixel(framebuffer_t *fb, uint16_t x, uint16_t y)
{
    if (!fb->dirty) {
        fb->dirty = true;
        fb->dirty_rect.x = x;
        fb->dirty_rect.y = y;
        fb->dirty_rect.w = 1;
        fb->dirty_rect.h = 1;
        return;
    }

    const int32_t x0 = fb->dirty_rect.x < x ? fb->dirty_rect.x : x;
    const int32_t y0 = fb->dirty_rect.y < y ? fb->dirty_rect.y : y;
    const int32_t old_x1 = (int32_t)fb->dirty_rect.x + fb->dirty_rect.w;
    const int32_t old_y1 = (int32_t)fb->dirty_rect.y + fb->dirty_rect.h;
    const int32_t x1 = old_x1 > (int32_t)x + 1 ? old_x1 : (int32_t)x + 1;
    const int32_t y1 = old_y1 > (int32_t)y + 1 ? old_y1 : (int32_t)y + 1;

    fb->dirty_rect.x = (uint16_t)x0;
    fb->dirty_rect.y = (uint16_t)y0;
    fb->dirty_rect.w = (uint16_t)(x1 - x0);
    fb->dirty_rect.h = (uint16_t)(y1 - y0);
}

void framebuffer_set_pixel(framebuffer_t *fb, int32_t x, int32_t y, uint8_t value)
{
    if (fb->pixels == NULL || x < 0 || y < 0 || x >= (int32_t)fb->width ||
        y >= (int32_t)fb->height) {
        return;
    }

    const size_t row = (size_t)y * fb->stride;

    if (fb->format == PIXFMT_GRAY4) {
        const size_t index = row + (size_t)x / 2u;
        const uint8_t nibble = value & 0x0F;
        uint8_t byte = fb->pixels[index];
        if ((x & 1) == 0) {
            byte = (uint8_t)((byte & 0x0F) | (uint8_t)(nibble << 4));
        } else {
            byte = (uint8_t)((byte & 0xF0) | nibble);
        }
        fb->pixels[index] = byte;
    } else {
        const size_t index = row + (size_t)x / 8u;
        const uint8_t bit = (uint8_t)(0x80u >> (x & 7));
        if (value != 0) {
            fb->pixels[index] = (uint8_t)(fb->pixels[index] | bit);
        } else {
            fb->pixels[index] = (uint8_t)(fb->pixels[index] & (uint8_t)~bit);
        }
    }

    mark_dirty_pixel(fb, (uint16_t)x, (uint16_t)y);
}

uint8_t framebuffer_get_pixel(const framebuffer_t *fb, int32_t x, int32_t y)
{
    if (fb->pixels == NULL || x < 0 || y < 0 || x >= (int32_t)fb->width ||
        y >= (int32_t)fb->height) {
        return 0;
    }

    const size_t row = (size_t)y * fb->stride;

    if (fb->format == PIXFMT_GRAY4) {
        const uint8_t byte = fb->pixels[row + (size_t)x / 2u];
        return ((x & 1) == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
    }

    const uint8_t byte = fb->pixels[row + (size_t)x / 8u];
    const uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    return (uint8_t)((byte & bit) != 0 ? 1 : 0);
}

void framebuffer_reset_dirty(framebuffer_t *fb)
{
    fb->dirty = false;
    memset(&fb->dirty_rect, 0, sizeof(fb->dirty_rect));
}
