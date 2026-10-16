// SPDX-License-Identifier: Apache-2.0
//
// port_clock over esp_timer_get_time(): microseconds since this boot,
// truncated to milliseconds.
//
// Monotonicity across a deep sleep cycle is DOCUMENTED, not asserted: deep
// sleep on the ESP32-S3 resets esp_timer, so now_ms() restarts from 0 after
// waking, exactly like a power-on reset. The port contract already permits
// this (ports/port_clock.h: "deep sleep is a cold start, so it may restart
// from 0"), and core/app/update_policy.c already treats now_ms < the last
// recorded time as "due now" rather than as a fault. Whether this board's
// deep sleep genuinely behaves like esp_timer's own reset -- as opposed to,
// say, an RTC-backed continuation -- is an open item in
// docs/hardware/board_notes.md for the phase that actually exercises deep
// sleep; nothing here assumes an answer either way.
//
// wall_ms() is ESP-IDF's system time -- gettimeofday() -- and
// adp_clock_set_wall_ms() sets it with settimeofday(). The composition sets
// it from the PCF8563 at boot and from the console's time_set; phase 4's SNTP
// sets the same system time through ESP-IDF's own settimeofday(), and needs
// nothing here (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md).
//
// Until something has set it, wall_ms() is 0, as ports/port_clock.h promises
// for an unsynchronized clock: a system time before ADP_CLOCK_WALL_EARLIEST_MS
// is one nobody set. At a power-on or any other chip reset ESP-IDF's system
// time starts from the epoch -- its "boot time" lives in two RTC_CNTL store
// registers, which such a reset clears. A software reset (esp_restart(), a
// panic, a task watchdog) resets the CPUs only: the registers and the RTC
// timer under them carry on, so the system time a previous boot set comes back
// still running, before anything reads the RTC (components/newlib/src/port/
// esp_time_impl.c, ESP-IDF 5.5; read, not yet seen on the board).

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_CLOCK_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_CLOCK_H

#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: the earliest system time wall_ms() believes, 2026-01-01T00:00:00Z.
// Nothing this firmware sets can be earlier -- none of it existed before
// then -- and an RTC that says otherwise has not been set by it either, so
// the composition holds the PCF8563's time to the same floor.
#define ADP_CLOCK_WALL_EARLIEST_MS 1767225600000ull

typedef struct {
    port_clock_t port;
    const port_log_t *log;  // may be NULL; port_log_write() is NULL-safe
} adp_clock_t;

// `log` is borrowed and may be NULL (before a logger exists, or if none is
// wanted); delay_ms() uses it to report each call's requested and actual
// duration at LOG_LEVEL_DEBUG -- see the reasoning in adp_clock.c and
// docs/adr/0006-delay-ms-rounds-up-a-full-extra-tick.md for why that number
// is worth watching.
void adp_clock_init(adp_clock_t *c, const port_log_t *log);

const port_clock_t *adp_clock_port(const adp_clock_t *c);

// Sets the system time -- what wall_ms() returns from then on -- to
// `unix_ms`, milliseconds since the epoch, UTC. HAL_ERR_INVALID_ARG, with the
// system time unchanged, for a time before ADP_CLOCK_WALL_EARLIEST_MS;
// HAL_ERR_IO if settimeofday() refuses it.
hal_status_t adp_clock_set_wall_ms(adp_clock_t *c, uint64_t unix_ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_CLOCK_H
