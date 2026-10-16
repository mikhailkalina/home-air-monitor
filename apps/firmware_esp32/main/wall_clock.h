// SPDX-License-Identifier: Apache-2.0
//
// The device's wall clock, between the PCF8563 and ESP-IDF's system time:
// what main.c reads from the RTC at boot and the console's time_set writes to
// it. Pure over port_i2c and the PCF8563 driver -- the system time itself is
// adp_clock's (adp_clock_set_wall_ms()) -- so it is tested on the host
// against the fake bus (tests/unit/test_wall_clock.c), as i2c_scan.c is.
//
// The RTC holds UTC. A time it reads is a time to set the system time from
// only when the chip says it is valid -- answering, VL clear, running,
// decodable -- and it is no earlier than the earliest believable time the
// caller passes (ADP_CLOCK_WALL_EARLIEST_MS on the board): a chip that says
// 2000 was never set by this firmware
// (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md).

#ifndef HAC_APPS_FIRMWARE_ESP32_WALL_CLOCK_H
#define HAC_APPS_FIRMWARE_ESP32_WALL_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "pcf8563.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// Where the system time came from, for the console's `time` and the log.
typedef enum {
    WALL_CLOCK_SOURCE_NONE = 0,  // nothing set it: wall_ms() is 0
    WALL_CLOCK_SOURCE_RTC,       // the PCF8563, read at boot
    WALL_CLOCK_SOURCE_CONSOLE,   // time_set
    WALL_CLOCK_SOURCE_KEPT,      // what a software reset kept, the RTC giving none
} wall_clock_source_t;

typedef struct {
    pcf8563_reading_t chip;  // what the RTC said, and the driver's verdict on it
    bool decoded;            // unix_ms holds the chip's time, usable or not
    uint64_t unix_ms;
    bool before_earliest;  // valid on the chip, but earlier than any time this firmware set
    bool usable;           // a time to set the system time from
} wall_clock_rtc_t;

// Reads the RTC at `address` and judges it (above).
void wall_clock_read_rtc(port_i2c_t *bus, uint8_t address, uint64_t earliest_ms,
                         wall_clock_rtc_t *out);

// Why `rtc` gives no time: "rtc: not answering", "rtc: VL set", "rtc:
// stopped", "rtc: undecodable", "rtc: implausible year"; "rtc: valid" when it
// does. Never NULL.
const char *wall_clock_rtc_reason(const wall_clock_rtc_t *rtc);

// Sets the RTC to `unix_ms`, UTC, to the second (rounded down), which clears
// its VL bit and starts it if it was stopped. HAL_ERR_INVALID_ARG, with
// nothing written, outside the chip's 2000-2099; else pcf8563_set()'s status.
hal_status_t wall_clock_write_rtc(port_i2c_t *bus, uint8_t address, uint64_t unix_ms);

// "none", "rtc", "console", "kept across a software reset"; never NULL.
const char *wall_clock_source_name(wall_clock_source_t source);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_WALL_CLOCK_H
