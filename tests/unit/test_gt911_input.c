// SPDX-License-Identifier: Apache-2.0
//
// port_input over the GT911 (drivers/gt911/gt911_input.h) against the model:
// an interrupt and a service() make events in panel coordinates, stamped
// with the time of the read; a finger is read while down even without an
// interrupt, and lifted when its reports stop; the queue holds what the
// consumer has not taken, and counts what it could not; sleep lifts what
// was down.

#include <stdint.h>
#include <string.h>

#include "gt911_input.h"
#include "gt911_rig.h"
#include "hac_test.h"

static gt911_rig_t g_rig;

static port_input_t *setup(void)
{
    gt911_rig_init(&g_rig, GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT(gt911_rig_input_setup(&g_rig), HAL_OK);
    HAC_CHECK_EQ_INT(gt911_input_init(&g_rig.input), HAL_OK);
    return gt911_input_port(&g_rig.input);
}

static void touch(uint8_t id, uint16_t x, uint16_t y)
{
    const gt911_model_point_t p = gt911_rig_point(id, x, y);
    gt911_model_touch(&g_rig.model, &p, 1u);
}

static size_t drain(port_input_t *in, input_event_t *out, size_t cap)
{
    size_t n = 0u;
    input_event_t ev;
    while (in->poll(in, &ev)) {
        if (n < cap) {
            out[n] = ev;
        }
        ++n;
    }
    return n;
}

static void nothing_until_init_and_nothing_while_untouched(void)
{
    gt911_rig_init(&g_rig, GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT(gt911_rig_input_setup(&g_rig), HAL_OK);
    port_input_t *in = gt911_input_port(&g_rig.input);
    input_event_t ev;
    HAC_CHECK(!in->poll(in, &ev));
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), GT911_INPUT_NO_DEADLINE);
    HAC_CHECK_EQ_INT(gt911_input_init(&g_rig.input), HAL_OK);
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), GT911_INPUT_NO_DEADLINE);
    HAC_CHECK(!in->poll(in, &ev));
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->reads, 0);
}

static void a_tap_is_a_down_and_an_up_in_panel_coordinates(void)
{
    port_input_t *in = setup();
    // The model's portrait frame: (100, 700) is, through the vendor's swap
    // and Y mirror, the panel's (700, 539 - 100).
    touch(0u, 100u, 700u);
    HAC_CHECK(g_rig.input.signalled);
    const uint64_t deadline = gt911_input_service(&g_rig.input);
    const uint64_t read_at = g_rig.clock.now_ms;
    HAC_CHECK(deadline != GT911_INPUT_NO_DEADLINE);  // a finger is down: it wants to read again

    input_event_t ev[4];
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_DOWN);
    HAC_CHECK_EQ_INT(ev[0].x, 700);
    HAC_CHECK_EQ_INT(ev[0].y, 439);
    HAC_CHECK_EQ_INT(ev[0].id, 0);
    HAC_CHECK_EQ_U64(ev[0].ts_ms, read_at);

    adp_clock_virtual_advance(&g_rig.clock, 80u);
    gt911_model_touch(&g_rig.model, NULL, 0u);
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), GT911_INPUT_NO_DEADLINE);
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_UP);
    HAC_CHECK_EQ_INT(ev[0].x, 700);
    HAC_CHECK_EQ_INT(ev[0].y, 439);
    HAC_CHECK_EQ_U64(ev[0].ts_ms, g_rig.clock.now_ms);

    // What the console shows: raw and mapped, oldest first.
    gt911_input_record_t hist[GT911_INPUT_HISTORY];
    HAC_CHECK_EQ_INT((int)gt911_input_history(&g_rig.input, hist, GT911_INPUT_HISTORY), 2);
    HAC_CHECK_EQ_INT(hist[0].kind, INPUT_TOUCH_DOWN);
    HAC_CHECK_EQ_INT(hist[0].raw_x, 100);
    HAC_CHECK_EQ_INT(hist[0].raw_y, 700);
    HAC_CHECK_EQ_INT(hist[1].kind, INPUT_TOUCH_UP);
    HAC_CHECK_EQ_INT(hist[1].raw_x, 100);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.config_writes, 0);
}

