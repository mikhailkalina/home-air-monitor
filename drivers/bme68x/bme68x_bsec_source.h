// SPDX-License-Identifier: Apache-2.0
//
// The BME68x as a sensor_source_t (ports/port_sensor.h) measured as Bosch's
// BSEC2 library asks, with BSEC's index of air quality among its readings
// (docs/adr/0020-bsec-iaq.md). Bosch's Sensor API reads the sensor over
// port_i2c, as in bme68x_source.h; BSEC decides when it measures and with
// which heater profile, and turns each measurement into its outputs.
//
// Built only with the library -- CONFIG_HAC_BSEC on the board, HAC_BSEC on
// the host (cmake/bsec.cmake) -- which is not in this repository. A build
// without it measures the same sensor through bme68x_source.h, and shows raw
// gas resistance where this source puts IAQ (docs/requirements.md 17.2).
// There is no port between the two: one real implementation, chosen by the
// composition.
//
// The schedule is BSEC's. Each call of bsec_sensor_control() says whether to
// measure now, with which oversampling and which heater temperature and
// duration, and when to call again: 3 s later in low-power mode
// (HAC_BSEC_PERIOD_MS), counted from the call itself, so a late call moves
// every later one. The first measurements after BSEC starts are its
// "power-on conditioning" -- the heater at up to 380 C for a second -- and
// then 320 C for 197 ms (BSEC 2.6.1.0 with this configuration, seen on the
// host). This source makes the call only once its time has come, never
// before -- BSEC answers an early call with a warning and no measurement --
// from whichever of start() and read() the caller makes at that time:
//
//   start()        HAL_OK: the schedule runs from now until the next init().
//                  If BSEC's time has come, its call and the measurement it
//                  asks for begin here.
//   ready_in_ms()  while a measurement runs, how long until it is done, by
//                  Bosch's estimate plus the heater's time; otherwise how
//                  long until BSEC's next call. 0 means now.
//   read()         a finished measurement collected and handed to BSEC:
//                  HAL_OK with the sample. Otherwise HAL_ERR_NOT_READY with
//                  `out` untouched -- after making BSEC's call and starting
//                  its measurement, if their time has come. Before start(),
//                  it touches nothing.
//
// So under the sensor manager the call is made at the first poll after
// BSEC's time, and is as late as that poll. BSEC tolerates 6.25 % of the
// period, 187 ms; a later call is still answered, and measured, with
// BSEC_W_SC_CALL_TIMING_VIOLATION. The firmware's event loop can be that late
// -- a full refresh of the panel holds it for over 3 s -- so each late call is
// logged at WARN with how late it was and how many there have been, and
// counted in late_calls (the ADR, "Timing").
//
// What read() leaves in `out` on HAL_OK:
//
//   temperature,   BSEC's heat-compensated outputs -- the sensor's own
//   humidity       heating taken off by BSEC, HAC_BSEC_DEVICE_HEAT_C (an
//                  ESTIMATE) for the board's -- VAL_OK.
//   pressure       the sensor's, in hPa, VAL_OK; VAL_ERROR outside the
//                  operating range.
//   gas_resistance the sensor's, status by bme68x_bsec_gas_status(): BSEC's
//                  stabilization and run-in statuses, not a fixed burn-in.
//   iaq            BSEC's index, status by bme68x_bsec_iaq_status() from its
//                  accuracy (0 is a warm-up, 1-3 a reading), and the accuracy
//                  itself as value_accuracy_t (bme68x_bsec_iaq_accuracy());
//                  VAL_ERROR when BSEC returned none, as without valid gas.
//   co2, voc       VAL_UNAVAILABLE, always. BSEC's CO2 equivalent is an
//                  estimate from VOCs, not CO2 (requirements 4.1, 13.3): it
//                  is not subscribed to, so not even computed. Its breath-VOC
//                  equivalent, in ppm, is not the VOC index either.
//
// A die temperature outside the operating range makes all five VAL_ERROR,
// and that measurement is not handed to BSEC. A bus or sensor failure makes
// all five VAL_ERROR and abandons the measurement, as in bme68x_source.h;
// BSEC's own refusals (a negative result) count as HAL_ERR_IO, warnings are
// logged once per streak and the outputs used.
//
// init() brings the sensor up exactly as bme68x_source does (bme68x_common.h:
// start-up time, soft reset, ids, a check that it sleeps) and then BSEC: the
// configuration of cmake/bsec.cmake and the subscription above, through
// BSEC's multi-instance interface into this source's own instance. Called
// again -- a failing sensor started over -- it keeps what BSEC has learnt since
// boot: the state is taken from the instance before BSEC is reset and given
// back after. Across a reboot nothing is kept yet (board_notes.md, open item
// "BSEC's state restarts at every boot"): IAQ accuracy starts from 0.

