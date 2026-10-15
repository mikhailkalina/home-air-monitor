// SPDX-License-Identifier: Apache-2.0
//
// The SCD4x as a sensor_source_t (ports/port_sensor.h): CO2, temperature and
// humidity, read through Sensirion's own driver (vendor/scd4x_i2c.c) over
// port_i2c. It lives in drivers/, not platform/esp32_t5s3/, so a PC runs it
// over the fake bus and its SCD41 model exactly as the board runs it over
// adp_i2c (docs/adr/0014-sensor-sources-live-in-drivers.md).
//
// Two modes, one source -- the mode is configuration, not a second driver:
//
//   * periodic (docs/requirements.md 11.2, USB power): start() starts the
//     sensor's own measurement, a new reading every 5 s; read() asks
//     get_data_ready_status and, when there is one, read_measurement.
//   * single shot (battery, phase 5; SCD41 and SCD43 only): each start()
//     triggers one measurement, and read() collects it 5 s later.
//
// Waiting. A command's execution time -- 1 ms for most, 500 ms for
// stop_periodic_measurement -- is waited inside Sensirion's call, through
// port_clock's delay_ms(): short, bounded, and only in init(), suspend() and
// scd4x_source_set_ambient_pressure(). A measurement is never waited for.
// ready_in_ms() says when to call read(), and read() before then returns
// HAL_ERR_NOT_READY; in single-shot mode it does not even touch the bus,
// because the sensor must not be addressed while it measures.
//
// Busy, not absent. The sensor refuses its address while it executes a
// command (ports/port_i2c.h). So a refused address inside a window this
// source opened itself -- a single shot it started, plus SCD4X_BUSY_GRACE_MS
// -- means "still measuring", and read() returns HAL_ERR_NOT_READY; a read
// header refused right after an acknowledged read_measurement means "nothing
// buffered yet" (datasheet 3.6.2), likewise. A refused address anywhere else
// means the sensor is not answering.
//
// What read() leaves in `out`:
//
//   HAL_OK             CO2, temperature and humidity, each with its own
//                      status: VAL_ERROR outside the datasheet's range
//                      (scd4x_limits.h), else VAL_WARMUP for SCD4X_WARMUP_MS
//                      after the first start(), else VAL_OK. Every other
//                      field VAL_UNAVAILABLE.
//   HAL_ERR_NOT_READY  untouched: nothing new yet; ask again after
//                      ready_in_ms().
//   anything else      CO2, temperature and humidity VAL_ERROR: the bus or the
//                      sensor failed. HAL_ERR_NOT_FOUND: not answering;
//                      HAL_ERR_IO: a refused byte or a bad CRC;
//                      HAL_ERR_TIMEOUT: the bus.
//
// A stalled sensor -- one that never has data again -- is not an error here:
// read() keeps saying HAL_ERR_NOT_READY, and the readings it last returned
// age into VAL_STALE in the core (measurement_apply_age()).
//
// Resets. On this board a hardware reset power-cycles the sensor, so after
// one it is idle; a software reset (esp_restart(), seen 2026-10-16) leaves
// it powered, and still measuring if it was (docs/hardware/board_notes.md,
// "Power rails and connectors" and the open item on software resets). While
// it measures it refuses all but five commands. init() therefore stops
// periodic measurement first and reinitialises, whatever the reset was, so
// both cases continue identically. It costs about 0.56 s of init(). Which
// case it was is logged (found_measuring): before the stop, init() asks for
// the serial number once, which a measuring sensor refuses.
//
// One SCD4x per image: see scd4x_hal.h.

#ifndef HAC_DRIVERS_SCD4X_SCD4X_SOURCE_H
#define HAC_DRIVERS_SCD4X_SCD4X_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"
#include "port_log.h"
#include "port_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCD4X_MODE_PERIODIC = 0,
    SCD4X_MODE_SINGLE_SHOT,
} scd4x_mode_t;

typedef enum {
    SCD4X_VARIANT_UNKNOWN = 0,  // get_sensor_variant failed or named no known part
    SCD4X_VARIANT_SCD40,
    SCD4X_VARIANT_SCD41,
    SCD4X_VARIANT_SCD43,
} scd4x_variant_t;

typedef struct {
    port_i2c_t *bus;
    const port_clock_t *clock;
    const port_log_t *log;  // may be NULL
    uint8_t address;        // BOARD_SCD4X_I2C_ADDRESS on the board
    uint8_t id;             // becomes source_id on every reading
    scd4x_mode_t mode;
} scd4x_source_config_t;

// "0x" and twelve hex digits: the 48-bit serial number, for the log.
#define SCD4X_SERIAL_TEXT_LEN 15u

typedef struct {
    sensor_source_t port;
    scd4x_source_config_t cfg;

    bool initialized;
    bool measuring;  // periodic: running; single shot: a shot not yet collected
    bool warmup_started;
    uint64_t warmup_until_ms;
    uint64_t next_read_ms;   // what ready_in_ms() counts down to
    uint64_t busy_until_ms;  // single shot: not to be addressed before this
    bool failing;            // the last read failed; logged once per streak

    uint64_t serial;  // 48 bits, read by init()
    scd4x_variant_t variant;
    bool found_measuring;  // init() found it measuring: a reset kept its supply
    char serial_text[SCD4X_SERIAL_TEXT_LEN];
} scd4x_source_t;

// Wires `s->port` up as a sensor_source_t named "scd4x", with CO2,
// temperature and humidity capabilities. Touches no bus: init() does that.
// `s` and everything `cfg` points to must outlive the source.
// HAL_ERR_INVALID_ARG on a NULL bus or clock.
hal_status_t scd4x_source_setup(scd4x_source_t *s, const scd4x_source_config_t *cfg);

// Hands the sensor the ambient pressure for its CO2 compensation (datasheet
// 3.7.5) -- allowed while it measures, and recommended where the pressure
// changes. HAL_ERR_INVALID_ARG outside 700-1200 hPa or before init();
// HAL_ERR_BUSY while a single shot is being measured. Nothing feeds it yet:
// bme68x_source measures pressure since phase 3b, and connecting the two is
// the composition's job.
hal_status_t scd4x_source_set_ambient_pressure(scd4x_source_t *s, float pressure_hpa);

const char *scd4x_variant_name(scd4x_variant_t variant);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_SCD4X_SCD4X_SOURCE_H
