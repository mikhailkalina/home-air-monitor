// SPDX-License-Identifier: Apache-2.0

#include "bringup_reading.h"

#include <math.h>

// Not from libm's M_PI: that macro is not part of standard C11 and is not
// guaranteed to be declared under this project's strict -std=c11 build.
#define BRINGUP_TWO_PI 6.283185307179586f

#define BRINGUP_CO2_SOURCE_ID 1u
#define BRINGUP_ENV_SOURCE_ID 2u

// Ten minutes end to end, rising then falling, so both directions of
// core/app/update_policy.c's 50 ppm threshold get exercised, not just one.
#define BRINGUP_CO2_PERIOD_MS 600000.0f
#define BRINGUP_CO2_MIN_PPM 600.0f
#define BRINGUP_CO2_MAX_PPM 1600.0f

#define BRINGUP_TEMP_PERIOD_MS 480000.0f
#define BRINGUP_TEMP_CENTER_C 22.0f
#define BRINGUP_TEMP_AMPLITUDE_C 1.5f

#define BRINGUP_HUMIDITY_PERIOD_MS 720000.0f
#define BRINGUP_HUMIDITY_CENTER_RH 45.0f
#define BRINGUP_HUMIDITY_AMPLITUDE_RH 8.0f

// Rises for the first half of `period_ms`, falls for the second: a triangle
// rather than a sine, so the CO2 delta near the peak stays large enough to
// keep tripping the threshold instead of flattening out the way a sine's
// peak would.
static float triangle_wave(float t_ms, float period_ms, float min_v, float max_v)
{
    const float phase = fmodf(t_ms, period_ms) / period_ms;  // [0, 1)
    const float tri = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
    return min_v + tri * (max_v - min_v);
}

static float sine_wave(float t_ms, float period_ms, float center, float amplitude)
{
    const float phase = fmodf(t_ms, period_ms) / period_ms;
    return center + amplitude * sinf(phase * BRINGUP_TWO_PI);
}

void bringup_reading_update(void *ctx, air_reading_t *reading, uint64_t now_ms)
{
    (void)ctx;

    // A float loses precision against a uint64_t millisecond count once
    // now_ms grows past a few hours, but every period above is minutes long
    // and this branch's bring-up run is measured in minutes too -- see the
    // phase-2a plan's verification section ("several minutes with no
    // watchdog reset"). Good enough here; not a pattern to copy for
    // something that must still make sense after days of uptime.
    const float t_ms = (float)now_ms;

    const float co2 =
        triangle_wave(t_ms, BRINGUP_CO2_PERIOD_MS, BRINGUP_CO2_MIN_PPM, BRINGUP_CO2_MAX_PPM);
    reading->co2_ppm = measurement_ok(co2, now_ms, BRINGUP_CO2_SOURCE_ID);

    const float temp =
        sine_wave(t_ms, BRINGUP_TEMP_PERIOD_MS, BRINGUP_TEMP_CENTER_C, BRINGUP_TEMP_AMPLITUDE_C);
    reading->temperature_c = measurement_ok(temp, now_ms, BRINGUP_ENV_SOURCE_ID);

    const float humidity = sine_wave(t_ms, BRINGUP_HUMIDITY_PERIOD_MS, BRINGUP_HUMIDITY_CENTER_RH,
                                     BRINGUP_HUMIDITY_AMPLITUDE_RH);
    reading->humidity_rh = measurement_ok(humidity, now_ms, BRINGUP_ENV_SOURCE_ID);
}
