// SPDX-License-Identifier: Apache-2.0
//
// The normalized measurement contract shared by the sensor manager, the UI and
// every reporting backend.
//
// Two properties matter more than the field list:
//
//   * every quantity carries its own status and acquisition time, so a failed
//     SCD41 is distinguishable from a genuine 0 ppm, and a stale value is
//     distinguishable from a fresh one;
//   * ageing is derived, not observed. Nothing here reads the system clock;
//     the caller passes now_ms, which it obtained from port_clock.

#ifndef HAC_CORE_DOMAIN_MEASUREMENT_H
#define HAC_CORE_DOMAIN_MEASUREMENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// VAL_UNAVAILABLE is 0 so that a zero-filled air_reading_t reads as
// "nothing known yet" rather than as a set of valid zeroes.
typedef enum {
    VAL_UNAVAILABLE = 0,  // sensor absent, or does not provide this quantity
    VAL_WARMUP,           // sensor warming up (SCD41, BME68x burn-in)
    VAL_OK,               // fresh and trustworthy
    VAL_STALE,            // value present but older than the allowed age
    VAL_ERROR,            // read failure, or a physically implausible value
} value_status_t;

typedef struct {
    float value;
    value_status_t status;
    uint64_t ts_ms;     // monotonic acquisition time, from port_clock
    uint8_t source_id;  // which sensor_source produced it; 0 = none
} measurement_f32_t;

typedef struct {
    measurement_f32_t temperature_c;
    measurement_f32_t humidity_rh;
    measurement_f32_t pressure_hpa;
    measurement_f32_t co2_ppm;
    measurement_f32_t iaq;
    measurement_f32_t voc_index;
    measurement_f32_t gas_resistance_ohm;

    measurement_f32_t battery_percent;
    measurement_f32_t battery_mv;
    bool charging;

    int8_t wifi_rssi_dbm;
    bool net_connected;
    bool telemetry_connected;

    uint32_t uptime_s;
    uint32_t boot_count;
    uint64_t wall_time_ms;  // real time, if synchronized; 0 otherwise
} air_reading_t;

// Returned by measurement_age_ms() when the age cannot be established: the
// measurement carries no timestamp, or now_ms predates it because the
// monotonic clock restarted (deep sleep is a cold boot). An unknown age is
// always older than any limit, so such a value is never treated as fresh.
#define MEASUREMENT_AGE_UNKNOWN UINT64_MAX

// --- construction ----------------------------------------------------------

// A quantity this source does not provide.
measurement_f32_t measurement_unavailable(void);

// A successful reading. A non-finite value is rejected as VAL_ERROR, so a NaN
// out of a driver cannot reach the UI or the MQTT payload.
measurement_f32_t measurement_ok(float value, uint64_t ts_ms, uint8_t source_id);

// A reading that failed or is not usable yet: VAL_WARMUP, VAL_ERROR. The value
// is zeroed, since no caller may use it.
measurement_f32_t measurement_status(value_status_t status, uint64_t ts_ms, uint8_t source_id);

// --- queries ---------------------------------------------------------------

// True when the measurement carries a number that may be shown or published,
// whether or not it is still fresh (VAL_OK or VAL_STALE).
bool measurement_has_value(const measurement_f32_t *m);

// Time elapsed since acquisition, or MEASUREMENT_AGE_UNKNOWN.
uint64_t measurement_age_ms(const measurement_f32_t *m, uint64_t now_ms);

// True when the measurement is older than max_age_ms, or already VAL_STALE.
// Statuses that carry no value at all (VAL_UNAVAILABLE, VAL_WARMUP, VAL_ERROR)
// are never stale: they are already worse than stale.
bool measurement_is_stale(const measurement_f32_t *m, uint64_t now_ms, uint32_t max_age_ms);

// --- ageing ----------------------------------------------------------------

// Derive VAL_STALE: demote VAL_OK to VAL_STALE once it is older than
// max_age_ms. Idempotent, and it never revives a value that already aged out.
void measurement_apply_age(measurement_f32_t *m, uint64_t now_ms, uint32_t max_age_ms);

// --- air_reading_t ---------------------------------------------------------

// Reset every quantity to VAL_UNAVAILABLE and every flag to false.
void air_reading_init(air_reading_t *r);

// Apply measurement_apply_age() to every quantity in the reading.
void air_reading_apply_age(air_reading_t *r, uint64_t now_ms, uint32_t max_age_ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_DOMAIN_MEASUREMENT_H
