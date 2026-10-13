// SPDX-License-Identifier: Apache-2.0
//
// The arithmetic and the decisions platform/esp32_t5s3/adp_i2c.c makes about
// the ESP-IDF i2c_master driver, kept free of ESP-IDF headers so the host
// build can test them (tests/unit/test_adp_i2c_pure.c), the way
// apps/firmware_esp32/main/duration_stats.c is. Everything that has to touch
// the driver stays in adp_i2c.c.
//
// Two driver behaviours, both read from ESP-IDF 5.5's
// components/esp_driver_i2c/i2c_master.c, make these necessary -- see
// docs/adr/0011-i2c-bus-adapter.md:
//
//   * every wait inside the driver is pdMS_TO_TICKS(xfer_timeout_ms), which
//     rounds DOWN. At the board's 100 Hz tick a 1-9 ms timeout becomes 0
//     ticks: the driver starts the transfer, polls its event queue once
//     without waiting, and reports failure while the hardware is still
//     clocking. The same truncation, and the same one-tick fix, as
//     docs/adr/0006 for vTaskDelay().
//   * a failed i2c_master_transmit/receive/transmit_receive returns
//     ESP_ERR_INVALID_STATE for an address NACK, a data NACK, a hardware SCL
//     timeout, lost arbitration and the driver's own software timeout alike
//     (s_i2c_transaction_start). The return code alone cannot say which
//     port_i2c error it was; a follow-up address-only probe can.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_I2C_PURE_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_I2C_PURE_H

#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// The largest wait handed to the driver, whose timeout parameter is an int
// (and -1 there means "forever", which a deadline never should).
#define ADP_I2C_DRIVER_WAIT_MAX_MS 0x7FFFFFFFu

// The xfer_timeout_ms to hand the driver when `remaining_us` of the caller's
// deadline is left, at a scheduler tick rate of `tick_hz`. Never shorter
// than what remains: rounded up to whole ticks, plus one tick for the
// partial tick already under way when the driver starts waiting (ADR 0006),
// expressed back in milliseconds so that the driver's own pdMS_TO_TICKS()
// recovers at least that tick count. 0 when nothing remains -- the caller's
// deadline has passed and no wait should start. `tick_hz` must be non-zero.
uint32_t adp_i2c_driver_wait_ms(uint64_t remaining_us, uint32_t tick_hz);

// What a follow-up address-only probe of the same address found, after a
// transfer failed with ESP_ERR_INVALID_STATE.
typedef enum {
    ADP_I2C_PROBE_NOT_RUN = 0,  // the caller's deadline had already passed
    ADP_I2C_PROBE_ACK,          // ESP_OK: something answers at that address
    ADP_I2C_PROBE_NACK,         // ESP_ERR_NOT_FOUND: nothing answers
    ADP_I2C_PROBE_TIMEOUT,      // ESP_ERR_TIMEOUT: the bus itself is not completing
    ADP_I2C_PROBE_OTHER,        // anything else the driver returned
} adp_i2c_probe_t;

// The port_i2c error for that failed transfer, per ports/port_i2c.h:
//
//   NOT_RUN -> HAL_ERR_TIMEOUT   the deadline is what ended it
//   ACK     -> HAL_ERR_IO        the address answers now, so a later byte failed
//   NACK    -> HAL_ERR_NOT_FOUND the address does not answer at all
//   TIMEOUT -> HAL_ERR_TIMEOUT   stretched or stuck lines, lost arbitration
//   OTHER   -> HAL_ERR_IO
hal_status_t adp_i2c_classify_failure(adp_i2c_probe_t probe);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_I2C_PURE_H
