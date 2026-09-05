// SPDX-License-Identifier: Apache-2.0
//
// A port_clock for the host: time advances only when the adapter is told to,
// either directly (tests, "force cold boot", stepping a scenario) or scaled
// from real elapsed time (a --time-scale N simulator run). Splitting those
// two into adp_clock_virtual_scale_ms() (pure, no OS clock involved) and
// adp_clock_virtual_advance() (mutates now_ms) keeps the scaling arithmetic
// unit-testable without touching real time.

#ifndef HAC_PLATFORM_HOST_ADP_CLOCK_VIRTUAL_H
#define HAC_PLATFORM_HOST_ADP_CLOCK_VIRTUAL_H

#include <stdbool.h>
#include <stdint.h>

#include "port_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_clock_t port;

    uint64_t now_ms;
    double time_scale;  // e.g. 60.0: a minute of real time is an hour of virtual time
    bool wall_synced;
    uint64_t wall_offset_ms;  // wall_ms() == now_ms + wall_offset_ms once synced

    uint32_t delay_calls;
    uint64_t delayed_total_ms;
} adp_clock_virtual_t;

// Cold boot: monotonic time at 0, wall clock unsynchronized. `time_scale`
// must be > 0; 1.0 means virtual time tracks real time one-for-one. Calling
// this again on a live clock models the restart after deep sleep.
void adp_clock_virtual_init(adp_clock_virtual_t *vc, double time_scale);

// The port to hand to app_core / a sensor_source_t / anything else that only
// knows port_clock_t.
const port_clock_t *adp_clock_virtual_port(const adp_clock_virtual_t *vc);

// Moves now_ms forward by `virtual_ms` directly. Used by tests and by
// anything that already knows how much virtual time it wants to pass.
void adp_clock_virtual_advance(adp_clock_virtual_t *vc, uint64_t virtual_ms);

// Converts a duration of real (wall-clock) time into virtual time under the
// configured scale, rounding down. Pure: it does not read the OS clock or
// touch `vc->now_ms`. The event loop that measures real elapsed time is
// expected to pass the result to adp_clock_virtual_advance().
uint64_t adp_clock_virtual_scale_ms(const adp_clock_virtual_t *vc, uint64_t real_ms);

void adp_clock_virtual_set_scale(adp_clock_virtual_t *vc, double time_scale);
double adp_clock_virtual_scale(const adp_clock_virtual_t *vc);

// Synchronize the wall clock: wall_ms() returns wall_ms now, and tracks the
// monotonic clock from here on.
void adp_clock_virtual_set_wall(adp_clock_virtual_t *vc, uint64_t wall_ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_CLOCK_VIRTUAL_H