static void the_four_corners_land_on_the_panels_corners(void)
{
    port_input_t *in = setup();
    const uint16_t raw[4][2] = {{0u, 0u}, {539u, 0u}, {0u, 959u}, {539u, 959u}};
    const uint16_t panel[4][2] = {{0u, 539u}, {0u, 0u}, {959u, 539u}, {959u, 0u}};
    for (size_t i = 0u; i < 4u; ++i) {
        touch(1u, raw[i][0], raw[i][1]);
        (void)gt911_input_service(&g_rig.input);
        gt911_model_touch(&g_rig.model, NULL, 0u);
        (void)gt911_input_service(&g_rig.input);
        input_event_t ev[4];
        HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 2);
        HAC_CHECK_EQ_INT(ev[0].x, panel[i][0]);
        HAC_CHECK_EQ_INT(ev[0].y, panel[i][1]);
    }
}

static void a_finger_is_read_while_down_without_an_interrupt(void)
{
    port_input_t *in = setup();
    touch(0u, 100u, 100u);
    const uint64_t due = gt911_input_service(&g_rig.input);
    input_event_t ev[4];
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);

    // The interrupt no longer reaches it -- an edge missed -- while the
    // finger drags.
    g_rig.input_set_up = false;
    touch(0u, 100u, 300u);
    HAC_CHECK(!g_rig.input.signalled);
    HAC_CHECK_EQ_U64(due, g_rig.clock.now_ms + GT911_INPUT_POLL_WHILE_DOWN_MS);
    adp_clock_virtual_advance(&g_rig.clock, GT911_INPUT_POLL_WHILE_DOWN_MS);
    (void)gt911_input_service(&g_rig.input);
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_MOVE);
}

static void a_finger_whose_reports_stop_is_lifted(void)
{
    port_input_t *in = setup();
    touch(0u, 100u, 100u);
    (void)gt911_input_service(&g_rig.input);
    input_event_t ev[4];
    (void)drain(in, ev, 4u);

    // The lift's report lost: nothing more comes.
    gt911_model_drop_reports(&g_rig.model, 1u);
    gt911_model_touch(&g_rig.model, NULL, 0u);
    uint64_t due = gt911_input_service(&g_rig.input);
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 0);
    while (due != GT911_INPUT_NO_DEADLINE && g_rig.clock.now_ms < 10000u) {
        adp_clock_virtual_advance(&g_rig.clock, due - g_rig.clock.now_ms);
        due = gt911_input_service(&g_rig.input);
    }
    HAC_CHECK_EQ_U64(due, GT911_INPUT_NO_DEADLINE);
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_UP);
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->timed_out_lifts, 1);
}

static void a_finger_held_still_is_neither_lifted_nor_moved(void)
{
    port_input_t *in = setup();
    touch(0u, 100u, 100u);
    (void)gt911_input_service(&g_rig.input);
    input_event_t ev[4];
    (void)drain(in, ev, 4u);
    // Two seconds of scan cycles, a report every 10 ms.
    for (int i = 0; i < 200; ++i) {
        adp_clock_virtual_advance(&g_rig.clock, 10u);
        gt911_model_scan(&g_rig.model);
        (void)gt911_input_service(&g_rig.input);
    }
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 0);
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->timed_out_lifts, 0);
    HAC_CHECK(gt911_input_stats(&g_rig.input)->reports > 150u);
}

static void an_interrupt_with_nothing_behind_it_makes_no_event(void)
{
    port_input_t *in = setup();
    gt911_model_spurious_interrupt(&g_rig.model);
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), GT911_INPUT_NO_DEADLINE);
    input_event_t ev;
    HAC_CHECK(!in->poll(in, &ev));
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->empty_reads, 1);
}

