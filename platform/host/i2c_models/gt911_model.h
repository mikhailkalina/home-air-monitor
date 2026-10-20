// SPDX-License-Identifier: Apache-2.0
//
// A Goodix GT911 on the fake I2C bus: 16-bit register addresses, the status
// register's buffer-ready bit and the host clearing it, point records with
// track IDs, sleep and the INT pulse that ends it -- modelled from Goodix's
// documents (drivers/gt911/gt911.h names them), independently of the driver
// under test. Nothing here includes the driver's header or shares its
// constants: where the two disagree a test fails, and the documents decide.
//
// What it promises, from the GT911 datasheet Rev.10:
//
//   * a write is a register address, high byte first, then data, the
//     pointer advancing after every byte; a read starts at the pointer the
//     last write set (section 6).
//   * sleep: the command 5 to 0x8040 "requires INT output low before the
//     command"; the host ends it by driving INT high for 2 to 5 ms, no
//     sooner than 58 ms after the command (8.1 d).
//   * while touched, a report in every scan cycle, each announced by a pulse
//     on INT (8.2) -- here gt911_model_touch() and gt911_model_scan(), and
//     the interrupt callback.
//
// From the programming guide's register map: the status register 0x814E
// (bit 7 a report is ready, bits 3:0 its points), the records from 0x814F,
// eight bytes each, the product ID at 0x8140, the configuration at
// 0x8047-0x8100 with its checksum at 0x80FF.
//
// Where neither says, the model chooses, marked ASSUMED where it does it:
//
//   * a report is held until the host writes 0x814E; one produced meanwhile
//     waits, and is latched -- the newest points -- when the host clears.
//   * the lift of the last finger is a report of its own, with no points.
//   * a write into the configuration area has its data byte refused
//     (HAL_ERR_IO), and is counted. The real part would accept it and keep
//     it in flash; the model refuses so that no test can pass by writing
//     the vendor's calibration -- the driver must never (gt911.h).
//   * the sleep command with INT not held low is ignored.
//   * asleep, the address is refused -- seen on the board since
//     (2026-10-20: a bus scan of a sleeping controller found nothing at
//     either address). A pulse shorter than 2 ms, or one sooner than 58 ms
//     after the command, does not wake it.

#ifndef HAC_PLATFORM_HOST_I2C_MODELS_GT911_MODEL_H
#define HAC_PLATFORM_HOST_I2C_MODELS_GT911_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "i2c_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// The datasheet's figures this model keeps time by.
#define GT911_MODEL_WAKE_PULSE_MIN_US 2000u
#define GT911_MODEL_WAKE_PULSE_MAX_US 5000u
#define GT911_MODEL_SLEEP_TO_WAKE_MS 58u

#define GT911_MODEL_MAX_POINTS 5u

// The configuration area, 0x8047-0x8100.
#define GT911_MODEL_CONFIG_FIRST 0x8047u
#define GT911_MODEL_CONFIG_LEN 186u

// What the model reports by default: this board's controller, as its boot
// log read it on 2026-10-17 (docs/hardware/board_notes.md, "The touch panel
// on the board") -- firmware 0x1060, configuration version 0x42, a portrait
// frame 540 x 960, two touch points, Module_Switch1 0x05: bits 1:0 = 01, a
// falling edge. The rest of the configuration area is zeros here, the
// checksum made right for them: the board's own bytes were not dumped.
#define GT911_MODEL_DEFAULT_X_MAX 540u
#define GT911_MODEL_DEFAULT_Y_MAX 960u
#define GT911_MODEL_DEFAULT_FIRMWARE 0x1060u
#define GT911_MODEL_DEFAULT_CONFIG_VERSION 0x42u
#define GT911_MODEL_DEFAULT_TOUCH_NUMBER 2u
#define GT911_MODEL_DEFAULT_MODULE_SWITCH1 0x05u  // bits 1:0 = 01: falling edge

