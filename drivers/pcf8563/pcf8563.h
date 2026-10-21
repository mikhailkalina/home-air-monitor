// SPDX-License-Identifier: Apache-2.0
//
// The PCF8563 real-time clock over port_i2c: read the time, set the time --
// since phase 4f also to a second's start -- and arm its alarm, whose INT
// marks the clock's own tick.
// Plain functions, not a port -- there is one RTC, on the board, and the
// simulator's wall time comes from adp_clock_virtual, so there is no second
// implementation for a port to abstract over (CLAUDE.md). Like the sensor
// drivers it includes nothing but ports/, so it builds and is tested on a PC
// against the fake bus, whose register file is this chip as it is: a pointer
// set by the first byte of a write, auto-incremented by every byte after it.
//
// The clock holds UTC, never local time. Local time is the core's business
// (core/domain/civil_time.h), from a rule that knows summer time; a clock
// that held local time would be an hour wrong twice a year until someone set
// it again.
//
// Registers (NXP PCF8563 datasheet, register descriptions; the masks and the
// VL and century bits as Linux's drivers/rtc/rtc-pcf8563.c reads them),
// time fields in BCD:
//
//   0x00 Control_status_1  TEST1 (7), STOP (5), TESTC (3); 0 is normal
//                          running. STOP set holds the clock's divider
//                          chain: the time does not advance.
//   0x02 VL_seconds        VL (7): the clock's integrity is not guaranteed --
//                          its supply fell below what the oscillator needs
//                          since the time was last written; only writing the
//                          register clears it. Seconds 0-59 in 6:0.
//   0x03 Minutes           0-59 in 6:0
//   0x04 Hours             0-23 in 5:0
//   0x05 Days              1-31 in 5:0
//   0x06 Weekdays          0-6 in 2:0 -- a counter the chip only increments
//   0x07 Century_months    C (7); months 1-12 in 4:0
//   0x08 Years             0-99
//   0x01 Control_status_2  AF (3), AIE (1); TI_TP (4), TF (2) and TIE (0)
//                          are the timer's, never used here. Writing 0 to
//                          AF clears it; 1 leaves it (Table 6).
//   0x09-0x0C              the minute, hour, day and weekday alarms, each
//                          AE_x in bit 7 -- 1 is off (Tables 18-21)
//
// Years are 2000-2099: the years register and nothing else. The century bit
// is not read: its polarity differs between this chip and its look-alikes
// (the datasheet's 0 = 20xx, the RTC8564's set-in-2000), Linux guesses it
// per chip, and the vendor's firmware may have written either. The chip adds
// a 29th of February in every year divisible by 4, 00 included, which is
// right for exactly 2000-2099. The bit is written 0, the datasheet's 20xx.
// Bits the datasheet leaves unused are masked off before decoding; the
// weekday is written but never judged on read, since nothing reads it and
// the chip does not check it against the date.
//
// The chip holds its time counters still while an I2C access is in progress
// (for up to a second), so one burst read of 0x00-0x08 sees a coherent time
// and one burst write sets one. A tick that falls inside an access is held
// and counted when it ends (datasheet Rev. 11.1, 8.5).
//
// Where in its second a clock is set (phase 4f,
// docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md, "Amendment:
// time from the network"): writing the time registers of a running clock
// leaves its prescaler where it was, so the next tick comes wherever in the
// second the old phase puts it -- anything up to a second off. Only STOP
// resets the prescaler (8.10): pcf8563_stop(), pcf8563_write_time(), then
// pcf8563_start() at the right moment, and the first tick follows the start
// by PCF8563_FIRST_TICK_AFTER_START_US.

#ifndef HAC_DRIVERS_PCF8563_PCF8563_H
#define HAC_DRIVERS_PCF8563_PCF8563_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// What pcf8563_read() reads, registers 0x00-0x08 in one burst.
#define PCF8563_READ_LEN 9u
// What pcf8563_encode() produces, registers 0x02-0x08.
#define PCF8563_TIME_LEN 7u

#define PCF8563_YEAR_MIN 2000u
#define PCF8563_YEAR_MAX 2099u

// The datasheet's (Rev. 11.1, 8.10, Table 26 and Figure 11): "The first
// increment of the time circuits is between 0.507813 s and 0.507935 s after
// the STOP bit is released" -- the prescaler's upper stages restart from 0,
// its lowest two and the crystal's own phase are not known. The middle of
// that range, in microseconds; it is off by 61 us at most.
#define PCF8563_FIRST_TICK_AFTER_START_US 507874u

