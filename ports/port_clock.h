// SPDX-License-Identifier: Apache-2.0
//
// The clock port.
//
// The core never calls esp_timer_get_time(), xTaskGetTickCount() or time():
// every notion of "now" and every interval is derived from this port. That is
// what lets a test push a full day through the core in milliseconds, and lets
// the simulator replay a scenario at --time-scale 3600.

#ifndef HAC_PORTS_PORT_CLOCK_H
#define HAC_PORTS_PORT_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct port_clock_s port_clock_t;

struct port_clock_s {
    // Milliseconds since an arbitrary origin. Never goes backwards within one
    // power cycle; deep sleep is a cold start, so it may restart from 0.
    uint64_t (*now_ms)(const port_clock_t *self);

    // Milliseconds since the Unix epoch (UTC), or 0 while unsynchronized.
    uint64_t (*wall_ms)(const port_clock_t *self);

    // Busy-wait or yield for the given duration. Must return only after
    // now_ms() has advanced by at least `ms` -- never less, for any `ms`,
    // including values shorter than whatever scheduling granularity the
    // platform happens to have (see tests/contract/port_clock_contract.c's
    // clock.delay_ms(N).advances_by_at_least_n, and
    // docs/adr/0006-delay-ms-rounds-up-a-full-extra-tick.md for a concrete
    // case where an OS scheduler's own primitive does not honour this on its
    // own). A virtual clock implements this by advancing its own time
    // instead of blocking.
    void (*delay_ms)(const port_clock_t *self, uint32_t ms);

    void *impl;
};

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_CLOCK_H
