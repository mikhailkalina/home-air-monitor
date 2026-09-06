// SPDX-License-Identifier: Apache-2.0
//
// Every constant specific to this board, in one place, each citing where it
// came from. Nothing else in the codebase may hardcode a pin, a panel
// dimension or an I2C address for this board -- ./scripts/check_sources.sh
// enforces that a GPIO_NUM_ or BOARD_*_GPIO literal appears only here.
//
// Board identity (docs/hardware/board_notes.md is the source of truth for
// anything physical -- update it first, then cite the update here):
//
//   Product:  LILYGO T5 4.7-inch e-Paper, ESP32-S3, touch version (SKU H716)
//   Revision: Screen-4.7-S3 v2.4, 2024-12-03 (silkscreen -- the authority;
//             the vendor product page is not always current, see
//             docs/hardware/board_notes.md "Identification")
//   MCU:      ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
//   Panel:    ED047TC1, 960x540 landscape as the driver presents it (the
//             vendor's "540x960" is the glass datasheet orientation, not
//             this one -- docs/hardware/board_notes.md "Display orientation")
//
// A second unit, or a later silkscreen revision, is not guaranteed to share
// any of this: re-verify against docs/hardware/board_notes.md before reusing
// this file for different hardware.
//
// Pin and bus sources: Xinyuan-LilyGO/LilyGo-EPD47, branch esp32s3, commit
// 391b0e25d7a39897e3a00af34053250df031d699 (2026-08-19) -- the same commit
// docs/hardware/board_notes.md cites for the panel orientation. Every pin
// below is cross-checked against two independent places in that repository:
// a driver header (giving line numbers) and the vendor's own "GPIO List"
// table in README.MD (giving no lines, but written by LilyGO, not
// reverse-engineered), and the two never disagree. See
// docs/hardware/board_notes.md "Pin assignments" for the full table and the
// reasoning; this file only carries the constants forward.

#ifndef HAC_PLATFORM_ESP32_T5S3_BOARD_CONFIG_H
#define HAC_PLATFORM_ESP32_T5S3_BOARD_CONFIG_H

#include <stdint.h>

// --- board identity ---------------------------------------------------------

#define BOARD_SILKSCREEN_REVISION "Screen-4.7-S3 v2.4, 2024-12-03"

// --- panel geometry -----------------------------------------------------
//
// docs/hardware/board_notes.md "Display orientation": matches EPD_WIDTH /
// EPD_HEIGHT in src/epd_driver.h:28,33 on the reference branch. GRAY4 is the
// only format this panel supports (16 grey levels).

#define BOARD_EPD_WIDTH 960u
#define BOARD_EPD_HEIGHT 540u

// GUESSED -- these two are not yet measured on this board (phase 2c, with a
// scope and a stopwatch on the panel, replaces them). They are kept equal to
// platform/host/epd_emulation.h's EPD_SIM_MIN_FULL_REFRESH_INTERVAL_MS and
// EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL on purpose, so update_policy
// behaves identically on the device and in the simulator until a real
// measurement exists. If phase 2c changes one, change the other in the same
// commit and say so in docs/hardware/board_notes.md, or the two stop meaning
// the same thing.
#define BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS 60000u
#define BOARD_EPD_MAX_PARTIAL_REFRESHES_BEFORE_FULL 20u

// --- e-paper control: 74HCT4094D shift register --------------------------
//
// src/ed047tc1.h:48-50 (CONFIG_IDF_TARGET_ESP32S3 block). Also drives panel
// power (README.MD FAQ 1: "the voltage for the Molex connector is controlled
// by Pin 13 of the 74HCT4094 ... software must call epd_poweron() to enable
// it") and the board's one status LED -- there is no separate LED GPIO.
//
// CFG_STR is GPIO0, the boot-mode strapping pin: driving it is safe once the
// system has booted, but adp_display_null.c never touches these pins at all
// (it drives no hardware), so this matters starting in phase 2b.
#define BOARD_EPD_CFG_DATA_GPIO 13
#define BOARD_EPD_CFG_CLK_GPIO 12
#define BOARD_EPD_CFG_STR_GPIO 0

