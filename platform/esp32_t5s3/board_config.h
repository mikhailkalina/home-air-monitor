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
// THE OWNER'S CHOICE (2026-10-16, docs/adr/0019-partials-past-the-budget.md),
// within what was OBSERVED (phase 2c, ADR 0008): twenty partial refreshes,
// half of the forty the ghosting probes ran with nothing visible from 50 cm.
// Close up, faint outlines of earlier digits appeared from the 11th in the
// worst pattern; phase 2c's budget of 10 sat below that, this one accepts
// them for fewer flashing full refreshes.
#define BOARD_EPD_MAX_PARTIAL_REFRESHES_BEFORE_FULL 20u

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
//
// Kept in phase 3a, the decision this comment used to defer. Nothing needs
// the speed: the SCD4x moves about a dozen bytes every 5 s. And nothing yet
// shows the bus could take 400 kHz: the board's own pull-ups come to about
// 20 kOhm per line (unconfirmed), the modules add theirs, of unknown value,
// and the sensors hang on wires from the 40-pin header -- fast mode's 300 ns
// rise time is not something to assume from that
// (docs/hardware/board_notes.md, "Sensors on the bus"). Raise it only with
// the pull-ups known and the rise time measured.
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

// The RTC's own wiring, read from the v2.4 schematic (docs/hardware/
// board_notes.md, "The RTC"): its VDD is VCC_RTC -- VDD3V3 through a 1N4148
// (D3), with an MS412FE rechargeable cell behind it -- so it is not on
// PWR_EN, no reset stops it, and it should keep time with the board
// unpowered, if the cell is fitted and charged (open item). Its open-drain
// INT goes to IO9, pulled up to VDD3V3 through 10 kOhm: alarm and timer
// interrupts, unused -- a candidate wake source for phase 5's deep sleep.
// CLKOUT is pulled up and goes nowhere this firmware uses.
#define BOARD_RTC_INT_GPIO 9

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

// How the controller's frame lies on the panel's 960x540
// (drivers/gt911/gt911_touch.h: swapped first, then scaled from the
// controller's configured resolution, then mirrored).
//
// THE VENDOR'S CHOICE, NOT YET SEEN ON THIS UNIT: the vendor's S3 touch
// example sets the maximum coordinates to EPD_WIDTH x EPD_HEIGHT, swaps X and
// Y and mirrors Y alone (examples/touch/touch.ino, setSwapXY(true),
// setMirrorXY(false, true) -- read for that fact only: the repository is
// GPL-3.0, docs/adr/0007). A finger at each corner settles it: the console's
// `touch` prints each event raw and mapped (docs/hardware/board_notes.md,
// open item "The touch panel on the board"). Correct here, and there.
#define BOARD_TOUCH_SWAP_XY 1
#define BOARD_TOUCH_MIRROR_X 0
#define BOARD_TOUCH_MIRROR_Y 1

// A CHOICE: the drive strength of the GT911 wake pulse on INT. The pulse is
// sent at every init(), and a controller that was awake drives INT itself:
// for the pulse's 3 ms the two outputs meet, and the weakest of the S3's four
// drive strengths (about 5 mA) bounds what flows. A name ESP-IDF's
// driver/gpio.h defines, expanded only where that is included.
#define BOARD_TOUCH_INT_WAKE_DRIVE GPIO_DRIVE_CAP_0

// --- sensors (phase 3) ------------------------------------------------------
// On the bus through the 40-pin header, powered from the white 4-pin
// connector's switched 3V3 (docs/hardware/board_notes.md, "Power rails and
// connectors" and "Sensors on the bus").
//
// OBSERVED 2026-10-16 (boot scan, ELF SHA256 c59289da4...): both answer --
// 0x62 and 0x77 found, 0x76 silent, nothing unexpected.
//
// SCD4x: 0x62 is also the only address the part has (Sensirion SCD4x
// datasheet v1.7, Table 8); nothing on a module can change it.
#define BOARD_SCD4X_I2C_ADDRESS 0x62u

