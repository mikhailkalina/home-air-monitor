// SPDX-License-Identifier: Apache-2.0
//
// Snapshots to events, and the controller's frame to the panel's
// (drivers/gt911/gt911_touch.h), at the edges: a lift and a new finger in
// one report, track IDs reused, a report lost, a third finger, the move
// threshold, and the corners of the mapping.

#include <stdint.h>
#include <string.h>

#include "gt911_touch.h"
#include "hac_test.h"

#define THRESHOLD 8u
#define TS 1234u

static gt911_touch_t g_t;
static input_event_t g_ev[GT911_TOUCH_MAX_EVENTS];

static size_t snap(const gt911_touch_point_t *pts, size_t n)
{
    memset(g_ev, 0xA5, sizeof(g_ev));
    return gt911_touch_update(&g_t, pts, n, THRESHOLD, TS, g_ev, GT911_TOUCH_MAX_EVENTS);
}

static gt911_touch_point_t pt(uint8_t id, uint16_t x, uint16_t y)
{
    const gt911_touch_point_t p = {.track_id = id, .x = x, .y = y};
    return p;
}

static bool is(const input_event_t *e, input_kind_t kind, uint8_t id, uint16_t x, uint16_t y)
{
    return e->kind == kind && e->id == id && e->x == x && e->y == y && e->ts_ms == TS;
}

static void a_finger_down_and_up(void)
{
    gt911_touch_init(&g_t);
    const gt911_touch_point_t p = pt(5u, 100u, 200u);
    HAC_CHECK_EQ_INT((int)snap(&p, 1u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_DOWN, 0u, 100u, 200u));
    HAC_CHECK(gt911_touch_any_down(&g_t));

    // The same report again: nothing new.
    HAC_CHECK_EQ_INT((int)snap(&p, 1u), 0);

    HAC_CHECK_EQ_INT((int)snap(NULL, 0u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_UP, 0u, 100u, 200u));
    HAC_CHECK(!gt911_touch_any_down(&g_t));
    HAC_CHECK_EQ_INT((int)snap(NULL, 0u), 0);
}

static void a_move_only_past_the_threshold_from_the_last_report(void)
{
    gt911_touch_init(&g_t);
    gt911_touch_point_t p = pt(1u, 100u, 100u);
    (void)snap(&p, 1u);

    // Creeping a pixel at a time: no move until it is more than the threshold
    // away from where it was last reported -- not from the last snapshot.
    for (uint16_t dx = 1u; dx <= THRESHOLD; ++dx) {
        p.x = (uint16_t)(100u + dx);
        HAC_CHECK_EQ_INT((int)snap(&p, 1u), 0);
    }
    p.x = 100u + THRESHOLD + 1u;
    HAC_CHECK_EQ_INT((int)snap(&p, 1u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_MOVE, 0u, 109u, 100u));

    // On the other axis, and backwards.
    p.y = 100u - THRESHOLD - 1u;
    HAC_CHECK_EQ_INT((int)snap(&p, 1u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_MOVE, 0u, 109u, 91u));

    // The lift is where it was last reported, not where the creep had got to.
    p.x = 112u;
    HAC_CHECK_EQ_INT((int)snap(&p, 1u), 0);
    HAC_CHECK_EQ_INT((int)snap(NULL, 0u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_UP, 0u, 109u, 91u));
}

static void two_fingers_take_places_zero_and_one(void)
{
    gt911_touch_init(&g_t);
    const gt911_touch_point_t first = pt(3u, 10u, 10u);
    HAC_CHECK_EQ_INT((int)snap(&first, 1u), 1);
    const gt911_touch_point_t both[2] = {pt(3u, 10u, 10u), pt(4u, 500u, 300u)};
    HAC_CHECK_EQ_INT((int)snap(both, 2u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_DOWN, 1u, 500u, 300u));

    // The first lifts; the second keeps its place.
    const gt911_touch_point_t second = pt(4u, 500u, 300u);
    HAC_CHECK_EQ_INT((int)snap(&second, 1u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_UP, 0u, 10u, 10u));
    // A new finger takes the free place 0.
    const gt911_touch_point_t again[2] = {pt(4u, 500u, 300u), pt(6u, 50u, 60u)};
    HAC_CHECK_EQ_INT((int)snap(again, 2u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_DOWN, 0u, 50u, 60u));
}

