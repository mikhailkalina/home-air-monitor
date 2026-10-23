// SPDX-License-Identifier: Apache-2.0
//
// The board's one user button, BOARD_BUTTON_GPIO (IO21, `S4` on the v2.4
// schematic): low while pressed, pulled up to the always-on VDD3V3 by `R8`
// (100 kOhm) with `C13` (4.7 uF) to ground -- read from the schematic, not
// measured. First used by the card's USB disk mode
// (docs/adr/0036-the-card-as-a-usb-disk.md), which samples it from the card's
// task; debouncing is that caller's (usb_disk_pure.h).

#ifndef HAC_PLATFORM_ESP32_T5S3_BOARD_BUTTON_H
#define HAC_PLATFORM_ESP32_T5S3_BOARD_BUTTON_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The pin as an input, with the S3's own pull-up beside R8: it keeps the
// level defined should R8 be missing on some unit, and shortens the release
// C13 makes slow. No interrupt.
esp_err_t board_button_init(void);

// The level now: true while pressed.
bool board_button_pressed(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_BUTTON_H
