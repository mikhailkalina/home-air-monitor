// SPDX-License-Identifier: Apache-2.0
//
// The emulated panel's own behaviour, shared by the windowed and the headless
// display adapters.
//
// The case that matters is the last one: a caller that keeps asking for
// partial refreshes past the budget gets a full refresh anyway, and it is
// recorded as forced. That is the simulator standing in for hardware the
// vendor says would be damaged, so it has to hold whether or not anyone is
// looking at a window.
//
// The timings are measured (epd_emulation.h). Two kinds of check hold them:
// the shape of the model -- partial refreshes cost by rows, not by area -- and
// the size of the lie, against the phase-2c measurements themselves, so an
// edit to the constants that drifts away from the glass fails here rather
// than going unnoticed.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "domain/measurement.h"
#include "epd_emulation.h"
#include "epd_model.h"
#include "hac_test.h"
#include "ui/framebuffer.h"
#include "ui/gfx.h"
#include "ui/screen_home.h"
#include "ui/view_model.h"

#define PANEL_W 960
#define PANEL_H 540
#define TEST_BUDGET 4u

static rect_t make_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    rect_t r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static void a_null_area_means_the_whole_panel(void)
{
    epd_model_t m;
    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    const rect_t all = epd_model_clip(&m, NULL);
    HAC_CHECK_EQ_INT(all.x, 0);
    HAC_CHECK_EQ_INT(all.y, 0);
    HAC_CHECK_EQ_INT(all.w, PANEL_W);
    HAC_CHECK_EQ_INT(all.h, PANEL_H);
}

static void an_oversized_area_is_clipped_and_an_offscreen_one_is_empty(void)
{
    epd_model_t m;
    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    const rect_t over = make_rect(900u, 500u, 400u, 400u);
    const rect_t clipped = epd_model_clip(&m, &over);
    HAC_CHECK_EQ_INT(clipped.w, PANEL_W - 900);
    HAC_CHECK_EQ_INT(clipped.h, PANEL_H - 500);

    const rect_t outside = make_rect(PANEL_W, 0u, 10u, 10u);
    const rect_t empty = epd_model_clip(&m, &outside);
    HAC_CHECK_EQ_INT(empty.w, 0);
    HAC_CHECK_EQ_INT(empty.h, 0);
}

static void a_partial_refresh_costs_by_rows_not_by_area(void)
{
    epd_model_t m;
    epd_flush_t narrow;
    epd_flush_t wide;
    epd_flush_t tall;

    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    // Same rows, four times the area: the same time. 960x228 and 240x228
    // measured 646.8 and 646.6 ms on the panel.
    const rect_t narrow_rect = make_rect(0u, 0u, 240u, 228u);
    const rect_t wide_rect = make_rect(0u, 0u, PANEL_W, 228u);
    epd_model_flush(&m, &narrow_rect, REFRESH_PARTIAL, NULL, NULL, &narrow);
    epd_model_flush(&m, &wide_rect, REFRESH_PARTIAL, NULL, NULL, &wide);
    HAC_CHECK_EQ_U64(narrow.duration_ms, wide.duration_ms);

    // More rows, same width: longer.
    const rect_t tall_rect = make_rect(0u, 0u, 240u, PANEL_H);
    epd_model_flush(&m, &tall_rect, REFRESH_PARTIAL, NULL, NULL, &tall);
    HAC_CHECK(tall.duration_ms > narrow.duration_ms);

    // Fast mono is the same shape at a sixth of the passes: cheaper per row.
    epd_flush_t mono;
    epd_model_flush(&m, &narrow_rect, REFRESH_FAST_MONO, NULL, NULL, &mono);
    HAC_CHECK(mono.duration_ms < narrow.duration_ms);
}

static void whole_panel_modes_ignore_the_area_asked_for(void)
{
    epd_model_t m;
    epd_flush_t f;

    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    const rect_t tenth = make_rect(0u, 0u, PANEL_W, PANEL_H / 10u);
    epd_model_flush(&m, &tenth, REFRESH_CLEAR, NULL, NULL, &f);
    HAC_CHECK_EQ_U64(f.duration_ms, EPD_SIM_CLEAR_REFRESH_MS);

    epd_model_flush(&m, &tenth, REFRESH_FULL, NULL, NULL, &f);
    HAC_CHECK_EQ_INT(f.area_percent, 100);
    HAC_CHECK_EQ_INT(f.area.h, PANEL_H);
    HAC_CHECK_EQ_U64(f.changed_rows, PANEL_H);
}

// --- rows the drive touches ------------------------------------------------

