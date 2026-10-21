// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics screen at the panel's real 960x540, byte-compared against
// tests/golden/screen_diagnostics_*.bin (docs/adr/0025-touch-input-gt911.md):
//
//   normal   both sensors running, every reading fresh, BSEC's IAQ rated
//            high, the clock set from the RTC, recording on, the MQTT
//            session up over the Wi-Fi
//   trouble  the BME68x failing and restarted twice, its readings in error
//            or taken over by the SCD41, the clock not set, no card, the
//            Wi-Fi being joined again, no session, refusals, the last
//            message from the evening before
//   no_telemetry  the normal device without telemetry, as the board is
//            until its Wi-Fi adapter lands: both network lines "off"
//
// (docs/adr/0031-the-network-on-the-screen.md for the network's lines.)
//
// Run with UPDATE_GOLDEN=1 to adopt an intentional rendering change:
//   UPDATE_GOLDEN=1 ./build/host-debug/tests/golden/test_golden_screen_diagnostics

#ifndef HAC_GOLDEN_DIR
#error "HAC_GOLDEN_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif
#ifndef HAC_DUMP_DIR
#error "HAC_DUMP_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif

#include <stdint.h>
#include <string.h>

#include "app/sensor_manager.h"
#include "golden_check.h"
#include "hac_test.h"
#include "ui/diagnostics_view.h"
#include "ui/framebuffer.h"
#include "ui/screen_diagnostics.h"

#define PANEL_WIDTH 960
#define PANEL_HEIGHT 540
#define PANEL_STRIDE (PANEL_WIDTH / 2)

#define TS_MS 60000u
#define WALL_MS 1792148862000u  // 2026-10-16T11:07:42Z, read at NOW_MS
#define NOW_MS 90000000u

static const time_zone_t k_vilnius_summer = {180, 0, {0u, 0u, 0u, 0}, {0u, 0u, 0u, 0}};

static uint8_t g_pixels[PANEL_STRIDE * PANEL_HEIGHT];
static sensor_manager_t g_mgr;
static sensor_source_t g_scd = {.name = "scd4x", .id = 1u};
static sensor_source_t g_bme = {.name = "bme68x_bsec", .id = 2u};

static void render_and_check(const diag_facts_t *facts, const char *name)
{
    framebuffer_t fb;
    framebuffer_init(&fb, g_pixels, sizeof(g_pixels), PANEL_WIDTH, PANEL_HEIGHT, PIXFMT_GRAY4);
    diag_view_t v;
    diag_view_build(&v, facts);
    screen_diagnostics_render(&fb, &v);
    HAC_CHECK(golden_check_framebuffer(&fb, HAC_GOLDEN_DIR, HAC_DUMP_DIR, name));
}

static void two_sources_running(void)
{
    memset(&g_mgr, 0, sizeof(g_mgr));
    g_mgr.count = 2u;
    g_mgr.slots[0].source = &g_scd;
    g_mgr.slots[1].source = &g_bme;
    for (size_t i = 0u; i < 2u; ++i) {
        g_mgr.slots[i].state = SENSOR_SLOT_IDLE;
        g_mgr.slots[i].ever_ready = true;
        g_mgr.slots[i].inits = 1u;
    }
}

static air_reading_t fresh(void)
{
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_ok(938.0f, TS_MS, 1u);
    r.temperature_c = measurement_ok(23.4f, TS_MS, 2u);
    r.humidity_rh = measurement_ok(45.2f, TS_MS, 2u);
    r.pressure_hpa = measurement_ok(1012.8f, TS_MS, 2u);
    r.iaq = measurement_ok(84.0f, TS_MS, 2u);
    r.iaq.accuracy = ACC_HIGH;
    r.gas_resistance_ohm = measurement_ok(153000.0f, TS_MS, 2u);
    r.wall_time_ms = WALL_MS;
    return r;
}

// The session up: the simulator's network, a minute's messages since boot.
static reporter_status_t online(void)
{
    reporter_status_t n;
    memset(&n, 0, sizeof(n));
    n.link = LINK_ONLINE;
    n.has_net = true;
    n.net = NET_UP;
    n.rssi_dbm = -61;
    strcpy(n.net_info.ssid, "sim-ap");
    n.net_info.ipv4[0] = 192u;
    n.net_info.ipv4[1] = 0u;
    n.net_info.ipv4[2] = 2u;
    n.net_info.ipv4[3] = 10u;
    n.state = REPORTER_ONLINE;
    n.state_messages = 1537u;
    n.published = true;
    n.last_published_ms = NOW_MS - 20000u;  // 14:07:22 in Vilnius
    return n;
}

static void normal(void)
{
    two_sources_running();
    air_reading_t r = fresh();
    r.link = LINK_ONLINE;
    const reporter_status_t n = online();
    const diag_facts_t f = {
        .reading = &r,
        .sensors = &g_mgr,
        .recorder = {.present = true, .state = RECORDER_ON},
        .clock_source = "rtc",
        .network = &n,
        .now_ms = NOW_MS,
        .zone = &k_vilnius_summer,
    };
    render_and_check(&f, "screen_diagnostics_normal");
}

static void no_telemetry(void)
{
    two_sources_running();
    const air_reading_t r = fresh();
    const diag_facts_t f = {
        .reading = &r,
        .sensors = &g_mgr,
        .recorder = {.present = true, .state = RECORDER_ON},
        .clock_source = "rtc",
        .network = NULL,
        .now_ms = NOW_MS,
        .zone = &k_vilnius_summer,
    };
    render_and_check(&f, "screen_diagnostics_no_telemetry");
}

static void trouble(void)
{
    two_sources_running();
    g_mgr.slots[1].failing = true;
    g_mgr.slots[1].failures = 9u;
    g_mgr.slots[1].inits = 3u;
    g_mgr.slots[1].state = SENSOR_SLOT_BACKOFF;
    air_reading_t r = fresh();
    r.temperature_c = measurement_ok(23.9f, TS_MS, 1u);  // the SCD41 as fallback
    r.humidity_rh = measurement_ok(44.1f, TS_MS, 1u);
    r.pressure_hpa = measurement_status(VAL_ERROR, TS_MS, 2u);
    r.iaq = measurement_status(VAL_ERROR, TS_MS, 2u);
    r.gas_resistance_ohm = measurement_status(VAL_ERROR, TS_MS, 2u);
    // The Wi-Fi lost a minute ago and being joined again, no session; the
    // queue refused three messages before that. The clock is not set, so
    // the last message has no local time.
    r.wall_time_ms = 0u;
    r.link = LINK_NO_WIFI;
    reporter_status_t n = online();
    n.link = LINK_NO_WIFI;
    n.net = NET_CONNECTING;
    n.rssi_dbm = PORT_NET_RSSI_NONE;
    memset(n.net_info.ipv4, 0, sizeof(n.net_info.ipv4));
    n.state = REPORTER_OFFLINE;
    n.refusals = 3u;
    n.last_published_ms = NOW_MS - 75000u;
    const diag_facts_t f = {
        .reading = &r,
        .sensors = &g_mgr,
        .recorder = {.present = true, .state = RECORDER_NO_MEDIUM},
        .clock_source = NULL,
        .network = &n,
        .now_ms = NOW_MS,
        .zone = &k_vilnius_summer,
    };
    render_and_check(&f, "screen_diagnostics_trouble");
}

int main(void)
{
    HAC_RUN(normal);
    HAC_RUN(trouble);
    HAC_RUN(no_telemetry);
    return hac_test_summary();
}
