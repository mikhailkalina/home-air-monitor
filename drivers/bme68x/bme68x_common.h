// SPDX-License-Identifier: Apache-2.0
//
// What the two BME68x sources share: bme68x_source.h, which measures in
// forced mode on a heater profile of its own, and bme68x_bsec_source.h, which
// measures when and how Bosch's BSEC library asks (docs/adr/0020-bsec-iaq.md).
// Bringing the sensor up through Bosch's Sensor API over port_i2c, reading
// Bosch's results as the port saw them, and the lines both log. Not a source
// of its own, and nothing here keeps state.
//
// Bosch's bme68x_set_op_mode(), which every configuration call goes
// through, waits for the sensor to report sleep mode with no bound on how
// long. A sensor that never finished a measurement would hang it. So both
// sources read the mode first (bme68x_check_asleep()), and call into that
// path only once they have seen the sensor asleep.

#ifndef HAC_DRIVERS_BME68X_BME68X_COMMON_H
#define HAC_DRIVERS_BME68X_BME68X_COMMON_H

#include <stdint.h>

#include "bme68x.h"
#include "bme68x_hal.h"
#include "hal_status.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// "0x" and two hex digits, for the log.
#define BME68X_ID_TEXT_LEN 5u

// `id` as "0x61".
void bme68x_format_id(uint8_t id, char out[BME68X_ID_TEXT_LEN]);

// "BME680", "BME688" or "unknown", from the variant_id register (0xF0).
// The BME688 datasheet gives 0x01 for itself; the BME680's datasheet has no
// such register, and Bosch's API reads 0x00 there as a BME680.
const char *bme68x_variant_name(uint8_t variant_id);

// What one of Bosch's int8_t results was, as the port saw it:
// BME68X_E_COM_FAIL becomes the port's own status (bme68x_hal_status()), a
// wrong chip id HAL_ERR_UNSUPPORTED, no new data HAL_ERR_NOT_READY.
hal_status_t bme68x_vendor_status(const struct bme68x_dev *dev, int8_t rslt);

// A short, stable name for a status, for log lines. Never NULL.
const char *bme68x_status_name(hal_status_t status);

// Brings the sensor up: waits its start-up time (bme68x_limits.h), points
// `dev` at `hal` -- whose bus, clock and address the caller has set -- and
// runs Bosch's bme68x_init(): soft reset, chip id, variant id, calibration.
// Then checks the sensor is asleep, as the reset leaves it. On a failure
// `*failed_step` names the step: "init" (Bosch's init, HAL_ERR_UNSUPPORTED
// when something else answers at the address) or "reset" (the sensor is not
// asleep after it, HAL_ERR_IO). dev->chip_id holds what was read either way.
hal_status_t bme68x_bring_up(struct bme68x_dev *dev, bme68x_hal_t *hal, const char **failed_step);

// HAL_OK when the sensor reports sleep mode, HAL_ERR_BUSY while it is still
// measuring, or the bus's failure.
hal_status_t bme68x_check_asleep(struct bme68x_dev *dev);

// The sensor's calibration NVM, in five INFO lines named as the host's BME680
// model names them (platform/host/i2c_models/bme680_model.h), so a unit's set
// can become the model's. Five lines, as a log line carries six fields at
// most.
void bme68x_log_calibration(const port_log_t *log, const char *tag,
                            const struct bme68x_calib_data *c);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_BME68X_BME68X_COMMON_H
