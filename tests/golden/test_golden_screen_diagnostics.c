// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics screen at the panel's real 960x540, byte-compared against
// tests/golden/screen_diagnostics_*.bin (docs/adr/0025-touch-input-gt911.md):
//
//   normal   both sensors running, every reading fresh, BSEC's IAQ rated
//            high, the clock set from the RTC, recording on
//   trouble  the BME68x failing and restarted twice, its readings in error
//            or taken over by the SCD41, the clock not set, no card
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
#define WALL_MS 1792148862000u

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

static void normal(void)
{
    two_sources_running();
    const air_reading_t r = fresh();
    const diag_facts_t f = {
        .reading = &r,
        .sensors = &g_mgr,
        .recorder = {.present = true, .state = RECORDER_ON},
        .clock_source = "rtc",
    };
    render_and_check(&f, "screen_diagnostics_normal");
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
    r.wall_time_ms = 0u;
    const diag_facts_t f = {
        .reading = &r,
        .sensors = &g_mgr,
        .recorder = {.present = true, .state = RECORDER_NO_MEDIUM},
        .clock_source = NULL,
    };
    render_and_check(&f, "screen_diagnostics_trouble");
}

int main(void)
{
    HAC_RUN(normal);
    HAC_RUN(trouble);
    return hac_test_summary();
}