#ifndef HAC_DRIVERS_BME68X_BME68X_BSEC_SOURCE_H
#define HAC_DRIVERS_BME68X_BME68X_BSEC_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bme68x.h"
#include "bme68x_bsec_limits.h"
#include "bme68x_common.h"
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
} bme68x_bsec_source_config_t;

// "255.255.255.255" and its terminator.
#define BME68X_BSEC_VERSION_TEXT_LEN 16u

typedef struct {
    sensor_source_t port;
    bme68x_bsec_source_config_t cfg;
    bme68x_hal_t hal;

    // Bosch's Sensor API: the calibration it read, the settings BSEC last
    // asked for.
    struct bme68x_dev dev;
    struct bme68x_conf conf;
    struct bme68x_heatr_conf heater;

    // BSEC: this source's instance, and its state carried over init().
    _Alignas(max_align_t) uint8_t instance[HAC_BSEC_INSTANCE_BYTES];
    bool bsec_started;    // the instance has been initialised since setup
    bool state_restored;  // the last init() gave BSEC back what it had learnt
    uint8_t state[HAC_BSEC_STATE_BYTES];
    char bsec_version_text[BME68X_BSEC_VERSION_TEXT_LEN];

    bool initialized;
    bool running;             // start() since init()
    bool next_call_known;     // BSEC has named its next call since init()
    uint64_t next_call_ms;    // when BSEC wants bsec_sensor_control() next
    bool measuring;           // a measurement BSEC asked for, not yet collected
    uint64_t measured_at_ms;  // the call that started it: its inputs' time stamp
    uint64_t ready_at_ms;     // when it should be done
    uint64_t next_read_ms;    // what ready_in_ms() counts down to while measuring
    uint32_t process_data;    // which inputs BSEC wants from it
    uint16_t heater_temp_c;   // the heater profile last used, logged on a change
    uint16_t heater_dur_ms;

    // BSEC's verdicts, logged on a change.
    bool have_accuracy;
    uint8_t iaq_accuracy;
    bool have_gas_state;
    bool stabilized;
    bool run_in_done;

    uint32_t measurements;    // measurements BSEC asked for since setup
    uint32_t late_calls;      // calls BSEC answered BSEC_W_SC_CALL_TIMING_VIOLATION
    bool failing;             // the last read failed; logged once per streak
    bool gas_failing;         // the last gas value was not valid; likewise
    bool bsec_warning;        // the last bsec_do_steps() warned; likewise
    bool calibration_logged;  // the first init() logs the sensor's calibration

    uint8_t chip_id;
    uint8_t variant_id;
    char chip_id_text[BME68X_ID_TEXT_LEN];
    char variant_id_text[BME68X_ID_TEXT_LEN];
} bme68x_bsec_source_t;

// Wires `s->port` up as a sensor_source_t named "bme68x_bsec", with
// temperature, humidity, pressure, gas-resistance and IAQ capabilities. Touches
// neither the bus nor BSEC: init() does that. `s` and everything `cfg` points
// to must outlive the source. HAL_ERR_INVALID_ARG on a NULL bus or clock.
hal_status_t bme68x_bsec_source_setup(bme68x_bsec_source_t *s,
                                      const bme68x_bsec_source_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_BME68X_BME68X_BSEC_SOURCE_H