// One GRAY4 panel and one glass, both white.
static uint8_t s_pixels[(PANEL_W / 2u) * PANEL_H];
static uint8_t s_glass[PANEL_W * PANEL_H];

static framebuffer_t white_frame(void)
{
    framebuffer_t fb;
    framebuffer_init(&fb, s_pixels, sizeof(s_pixels), PANEL_W, PANEL_H, PIXFMT_GRAY4);
    framebuffer_clear(&fb, EPD_MODEL_GRAY4_WHITE);
    memset(s_glass, EPD_MODEL_GRAY4_WHITE, sizeof(s_glass));
    return fb;
}

// What the product's partial refreshes look like: two separate bands changing
// inside one bounding rectangle, with untouched rows between them. EPDiy skips
// the untouched rows, and so must the model -- on the panel, the product's
// 824 x 228 rectangle takes ~400 ms, not the 646 its height would say.
static void a_partial_refresh_counts_only_the_rows_that_changed(void)
{
    framebuffer_t fb = white_frame();
    gfx_fill_rect(&fb, 0, 10, 100, 10, 0u);   // rows 10-19
    gfx_fill_rect(&fb, 0, 200, 100, 10, 0u);  // rows 200-209

    epd_model_t m;
    epd_flush_t f;
    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    const rect_t r = make_rect(0u, 0u, 824u, 228u);
    epd_model_flush(&m, &r, REFRESH_PARTIAL, &fb, s_glass, &f);
    HAC_CHECK_EQ_U64(f.changed_rows, 20u);
    // EPDiy also writes the first two rows of each unchanged run: rows 0-1,
    // 20-21 and 210-211 here.
    HAC_CHECK_EQ_U64(f.written_rows, 26u);

    epd_flush_t every_row;
    epd_model_flush(&m, &r, REFRESH_PARTIAL, NULL, NULL, &every_row);
    HAC_CHECK_EQ_U64(every_row.changed_rows, 228u);
    HAC_CHECK(f.duration_ms < every_row.duration_ms);

    // A change outside the rectangle is not this flush's business: EPDiy
    // computes the difference over the rectangle, finds none and draws nothing.
    const rect_t below = make_rect(0u, 300u, 824u, 100u);
    epd_model_flush(&m, &below, REFRESH_PARTIAL, &fb, s_glass, &f);
    HAC_CHECK_EQ_U64(f.changed_rows, 0u);
    HAC_CHECK_EQ_U64(f.written_rows, 0u);
    HAC_CHECK(f.duration_ms < 25u);  // the difference pass over 100 rows only
}

// A full refresh is the clear plus GC16 over the rows that are not white; a
// white frame needs no GC16 pass at all.
static void a_full_refresh_costs_the_clear_plus_its_non_white_rows(void)
{
    framebuffer_t fb = white_frame();

    epd_model_t m;
    epd_flush_t f;
    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    epd_model_flush(&m, NULL, REFRESH_FULL, &fb, s_glass, &f);
    HAC_CHECK_EQ_U64(f.changed_rows, 0u);
    // The clear, and the whole panel's difference pass that finds nothing.
    HAC_CHECK(f.duration_ms >= EPD_SIM_CLEAR_REFRESH_MS);
    HAC_CHECK(f.duration_ms <= EPD_SIM_CLEAR_REFRESH_MS + 110u);

    // The glass plays no part: after the clear, what counts is "not white".
    gfx_fill_rect(&fb, 0, 100, 10, 30, 7u);
    memset(s_glass, 0u, sizeof(s_glass));
    epd_model_flush(&m, NULL, REFRESH_FULL, &fb, s_glass, &f);
    HAC_CHECK_EQ_U64(f.changed_rows, 30u);
    HAC_CHECK(f.duration_ms > EPD_SIM_CLEAR_REFRESH_MS + 110u);
}

// --- how far the model is from the glass -----------------------------------
//
// Means measured by apps/firmware_esp32/main/epd_calibration.c on 2026-10-13
// (docs/adr/0008-panel-timing.md): ten samples per case for the timing
// shapes, and the ghosting probe's own partial refreshes of the CO2 headline,
// whose rectangles are the product's. The model is held to 1 % on all of them.
typedef struct {
    refresh_mode_t mode;
    uint16_t w;
    uint16_t h;
    uint32_t measured_us;
} measured_case_t;