// BME68x: 0x76 with SDO to GND, 0x77 with SDO to VDDIO (Bosch BME680
// datasheet, "I2C interface"), undefined with SDO left floating. This
// CJMCU-680-style module answers at 0x77, so it straps SDO high. 0x76 stays
// named, as the GT911's second address does: a module re-strapped, or a
// floating SDO on another unit, would show there in the scan.
#define BOARD_BME68X_I2C_ADDRESS 0x77u      // SDO high -- this module
#define BOARD_BME68X_ALT_I2C_ADDRESS 0x76u  // SDO low

// How long after EPDiy's init raises PWR_EN -- and with it the switched 3V3
// the sensors run from -- before the firmware touches the bus. Until the rail
// is up, the unpowered sensors hold both lines low, and touching the bus
// then ended in a reboot loop (2026-10-15, docs/hardware/board_notes.md).
// An ESTIMATE, now seen to be enough: the longest power-up time of a part on
// that rail is the SCD4x's 30 ms (its datasheet, Table 7; the BME68x needs
// 2 ms), and how fast the rail itself rises through Q4 and the modules'
// regulators is not measured. The boot scan of 2026-10-16, 60 ms after the
// display adapter's last log line, found every sensor on the first try. The
// minimum is not measured; a scope on the rail would give it.
#define BOARD_SWITCHED_3V3_SETTLE_MS 50u

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

// --- microSD card slot (phase 3f) ------------------------------------------
// Pins: src/utilities.h:34-37 (CONFIG_IDF_TARGET_ESP32S3 block). The slot's
// wiring, from the v2.4 schematic (docs/hardware/board_notes.md, "The
// microSD slot"): SPI mode only -- DAT1 and DAT2 go nowhere, so the 4-bit
// SDMMC mode is not available; VDD from VDD3V3, so the card is powered
// whenever the board is and not switched by PWR_EN; 10 kOhm pull-ups on CS,
// MOSI and CLK, none fitted on MISO (R14 is NC), so platform/esp32_t5s3/
// adp_sd.c enables the S3's internal one there; card-detect (CD#) reaches
// only test pad TP72, so firmware cannot see a card arrive or leave.
//
// IO15 and IO16 are also pins 2 and 3 of the white connector P5. From phase
// 3f they belong to the card; the sensors use only P5's V and G.
#define BOARD_SD_MISO_GPIO 16
#define BOARD_SD_MOSI_GPIO 15
#define BOARD_SD_SCLK_GPIO 11
#define BOARD_SD_CS_GPIO 42

// The SPI controller the card is on: SPI2 (FSPI), the general-purpose one --
// SPI0/1 run the flash and the PSRAM, EPDiy drives the panel through LCD_CAM,
// and nothing else in this firmware uses SPI. A name ESP-IDF's
// driver/spi_common.h defines, so this expands only where that is included.
#define BOARD_SD_SPI_HOST SPI2_HOST

// A CHOICE, not a measurement: ESP-IDF's SD-over-SPI default, 20 MHz
// (SDMMC_FREQ_DEFAULT). The card is a few centimetres from the S3 on the
// board itself, and the recordings need some kilobytes a minute; nothing
// asks for more, and nothing has shown that less is needed.
#define BOARD_SD_SPI_MAX_KHZ 20000

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

// The display temperature handed to EPDiy's update while no measured one is
// known. A PLACEHOLDER, not a measurement: EPDiy selects a waveform
// temperature range with it, and the ED047TC1 waveform it ships declares a
// single range (num_temp_ranges = 1), so today every value lands in the same
// range and the number cannot misbehave. It stops being free the moment a
// waveform with several ranges is used.
//
// Since phase 3c only the fallback: the firmware's composition hands the
// sensor manager's temperature -- the BME68x's, or the SCD41's when it is
// down -- to adp_display_set_temperature() after every poll, and this
// applies only before the first reading and while no source has a fresh
// one. EPDiy also offers epd_ambient_temperature() for boards with a sensor
// on the panel itself, which this one does not have.
#define BOARD_EPD_ASSUMED_TEMPERATURE_C 25

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_CONFIG_H
