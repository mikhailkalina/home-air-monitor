// SPDX-License-Identifier: Apache-2.0
//
// The home screen at the panel's real 960x540, byte-compared against
// tests/golden/screen_home_*.bin.
//
// Three readings, chosen because they are the three the requirements care
// about and the three a rendering change is most likely to break:
//
//   normal   every quantity fresh -- the picture the device shows most of the time
//   error    one sensor down, requirements 15.1: the fault must be on the glass
//   warmup   a cold start, requirements 13.1: no garbage numbers before the
//            SCD41 and the BME68x have settled
//
// Run with UPDATE_GOLDEN=1 to adopt an intentional rendering change:
//   UPDATE_GOLDEN=1 ./build/host-debug/tests/golden/test_golden_screen_home

#ifndef HAC_GOLDEN_DIR
#error "HAC_GOLDEN_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif
#ifndef HAC_DUMP_DIR
#error "HAC_DUMP_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif

#include "golden_check.h"
#include "hac_test.h"
#include "ui/framebuffer.h"
#include "ui/screen_home.h"
#include "ui/view_model.h"

// The ED047TC1 as the core sees it (docs/hardware/board_notes.md).
#define PANEL_WIDTH 960
#define PANEL_HEIGHT 540
#define PANEL_STRIDE (PANEL_WIDTH / 2)  // GRAY4, two pixels per byte

#define SAMPLE_TS_MS 60000u
#define RENDER_TS_MS 72000u  // 12 s after the sample, so "updated 12 s ago"

static uint8_t g_pixels[PANEL_STRIDE * PANEL_HEIGHT];

static void render_and_check(const air_reading_t *reading, const char *name)
{
    framebuffer_t fb;
    framebuffer_init(&fb, g_pixels, sizeof(g_pixels), PANEL_WIDTH, PANEL_HEIGHT, PIXFMT_GRAY4);

    view_model_t vm;
    view_model_build(&vm, reading, RENDER_TS_MS);
    screen_home_render(&fb, &vm);

    HAC_CHECK(golden_check_framebuffer(&fb, HAC_GOLDEN_DIR, HAC_DUMP_DIR, name));
}

// A settled room: CO2 in the "Moderate" band, everything else nominal.
static air_reading_t base_reading(void)
{
    air_reading_t r;

    air_reading_init(&r);
    r.co2_ppm = measurement_ok(938.0f, SAMPLE_TS_MS, 1u);
    r.temperature_c = measurement_ok(23.4f, SAMPLE_TS_MS, 2u);
    r.humidity_rh = measurement_ok(45.2f, SAMPLE_TS_MS, 2u);
    r.pressure_hpa = measurement_ok(1012.8f, SAMPLE_TS_MS, 2u);
    r.iaq = measurement_ok(84.0f, SAMPLE_TS_MS, 2u);
    r.battery_percent = measurement_ok(87.0f, SAMPLE_TS_MS, 3u);
    r.net_connected = true;
    r.telemetry_connected = true;
    r.wifi_rssi_dbm = -61;
    r.uptime_s = 3600u;
    r.boot_count = 1u;
    return r;
}

static void normal_reading(void)
{
    const air_reading_t r = base_reading();
    render_and_check(&r, "screen_home_normal");
}

static void one_sensor_in_error(void)
{
    air_reading_t r = base_reading();

    // The SCD41 stops answering: the headline has no number, the band is
    // unknown, and the footer says so.
    r.co2_ppm = measurement_status(VAL_ERROR, SAMPLE_TS_MS, 1u);
    render_and_check(&r, "screen_home_sensor_error");
}

static void everything_warming_up(void)
{
    air_reading_t r;

    // A cold start: both sensors are powered but neither has settled, and the
    // battery gauge has not been read at all -- so this frame also covers
    // VAL_UNAVAILABLE and the offline link state.
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_WARMUP, SAMPLE_TS_MS, 1u);
    r.temperature_c = measurement_status(VAL_WARMUP, SAMPLE_TS_MS, 2u);
    r.humidity_rh = measurement_status(VAL_WARMUP, SAMPLE_TS_MS, 2u);
    r.pressure_hpa = measurement_status(VAL_WARMUP, SAMPLE_TS_MS, 2u);
    r.iaq = measurement_status(VAL_WARMUP, SAMPLE_TS_MS, 2u);
    render_and_check(&r, "screen_home_warmup");
}

int main(void)
{
    HAC_RUN(normal_reading);
    HAC_RUN(one_sensor_in_error);
    HAC_RUN(everything_warming_up);
    return hac_test_summary();
}
