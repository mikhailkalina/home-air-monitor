// SPDX-License-Identifier: Apache-2.0
//
// The BME68x as a sensor_source_t (ports/port_sensor.h): temperature,
// humidity, pressure and raw gas resistance, read through Bosch's own
// Sensor API (vendor/bme68x.c) over port_i2c. It lives in drivers/, not
// platform/esp32_t5s3/, so a PC runs it over the fake bus and its BME680
// model exactly as the board runs it over adp_i2c
// (docs/adr/0014-sensor-sources-live-in-drivers.md).
//
// No IAQ. An index of air quality needs Bosch's BSEC library, which is not
// linked (THIRD_PARTY_NOTICES.md), so this source does not claim
// SENS_CAP_IAQ or SENS_CAP_VOC: it reports the gas sensor's raw resistance
// (SENS_CAP_GAS_RES), which docs/requirements.md 17.2 asks for in its place.
// Higher means cleaner air; the number means little across sensors or
// before burn-in.
//
// Forced mode, one measurement per start(): the sensor converts temperature,
// pressure and humidity, heats its gas plate to the profile in
// bme68x_limits.h and measures its resistance, then goes back to sleep by
// itself (datasheet 3.1). How often to measure is the caller's choice.
// start() triggers the measurement and returns at once; ready_in_ms() says
// when it will be done, by Bosch's estimate plus the heater's time; read()
// before then returns HAL_ERR_NOT_READY without touching the bus.
//
// What read() leaves in `out`:
//
//   HAL_OK             temperature, humidity and pressure VAL_OK, or
//                      VAL_ERROR all three if the temperature is outside the
//                      operating range (the other two are compensated with
//                      it), or VAL_ERROR alone if their own value is.
//                      Temperature has the self-heating ESTIMATE taken off,
//                      and humidity is moved to that temperature
//                      (bme68x_relative_humidity_at()). Gas resistance
//                      VAL_ERROR unless the sensor marked it both valid and
//                      heater-stable -- without those bits it is not a
//                      reading (datasheet 3.4) -- else VAL_WARMUP for
//                      HAC_BME68X_GAS_BURN_IN_MS after the first start(),
//                      else VAL_OK. Every other field VAL_UNAVAILABLE.
//   HAL_ERR_NOT_READY  untouched: nothing new yet; ask again after
//                      ready_in_ms().
//   anything else      all four VAL_ERROR: HAL_ERR_NOT_FOUND, not
//                      answering; HAL_ERR_IO, a refused byte;
//                      HAL_ERR_TIMEOUT, the bus, or a measurement still not
//                      finished HAC_BME68X_MEASUREMENT_GRACE_MS after it
//                      should have. The measurement is abandoned; the next
//                      start() begins another.
//
// Resets. On this board a hardware reset power-cycles the sensor, and a
// software reset leaves it powered and configured as the last firmware left
// it (docs/hardware/board_notes.md, the open item on software resets).
// init() soft-resets it either way -- bme68x_init() begins with that -- so
// both continue from the sensor's power-on defaults.
//
// Bosch's bme68x_set_op_mode(), which every configuration call goes
// through, waits for the sensor to report sleep mode with no bound on how
// long. A sensor that never finished a measurement would hang it. So this
// source reads the mode itself first, and calls into that path only once it
// has seen the sensor asleep: start() answers HAL_ERR_BUSY instead.

#ifndef HAC_DRIVERS_BME68X_BME68X_SOURCE_H
#define HAC_DRIVERS_BME68X_BME68X_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "bme68x.h"
#include "bme68x_hal.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"
#include "port_log.h"
#include "port_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_i2c_t *bus;
    const port_clock_t *clock;
    const port_log_t *log;  // may be NULL
    uint8_t address;        // BOARD_BME68X_I2C_ADDRESS on the board
    uint8_t id;             // becomes source_id on every reading
} bme68x_source_config_t;

// "0x" and two hex digits, for the log.
#define BME68X_ID_TEXT_LEN 5u

typedef struct {
    sensor_source_t port;
    bme68x_source_config_t cfg;
    bme68x_hal_t hal;

    // Bosch's state: the calibration it read, and the settings init() gave it.
    struct bme68x_dev dev;
    struct bme68x_conf conf;
    struct bme68x_heatr_conf heater;

    bool initialized;
    uint32_t measurement_ms;  // one forced measurement, rounded up; from init()
    bool measuring;           // a measurement started and not yet collected
    uint64_t ready_at_ms;     // when it should be done
    uint64_t next_read_ms;    // what ready_in_ms() counts down to
    bool burn_in_started;
    uint64_t burn_in_until_ms;
    bool failing;      // the last read failed; logged once per streak
    bool gas_failing;  // the last gas value was not valid; likewise

    uint8_t chip_id;     // BME68X_CHIP_ID (0x61) for any BME68x
    uint8_t variant_id;  // 0x00 a BME680, 0x01 a BME688
    char chip_id_text[BME68X_ID_TEXT_LEN];
    char variant_id_text[BME68X_ID_TEXT_LEN];
} bme68x_source_t;

// Wires `s->port` up as a sensor_source_t named "bme68x", with temperature,
// humidity, pressure and gas-resistance capabilities. Touches no bus:
// init() does that. `s` and everything `cfg` points to must outlive the
// source. HAL_ERR_INVALID_ARG on a NULL bus or clock.
hal_status_t bme68x_source_setup(bme68x_source_t *s, const bme68x_source_config_t *cfg);

// "BME680", "BME688" or "unknown", from the variant_id register (0xF0).
// The BME688 datasheet gives 0x01 for itself; the BME680's datasheet has no
// such register, and Bosch's API reads 0x00 there as a BME680.
const char *bme68x_variant_name(uint8_t variant_id);

// Relative humidity at `to_c` of air whose relative humidity is `rh` at
// `from_c`: the same water vapour against a different saturation pressure
// (the Magnus formula, bme68x_limits.h). Clamped to 0..100.
float bme68x_relative_humidity_at(float rh, float from_c, float to_c);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_BME68X_BME68X_SOURCE_H
