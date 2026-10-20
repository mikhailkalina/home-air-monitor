// SPDX-License-Identifier: Apache-2.0
//
// What counts as a tap (core/app/tap.h), at the edges of its two limits and
// with the events port_input can deliver out of the ordinary: an up never
// preceded by a down, a down whose up was lost, a second finger.

#include <stdint.h>

#include "app/tap.h"
#include "hac_test.h"

static tap_t g_tap;
static tap_event_t g_out;

static bool feed(input_kind_t kind, uint8_t id, uint16_t x, uint16_t y, uint64_t ts)
{
    const input_event_t ev = {.kind = kind, .x = x, .y = y, .id = id, .ts_ms = ts};
    return tap_feed(&g_tap, &ev, &g_out);
}

static void setup(void)
{
    const tap_config_t cfg = tap_config_default();
    tap_init(&g_tap, &cfg);
}

static void a_quick_still_touch_is_a_tap_where_it_went_down(void)
{
    setup();
    HAC_CHECK(!feed(INPUT_TOUCH_DOWN, 0u, 480u, 270u, 1000u));
    HAC_CHECK(feed(INPUT_TOUCH_UP, 0u, 482u, 271u, 1120u));
    HAC_CHECK_EQ_INT(g_out.x, 480);
    HAC_CHECK_EQ_INT(g_out.y, 270);
    HAC_CHECK_EQ_U64(g_out.down_ms, 1000u);
    HAC_CHECK_EQ_U64(g_out.up_ms, 1120u);
    // And the next one is a tap of its own.
    HAC_CHECK(!feed(INPUT_TOUCH_DOWN, 0u, 10u, 10u, 2000u));
    HAC_CHECK(feed(INPUT_TOUCH_UP, 0u, 10u, 10u, 2050u));
}

static void the_duration_limit_is_inclusive(void)
{
    setup();
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 0u);
    HAC_CHECK(feed(INPUT_TOUCH_UP, 0u, 100u, 100u, TAP_DEFAULT_MAX_DURATION_MS));
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 10000u);
    HAC_CHECK(!feed(INPUT_TOUCH_UP, 0u, 100u, 100u, 10000u + TAP_DEFAULT_MAX_DURATION_MS + 1u));
}

static void the_travel_limit_holds_on_either_axis_and_through_moves(void)
{
    setup();
    const uint16_t edge = (uint16_t)(100u + TAP_DEFAULT_MAX_TRAVEL_PX);
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 0u);
    HAC_CHECK(feed(INPUT_TOUCH_UP, 0u, edge, 100u, 100u));

    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 1000u);
    HAC_CHECK(!feed(INPUT_TOUCH_UP, 0u, 100u, (uint16_t)(edge + 1u), 1100u));

    // Out and back: the move spoils it, though the up is where it began.
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 2000u);
    (void)feed(INPUT_TOUCH_MOVE, 0u, 300u, 100u, 2050u);
    HAC_CHECK(!feed(INPUT_TOUCH_UP, 0u, 100u, 100u, 2100u));

    // A move inside the limit does not.
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 3000u);
    (void)feed(INPUT_TOUCH_MOVE, 0u, 110u, 95u, 3050u);
    HAC_CHECK(feed(INPUT_TOUCH_UP, 0u, 110u, 95u, 3100u));
}

static void a_second_finger_spoils_it_either_way_round(void)
{
    setup();
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 0u);
    (void)feed(INPUT_TOUCH_DOWN, 1u, 600u, 300u, 50u);
    HAC_CHECK(!feed(INPUT_TOUCH_UP, 1u, 600u, 300u, 80u));
    HAC_CHECK(!feed(INPUT_TOUCH_UP, 0u, 100u, 100u, 100u));

    // A finger landing while another already rests on the glass.
    (void)feed(INPUT_TOUCH_DOWN, 1u, 600u, 300u, 1000u);
    (void)feed(INPUT_TOUCH_UP, 1u, 600u, 300u, 1050u);  // its own tap, alone: fine
    (void)feed(INPUT_TOUCH_DOWN, 1u, 600u, 300u, 2000u);
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 2010u);
    (void)feed(INPUT_TOUCH_UP, 1u, 600u, 300u, 2020u);
    HAC_CHECK(!feed(INPUT_TOUCH_UP, 0u, 100u, 100u, 2100u));

    // Both lifted: taps work again.
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 3000u);
    HAC_CHECK(feed(INPUT_TOUCH_UP, 0u, 100u, 100u, 3050u));
}

static void stray_events_start_over_rather_than_guess(void)
{
    setup();
    HAC_CHECK(!feed(INPUT_TOUCH_UP, 0u, 100u, 100u, 0u));    // its down never seen
    HAC_CHECK(!feed(INPUT_TOUCH_MOVE, 0u, 100u, 100u, 5u));  // nothing followed
    // A down whose up was lost, then another down of the same finger: the
    // second one starts the tap.
    (void)feed(INPUT_TOUCH_DOWN, 0u, 100u, 100u, 1000u);
    (void)feed(INPUT_TOUCH_DOWN, 0u, 700u, 400u, 5000u);
    HAC_CHECK(feed(INPUT_TOUCH_UP, 0u, 700u, 400u, 5100u));
    HAC_CHECK_EQ_INT(g_out.x, 700);
    // A button is not a touch.
    HAC_CHECK(!feed(INPUT_BUTTON, 0u, 0u, 0u, 6000u));
}

int main(void)
{
    HAC_RUN(a_quick_still_touch_is_a_tap_where_it_went_down);
    HAC_RUN(the_duration_limit_is_inclusive);
    HAC_RUN(the_travel_limit_holds_on_either_axis_and_through_moves);
    HAC_RUN(a_second_finger_spoils_it_either_way_round);
    HAC_RUN(stray_events_start_over_rather_than_guess);
    return hac_test_summary();
}
