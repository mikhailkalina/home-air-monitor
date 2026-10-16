// SPDX-License-Identifier: Apache-2.0
//
// The switched 3V3 rail, raised without EPDiy.
//
// On this board the sensors run from the switched 3V3 behind the white 4-pin
// connector, which PWR_EN -- output QP5 of the 74HCT4094 that also carries
// the panel's control lines -- switches (docs/hardware/board_notes.md, "Power
// rails and connectors"). EPDiy's init raises it in the normal firmware, and
// keeps it raised. Two builds never run EPDiy's init and so never raise it:
// CONFIG_HAC_DISPLAY_NULL, and the on-target test app. Until the rail is up
// the sensors sit on the bus unpowered and hold both lines low, which takes
// the RTC and the touch controller down with them -- so those builds call
// this before anything touches the bus.
//
// What it latches is the state EPDiy's epd_poweroff() leaves the register in
// (epd_board_lilygo_t5_47_s3.c): PWR_EN set, the panel's output enable, mode
// and start pulse low, and EPDiy's three rail bits as it leaves them -- which
// drive nothing on this revision. With output enable and the start pulse low
// nothing reaches the glass, though the panel's high-voltage boost runs while
// PWR_EN is up, as it does between EPDiy's refreshes.
//
// Not for a build that runs EPDiy: EPDiy owns the register there, and its
// next write would replace this one anyway.

#ifndef HAC_PLATFORM_ESP32_T5S3_BOARD_POWER_H
#define HAC_PLATFORM_ESP32_T5S3_BOARD_POWER_H

#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shifts the register out with PWR_EN set and latches it. The rail then
// rises; the caller waits BOARD_SWITCHED_3V3_SETTLE_MS before touching the
// bus, as after EPDiy's init. `log` may be NULL.
void board_power_switched_3v3_on(const port_log_t *log);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_POWER_H
