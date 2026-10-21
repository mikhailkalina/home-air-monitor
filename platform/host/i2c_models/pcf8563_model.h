// SPDX-License-Identifier: Apache-2.0
//
// An NXP PCF8563 on the fake I2C bus that keeps time: its sixteen registers
// behind an auto-incrementing pointer, the time counters ticking once a
// second on the bus's clock, and the STOP bit that holds them -- modelled
// from the datasheet (Rev. 11.1, 19 January 2026), independently of the
// driver under test (drivers/pcf8563/pcf8563.h). Nothing here includes the
// driver's header or shares its constants: where the two disagree a test
// fails, and the datasheet decides.
//
// The fake bus's plain register file (regfile_model.h) is the chip as it
// stands still, which is all reading and setting the time needs. Phase 4f
// sets it to the millisecond and measures its drift against the network's
// time (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md,
// "Amendment: time from the network"), which needs a chip whose seconds
// tick, whose prescaler a write leaves alone and STOP resets, and whose
// crystal is off by some ppm.
//
// What it promises, from the datasheet:
//
//   * the register address increments after every byte read or written
//     (section 8);
//   * the time counters, 02h-08h, count in BCD with the calendar's carries:
//     a 29th of February in every year divisible by 4, 00 included (8.4.4,
//     note [1]); the weekday counts 0-6 alongside the day; the century bit C
//     toggles when the years overflow from 99 to 00 (8.4.7, note [1]);
//   * VL, bit 7 of 02h, is 1 at power-up (Table 8, "startup value") and
//     stays until 02h is written with it clear; Control_status_1 is 08h,
//     TESTC alone (Table 27) -- as this board's chip read before anything
//     wrote it (docs/hardware/board_notes.md, "The RTC on the board");
//   * STOP, bit 5 of 00h, holds the prescaler's upper stages at 0: no tick
//     while it is set. Released, "the first increment of the time circuits
//     is between 0.507813 s and 0.507935 s after the STOP bit is released"
//     (8.10, Table 26), then one a second. Writing the time registers of a
//     running clock does not touch the prescaler: the next tick comes where
//     the old phase puts it;
//   * an access holds the counters, and a tick that falls inside it is
//     counted when it ends (8.5). The model's accesses take no time, so the
//     counters are brought up to the access's moment before it;
//   * the alarms, 09h-0Ch, each off while its AE_x (bit 7) is set: "when all
//     enabled comparisons match for the first time", AF (bit 3 of 01h) sets
//     -- "only on increment to a matched case" (8.6.5), never because a
//     write made them match. With AIE (bit 1 of 01h) set, INT "follows the
//     condition of bit AF" (8.6.5): low, open-drain, until AF is cleared by
//     writing it 0 (Table 6). The model reports INT going low to a callback,
//     with the moment it did -- the tick's, to the microsecond.
//
// Where the datasheet does not say, the model chooses, marked ASSUMED:
//
//   * the time registers read 2000-01-01 00:00:00, a Saturday (6), at
//     power-up -- the datasheet says undefined (Table 27, "x");
//   * the pointer wraps from 0Fh to 00h;
//   * a counter holding something that is not BCD, or out of its range,
//     counts as if it held 0 -- the real chip's behaviour is undefined;
//   * with TEST1 set (the EXT_CLK test mode, 8.9) nothing ticks: no clock
//     is applied to CLKOUT here;
//   * the countdown timer is not modelled: TF never sets;
//   * bits a register does not use keep what was written into them.
//
// The crystal: one second is 1,000,000 us less `ppm` -- a clock `ppm` parts
// per million fast gains that many microseconds a second.

#ifndef HAC_PLATFORM_HOST_I2C_MODELS_PCF8563_MODEL_H
#define HAC_PLATFORM_HOST_I2C_MODELS_PCF8563_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "i2c_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF8563_MODEL_REGISTERS 16u

// 8.10: 507,813 to 507,935 us from the release of STOP to the first tick.
// The model's default is the middle; `first_tick_after_start_us` moves it.
#define PCF8563_MODEL_FIRST_TICK_MIN_US 507813u
#define PCF8563_MODEL_FIRST_TICK_MAX_US 507935u

#define PCF8563_MODEL_SECOND_US 1000000u

typedef struct {
    uint32_t reads;
    uint32_t writes;
    uint32_t ticks;
    uint32_t stops;        // STOP written 1 on a running clock
    uint32_t starts;       // STOP written 0 on a stopped one
    uint32_t time_writes;  // writes that reached any of 02h-08h
    uint32_t alarms;       // AF set by an increment
    uint32_t int_falls;    // INT driven low
} pcf8563_model_stats_t;

// INT went low at `at_us` on the bus's clock, in microseconds.
typedef void (*pcf8563_model_int_fn)(void *ctx, uint64_t at_us);

typedef struct {
    i2c_model_t model;

    uint8_t regs[PCF8563_MODEL_REGISTERS];
    uint8_t pointer;

    // The time of the next tick on the bus's clock, in microseconds, while
    // running (STOP and TEST1 clear).
    uint64_t next_tick_us;

    // Knobs.
    int32_t ppm;                         // the crystal's error; + runs fast
    uint32_t first_tick_after_start_us;  // 8.10's delay; the datasheet's middle

    // Whether the enabled alarms matched at the last look: AF sets only on
    // the increment that makes them match.
    bool alarm_matched;
    bool int_low;
    pcf8563_model_int_fn on_int;  // may be NULL: INT wired to nothing
    void *on_int_ctx;

    pcf8563_model_stats_t stats;
} pcf8563_model_t;

// A chip powered up at `now_ms`: VL set, the power-up time (above), running,
// its first tick `phase_us` (below 1,000,000) later -- where in the second a
// clock that nobody set ticks is whatever its power-up made it.
void pcf8563_model_init(pcf8563_model_t *m, uint64_t now_ms, uint32_t phase_us);

i2c_model_t *pcf8563_model_port(pcf8563_model_t *m);

// Brings the counters up to `now_ms`, as an access does first. For a test
// that looks at `regs` without going through the bus.
void pcf8563_model_advance(pcf8563_model_t *m, uint64_t now_ms);

// Whether the counters are counting: STOP and TEST1 clear.
bool pcf8563_model_running(const pcf8563_model_t *m);

// Whom INT tells when it goes low; NULL for nobody -- the pin not wired.
void pcf8563_model_on_int(pcf8563_model_t *m, pcf8563_model_int_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_I2C_MODELS_PCF8563_MODEL_H
