// SPDX-License-Identifier: Apache-2.0
//
// apps/slideshow/slideshow.c, compiled from where both of the slideshow's
// compositions compile it. What matters is the schedule -- it must never
// drive the panel faster than the panel's own minimum full-refresh interval
// -- and where a picture lands on the panel.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hac_test.h"
#include "slideshow.h"
#include "ui/framebuffer.h"
#include "ui/image.h"

#define MINUTE_MS 60000u
#define BACKGROUND 15u

// Three 2x2 pictures, each one flat level: 3, 6 and 9.
static const uint8_t k_pixels_a[2] = {0x33, 0x33};
static const uint8_t k_pixels_b[2] = {0x66, 0x66};
static const uint8_t k_pixels_c[2] = {0x99, 0x99};
static const image_t k_a = {
    .pixels = k_pixels_a, .width = 2, .height = 2, .stride = 1, .format = PIXFMT_GRAY4};
static const image_t k_b = {
    .pixels = k_pixels_b, .width = 2, .height = 2, .stride = 1, .format = PIXFMT_GRAY4};
static const image_t k_c = {
    .pixels = k_pixels_c, .width = 2, .height = 2, .stride = 1, .format = PIXFMT_GRAY4};
static const image_t *const k_images[3] = {&k_a, &k_b, &k_c};
static const char *const k_names[3] = {"a", "b", "c"};

static void make_show(slideshow_t *s, uint32_t interval_ms, uint32_t panel_min_ms)
{
    const slideshow_config_t cfg = {
        .interval_ms = interval_ms,
        .background = BACKGROUND,
        .caption = false,
    };
    slideshow_init(s, k_images, k_names, 3u, &cfg, panel_min_ms);
}

// --- schedule ----------------------------------------------------------

static void the_first_picture_is_due_at_once(void)
{
    slideshow_t s;
    make_show(&s, MINUTE_MS, MINUTE_MS);

    HAC_CHECK_EQ_U64(slideshow_next_due_ms(&s), 0u);
    HAC_CHECK_EQ_U64(slideshow_current(&s), 0u);
}

// From the end of the refresh, as update_policy measures the panel's
// minimum: a refresh that ended at 3.3 s makes the next one due at 63.3 s.
static void the_interval_runs_from_the_end_of_the_last_refresh(void)
{
    slideshow_t s;
    make_show(&s, MINUTE_MS, MINUTE_MS);

    slideshow_flushed(&s, 3300u, true);

    HAC_CHECK_EQ_U64(slideshow_next_due_ms(&s), 63300u);
}

static void an_interval_shorter_than_the_panel_allows_is_raised_to_it(void)
{
    slideshow_t s;
    make_show(&s, 1000u, MINUTE_MS);

    HAC_CHECK_EQ_U64(slideshow_interval_ms(&s), MINUTE_MS);
    slideshow_flushed(&s, 0u, true);
    HAC_CHECK_EQ_U64(slideshow_next_due_ms(&s), MINUTE_MS);
}

static void a_longer_interval_is_kept(void)
{
    slideshow_t s;
    make_show(&s, 5u * MINUTE_MS, MINUTE_MS);

    HAC_CHECK_EQ_U64(slideshow_interval_ms(&s), 5u * MINUTE_MS);
}

static void a_panel_with_no_minimum_keeps_any_interval(void)
{
    slideshow_t s;
    make_show(&s, 1000u, 0u);

    HAC_CHECK_EQ_U64(slideshow_interval_ms(&s), 1000u);
}

static void pictures_advance_in_order_and_wrap(void)
{
    slideshow_t s;
    make_show(&s, MINUTE_MS, MINUTE_MS);

    const size_t expected[] = {0u, 1u, 2u, 0u, 1u};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        HAC_CHECK_EQ_U64(slideshow_current(&s), expected[i]);
        HAC_CHECK(strcmp(slideshow_current_name(&s), k_names[expected[i]]) == 0);
        slideshow_flushed(&s, (uint64_t)i * MINUTE_MS, true);
    }
}

// A failed full refresh may still have driven the glass: the same picture
// is tried again, but only after a whole interval, never at once.
static void a_failed_refresh_keeps_the_picture_and_still_waits(void)
{
    slideshow_t s;
    make_show(&s, MINUTE_MS, MINUTE_MS);

    slideshow_flushed(&s, 4000u, false);

    HAC_CHECK_EQ_U64(slideshow_current(&s), 0u);
    HAC_CHECK_EQ_U64(slideshow_next_due_ms(&s), 4000u + MINUTE_MS);
}

static void pixel_bytes_add_up_every_picture(void)
{
    slideshow_t s;
    make_show(&s, MINUTE_MS, MINUTE_MS);

    HAC_CHECK_EQ_U64(slideshow_pixel_bytes(&s), 6u);  // three pictures of 1 byte x 2 rows
}

// --- rendering ---------------------------------------------------------

