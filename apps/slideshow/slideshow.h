// SPDX-License-Identifier: Apache-2.0
//
// The slideshow test app's logic: which picture is next, when it is due, and
// how it is drawn. Shared by the app's two compositions --
// apps/slideshow/host/main.c (the simulator's window, or PNG frames) and
// apps/slideshow/esp32/main/main.c (the panel) -- which own the clock, the
// display and the waiting, as apps/simulator/main.c and apps/firmware_esp32
// do for the product.
//
// Every picture replaces the whole panel, so every frame is a REFRESH_FULL.
// The interval runs from the end of one refresh to the start of the next --
// the way core/app/update_policy.c measures the panel's minimum full-refresh
// interval -- and is never shorter than that minimum: a test app is no
// reason to drive the glass harder than the product may. With the board's
// 60 s minimum and a ~3.3 s refresh, "one a minute" is one every ~63 s.
//
// Pure: no clock, no allocation, no I/O. Tested on the host from where it
// lives (tests/unit/test_slideshow.c).

#ifndef HAC_APPS_SLIDESHOW_SLIDESHOW_H
#define HAC_APPS_SLIDESHOW_SLIDESHOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/framebuffer.h"
#include "ui/image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t interval_ms;  // asked for; raised to the panel's minimum if shorter
    uint8_t background;    // level around a picture smaller than the panel
    bool caption;          // "n/N name" in a box in the bottom-left corner
} slideshow_config_t;

typedef struct {
    const image_t *const *images;
    const char *const *names;  // may be NULL: captions then show the number only
    size_t count;
    slideshow_config_t cfg;  // interval_ms already raised to the panel's minimum

    size_t current;  // the picture the next frame shows
    bool have_flushed;
    uint64_t last_flush_ms;  // when the last flush attempt ended
} slideshow_t;

// `images` and `names` (count entries each) must outlive `s`.
// `panel_min_full_refresh_interval_ms` is the display port's own
// min_full_refresh_interval_ms, the one figure here that belongs to the panel.
void slideshow_init(slideshow_t *s, const image_t *const *images, const char *const *names,
                    size_t count, const slideshow_config_t *cfg,
                    uint32_t panel_min_full_refresh_interval_ms);

// The interval actually used, after slideshow_init() raised it.
uint32_t slideshow_interval_ms(const slideshow_t *s);

// The point on the caller's clock at which the next frame is due: 0 until
// the first flush, so the first picture appears at once.
uint64_t slideshow_next_due_ms(const slideshow_t *s);

size_t slideshow_current(const slideshow_t *s);

// The current picture's name, or "" when there are no names or no pictures.
const char *slideshow_current_name(const slideshow_t *s);

// Draws the current picture into `fb`: the background, the picture centred
// on it (a larger one cropped around its centre), and the caption if asked
// for. With no pictures, only the background.
void slideshow_render(const slideshow_t *s, framebuffer_t *fb);

// Reports the flush of what slideshow_render() drew, called once the flush
// has returned. Either way the interval restarts from `now_ms`: a failed full
// refresh may still have driven the panel, so it is no reason to retry
// sooner. Only a successful one moves on to the next picture.
void slideshow_flushed(slideshow_t *s, uint64_t now_ms, bool ok);

// Bytes of pixel data across all pictures, for the startup log.
size_t slideshow_pixel_bytes(const slideshow_t *s);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_SLIDESHOW_SLIDESHOW_H