static void a_full_queue_drops_the_newest_and_counts_them(void)
{
    port_input_t *in = setup();
    // A finger dragged while nobody takes the events: one move per report.
    touch(0u, 0u, 0u);
    (void)gt911_input_service(&g_rig.input);
    for (uint16_t i = 1u; i <= GT911_INPUT_QUEUE_LEN + 5u; ++i) {
        adp_clock_virtual_advance(&g_rig.clock, 10u);
        touch(0u, 0u, (uint16_t)(i * (GT911_INPUT_MOVE_THRESHOLD_PX + 1u)));
        (void)gt911_input_service(&g_rig.input);
    }
    input_event_t ev[GT911_INPUT_QUEUE_LEN + 8u];
    HAC_CHECK_EQ_INT((int)drain(in, ev, GT911_INPUT_QUEUE_LEN + 8u), (int)GT911_INPUT_QUEUE_LEN);
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->dropped, 6);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_DOWN);  // the oldest kept, in order
    for (size_t i = 1u; i < GT911_INPUT_QUEUE_LEN; ++i) {
        HAC_CHECK(ev[i].ts_ms >= ev[i - 1u].ts_ms);
    }
}

static void sleep_lifts_what_was_down_and_wake_reads_again(void)
{
    port_input_t *in = setup();
    touch(0u, 100u, 100u);
    (void)gt911_input_service(&g_rig.input);
    input_event_t ev[4];
    (void)drain(in, ev, 4u);

    HAC_CHECK_EQ_INT(gt911_input_sleep(&g_rig.input), HAL_OK);
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_UP);
    HAC_CHECK(g_rig.model.asleep);
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), GT911_INPUT_NO_DEADLINE);

    HAC_CHECK_EQ_INT(gt911_input_wake(&g_rig.input), HAL_OK);
    touch(0u, 200u, 200u);
    (void)gt911_input_service(&g_rig.input);
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_DOWN);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.config_writes, 0);
}

// A read that fails -- here the bus hung -- loses the controller: it is
// looked for again at once, and when the bus is still hung, a minute later.
static void a_failed_read_loses_the_controller_and_it_is_looked_for_again(void)
{
    port_input_t *in = setup();
    touch(0u, 100u, 100u);
    adp_i2c_fake_hang(&g_rig.bus, 1u);
    const uint64_t due = gt911_input_service(&g_rig.input);
    input_event_t ev;
    HAC_CHECK(!in->poll(in, &ev));
    HAC_CHECK(!gt911_input_ready(&g_rig.input));
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->read_errors, 1);
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->losses, 1);
    HAC_CHECK_EQ_U64(due, g_rig.clock.now_ms + GT911_INPUT_RETRY_INIT_MS);

    port_i2c_t *bus = adp_i2c_fake_port(&g_rig.bus);
    HAC_CHECK_EQ_INT(bus->recover(bus), HAL_OK);
    // An interrupt before the minute is up reads nothing.
    gt911_input_signal(&g_rig.input);
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), due);
    HAC_CHECK(!gt911_input_ready(&g_rig.input));

    adp_clock_virtual_advance(&g_rig.clock, due - g_rig.clock.now_ms);
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), GT911_INPUT_NO_DEADLINE);
    HAC_CHECK(gt911_input_ready(&g_rig.input));
    // The finger still down: its next report reaches the consumer.
    gt911_model_scan(&g_rig.model);
    (void)gt911_input_service(&g_rig.input);
    HAC_CHECK(in->poll(in, &ev));
    HAC_CHECK_EQ_INT(ev.kind, INPUT_TOUCH_DOWN);
}

