// SPDX-License-Identifier: Apache-2.0
//
// The sensor abstraction, one level above the bus.
//
// sensor_source_t is the seam the simulator plugs into: on hardware, impl is
// a driver talking over port_i2c; on a PC it is CSV playback
// (platform/host/replay/replay_source.h). The sensor manager sees the same
// interface either way, which is what lets fault tolerance, thresholds and
// the UI be exercised without hardware (see docs/architecture.md §5.3).

#ifndef HAC_PORTS_PORT_SENSOR_H
#define HAC_PORTS_PORT_SENSOR_H

#include <stdint.h>

#include "domain/measurement.h"
#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bitmask of the quantities a source can produce. A source may set several
// bits (e.g. a BME68x reports temperature, humidity, pressure and gas
// resistance together).
typedef enum {
    SENS_CAP_TEMPERATURE = 1u << 0,
    SENS_CAP_HUMIDITY = 1u << 1,
    SENS_CAP_PRESSURE = 1u << 2,
    SENS_CAP_CO2 = 1u << 3,
    SENS_CAP_VOC = 1u << 4,
    SENS_CAP_IAQ = 1u << 5,
    SENS_CAP_GAS_RES = 1u << 6,
    SENS_CAP_PM25 = 1u << 7,  // provision for a future sensor; unused for now
} sensor_caps_t;

// One sample from a source. A field the source does not claim in `caps`
// (sensor_source_s::caps) is left at measurement_unavailable().
typedef struct {
    measurement_f32_t temperature_c;
    measurement_f32_t humidity_rh;
    measurement_f32_t pressure_hpa;
    measurement_f32_t co2_ppm;
    measurement_f32_t iaq;
    measurement_f32_t voc_index;
    measurement_f32_t gas_resistance_ohm;
} sensor_sample_t;

// What the rest of the system measured that a source can correct its own
// readings for (docs/adr/0018-ambient-compensation-through-port-sensor.md).
// A field the caller has no trustworthy value for is anything but VAL_OK,
// and the source ignores it.
typedef struct {
    measurement_f32_t pressure_hpa;  // the SCD4x's CO2 compensation (its datasheet, 3.7.5)
} sensor_ambient_t;

typedef struct sensor_source_s sensor_source_t;

// The life cycle the sensor manager (core/app/sensor_manager.h) drives, and
// tests/contract/port_sensor_contract.c holds every implementation to:
//
//   init()         bring the sensor to a known state. May be called again at
//                  any time, without deinit(), to start over -- after a
//                  failure, or after the sensor was unplugged and plugged
//                  back: it forgets every measurement in progress, as a
//                  reset would.
//   start()        begin a measurement, or -- for a sensor that measures on
//                  its own schedule -- make sure it is measuring. Calling it
//                  again before read() has collected the last one is
//                  allowed: HAL_OK or HAL_ERR_BUSY, never a second
//                  measurement on top of the first.
//   ready_in_ms()  how long until read() is worth calling; 0 means now.
//   read()         HAL_OK with a sample, HAL_ERR_NOT_READY with `out`
//                  untouched, or a failure.
//   set_ambient()  optional (NULL when the source corrects for nothing):
//                  hand it conditions measured elsewhere. HAL_OK when taken
//                  or when nothing in `ambient` concerns it,
//                  HAL_ERR_INVALID_ARG for a VAL_OK value outside what it
//                  accepts, HAL_ERR_BUSY when it cannot take one now,
//                  HAL_ERR_NOT_READY before init(). Lost by init(), which
//                  restores the sensor's own defaults.
struct sensor_source_s {
    const char *name;
    uint8_t id;          // becomes measurement_f32_t::source_id on every reading
    sensor_caps_t caps;  // OR of the SENS_CAP_* bits this source can produce

    hal_status_t (*init)(sensor_source_t *self);
    hal_status_t (*start)(sensor_source_t *self);          // begin a measurement
    uint32_t (*ready_in_ms)(const sensor_source_t *self);  // when to call read()
    hal_status_t (*read)(sensor_source_t *self, sensor_sample_t *out);
    hal_status_t (*suspend)(sensor_source_t *self);  // before deep sleep
    void (*deinit)(sensor_source_t *self);
    hal_status_t (*set_ambient)(sensor_source_t *self, const sensor_ambient_t *ambient);

    void *impl;
};

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_SENSOR_H
