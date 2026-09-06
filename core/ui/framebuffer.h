// SPDX-License-Identifier: Apache-2.0
//
// A platform-neutral pixel buffer. The core never allocates one: the caller
// (an adapter under platform/, or a test) supplies the storage, sized with
// framebuffer_required_size(), and framebuffer_init() only wraps it.
//
// Two packings are supported, matching the ED047TC1 panel and a possible
// 1bpp fast-refresh mode (see docs/architecture.md §5.5):
//
//   PIXFMT_GRAY4  4 bits per pixel, 16 grey levels, two pixels per byte.
//                 The odd pixel of a row occupies the high nibble, the even
//                 pixel the low nibble.
//
//                 That order is deliberate, and it is the opposite of the one
//                 a reader might assume: it matches the ED047TC1 reference
//                 layout used by LilyGo-EPD47 (`epd_draw_pixel()`,
//                 src/epd_driver.c:339-346 on branch `esp32s3`), which packs
//                 `x % 2` into the high nibble. Keeping the same order means
//                 the display adapter can hand this buffer straight to the
//                 panel driver with a memcpy, instead of walking 253 125
//                 bytes to swap every nibble on each flush.
//                 See docs/adr/0003-gray4-nibble-order.md.
//   PIXFMT_MONO1  1 bit per pixel, MSB-first, eight pixels per byte.
//
// Every row is padded to a whole number of bytes, so `stride` may exceed the
// minimum bytes needed for `width` pixels.

#ifndef HAC_CORE_UI_FRAMEBUFFER_H
#define HAC_CORE_UI_FRAMEBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIXFMT_GRAY4 = 0,
    PIXFMT_MONO1,
} pixel_format_t;

// A half-open pixel rectangle: it covers [x, x + w) by [y, y + h).
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} rect_t;

typedef struct {
    uint8_t *pixels;  // caller-owned; never allocated or freed here
    size_t pixels_size;
    uint16_t width;
    uint16_t height;
    size_t stride;  // bytes per row
    pixel_format_t format;

    // Bounding box of every pixel touched since the last
    // framebuffer_reset_dirty(). Undefined while `dirty` is false.
    bool dirty;
    rect_t dirty_rect;
} framebuffer_t;

// Bytes needed for `width` x `height` pixels in `format`. Callers size their
// storage with this before calling framebuffer_init().
size_t framebuffer_required_size(uint16_t width, uint16_t height, pixel_format_t format);

// Wraps `pixels` (at least framebuffer_required_size(width, height, format)
// bytes, ownership stays with the caller) as a width x height buffer in
// `format`. The buffer's initial contents are whatever `pixels` already held;
// call framebuffer_clear() for a known starting image.
//
// If `pixels` is NULL or `pixels_size` is too small, the framebuffer is left
// zero-sized: every later call clips to nothing instead of touching memory
// that is too small for it.
void framebuffer_init(framebuffer_t *fb, uint8_t *pixels, size_t pixels_size, uint16_t width,
                      uint16_t height, pixel_format_t format);

// Fills every pixel with `value` (0-15 for PIXFMT_GRAY4, 0/nonzero for
// PIXFMT_MONO1) and marks the whole buffer dirty.
void framebuffer_clear(framebuffer_t *fb, uint8_t value);

// Bounds-checked: a coordinate outside the buffer is a silent no-op, never
// undefined behaviour.
void framebuffer_set_pixel(framebuffer_t *fb, int32_t x, int32_t y, uint8_t value);

// Bounds-checked: a coordinate outside the buffer reads back 0.
uint8_t framebuffer_get_pixel(const framebuffer_t *fb, int32_t x, int32_t y);

// Clears the dirty flag and rect. Call once the buffer has been flushed to a
// display, so the next round of drawing starts a fresh bounding box.
void framebuffer_reset_dirty(framebuffer_t *fb);

// Computes the bounding box of every byte that differs between `current` and
// `previous` -- typically the buffer a screen was just rendered into and a
// snapshot of what the panel was last successfully flushed with. This is
// deliberately independent of `dirty`/`dirty_rect` above: a screen that
// redraws unconditionally on every call (as screen_home_render() does) marks
// the whole buffer touched every time, which is a correct answer to "what
// got drawn" and a useless one for "what should reach the panel". This
// function answers the second question by looking at the actual pixel
// bytes, so the caller never has to teach every draw routine to track what
// changed.
//
// `current` and `previous` must share width, height, format and stride --
// true of every real caller, since both are sized from the same display's
// geometry. A mismatch (or either buffer being unset) is reported as no
// difference rather than read out of bounds.
//
// Because PIXFMT_GRAY4 packs two pixels per byte, comparing whole bytes
// rather than individual pixels rounds the rectangle's x extent outward to
// an even pixel boundary automatically: the caller never receives a rect
// whose left or right edge splits a byte that a flush of that rect would
// have to touch anyway.
//
// Returns false, leaving *out zeroed, when the two buffers are pixel-for-
// pixel identical.
bool framebuffer_diff_dirty_rect(const framebuffer_t *current, const framebuffer_t *previous,
                                 rect_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_FRAMEBUFFER_H
