// SPDX-License-Identifier: Apache-2.0
//
// ESP-IDF's shared GPIO ISR service, installed once for every adapter whose
// interrupt sits on it -- the touch controller's INT (adp_input.c) and, since
// phase 4f, the RTC's (board_rtc_int.c). ESP-IDF logs an error when the
// service is installed a second time and offers no way to ask whether it is,
// so each adapter asks here; the firmware asks first, on app_main before the
// touch task exists (main.c, start_touch()), so that no two first calls race.
// Seen on the board 2026-10-22: the RTC's adapter installing it first, the
// touch task's later call logged "GPIO isr service already installed" at
// every boot.
//
// Its flags are none -- any priority level, a handler not in IRAM: while the
// flash's cache is off, every handler on it waits.

#ifndef HAC_PLATFORM_ESP32_T5S3_BOARD_GPIO_ISR_H
#define HAC_PLATFORM_ESP32_T5S3_BOARD_GPIO_ISR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Installs the service unless this has already; ESP_OK once it is there --
// installed here or, ESP_ERR_INVALID_STATE from ESP-IDF, by someone else --
// else ESP-IDF's refusal. From any task, but the first call should not race
// another.
esp_err_t board_gpio_isr_service(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_GPIO_ISR_H
