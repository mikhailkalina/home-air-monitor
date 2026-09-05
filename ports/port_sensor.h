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

typedef struct sensor_source_s sensor_source_t;

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

    void *impl;
};

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_SENSOR_H