typedef struct {
    uint8_t track_id;
    uint16_t x;
    uint16_t y;
    uint16_t size;
} gt911_model_point_t;

typedef enum {
    GT911_MODEL_INT_RELEASED = 0,  // an input on the host's side: the GT911's own output
    GT911_MODEL_INT_HOST_LOW,
    GT911_MODEL_INT_HOST_HIGH,
} gt911_model_int_t;

typedef struct {
    uint32_t config_writes;    // writes that reached the configuration area, all refused
    uint32_t status_clears;    // writes to 0x814E
    uint32_t reports;          // reports latched for the host
    uint32_t reports_dropped;  // lost by gt911_model_drop_reports()
    uint32_t interrupts;       // pulses on INT: reports and spurious ones
    uint32_t sleeps;
    uint32_t wakes;
    uint32_t ignored_sleep_commands;  // INT was not held low
    uint32_t early_wake_pulses;       // sooner than 58 ms after the command
    uint32_t short_wake_pulses;       // under 2 ms
    uint32_t long_wake_pulses;        // over 5 ms: they wake it, but are counted
    uint32_t pulses_while_awake;
    uint32_t address_nacks;
    uint32_t last_pulse_us;
} gt911_model_stats_t;

typedef void (*gt911_model_interrupt_fn)(void *ctx);

typedef struct {
    i2c_model_t model;

    uint8_t config[GT911_MODEL_CONFIG_LEN];
    char product_id[4];
    uint16_t firmware;
    uint8_t vendor_id;

    uint16_t pointer;
    uint8_t command;  // 0x8040 as last written

    bool asleep;
    uint64_t slept_at_ms;
    uint64_t answers_from_ms;  // the address is refused before this
    uint32_t answer_delay_ms;  // knob: how long after a wake it refuses its address
    gt911_model_int_t int_level;

    // What the panel feels now.
    gt911_model_point_t touching[GT911_MODEL_MAX_POINTS];
    size_t touching_count;

    // The report the host can read.
    bool buffer_ready;
    gt911_model_point_t buffer[GT911_MODEL_MAX_POINTS];
    uint8_t buffer_count;
    bool pending;  // a report waits for the host to clear the buffer

    unsigned drop_next;

    gt911_model_interrupt_fn on_interrupt;
    void *on_interrupt_ctx;

    gt911_model_stats_t stats;
} gt911_model_t;

// Awake, untouched, the default configuration with a correct checksum.
void gt911_model_init(gt911_model_t *m);

i2c_model_t *gt911_model_port(gt911_model_t *m);

// Called for every pulse on INT. May be NULL.
void gt911_model_set_interrupt(gt911_model_t *m, gt911_model_interrupt_fn fn, void *ctx);

// After a change to m->config: Config_Chksum made right again.
void gt911_model_fix_checksum(gt911_model_t *m);

// --- the host's side of INT --------------------------------------------------------

void gt911_model_int_drive_low(gt911_model_t *m, uint64_t now_ms);
void gt911_model_int_release(gt911_model_t *m, uint64_t now_ms);
// High for `us`, starting at `now_ms`, then released.
void gt911_model_int_pulse_high(gt911_model_t *m, uint32_t us, uint64_t now_ms);

// --- what the panel feels ----------------------------------------------------------

// The fingers on the glass are now `points` (`count` of them, at most
// GT911_MODEL_MAX_POINTS), and a scan reports them: a finger down, moving or
// lifted -- `count` 0 is the last one lifted.
void gt911_model_touch(gt911_model_t *m, const gt911_model_point_t *points, size_t count);

// One more scan cycle: while touched, another report of the same points.
void gt911_model_scan(gt911_model_t *m);

// The next `n` reports are lost: neither latched nor announced.
void gt911_model_drop_reports(gt911_model_t *m, unsigned n);

// A pulse on INT with no report behind it: the host's read finds nothing.
void gt911_model_spurious_interrupt(gt911_model_t *m);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_I2C_MODELS_GT911_MODEL_H
