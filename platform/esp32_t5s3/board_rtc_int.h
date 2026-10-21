// SPDX-License-Identifier: Apache-2.0
//
// The PCF8563's INT on IO9 (BOARD_RTC_INT_GPIO), each falling edge stamped
// with esp_timer's time in an interrupt (phase 4f,
// docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md, "Amendment
// (phase 4f): time from the network"). The chip's alarm drives the pin low
// at its own tick, so the stamp is when the RTC ticked, to the interrupt's
// latency -- microseconds -- whatever the tasks are doing then; whoever
// reads it later (apps/firmware_esp32/main/rtc_sync.h) carries it to the
// system time.
//
// Not a port: one board, and the host's PCF8563 model reports its edges
// itself (platform/host/i2c_models/pcf8563_model.h). The wiring -- the
// open-drain INT to IO9, 10 kOhm to VDD3V3 -- is the v2.4 schematic's, read,
// and seen working on 2026-10-22 (docs/hardware/board_notes.md, "The RTC");
// the S3's own pull-up is enabled beside it, harmless if the external one is
// there. IO9
// is one of the S3's RTC GPIOs, so phase 5 can wake from deep sleep on it.
//
// The handler sits on ESP-IDF's shared GPIO ISR service, installed without
// ESP_INTR_FLAG_IRAM (board_gpio_isr.h): while the flash's cache is off -- a
// write to flash -- the interrupt waits, and an edge then is stamped late.
// Nothing writes the flash in the steady state; the radio's calibration data
// is written once, at its first start.

#ifndef HAC_PLATFORM_ESP32_T5S3_BOARD_RTC_INT_H
#define HAC_PLATFORM_ESP32_T5S3_BOARD_RTC_INT_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t count;  // falling edges since board_rtc_int_start()
    uint64_t at_us;  // the last one's esp_timer time; 0 before one
} board_rtc_int_edge_t;

// IO9 an input with its pull-up, interrupting on a falling edge, the handler
// on the shared GPIO ISR service (board_gpio_isr.h). Logs the pin's level,
// which reads high while the alarm's flag is clear.
// HAL_ERR_IO when ESP-IDF refuses any of it; nothing is stamped then. Once.
hal_status_t board_rtc_int_start(const port_log_t *log);

// The edges so far, copied under the handler's lock. From any task.
void board_rtc_int_edge(board_rtc_int_edge_t *out);

// The pin now: low while the RTC's AF and AIE are both set.
bool board_rtc_int_low(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_RTC_INT_H