static void a_lift_and_a_new_finger_in_one_report_frees_the_place_first(void)
{
    gt911_touch_init(&g_t);
    const gt911_touch_point_t both[2] = {pt(0u, 10u, 10u), pt(1u, 20u, 20u)};
    (void)snap(both, 2u);
    // Track 0 lifts and track 2 lands, in one report: UP first, then DOWN,
    // and the new finger takes the place the lift freed.
    const gt911_touch_point_t next[2] = {pt(1u, 20u, 20u), pt(2u, 700u, 400u)};
    HAC_CHECK_EQ_INT((int)snap(next, 2u), 2);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_UP, 0u, 10u, 10u));
    HAC_CHECK(is(&g_ev[1], INPUT_TOUCH_DOWN, 0u, 700u, 400u));
}

static void a_track_id_reused_after_a_lift_is_a_new_finger(void)
{
    gt911_touch_init(&g_t);
    const gt911_touch_point_t a = pt(0u, 10u, 10u);
    (void)snap(&a, 1u);
    (void)snap(NULL, 0u);
    const gt911_touch_point_t b = pt(0u, 800u, 500u);
    HAC_CHECK_EQ_INT((int)snap(&b, 1u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_DOWN, 0u, 800u, 500u));
}

static void a_lost_lift_report_with_the_track_id_reused_reads_as_a_move(void)
{
    // The known limit (gt911_touch.h): the report of the lift never seen,
    // the next finger given the same track ID. Nothing tells them apart.
    gt911_touch_init(&g_t);
    const gt911_touch_point_t a = pt(0u, 10u, 10u);
    (void)snap(&a, 1u);
    const gt911_touch_point_t b = pt(0u, 800u, 500u);
    HAC_CHECK_EQ_INT((int)snap(&b, 1u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_MOVE, 0u, 800u, 500u));
}

static void lost_reports_in_the_middle_leave_down_and_up_balanced(void)
{
    gt911_touch_init(&g_t);
    // The down report itself lost: the first one seen is mid-drag.
    const gt911_touch_point_t mid = pt(2u, 300u, 300u);
    HAC_CHECK_EQ_INT((int)snap(&mid, 1u), 1);
    HAC_CHECK_EQ_INT(g_ev[0].kind, INPUT_TOUCH_DOWN);
    // Reports lost, the finger far away at the next one.
    const gt911_touch_point_t far = pt(2u, 600u, 100u);
    HAC_CHECK_EQ_INT((int)snap(&far, 1u), 1);
    HAC_CHECK_EQ_INT(g_ev[0].kind, INPUT_TOUCH_MOVE);
    HAC_CHECK_EQ_INT((int)snap(NULL, 0u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_UP, 0u, 600u, 100u));
}

static void a_third_finger_is_never_seen_even_when_a_place_frees(void)
{
    gt911_touch_init(&g_t);
    const gt911_touch_point_t three[3] = {pt(0u, 1u, 1u), pt(1u, 2u, 2u), pt(2u, 3u, 3u)};
    HAC_CHECK_EQ_INT((int)snap(three, 3u), 2);
    HAC_CHECK_EQ_INT(g_ev[0].id, 0);
    HAC_CHECK_EQ_INT(g_ev[1].id, 1);

    // Track 0 lifts; track 2 is still down but must not arrive mid-gesture.
    const gt911_touch_point_t two[2] = {pt(1u, 2u, 2u), pt(2u, 3u, 3u)};
    HAC_CHECK_EQ_INT((int)snap(two, 2u), 1);
    HAC_CHECK_EQ_INT(g_ev[0].kind, INPUT_TOUCH_UP);

    // Once it lifts and lands again, it is a finger like any other.
    const gt911_touch_point_t one = pt(1u, 2u, 2u);
    HAC_CHECK_EQ_INT((int)snap(&one, 1u), 0);
    const gt911_touch_point_t back[2] = {pt(1u, 2u, 2u), pt(2u, 9u, 9u)};
    HAC_CHECK_EQ_INT((int)snap(back, 2u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_DOWN, 0u, 9u, 9u));
}

