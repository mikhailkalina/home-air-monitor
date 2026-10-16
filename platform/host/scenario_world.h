// SPDX-License-Identifier: Apache-2.0
//
// The room the fake bus's sensor models measure in the simulator's fake-i2c
// mode (docs/architecture.md 5.4), played from the same scenario CSVs the
// replay sources read in replay mode. So one scenario reaches the screen two
// ways -- through replay_source, or through the real drivers over the SCD41
// and BME680 models -- and the two should agree; tests/unit/
// test_scenario_world.c checks that they do.
//
// Which file each model sees is the same split the replay sources make:
//
//   * the SCD41 measures the CO2 scenario's CO2, temperature and humidity.
//     A CO2 file without temperature or humidity columns
//     (sensor_scd41_fault.csv) takes them from the environment file.
//   * the BME680 measures the environment scenario's temperature, humidity,
//     pressure and gas resistance, through a die that runs warmer than the
//     room by `bme680_self_heating_c` -- which the composition sets to what
//     the driver subtracts (HAC_BME68X_SELF_HEATING_C), so the driver hands
//     the room back. Its humidity is moved to the die's temperature with the
//     Magnus formula, as the die would see it. A missing column keeps the
//     model's own default.
//
// A row whose status is not "ok" -- error, warmup, stale, nan, out_of_range
// -- is a measurement the sensor fails: the model refuses its address on the
// next transfer, which is the next read, and the driver reports the read
// failed. There is no fake-i2c form of "warmup" or "stale": the drivers make
// their own warm-up, and staleness is the core's to derive.
//
// Time is the virtual clock's: the scenario's t_offset_s 0 is the clock's 0,
// and it loops as replay does.

#ifndef HAC_PLATFORM_HOST_SCENARIO_WORLD_H
#define HAC_PLATFORM_HOST_SCENARIO_WORLD_H

#include <stdbool.h>

#include "hal_status.h"
#include "i2c_models/bme680_model.h"
#include "i2c_models/scd41_model.h"
#include "replay/replay_source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    replay_timeline_t co2;  // what the SCD41 measures; empty without a CO2 file
    replay_timeline_t env;  // what the BME680 measures; empty without an environment file
    bool have_co2;
    bool have_env;
    bool interpolate;
    float bme680_self_heating_c;

    scd41_model_t *scd41;  // borrowed, for the address refusal of a failing row
    bme680_model_t *bme680;

    // What each model was last handed, kept across a failing row or a
    // missing column.
    scd41_model_reading_t scd41_last;
    bme680_model_reading_t bme680_last;

    // Rows that failed a measurement, for tests.
    uint32_t scd41_failed_rows;
    uint32_t bme680_failed_rows;
} scenario_world_t;

// Loads the scenario files; either path may be NULL, not both.
// `interpolate` as replay_source_init()'s. HAL_ERR_INVALID_ARG with no file
// at all; otherwise whatever replay_parse_file() says about a bad one.
hal_status_t scenario_world_init(scenario_world_t *w, const char *co2_path, const char *env_path,
                                 bool interpolate, float bme680_self_heating_c);

// Makes the models measure this world: installs their reading callbacks.
// Either model may be NULL. The models must outlive the world's use.
void scenario_world_attach(scenario_world_t *w, scd41_model_t *scd41, bme680_model_t *bme680);

void scenario_world_dispose(scenario_world_t *w);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_SCENARIO_WORLD_H
