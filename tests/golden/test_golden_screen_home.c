// SPDX-License-Identifier: Apache-2.0
//
// The home screen at the panel's real 960x540, byte-compared against
// tests/golden/screen_home_*.bin.
//
// Seven readings, chosen because they are the ones the requirements care
// about and the ones a rendering change is most likely to break:
//
//   normal   every quantity fresh -- the picture the device shows most of the
//            time, IAQ 84 with its band, "Good", under it, and "updated
//            14:07" in Vilnius time
//   error    one sensor down, requirements 15.1: the fault must be on the glass
//   warmup   a cold start, requirements 13.1: no garbage numbers before the
//            SCD41 and the BME68x have settled
//   no_bsec  no BSEC linked, requirements 17.2: raw gas resistance in the IAQ
//            cell, under its own name
//   iaq_not_calibrated  BSEC's IAQ at accuracy 1, as every boot has it for
//            hours: the number, its band, the question mark, and the footer
//            saying so (docs/adr/0021-measurement-accuracy-and-iaq-levels.md)
//   updated_yesterday  every quantity stale since late the evening before:
//            the clock marks, and the footer's dated form, "updated 15.10
//            23:58" (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md)
//   no_wall_time  the normal reading on a device whose clock is not set:
//            "updated --:--"
//   no_broker  the normal reading with the Wi-Fi up and no MQTT session for a
//            minute: "no broker" in the bar
//            (docs/adr/0031-the-network-on-the-screen.md); the normal frame
//            says "online", the warm-up one -- no telemetry -- nothing
//
// Run with UPDATE_GOLDEN=1 to adopt an intentional rendering change:
//   UPDATE_GOLDEN=1 ./build/host-debug/tests/golden/test_golden_screen_home

#ifndef HAC_GOLDEN_DIR
#error "HAC_GOLDEN_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif
#ifndef HAC_DUMP_DIR
#error "HAC_DUMP_DIR must be defined by the build (see tests/golden/CMakeLists.txt)"
#endif

#include <stddef.h>
#include <stdint.h>

#include "domain/civil_time.h"
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
#define RENDER_TS_MS 72000u  // 12 s after the sample: fresh

// The wall time read with RENDER_TS_MS: 2026-10-16T11:07:42Z, 14:07:42 in
// Vilnius -- so the sample was taken at 14:07:30, "updated 14:07".
#define RENDER_WALL_MS 1792148862000u

// The device's zone (apps/firmware_esp32/main/main.c).
#define VILNIUS_STANDARD_MIN 120

static uint8_t g_pixels[PANEL_STRIDE * PANEL_HEIGHT];

// `render_ts_ms` is the monotonic time the reading's wall time was read at.
static void render_at_and_check(const air_reading_t *reading, uint64_t render_ts_ms,
                                const char *name)
{
    framebuffer_t fb;
    framebuffer_init(&fb, g_pixels, sizeof(g_pixels), PANEL_WIDTH, PANEL_HEIGHT, PIXFMT_GRAY4);

    const time_zone_t vilnius = time_zone_eu(VILNIUS_STANDARD_MIN);
    view_model_t vm;
    view_model_build(&vm, reading, render_ts_ms, &vilnius);
    screen_home_render(&fb, &vm);

    HAC_CHECK(golden_check_framebuffer(&fb, HAC_GOLDEN_DIR, HAC_DUMP_DIR, name));
}

static void render_and_check(const air_reading_t *reading, const char *name)
{
    render_at_and_check(reading, RENDER_TS_MS, name);
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
    r.link = LINK_ONLINE;
    r.uptime_s = 3600u;
    r.boot_count = 1u;
    r.wall_time_ms = RENDER_WALL_MS;
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

static void without_bsec(void)
{
    air_reading_t r = base_reading();

    // The device as phase 3b builds it: no BSEC, so nothing provides IAQ,
    // and the BME68x's raw gas resistance takes its cell
    // (docs/requirements.md 17.2).
    r.iaq = measurement_unavailable();
    r.gas_resistance_ohm = measurement_ok(182000.0f, SAMPLE_TS_MS, 2u);
    render_and_check(&r, "screen_home_no_bsec");
}

static void iaq_not_calibrated(void)
{
    air_reading_t r = base_reading();

    // BSEC running, its scale not yet trimmed to the room: a reading in the
    // "lightly polluted" band, on a scale its source rates ACC_LOW.
    r.iaq = measurement_ok(128.0f, SAMPLE_TS_MS, 2u);
    r.iaq.accuracy = ACC_LOW;
    r.gas_resistance_ohm = measurement_ok(153000.0f, SAMPLE_TS_MS, 2u);
    render_and_check(&r, "screen_home_iaq_not_calibrated");
}

static void updated_yesterday(void)
{
    air_reading_t r = base_reading();

    // The readings on the glass were last measured at 23:58:10 the evening
    // before: 14 h 9 min 32 s before the wall time read with them, 14:07:42.
    const uint64_t age_ms = (14u * 3600u + 9u * 60u + 32u) * 1000u;
    measurement_f32_t *fields[] = {&r.co2_ppm, &r.temperature_c, &r.humidity_rh, &r.pressure_hpa,
                                   &r.iaq};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        fields[i]->status = VAL_STALE;
    }
    render_at_and_check(&r, SAMPLE_TS_MS + age_ms, "screen_home_updated_yesterday");
}

static void no_wall_time(void)
{
    air_reading_t r = base_reading();
    r.wall_time_ms = 0u;  // the RTC held nothing valid and nobody has set it
    render_and_check(&r, "screen_home_no_wall_time");
}

static void no_broker(void)
{
    air_reading_t r = base_reading();
    r.link = LINK_NO_BROKER;  // held a minute: the broker gone, the Wi-Fi still there
    render_and_check(&r, "screen_home_no_broker");
}

static void everything_warming_up(void)
{
    air_reading_t r;

    // A cold start: both sensors are powered but neither has settled, and the
    // battery gauge has not been read at all -- so this frame also covers
    // VAL_UNAVAILABLE, and a build without telemetry: no link in the bar.
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
    HAC_RUN(without_bsec);
    HAC_RUN(iaq_not_calibrated);
    HAC_RUN(updated_yesterday);
    HAC_RUN(no_wall_time);
    HAC_RUN(no_broker);
    return hac_test_summary();
}