static const measured_case_t k_measured[] = {
    {REFRESH_PARTIAL, 960u, 540u, 1206117u},  {REFRESH_PARTIAL, 960u, 228u, 646803u},
    {REFRESH_PARTIAL, 240u, 228u, 646623u},   {REFRESH_PARTIAL, 240u, 60u, 345018u},
    {REFRESH_PARTIAL, 866u, 76u, 376080u},  // headline + level banner
    {REFRESH_PARTIAL, 458u, 55u, 338438u},  // headline, digit count changing
    {REFRESH_PARTIAL, 438u, 49u, 327600u},  // headline, the common case
    {REFRESH_FAST_MONO, 960u, 540u, 442912u}, {REFRESH_FAST_MONO, 240u, 228u, 211183u},
    {REFRESH_FAST_MONO, 240u, 60u, 86435u},   {REFRESH_CLEAR, 960u, 540u, 2455869u},
};

#define MODEL_TOLERANCE_PERCENT 1u

static bool within_percent(uint32_t model_ms, uint32_t measured_us, uint32_t percent)
{
    const uint64_t model_us = (uint64_t)model_ms * 1000u;
    const uint64_t diff =
        (model_us > measured_us) ? model_us - measured_us : (uint64_t)measured_us - model_us;
    return diff * 100u <= (uint64_t)measured_us * percent;
}

static void the_model_is_within_one_percent_of_every_measured_case(void)
{
    epd_model_t m;
    epd_model_init(&m, PANEL_W, PANEL_H, 1000u);  // budget out of the way

    for (size_t i = 0u; i < sizeof(k_measured) / sizeof(k_measured[0]); ++i) {
        const measured_case_t *c = &k_measured[i];
        const rect_t r = make_rect(0u, 0u, c->w, c->h);

        epd_flush_t f;
        epd_model_flush(&m, &r, c->mode, NULL, NULL, &f);
        HAC_CHECK(within_percent(f.duration_ms, c->measured_us, MODEL_TOLERANCE_PERCENT));
    }
}

// Full refreshes depend on content, so they are checked against content: a
// solid frame (every row non-white, as the measured black fill and grey ramp
// were), and screen_home rendered from the very reading the calibration run
// rendered (apps/firmware_esp32/main/epd_calibration.c, render_home() with
// CO2 600 ppm, 22 C, 45 %).
static void a_full_refresh_matches_the_panel_for_solid_content_and_screen_home(void)
{
    const uint32_t solid_fill_us = 3660569u;
    const uint32_t screen_home_us = 3324863u;
    const uint64_t now_ms = 1000u;

    epd_model_t m;
    epd_flush_t f;
    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    epd_model_flush(&m, NULL, REFRESH_FULL, NULL, NULL, &f);
    HAC_CHECK(within_percent(f.duration_ms, solid_fill_us, MODEL_TOLERANCE_PERCENT));

    framebuffer_t fb = white_frame();
    air_reading_t reading;
    air_reading_init(&reading);
    reading.co2_ppm = measurement_ok(600.0f, now_ms, 1u);
    reading.temperature_c = measurement_ok(22.0f, now_ms, 2u);
    reading.humidity_rh = measurement_ok(45.0f, now_ms, 2u);
    static view_model_t vm;
    view_model_build(&vm, &reading, now_ms);
    screen_home_render(&fb, &vm);

    epd_model_flush(&m, NULL, REFRESH_FULL, &fb, s_glass, &f);
    HAC_CHECK(within_percent(f.duration_ms, screen_home_us, MODEL_TOLERANCE_PERCENT));
    printf("screen_home full refresh: %u non-white rows, %u written, %u ms (panel: %u us)\n",
           f.changed_rows, f.written_rows, f.duration_ms, screen_home_us);
}

static void partial_refreshes_are_counted_and_a_full_one_resets_the_count(void)
{
    epd_model_t m;
    epd_flush_t f;

    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    for (uint32_t i = 1u; i <= TEST_BUDGET; ++i) {
        epd_model_flush(&m, NULL, REFRESH_PARTIAL, NULL, NULL, &f);
        HAC_CHECK_EQ_INT(f.performed, REFRESH_PARTIAL);
        HAC_CHECK(!f.forced_full);
        HAC_CHECK_EQ_U64(f.partials_since_full, i);
    }

    epd_model_flush(&m, NULL, REFRESH_FULL, NULL, NULL, &f);
    HAC_CHECK_EQ_U64(f.partials_since_full, 0u);
    HAC_CHECK(!f.forced_full);
    HAC_CHECK_EQ_U64(m.forced_full_count, 0u);
}

