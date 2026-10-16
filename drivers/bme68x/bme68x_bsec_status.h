// SPDX-License-Identifier: Apache-2.0
//
// What BSEC's own verdicts on its outputs mean as a value_status_t
// (docs/adr/0020-bsec-iaq.md, "Accuracy"). Pure, and free of Bosch's headers,
// so it builds and is tested in every build -- CI's included, which never has
// the library -- while bme68x_bsec_source.c, which calls it, builds only with
// BSEC.
//
// IAQ. BSEC returns an accuracy with each IAQ value (bsec_output_t::accuracy;
// BSEC 2.6.1.0's bsec_datatypes.h and its integration guide, 5.3.2.1):
//
//   0  "Stabilization / run-in ongoing"            -> VAL_WARMUP
//   1  low: not yet trimmed against clean and      -> VAL_OK
//      polluted air
//   2  medium: auto-trimming ongoing               -> VAL_OK
//   3  high                                        -> VAL_OK
//   anything else: not a value BSEC defines        -> VAL_ERROR
//
// Only 0 is a warm-up: the index BSEC reports then is a placeholder, and the
// state ends by itself within minutes of each start. 1 to 3 say how far BSEC's
// background calibration has come -- 1 until it has seen the room's air both
// clean and polluted, which can take hours or days, and which restarts at
// every boot while BSEC's state is not kept (board_notes.md, open item "BSEC's
// state restarts at every boot"). Treating 1 as a warm-up would leave the IAQ
// cell saying so for most of every day; the index BSEC computes at 1 is real,
// only not yet scaled to this room. The accuracy itself is logged on every
// change. It also travels with the value, as value_accuracy_t
// (core/domain/measurement.h, docs/adr/0021): 0 ACC_UNRELIABLE, 1 ACC_LOW,
// 2 ACC_MEDIUM, 3 ACC_HIGH, and the screen marks a number at 1 with a
// question mark and says "IAQ not calibrated".
//
// Gas resistance. BSEC passes the raw value through, and reports the gas
// element's own state in two outputs, its stabilization status and its run-in
// status (BSEC_OUTPUT_STABILIZATION_STATUS, BSEC_OUTPUT_RUN_IN_STATUS):
//
//   not marked valid by the sensor                 -> VAL_ERROR
//   valid, either status still 0                   -> VAL_WARMUP
//   valid, both 1                                  -> VAL_OK
//
// BSEC's run-in replaces the forced-mode driver's fixed burn-in estimate
// (HAC_BME68X_GAS_BURN_IN_MS) in a build with BSEC.

#ifndef HAC_DRIVERS_BME68X_BME68X_BSEC_STATUS_H
#define HAC_DRIVERS_BME68X_BME68X_BSEC_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// bsec_output_t::accuracy, as BSEC 2.6.1.0 defines it.
#define BME68X_BSEC_ACCURACY_UNRELIABLE 0u
#define BME68X_BSEC_ACCURACY_LOW 1u
#define BME68X_BSEC_ACCURACY_MEDIUM 2u
#define BME68X_BSEC_ACCURACY_HIGH 3u

// The status an IAQ value with this accuracy is reported with (above).
value_status_t bme68x_bsec_iaq_status(uint8_t accuracy);

// The same accuracy on the core's scale (above); ACC_NOT_REPORTED for a value
// BSEC does not define.
value_accuracy_t bme68x_bsec_iaq_accuracy(uint8_t accuracy);

// The status of the raw gas resistance, given whether the sensor marked the
// gas conversion valid and BSEC's two statuses of the gas element (above).
value_status_t bme68x_bsec_gas_status(bool gas_valid, bool stabilized, bool run_in_done);

// "unreliable", "low", "medium", "high" or "?", for the log.
const char *bme68x_bsec_accuracy_name(uint8_t accuracy);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_BME68X_BME68X_BSEC_STATUS_H
