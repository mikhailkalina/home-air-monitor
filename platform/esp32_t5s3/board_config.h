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

// The two panel limits port_display reports to core/app/update_policy.c.
// They must equal platform/host/epd_emulation.h's
// EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL and
// EPD_SIM_MIN_FULL_REFRESH_INTERVAL_MS, or the device and the simulator run
// different policies; tests/unit/test_panel_limits_agree.c fails the host
// build when they do not. The reasoning lives in epd_emulation.h and
// docs/adr/0008-panel-timing.md, not here.
//
// OBSERVED (phase 2c, 2026-10-13): ten partial refreshes, one below the
// earliest step at which ghosting probes showed anything on this panel, even
// looked at closely.
#define BOARD_EPD_MAX_PARTIAL_REFRESHES_BEFORE_FULL 10u

// A GUARD, NOT A MEASUREMENT: a rate limit on the 3.7 s flashing refresh. No
// stopwatch measures how often a panel "tolerates" a full refresh, the vendor
// gives no figure, and no phase is scheduled to replace it.
#define BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS 60000u

// The refresh timings themselves are not here: nothing on the device consumes
// them. They are measured, and they live where they are used --
// platform/host/epd_emulation.h -- with the raw figures in
// docs/adr/0008-panel-timing.md.

// --- e-paper control: 74HCT4094D shift register --------------------------
//
// src/ed047tc1.h:48-50 (CONFIG_IDF_TARGET_ESP32S3 block). Its pin 13 (QP5,
// PWR_EN) also switches the 3V3 rail behind the white 4-pin connector, the
// blue LED and the panel's high-voltage boost -- which is where phase 3's
// sensors take their power. EPDiy keeps it set, and a hardware reset clears
// it: docs/hardware/board_notes.md "Power rails and connectors".
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

// A CHOICE, not a measurement: standard mode, 100 kHz. It is what the
// vendor's S3 examples run the bus at -- examples/demo/demo.ino:219 calls
// Wire.begin() and nothing anywhere calls setClock(), so Arduino's default
// applies -- and every device on the bus or planned for phase 3 supports it.
// Raising it is a phase-3 decision, once the sensors and their wiring are on
// the bus and can be checked at the higher rate.
#define BOARD_I2C_SCL_HZ 100000u

// How long one address-only probe of a bus scan may take. A probe is ~0.1 ms
// of bus time at 100 kHz; 20 ms is two 10 ms scheduler ticks of margin, so a
// scan of the 112 usable addresses costs well under a second even on a bus
// where nothing answers.
#define BOARD_I2C_PROBE_TIMEOUT_MS 20u

// --- RTC ----------------------------------------------------------------------
// PCF8563, address 0x51 (NXP PCF8563 datasheet: slave address A2h write /
// A3h read). The vendor's S3 demo probes exactly this address
// (examples/demo/demo.ino:223, inside the CONFIG_IDF_TARGET_ESP32S3 block at
// :208). It is the one device on the bus whose address is not in doubt, so
// it is also the on-target port_i2c contract's known-present slave -- the
// boot-time scan shows whether it answers before that contract relies on it.
#define BOARD_RTC_I2C_ADDRESS 0x51u

// An address nothing on this board answers to, for the on-target port_i2c
// contract's "absent device" checks. 0x08 is the SMBus host address, which no
// slave on this board -- nor any sensor phase 3 plans -- uses.
#define BOARD_I2C_ABSENT_TEST_ADDRESS 0x08u