static void the_panel_forces_a_full_refresh_when_the_budget_is_spent(void)
{
    epd_model_t m;
    epd_flush_t f;

    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    for (uint32_t i = 0u; i < TEST_BUDGET; ++i) {
        epd_model_flush(&m, NULL, REFRESH_PARTIAL, NULL, NULL, &f);
    }

    // One partial refresh too many. The panel does not comply: it refreshes
    // fully and says so, which is the simulator refusing to model damage it
    // would not survive.
    epd_model_flush(&m, NULL, REFRESH_PARTIAL, NULL, NULL, &f);
    HAC_CHECK_EQ_INT(f.requested, REFRESH_PARTIAL);
    HAC_CHECK_EQ_INT(f.performed, REFRESH_FULL);
    HAC_CHECK(f.forced_full);
    HAC_CHECK_EQ_INT(f.area.h, PANEL_H);
    HAC_CHECK_EQ_U64(f.partials_since_full, 0u);
    HAC_CHECK_EQ_U64(m.forced_full_count, 1u);
}

static void a_panel_with_no_partial_mode_refreshes_fully_every_time(void)
{
    epd_model_t m;
    epd_flush_t f;

    epd_model_init(&m, PANEL_W, PANEL_H, /*max_partial_refreshes_before_full=*/0u);

    epd_model_flush(&m, NULL, REFRESH_PARTIAL, NULL, NULL, &f);
    HAC_CHECK_EQ_INT(f.performed, REFRESH_FULL);
    HAC_CHECK(f.forced_full);

    epd_model_flush(&m, NULL, REFRESH_FAST_MONO, NULL, NULL, &f);
    HAC_CHECK_EQ_INT(f.performed, REFRESH_FULL);
    HAC_CHECK(f.forced_full);
}

// --- what a full refresh shows, in turn -------------------------------
//
// The window plays a full refresh as the panel does: the clear's flashing,
// the difference pass, GC16's dark half, then the picture. The phases must
// add up to the refresh's own duration, so playing them takes exactly as long
// as the panel; and the dark half must be the waveform's share of the draw.

static void a_full_refresh_splits_into_phases_that_add_up(void)
{
    epd_model_t m;
    epd_flush_t f;
    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);
    framebuffer_t fb = white_frame();
    framebuffer_clear(&fb, 0u);  // solid black: every row written

    epd_model_flush(&m, NULL, REFRESH_FULL, &fb, s_glass, &f);
    const epd_full_phases_t p = epd_model_full_phases(&f);

    HAC_CHECK_EQ_U64((uint64_t)p.clear_ms + p.diff_ms + p.dark_ms + p.light_ms, f.duration_ms);
    HAC_CHECK_EQ_U64(p.clear_ms, EPD_SIM_CLEAR_REFRESH_MS);
    HAC_CHECK_EQ_U64(p.diff_ms, 106u);  // 540 rows x 196 us, as EPDiy logs it
    const uint32_t draw = p.dark_ms + p.light_ms;
    HAC_CHECK_EQ_U64(p.dark_ms, draw * EPD_SIM_GC16_DARK_PASSES / EPD_SIM_GC16_PASSES);
    HAC_CHECK(p.dark_ms > 0u && p.light_ms > 0u);
}

static void other_refreshes_have_no_dark_phase(void)
{
    epd_model_t m;
    epd_flush_t f;
    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    epd_model_flush(&m, NULL, REFRESH_CLEAR, NULL, NULL, &f);
    epd_full_phases_t p = epd_model_full_phases(&f);
    HAC_CHECK_EQ_U64(p.clear_ms, f.duration_ms);
    HAC_CHECK_EQ_U64((uint64_t)p.diff_ms + p.dark_ms + p.light_ms, 0u);

    const rect_t band = make_rect(0u, 0u, PANEL_W, 60u);
    epd_model_flush(&m, &band, REFRESH_PARTIAL, NULL, NULL, &f);
    p = epd_model_full_phases(&f);
    HAC_CHECK_EQ_U64(p.light_ms, f.duration_ms);
    HAC_CHECK_EQ_U64((uint64_t)p.clear_ms + p.diff_ms + p.dark_ms, 0u);
}

// GC16 after a clear drives the rows that hold a non-white pixel, and through
// EPDiy's global column mask, the columns that do: a pixel goes dark where
// both hold, white rows and white columns stay white.
static void gc16_drives_the_rows_and_columns_that_hold_ink(void)
{
    framebuffer_t fb = white_frame();
    framebuffer_set_pixel(&fb, 2, 1, 7u);
    framebuffer_set_pixel(&fb, 500, 300, 0u);

    static uint8_t rows[PANEL_H];
    static uint8_t cols[PANEL_W];
    const rect_t all = make_rect(0u, 0u, PANEL_W, PANEL_H);
    epd_model_gc16_masks(&fb, &all, rows, cols);

    size_t marked_rows = 0u;
    size_t marked_cols = 0u;
    for (size_t y = 0u; y < PANEL_H; ++y) {
        marked_rows += rows[y];
    }
    for (size_t x = 0u; x < PANEL_W; ++x) {
        marked_cols += cols[x];
    }
    HAC_CHECK_EQ_U64(marked_rows, 2u);
    HAC_CHECK_EQ_U64(marked_cols, 2u);
    HAC_CHECK(rows[1] && rows[300] && cols[2] && cols[500]);

    // Outside the area nothing is marked, even ink.
    const rect_t left = make_rect(0u, 0u, 100u, PANEL_H);
    epd_model_gc16_masks(&fb, &left, rows, cols);
    HAC_CHECK(rows[1] && cols[2]);
    HAC_CHECK(!rows[300] && !cols[500]);
}