// --- e-paper control lines -------------------------------------------------
// src/ed047tc1.h:53,54,57 (CONFIG_IDF_TARGET_ESP32S3 block).
#define BOARD_EPD_CKV_GPIO 38
#define BOARD_EPD_STH_GPIO 40
#define BOARD_EPD_CKH_GPIO 41

// --- e-paper 8-bit parallel data bus ---------------------------------------
// src/ed047tc1.h:60-67 (CONFIG_IDF_TARGET_ESP32S3 block).
#define BOARD_EPD_D0_GPIO 8
#define BOARD_EPD_D1_GPIO 1
#define BOARD_EPD_D2_GPIO 2
#define BOARD_EPD_D3_GPIO 3
#define BOARD_EPD_D4_GPIO 4
#define BOARD_EPD_D5_GPIO 5
#define BOARD_EPD_D6_GPIO 6
#define BOARD_EPD_D7_GPIO 7

// --- shared I2C bus: RTC (PCF8563) + touch + our sensors -------------------
// src/utilities.h:39-40 (CONFIG_IDF_TARGET_ESP32S3 block), matching
// README.MD's "RTC (Touchscreen) SDA/SCL" row exactly.
#define BOARD_I2C_SDA_GPIO 18
#define BOARD_I2C_SCL_GPIO 17

// --- touch panel ------------------------------------------------------------
// IRQ: src/utilities.h:41. Address: src/touch.h:10 (TOUCH_SLAVE_ADDRESS).
//
// NOT a GT911: docs/architecture.md 6.2 and docs/requirements.md 6.2 both
// assume GT911, which answers at 0x5D/0x14 with a different register map.
// The reference driver (src/touch.cpp) uses a 0xD0/0xD1 register protocol at
// this address instead. The address is settled; the actual part number is
// still an open item in docs/hardware/board_notes.md.
#define BOARD_TOUCH_IRQ_GPIO 47
#define BOARD_TOUCH_I2C_ADDRESS 0x5Au

// --- button, battery -------------------------------------------------------
// src/utilities.h:30,32 (CONFIG_IDF_TARGET_ESP32S3 block).
//
// There is no battery gauge IC on this board: BOARD_BATTERY_ADC_GPIO is a
// plain resistor divider read through the ADC (examples/demo/demo.ino:331,
// "battery_voltage = (v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0)"), and the
// divider only carries a valid reading while EPD power is on, because it
// hangs off the same 74HCT4094 rail (demo.ino:327-330). GPIO14 is an ADC2
// input on the S3, which the IDF documents as unreliable while Wi-Fi is
// active -- open item for phase 3/4 in docs/hardware/board_notes.md.
#define BOARD_BUTTON_GPIO 21
#define BOARD_BATTERY_ADC_GPIO 14

// --- SD card (free for other use if no card is fitted) ---------------------
// src/utilities.h:34-37 (CONFIG_IDF_TARGET_ESP32S3 block). Not used in this
// phase; recorded because these four GPIOs are claimed the moment an SD
// card is present, and any future use of them elsewhere must know that.
#define BOARD_SD_MISO_GPIO 16
#define BOARD_SD_MOSI_GPIO 15
#define BOARD_SD_SCLK_GPIO 11
#define BOARD_SD_CS_GPIO 42

// --- genuinely free GPIOs ---------------------------------------------------
// README.MD "GPIO List": marked free (checked) with no on-board connection.
// GPIO10 is additionally noted there as usable for analog input only.
#define BOARD_FREE_GPIO_1 45
#define BOARD_FREE_GPIO_2 10  // analog input only, per README.MD
#define BOARD_FREE_GPIO_3 48
#define BOARD_FREE_GPIO_4 39

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_CONFIG_H
