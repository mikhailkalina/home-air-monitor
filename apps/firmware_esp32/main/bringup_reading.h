// SPDX-License-Identifier: Apache-2.0
//
// Phase-2a-only scaffolding standing in for a sensor_manager: there is no
// I2C, no SCD41, no BME68x wired up yet (no panel either -- this whole
// branch is "no sensors, no I2C, no real display"). Something still has to
// hand update_policy varying numbers, or the whole point of this bring-up
// run -- watching it cross real thresholds and issue a mix of partial and
// full redraws -- cannot be observed. This manufactures a slow CO2 sawtooth
// plus a gentle temperature/humidity drift from `now_ms` alone.
//
// Every quantity this does not fabricate (pressure, IAQ, battery, ...) is
// left VAL_UNAVAILABLE, exactly like an air_reading_t no source has ever
// touched -- this file has no opinion about them.
//
// Deleted in phase 3, when a real sensor_manager takes this seat as
// event_loop_deps_t::read_reading.

#ifndef HAC_APPS_FIRMWARE_ESP32_BRINGUP_READING_H
#define HAC_APPS_FIRMWARE_ESP32_BRINGUP_READING_H

#include <stdint.h>

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// Matches the shape of SIM_MAX_READING_AGE_MS in apps/simulator/main.c.
// Bring-up data is regenerated every iteration, so this mainly documents
// intent for whoever wires real sensors in next.
#define BRINGUP_MAX_READING_AGE_MS 60000u

// Matches event_loop_reading_fn. `ctx` is unused (pass NULL).
void bringup_reading_update(void *ctx, air_reading_t *reading, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_BRINGUP_READING_H