static void a_smaller_picture_is_centred_on_the_background(void)
{
    slideshow_t s;
    make_show(&s, MINUTE_MS, MINUTE_MS);

    uint8_t storage[3 * 4];  // 6x4 GRAY4
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 6, 4, PIXFMT_GRAY4);
    slideshow_render(&s, &fb);

    for (int32_t y = 0; y < 4; ++y) {
        for (int32_t x = 0; x < 6; ++x) {
            const bool inside = x >= 2 && x < 4 && y >= 1 && y < 3;
            HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, y), inside ? 3 : (int)BACKGROUND);
        }
    }
}

static void a_larger_picture_is_cropped_around_its_centre(void)
{
    // 4x1, levels 1 2 3 4, shown on a 2x1 panel: the middle two survive.
    static const uint8_t pixels[2] = {0x21, 0x43};
    static const image_t wide = {
        .pixels = pixels, .width = 4, .height = 1, .stride = 2, .format = PIXFMT_GRAY4};
    static const image_t *const images[1] = {&wide};
    const slideshow_config_t cfg = {.interval_ms = MINUTE_MS, .background = 0, .caption = false};
    slideshow_t s;
    slideshow_init(&s, images, NULL, 1u, &cfg, MINUTE_MS);

    uint8_t storage[1];
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 2, 1, PIXFMT_GRAY4);
    slideshow_render(&s, &fb);

    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 0), 2);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 1, 0), 3);
    HAC_CHECK(strcmp(slideshow_current_name(&s), "") == 0);  // no names given
}

// The caption goes in the bottom-left corner, in a box with a black border,
// and leaves the rest of the picture alone.
static void the_caption_is_boxed_in_the_bottom_left_corner(void)
{
    static const uint8_t pixels[60 * 20];  // a 120x20 picture of level 0
    static const image_t dark = {
        .pixels = pixels, .width = 120, .height = 20, .stride = 60, .format = PIXFMT_GRAY4};
    static const image_t *const images[1] = {&dark};
    static const char *const names[1] = {"dark"};
    const slideshow_config_t cfg = {.interval_ms = MINUTE_MS, .background = 0, .caption = true};
    slideshow_t s;
    slideshow_init(&s, images, names, 1u, &cfg, MINUTE_MS);

    static uint8_t storage[100 * 60];  // 200x60 GRAY4
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 200, 60, PIXFMT_GRAY4);
    slideshow_render(&s, &fb);

    // Somewhere in the lower left there is white paper; nothing in the top
    // right is anything but the (black) picture and background.
    bool paper = false;
    for (int32_t y = 30; y < 60; ++y) {
        for (int32_t x = 0; x < 100; ++x) {
            paper = paper || framebuffer_get_pixel(&fb, x, y) == 15u;
        }
    }
    HAC_CHECK(paper);

    bool untouched = true;
    for (int32_t y = 0; y < 20; ++y) {
        for (int32_t x = 120; x < 200; ++x) {
            untouched = untouched && framebuffer_get_pixel(&fb, x, y) == 0u;
        }
    }
    HAC_CHECK(untouched);

    // The box's border sits on the 8 px margin.
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 8, 60 - 8 - 1), 0);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 9, 60 - 8 - 2), 15);
}

static void no_pictures_draws_the_background_and_nothing_else(void)
{
    const slideshow_config_t cfg = {.interval_ms = MINUTE_MS, .background = 7, .caption = true};
    slideshow_t s;
    slideshow_init(&s, NULL, NULL, 0u, &cfg, MINUTE_MS);

    uint8_t storage[2];
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 2, 2, PIXFMT_GRAY4);
    slideshow_render(&s, &fb);
    slideshow_flushed(&s, 0u, true);

    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 1, 1), 7);
    HAC_CHECK_EQ_U64(slideshow_current(&s), 0u);
    HAC_CHECK(strcmp(slideshow_current_name(&s), "") == 0);
    HAC_CHECK_EQ_U64(slideshow_pixel_bytes(&s), 0u);
}

int main(void)
{
    HAC_RUN(the_first_picture_is_due_at_once);
    HAC_RUN(the_interval_runs_from_the_end_of_the_last_refresh);
    HAC_RUN(an_interval_shorter_than_the_panel_allows_is_raised_to_it);
    HAC_RUN(a_longer_interval_is_kept);
    HAC_RUN(a_panel_with_no_minimum_keeps_any_interval);
    HAC_RUN(pictures_advance_in_order_and_wrap);
    HAC_RUN(a_failed_refresh_keeps_the_picture_and_still_waits);
    HAC_RUN(pixel_bytes_add_up_every_picture);

    HAC_RUN(a_smaller_picture_is_centred_on_the_background);
    HAC_RUN(a_larger_picture_is_cropped_around_its_centre);
    HAC_RUN(the_caption_is_boxed_in_the_bottom_left_corner);
    HAC_RUN(no_pictures_draws_the_background_and_nothing_else);

    return hac_test_summary();
}
