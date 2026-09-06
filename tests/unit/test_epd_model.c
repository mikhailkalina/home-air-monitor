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
// The timings asserted here are the unmeasured estimates in epd_emulation.h;
// the test pins the shape of the model (area-scaled, mode-dependent), not the
// truth of the numbers.

#include "epd_emulation.h"
#include "epd_model.h"
#include "hac_test.h"

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

static void the_duration_scales_with_the_area_within_the_mode(void)
{
    epd_model_t m;
    epd_flush_t f;

    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    // Whole panel: the top of the mode's quoted range.
    epd_model_flush(&m, NULL, REFRESH_FULL, &f);
    HAC_CHECK_EQ_U64(f.duration_ms, EPD_SIM_FULL_REFRESH_MAX_MS);
    HAC_CHECK_EQ_INT(f.area_percent, 100);

    // A tenth of the panel: near the bottom of the range.
    const rect_t tenth = make_rect(0u, 0u, PANEL_W, PANEL_H / 10u);
    epd_model_flush(&m, &tenth, REFRESH_PARTIAL, &f);
    HAC_CHECK(f.duration_ms >= EPD_SIM_PARTIAL_REFRESH_MIN_MS);
    HAC_CHECK(f.duration_ms <
              EPD_SIM_PARTIAL_REFRESH_MIN_MS +
                  (EPD_SIM_PARTIAL_REFRESH_MAX_MS - EPD_SIM_PARTIAL_REFRESH_MIN_MS) / 5u);

    // A clear cycle drives the panel through several inversions regardless of
    // the area asked for.
    epd_model_flush(&m, &tenth, REFRESH_CLEAR, &f);
    HAC_CHECK_EQ_U64(f.duration_ms, EPD_SIM_CLEAR_REFRESH_MS);
}

static void partial_refreshes_are_counted_and_a_full_one_resets_the_count(void)
{
    epd_model_t m;
    epd_flush_t f;

    epd_model_init(&m, PANEL_W, PANEL_H, TEST_BUDGET);

    for (uint32_t i = 1u; i <= TEST_BUDGET; ++i) {
        epd_model_flush(&m, NULL, REFRESH_PARTIAL, &f);
        HAC_CHECK_EQ_INT(f.performed, REFRESH_PARTIAL);
        HAC_CHECK(!f.forced_full);
        HAC_CHECK_EQ_U64(f.partials_since_full, i);
    }

    epd_model_flush(&m, NULL, REFRESH_FULL, &f);
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
        epd_model_flush(&m, NULL, REFRESH_PARTIAL, &f);
    }

    // One partial refresh too many. The panel does not comply: it refreshes
    // fully and says so, which is the simulator refusing to model damage it
    // would not survive.
    epd_model_flush(&m, NULL, REFRESH_PARTIAL, &f);
    HAC_CHECK_EQ_INT(f.requested, REFRESH_PARTIAL);
    HAC_CHECK_EQ_INT(f.performed, REFRESH_FULL);
    HAC_CHECK(f.forced_full);
    HAC_CHECK_EQ_U64(f.duration_ms, EPD_SIM_FULL_REFRESH_MAX_MS);
    HAC_CHECK_EQ_U64(f.partials_since_full, 0u);
    HAC_CHECK_EQ_U64(m.forced_full_count, 1u);
}

static void a_panel_with_no_partial_mode_refreshes_fully_every_time(void)
{
    epd_model_t m;
    epd_flush_t f;

    epd_model_init(&m, PANEL_W, PANEL_H, /*max_partial_refreshes_before_full=*/0u);

    epd_model_flush(&m, NULL, REFRESH_PARTIAL, &f);
    HAC_CHECK_EQ_INT(f.performed, REFRESH_FULL);
    HAC_CHECK(f.forced_full);

    epd_model_flush(&m, NULL, REFRESH_FAST_MONO, &f);
    HAC_CHECK_EQ_INT(f.performed, REFRESH_FULL);
    HAC_CHECK(f.forced_full);
}

int main(void)
{
    HAC_RUN(a_null_area_means_the_whole_panel);
    HAC_RUN(an_oversized_area_is_clipped_and_an_offscreen_one_is_empty);
    HAC_RUN(the_duration_scales_with_the_area_within_the_mode);
    HAC_RUN(partial_refreshes_are_counted_and_a_full_one_resets_the_count);
    HAC_RUN(the_panel_forces_a_full_refresh_when_the_budget_is_spent);
    HAC_RUN(a_panel_with_no_partial_mode_refreshes_fully_every_time);
    return hac_test_summary();
}
