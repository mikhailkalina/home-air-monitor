// SPDX-License-Identifier: Apache-2.0
//
// The simulator's scripted taps (platform/host/adp_input_script.h): the
// --taps form parsed, refused when malformed, off the panel or out of order,
// and delivered once the virtual clock reaches each event.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_input_script.h"
#include "hac_test.h"

#define W 960u
#define H 540u

static adp_clock_virtual_t g_clock;
static adp_input_script_t g_s;
static char g_err[128];

static hal_status_t parse(const char *text)
{
    adp_clock_virtual_init(&g_clock, 1.0);
    adp_input_script_init(&g_s, adp_clock_virtual_port(&g_clock));
    g_err[0] = '\0';
    return adp_input_script_add_taps(&g_s, text, W, H, g_err, sizeof(g_err));
}

static void taps_become_a_down_and_an_up_each(void)
{
    HAC_CHECK_EQ_INT(parse("12:480,270;20.5:10,20"), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_s.count, 4);
    HAC_CHECK_EQ_INT(g_s.events[0].kind, INPUT_TOUCH_DOWN);
    HAC_CHECK_EQ_U64(g_s.events[0].ts_ms, 12000u);
    HAC_CHECK_EQ_INT(g_s.events[1].kind, INPUT_TOUCH_UP);
    HAC_CHECK_EQ_U64(g_s.events[1].ts_ms, 12000u + ADP_INPUT_SCRIPT_TAP_MS);
    HAC_CHECK_EQ_U64(g_s.events[2].ts_ms, 20500u);
    HAC_CHECK_EQ_INT(g_s.events[3].x, 10);
    HAC_CHECK_EQ_INT(g_s.events[3].y, 20);
}

static void events_arrive_when_the_clock_reaches_them(void)
{
    HAC_CHECK_EQ_INT(parse("1:100,200"), HAL_OK);
    port_input_t *in = adp_input_script_port(&g_s);
    input_event_t ev;
    HAC_CHECK_EQ_U64(adp_input_script_next_ms(&g_s), 1000u);
    adp_clock_virtual_advance(&g_clock, 999u);
    HAC_CHECK(!in->poll(in, &ev));
    adp_clock_virtual_advance(&g_clock, 1u);
    HAC_CHECK(in->poll(in, &ev));
    HAC_CHECK_EQ_INT(ev.kind, INPUT_TOUCH_DOWN);
    HAC_CHECK_EQ_U64(ev.ts_ms, 1000u);
    HAC_CHECK(!in->poll(in, &ev));
    // Late: stamped with its own time, not the poll's.
    adp_clock_virtual_advance(&g_clock, 5000u);
    HAC_CHECK(in->poll(in, &ev));
    HAC_CHECK_EQ_INT(ev.kind, INPUT_TOUCH_UP);
    HAC_CHECK_EQ_U64(ev.ts_ms, 1000u + ADP_INPUT_SCRIPT_TAP_MS);
    HAC_CHECK_EQ_U64(adp_input_script_next_ms(&g_s), ADP_INPUT_SCRIPT_NO_EVENT);
}

static void malformed_off_the_panel_or_out_of_order_is_refused(void)
{
    const char *bad[] = {"",        "12",           "12:480",      "12:480,",
                         "x:1,2",   "12:480,270;;", "1:2,3,4",     "-1:2,3",
                         "1:960,0", "1:0,540",      "5:1,1;4:1,1", "5:1,1;5.05:1,1"};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        const hal_status_t st = parse(bad[i]);
        hac_test_check(st == HAL_ERR_INVALID_ARG, "refused", bad[i], __LINE__);
        hac_test_check(g_err[0] != '\0', "says why", bad[i], __LINE__);
        hac_test_check(g_s.count == 0u, "adds nothing", bad[i], __LINE__);
    }
    // The last corner, and a tap right after the last one is over; a
    // trailing semicolon, as a shell user may leave one.
    HAC_CHECK_EQ_INT(parse("0:959,539;0.1:0,0;"), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_s.count, 4);
}

static void too_many_taps_are_refused(void)
{
    char text[2048] = "";
    for (unsigned i = 0u; i <= ADP_INPUT_SCRIPT_MAX_EVENTS / 2u; ++i) {
        char item[32];
        (void)snprintf(item, sizeof(item), "%s%u:1,1", (i == 0u) ? "" : ";", i);
        strcat(text, item);
    }
    HAC_CHECK_EQ_INT(parse(text), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT((int)g_s.count, 0);
}

int main(void)
{
    HAC_RUN(taps_become_a_down_and_an_up_each);
    HAC_RUN(events_arrive_when_the_clock_reaches_them);
    HAC_RUN(malformed_off_the_panel_or_out_of_order_is_refused);
    HAC_RUN(too_many_taps_are_refused);
    return hac_test_summary();
}