// A time as the clock holds it: UTC.
typedef struct {
    uint16_t year;    // 2000-2099
    uint8_t month;    // 1-12
    uint8_t day;      // 1-31, within the month
    uint8_t hour;     // 0-23
    uint8_t minute;   // 0-59
    uint8_t second;   // 0-59
    uint8_t weekday;  // 0 = Sunday ... 6 = Saturday when this driver wrote it
} pcf8563_time_t;

typedef enum {
    PCF8563_TIME_VALID = 0,
    PCF8563_TIME_NOT_ANSWERING,  // the transfer failed; `bus` says how
    PCF8563_TIME_VL_SET,         // the supply dropped since the time was set
    PCF8563_TIME_STOPPED,        // STOP set: the time is not advancing
    PCF8563_TIME_UNDECODABLE,    // a field not BCD, or out of range
} pcf8563_verdict_t;

typedef struct {
    pcf8563_verdict_t verdict;
    hal_status_t bus;  // the transfer's own status: HAL_OK unless NOT_ANSWERING
    // `time` holds what the registers say: true when they decode, whatever
    // the verdict -- a time with VL set is still worth printing.
    bool decoded;
    pcf8563_time_t time;
    uint8_t regs[PCF8563_READ_LEN];  // 0x00-0x08 as read, for a log line
} pcf8563_reading_t;

// Reads registers 0x00-0x08 at `address` and judges them, in this order: a
// failed transfer, VL, STOP, a field that does not decode. Only
// PCF8563_TIME_VALID is a time to set a clock from.
void pcf8563_read(port_i2c_t *bus, uint8_t address, pcf8563_reading_t *out);

// Sets the clock to `utc`: Control_status_1 to 0 -- running, not in a test
// mode -- then registers 0x02-0x08, which clears VL. HAL_ERR_INVALID_ARG,
// with nothing written, when `utc` is out of range (pcf8563_encode()); else
// the first failed transfer's status. To the second only: a clock that was
// running keeps its prescaler's phase (above).
hal_status_t pcf8563_set(port_i2c_t *bus, uint8_t address, const pcf8563_time_t *utc);

// Control_status_1 to STOP: the prescaler's upper stages held at 0, no tick,
// the time standing at what it reads until pcf8563_start(). A clock left so
// reads PCF8563_TIME_STOPPED.
hal_status_t pcf8563_stop(port_i2c_t *bus, uint8_t address);

// Registers 0x02-0x08 to `utc` in one access, VL clear; Control_status_1 is
// not touched, so a running clock keeps its prescaler's phase and a stopped
// one stays stopped. HAL_ERR_INVALID_ARG, with nothing written, when `utc`
// is out of range (pcf8563_encode()).
hal_status_t pcf8563_write_time(port_i2c_t *bus, uint8_t address, const pcf8563_time_t *utc);

// Control_status_1 to 0: running, not in a test mode. After pcf8563_stop()
// the first tick comes PCF8563_FIRST_TICK_AFTER_START_US after this write,
// and one every second after that -- by the datasheet; what this board's
// chip does is measured, not assumed (phase 4f's rtc_sync.h).
hal_status_t pcf8563_start(port_i2c_t *bus, uint8_t address);

// The alarm, as a mark of the clock's own tick (phase 4f). The flag AF
// "sets ... when the time increments to match the alarm condition" (8.6.5):
// with the minute alarm alone enabled, at the tick that starts that minute;
// with AIE set, the open-drain INT follows AF, low until AF is cleared.
//
// Arms it for `minute` (0-59): registers 0x09-0x0C in one access -- the
// minute alarm on, the hour, day and weekday alarms off -- then
// Control_status_2 to AIE alone, which clears AF and TF and leaves the
// timer's interrupt off. A minute that matches already does not set AF: only
// the increment into it does. HAL_ERR_INVALID_ARG for a minute above 59,
// nothing written.
hal_status_t pcf8563_alarm_at_minute(port_i2c_t *bus, uint8_t address, uint8_t minute);

// Control_status_2 to 0 -- AF and TF cleared, both interrupts off, INT let go
// -- then the minute alarm off.
hal_status_t pcf8563_alarm_off(port_i2c_t *bus, uint8_t address);

// The judgement pcf8563_read() makes of what it read, without a bus.
// out->bus is set to HAL_OK.
void pcf8563_decode(const uint8_t regs[PCF8563_READ_LEN], pcf8563_reading_t *out);

// Registers 0x02-0x08 for `utc`, VL clear and century bit 0. False, leaving
// `out` alone, when a field is out of range: a year outside 2000-2099, a
// day its month does not have, a weekday above 6.
bool pcf8563_encode(const pcf8563_time_t *utc, uint8_t out[PCF8563_TIME_LEN]);

// "valid", "not answering", "VL set", "stopped", "undecodable"; never NULL.
const char *pcf8563_verdict_name(pcf8563_verdict_t verdict);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_PCF8563_PCF8563_H
