// SPDX-License-Identifier: Apache-2.0
//
// A port_clock implementation driven entirely by the test.
//
// Time only moves when the test moves it, which is what makes "eight hours on
// battery" a millisecond-long assertion instead of an eight-hour observation.
// delay_ms() advances virtual time rather than blocking, for the same reason.

#ifndef HAC_TESTS_FAKE_CLOCK_H
#define HAC_TESTS_FAKE_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "port_clock.h"

typedef struct {
    port_clock_t port;

    uint64_t now_ms;
    bool wall_synced;
    uint64_t wall_offset_ms;  // wall_ms() == now_ms + wall_offset_ms once synced

    // Observability: what the code under test asked the clock to do.
    uint32_t delay_calls;
    uint64_t delayed_total_ms;
} fake_clock_t;

// Cold boot: monotonic time at 0, wall clock unsynchronized, counters cleared.
// Calling this again on a live clock models the restart after deep sleep.
void fake_clock_init(fake_clock_t *fc);

// The port to hand to the code under test.
const port_clock_t *fake_clock_port(const fake_clock_t *fc);

void fake_clock_advance(fake_clock_t *fc, uint64_t ms);

// Synchronize the wall clock: wall_ms() returns wall_ms now, and tracks the
// monotonic clock from here on.
void fake_clock_set_wall(fake_clock_t *fc, uint64_t wall_ms);

#endif  // HAC_TESTS_FAKE_CLOCK_H