static void a_track_id_twice_in_one_report_counts_once(void)
{
    gt911_touch_init(&g_t);
    const gt911_touch_point_t dup[2] = {pt(3u, 10u, 10u), pt(3u, 900u, 500u)};
    HAC_CHECK_EQ_INT((int)snap(dup, 2u), 1);
    HAC_CHECK(is(&g_ev[0], INPUT_TOUCH_DOWN, 0u, 10u, 10u));
}

static void events_never_run_past_the_buffer(void)
{
    gt911_touch_init(&g_t);
    const gt911_touch_point_t both[2] = {pt(0u, 1u, 1u), pt(1u, 2u, 2u)};
    input_event_t one[1];
    HAC_CHECK_EQ_INT((int)gt911_touch_update(&g_t, both, 2u, THRESHOLD, TS, one, 1u), 1);
}

static void map(const gt911_touch_map_t *m, uint16_t x_max, uint16_t y_max, uint16_t x, uint16_t y,
                uint16_t ex, uint16_t ey)
{
    uint16_t px = 0u;
    uint16_t py = 0u;
    gt911_touch_map_point(m, x_max, y_max, x, y, &px, &py);
    HAC_CHECK_EQ_INT(px, ex);
    HAC_CHECK_EQ_INT(py, ey);
}

static void the_mapping_at_the_corners(void)
{
    gt911_touch_map_t m = {.panel_width = 960u, .panel_height = 540u};
    // Identity: a controller already in the panel's frame.
    map(&m, 960u, 540u, 0u, 0u, 0u, 0u);
    map(&m, 960u, 540u, 959u, 539u, 959u, 539u);
    map(&m, 960u, 540u, 960u, 540u, 959u, 539u);  // its maximum itself clamps inside

    // The vendor's choice for this board: swapped, Y mirrored, from a
    // portrait 540 x 960 frame.
    m.swap_xy = true;
    m.mirror_y = true;
    map(&m, 540u, 960u, 0u, 0u, 0u, 539u);
    map(&m, 540u, 960u, 539u, 959u, 959u, 0u);
    map(&m, 540u, 960u, 0u, 959u, 959u, 539u);
    map(&m, 540u, 960u, 539u, 0u, 0u, 0u);

    // Mirrored in X alone.
    m.swap_xy = false;
    m.mirror_y = false;
    m.mirror_x = true;
    map(&m, 960u, 540u, 0u, 100u, 959u, 100u);

    // Scaled: a controller at half the resolution.
    m.mirror_x = false;
    map(&m, 480u, 270u, 240u, 135u, 480u, 270u);
    // Unknown maximum: the panel's own size.
    map(&m, 0u, 0u, 2000u, 30u, 959u, 30u);
    HAC_CHECK(strcmp(gt911_touch_kind_name(INPUT_TOUCH_MOVE), "move") == 0);
}

int main(void)
{
    HAC_RUN(a_finger_down_and_up);
    HAC_RUN(a_move_only_past_the_threshold_from_the_last_report);
    HAC_RUN(two_fingers_take_places_zero_and_one);
    HAC_RUN(a_lift_and_a_new_finger_in_one_report_frees_the_place_first);
    HAC_RUN(a_track_id_reused_after_a_lift_is_a_new_finger);
    HAC_RUN(a_lost_lift_report_with_the_track_id_reused_reads_as_a_move);
    HAC_RUN(lost_reports_in_the_middle_leave_down_and_up_balanced);
    HAC_RUN(a_third_finger_is_never_seen_even_when_a_place_frees);
    HAC_RUN(a_track_id_twice_in_one_report_counts_once);
    HAC_RUN(events_never_run_past_the_buffer);
    HAC_RUN(the_mapping_at_the_corners);
    return hac_test_summary();
}
