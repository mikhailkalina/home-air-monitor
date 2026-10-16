// SPDX-License-Identifier: Apache-2.0
//
// Real wall-clock time for the simulator's event loop -- and only for that.
//
// The core never sees this: it reads adp_clock_virtual, whose time moves only
// when the loop moves it. The loop is the one place that has to know how much
// real time passed, so that adp_clock_virtual_scale_ms() can turn it into
// virtual time at the configured --time-scale -- and what time of day it is
// when the run starts.

#ifndef HAC_PLATFORM_HOST_HOST_TIME_H
#define HAC_PLATFORM_HOST_HOST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Milliseconds from an arbitrary origin, monotonic.
uint64_t host_monotonic_ms(void);

// Milliseconds since the Unix epoch, UTC: the PC's own wall clock, which the
// simulator starts its virtual wall clock from. 0 if the PC cannot say.
uint64_t host_wall_ms(void);

void host_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_HOST_TIME_H