// --- how light each level looks ---------------------------------------
//
// The window and the headless frames show a level as light as the glass does
// (EPD_SIM_LEVEL_LIGHTNESS_PERMILLE). The table must run from black to white
// without ever turning back, or a lighter level would show darker; and the
// screen grey must be the sRGB encoding of the measured light, which the
// level-7 check pins at a value worked out by hand (0.5 -> 187.5 -> 188).

static void the_lightness_table_runs_from_black_to_white_and_never_falls(void)
{
    static const uint16_t lightness[16] = EPD_SIM_LEVEL_LIGHTNESS_PERMILLE;

    HAC_CHECK_EQ_INT(lightness[0], 0);
    HAC_CHECK_EQ_INT(lightness[15], 1000);
    for (size_t level = 1u; level < 16u; ++level) {
        HAC_CHECK(lightness[level] >= lightness[level - 1u]);
    }
}

static void a_level_shows_as_the_screen_grey_of_its_measured_light(void)
{
    static const uint16_t lightness[16] = EPD_SIM_LEVEL_LIGHTNESS_PERMILLE;

    HAC_CHECK_EQ_INT(epd_level_display_grey(0u), 0);
    HAC_CHECK_EQ_INT(epd_level_display_grey(15u), 255);
    HAC_CHECK_EQ_INT(epd_level_display_grey(16u), 255);  // out of range counts as 15
    if (lightness[7] == 500u) {
        HAC_CHECK_EQ_INT(epd_level_display_grey(7u), 188);
    }
    for (uint8_t level = 1u; level < 16u; ++level) {
        HAC_CHECK(epd_level_display_grey(level) >= epd_level_display_grey((uint8_t)(level - 1u)));
    }
}

// Phase 2c saw levels 0 and 1 as one; the table says so, and the window must
// not show a difference the glass does not.
static void levels_the_panel_shows_alike_look_alike(void)
{
    static const uint16_t lightness[16] = EPD_SIM_LEVEL_LIGHTNESS_PERMILLE;

    for (uint8_t level = 1u; level < 16u; ++level) {
        if (lightness[level] == lightness[level - 1u]) {
            HAC_CHECK_EQ_INT(epd_level_display_grey(level),
                             epd_level_display_grey((uint8_t)(level - 1u)));
        }
    }
    HAC_CHECK_EQ_INT(lightness[1], lightness[0]);
}

int main(void)
{
    HAC_RUN(a_null_area_means_the_whole_panel);
    HAC_RUN(an_oversized_area_is_clipped_and_an_offscreen_one_is_empty);
    HAC_RUN(a_partial_refresh_costs_by_rows_not_by_area);
    HAC_RUN(whole_panel_modes_ignore_the_area_asked_for);
    HAC_RUN(a_partial_refresh_counts_only_the_rows_that_changed);
    HAC_RUN(a_full_refresh_costs_the_clear_plus_its_non_white_rows);
    HAC_RUN(the_model_is_within_one_percent_of_every_measured_case);
    HAC_RUN(a_full_refresh_matches_the_panel_for_solid_content_and_screen_home);
    HAC_RUN(partial_refreshes_are_counted_and_a_full_one_resets_the_count);
    HAC_RUN(the_panel_forces_a_full_refresh_when_the_budget_is_spent);
    HAC_RUN(a_panel_with_no_partial_mode_refreshes_fully_every_time);

    HAC_RUN(a_full_refresh_splits_into_phases_that_add_up);
    HAC_RUN(other_refreshes_have_no_dark_phase);
    HAC_RUN(gc16_drives_the_rows_and_columns_that_hold_ink);

    HAC_RUN(the_lightness_table_runs_from_black_to_white_and_never_falls);
    HAC_RUN(a_level_shows_as_the_screen_grey_of_its_measured_light);
    HAC_RUN(levels_the_panel_shows_alike_look_alike);
    return hac_test_summary();
}