// --- touch panel ------------------------------------------------------------
// IRQ: src/utilities.h:41.
//
// Address OBSERVED 2026-10-13 (phase 2d boot scan, docs/hardware/board_notes.md
// "Touch controller"): a device answers at 0x5D and nothing at 0x5A or 0x14.
// That is a GT911 that latched INT low as it left reset -- the controller the
// vendor's own S3 examples drive (examples/touch/touch.ino:31 and
// examples/demo/demo.ino:32 include TouchDrvGT911.hpp). It replaces the 0x5A
// this file used to carry, which came from src/touch.h:10 -- a header nothing
// in the reference repository uses. An address-only probe cannot name the
// part; reading the GT911's product ID (register 0x8140, "911") can, and is
// left to whichever phase writes the touch driver.
//
// A GT911 takes 0x5D or 0x14 from its INT line's level as its reset rises,
// and on this board reset is a hardware pull-up, so the latch happens at
// power-up and the vendor probes both (touch.ino:109,113; demo.ino:239,243).
// Only 0x5D has been seen on this unit. The other stays named so the bus scan
// keeps reporting it, and so the touch driver can fall back to it as the
// vendor does.
#define BOARD_TOUCH_IRQ_GPIO 47
#define BOARD_TOUCH_I2C_ADDRESS 0x5Du      // GT911, INT low at reset
#define BOARD_TOUCH_ALT_I2C_ADDRESS 0x14u  // GT911, INT high at reset

// --- button, battery -------------------------------------------------------
// src/utilities.h:30,32 (CONFIG_IDF_TARGET_ESP32S3 block).
//
// There is no battery gauge IC on this board: BOARD_BATTERY_ADC_GPIO is a
// plain resistor divider read through the ADC (examples/demo/demo.ino:331,
// "battery_voltage = (v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0)"). The demo
// says the reading needs EPD power on (demo.ino:327-330), but the v2.4
// schematic shows an unswitched divider to ground -- unconfirmed, see
// docs/hardware/board_notes.md "Pin assignments". GPIO14 is an ADC2
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

// --- e-paper: EPDiy integration (docs/adr/0007-epaper-backend.md) ----------
//
// EPDiy owns the EPD pins at build time, not this file. Its board definition
// (`epd_board_lilygo_t5_47_s3`, in the pinned managed component -- see
// apps/firmware_esp32/components/hac_platform/idf_component.yml) carries its
// own CFG_DATA/CFG_CLK/CFG_STR, CKV/STH/CKH and D0..D7 defines, the
// 74HCT4094 push order, the power-on and power-off sequences, and the
// RMT-generated CKV gate clock.
//
// The EPD pin constants above are kept deliberately, for two reasons that
// are not "the code uses them":
//
//   * they are this project's record of the hardware, each citing the
//     reference line it came from, which docs/hardware/board_notes.md is the
//     source of truth for. Deleting them would move that record into a
//     third-party component this repository does not control.
//   * they are the cross-check. Every one of them was compared against
//     EPDiy's board definition for this board when the backend changed, and
//     they agree exactly -- including the silkscreen revision its header
//     names ("Screen-4.7-S3 V2.3/V2.4"). If a future pin bump makes them
//     disagree, the disagreement is a finding about the hardware, not a
//     merge conflict.
//
// Gone with FastEPD, recorded here so a reader of the git history is not left
// wondering where they went: BOARD_EPD_DC_DUMMY_GPIO, the BOARD_EPD_SHIFT_*
// aliases, BOARD_EPD_PIXEL_CLOCK_HZ, BOARD_EPD_DATA_BUS_D0_TO_D7, the eight
// BOARD_EPD_ROW_START_DELAY_*_US values, BOARD_EPD_ROW_CKV_PULSE_US,
// BOARD_EPD_LINE_PADDING_BYTES and BOARD_EPD_VCOM_MV. Every one of them was
// marked "taken from the working reference, not derived" or outright guessed,
// and every one is now EPDiy's own business. That is most of the unmeasured
// surface this board_config.h used to carry.

// The display temperature handed to epd_draw_base(). A PLACEHOLDER, not a
// measurement: EPDiy selects a waveform temperature range with it, and the
// ED047TC1 waveform it ships declares a single range (num_temp_ranges = 1),
// so today every value lands in the same range and the number cannot
// misbehave. It stops being free the moment a waveform with several ranges
// is used.
//
// Replaced in phase 3, which brings the BME68x and therefore a real ambient
// temperature; EPDiy also offers epd_ambient_temperature() for boards with a
// sensor on the panel itself, which this one does not have.
#define BOARD_EPD_ASSUMED_TEMPERATURE_C 25

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_CONFIG_H
