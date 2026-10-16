// SPDX-License-Identifier: Apache-2.0
//
// The PCF8563 real-time clock over port_i2c: read the time, set the time.
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
// and one burst write sets one.

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
// the first failed transfer's status.
hal_status_t pcf8563_set(port_i2c_t *bus, uint8_t address, const pcf8563_time_t *utc);

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