// What the board did on 2026-10-20: a power glitch the ESP32 never saw reset
// the controller, and it latched the other address. The first read at the
// old one fails -- the one refused data transfer this costs -- whatever was
// down is lifted, and the controller is found at the other address at once.
static void a_controller_moved_to_the_other_address_is_found_there(void)
{
    port_input_t *in = setup();
    touch(0u, 100u, 100u);
    (void)gt911_input_service(&g_rig.input);
    input_event_t ev[4];
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);

    g_rig.bus.slots[0].addr = GT911_RIG_ALT_ADDR;  // the latch, after the glitch
    touch(0u, 110u, 100u);
    (void)gt911_input_service(&g_rig.input);
    HAC_CHECK(gt911_input_ready(&g_rig.input));
    HAC_CHECK_EQ_INT(gt911_info(gt911_input_device(&g_rig.input))->address, GT911_RIG_ALT_ADDR);
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->refound, 1);
    HAC_CHECK_EQ_INT((int)g_rig.bus.refused_data_transfers, 1);
    // The finger that was down is lifted: nothing would have reported it.
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_UP);

    // And touches work at the new address.
    gt911_model_scan(&g_rig.model);
    (void)gt911_input_service(&g_rig.input);
    HAC_CHECK_EQ_INT((int)drain(in, ev, 4u), 1);
    HAC_CHECK_EQ_INT(ev[0].kind, INPUT_TOUCH_DOWN);
    HAC_CHECK_EQ_INT((int)g_rig.bus.refused_data_transfers, 1);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.config_writes, 0);
}

// No controller at boot: looked for every minute, and found when it comes.
static void a_controller_absent_at_init_is_looked_for_every_minute(void)
{
    gt911_rig_init(&g_rig, 0u);
    HAC_CHECK_EQ_INT(gt911_rig_input_setup(&g_rig), HAL_OK);
    HAC_CHECK_EQ_INT(gt911_input_init(&g_rig.input), HAL_ERR_NOT_FOUND);
    const uint64_t due = gt911_input_service(&g_rig.input);
    HAC_CHECK_EQ_U64(due, g_rig.input.next_init_ms);
    HAC_CHECK(due > g_rig.clock.now_ms);

    HAC_CHECK(adp_i2c_fake_attach(&g_rig.bus, GT911_RIG_ADDR, gt911_model_port(&g_rig.model)));
    adp_clock_virtual_advance(&g_rig.clock, due - g_rig.clock.now_ms);
    HAC_CHECK_EQ_U64(gt911_input_service(&g_rig.input), GT911_INPUT_NO_DEADLINE);
    HAC_CHECK(gt911_input_ready(&g_rig.input));
    HAC_CHECK_EQ_INT((int)gt911_input_stats(&g_rig.input)->init_attempts, 2);
    HAC_CHECK_EQ_INT((int)g_rig.bus.refused_data_transfers, 0);
}

static void setup_refuses_a_map_with_no_panel(void)
{
    gt911_rig_init(&g_rig, GT911_RIG_ADDR);
    gt911_input_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device = gt911_rig_device_config(&g_rig);
    HAC_CHECK_EQ_INT(gt911_input_setup(&g_rig.input, &cfg), HAL_ERR_INVALID_ARG);
}

int main(void)
{
    HAC_RUN(nothing_until_init_and_nothing_while_untouched);
    HAC_RUN(a_tap_is_a_down_and_an_up_in_panel_coordinates);
    HAC_RUN(the_four_corners_land_on_the_panels_corners);
    HAC_RUN(a_finger_is_read_while_down_without_an_interrupt);
    HAC_RUN(a_finger_whose_reports_stop_is_lifted);
    HAC_RUN(a_finger_held_still_is_neither_lifted_nor_moved);
    HAC_RUN(an_interrupt_with_nothing_behind_it_makes_no_event);
    HAC_RUN(a_full_queue_drops_the_newest_and_counts_them);
    HAC_RUN(sleep_lifts_what_was_down_and_wake_reads_again);
    HAC_RUN(a_failed_read_loses_the_controller_and_it_is_looked_for_again);
    HAC_RUN(a_controller_moved_to_the_other_address_is_found_there);
    HAC_RUN(a_controller_absent_at_init_is_looked_for_every_minute);
    HAC_RUN(setup_refuses_a_map_with_no_panel);
    return hac_test_summary();
}
