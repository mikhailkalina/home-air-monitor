// SPDX-License-Identifier: Apache-2.0
//
// The numbers bme68x_bsec_source.c works with beyond bme68x_limits.h's, in one
// place, marked as bme68x_limits.h marks its own (DATASHEET, ESTIMATE,
// CHOICE), plus:
//
//   BSEC      Bosch's BSEC 2.6.1.0, its headers or its integration guide
//             (BST-BME-Integration-Guide-AN011-61, 2024-07-25)
//   MEASURED  read off the library itself
//
// No Bosch header is included here, so the composition can include this
// file's source header without Bosch's include paths. Where a number here is
// one of Bosch's macros, bme68x_bsec_source.c checks the two agree at compile
// time.

#ifndef HAC_DRIVERS_BME68X_BME68X_BSEC_LIMITS_H
#define HAC_DRIVERS_BME68X_BME68X_BSEC_LIMITS_H

// CHOICE, within BSEC: low-power mode, a measurement every 3 s
// (BSEC_SAMPLE_RATE_LP, 0.33333 Hz). The configuration cmake/bsec.cmake
// links is the one for that rate ("_3s_"), and the two must agree. The
// alternative, ultra-low-power, measures every 300 s: BSEC's IAQ would be as
// fresh as a five-minute-old reading, and so would temperature and humidity,
// which come from the same measurement. The composition registers the source
// with the sensor manager at this interval (docs/adr/0020-bsec-iaq.md).
#define HAC_BSEC_PERIOD_MS 3000u

// BSEC, integration guide 3.2 and 3.3.4.1: a call of bsec_sensor_control()
// may come at most 6.25 % of the period after the time BSEC asked for --
// 187.5 ms in low-power mode -- before BSEC answers
// BSEC_W_SC_CALL_TIMING_VIOLATION. Kept here only to say what "late" means
// in the log; the source never decides anything by it.
#define HAC_BSEC_LATE_TOLERANCE_MS 187u

// MEASURED: one instance of BSEC 2.6.1.0's IAQ solution, in bytes:
// bsec_get_instance_size_m() on the x86-64 Linux library, and the 1296 bytes
// of .bss Bosch's own size log lists for the ESP32-S3 library
// (bin/esp/esp32_s3/libalgobsec.a.Size.log). Each source holds its own
// instance through BSEC's multi-instance interface, so no two sources share
// a global (a test can make as many as it likes). init() refuses to run with
// HAL_ERR_NO_MEM if the library linked asks for more.
#define HAC_BSEC_INSTANCE_BYTES 1296u

// BSEC: BSEC_MAX_STATE_BLOB_SIZE, the most bsec_get_state_m() returns. The
// source keeps BSEC's state in RAM across a second init() -- a sensor that
// failed and is started over keeps what BSEC has learnt since boot -- and
// nowhere else yet (board_notes.md, open item "BSEC's state restarts at every
// boot").
#define HAC_BSEC_STATE_BYTES 180u

// ESTIMATE: how much warmer the board and its surroundings keep the sensor
// than the room, beyond the sensor's own heater -- which BSEC compensates for
// itself, by its configuration's mode and supply. BSEC subtracts this from its
// heat-compensated temperature (BSEC_INPUT_HEATSOURCE) and moves humidity to
// match. Not measured on this board: 0 until the temperature comparison of
// board_notes.md's open item "Phase 3c's measurements" -- the BME68x beside a
// reference thermometer -- gives it a value. Bosch's own example uses the
// offset of its own application board, which is not this one.
#define HAC_BSEC_DEVICE_HEAT_C 0.0f

#endif  // HAC_DRIVERS_BME68X_BME68X_BSEC_LIMITS_H
