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
// wall_ms() returns 0 (unsynchronized) until phase 3 wires up SNTP.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_CLOCK_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_CLOCK_H

#include "port_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_clock_t port;
} adp_clock_t;

void adp_clock_init(adp_clock_t *c);

const port_clock_t *adp_clock_port(const adp_clock_t *c);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_CLOCK_H
