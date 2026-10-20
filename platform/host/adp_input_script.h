// SPDX-License-Identifier: Apache-2.0
//
// port_input from a script: touches at set times on the virtual clock, for
// the simulator's headless runs (`--taps`), so CI can tap through the
// screens (docs/adr/0025-touch-input-gt911.md). An event is delivered once
// the clock has reached its time, stamped with that time, as a controller's
// would be by the moment it was read.
//
// The command line's form, parsed by adp_input_script_add_taps():
//
//   --taps "<t_s>:<x>,<y>;<t_s>:<x>,<y>;..."
//
// one tap per item -- a DOWN at t_s seconds (a decimal), an UP
// ADP_INPUT_SCRIPT_TAP_MS later, at panel coordinates (x, y) -- in time
// order, each tap over before the next begins.

#ifndef HAC_PLATFORM_HOST_ADP_INPUT_SCRIPT_H
#define HAC_PLATFORM_HOST_ADP_INPUT_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADP_INPUT_SCRIPT_MAX_EVENTS 128u

// CHOICE: how long a scripted tap keeps the finger down: a quick tap, well
// inside TAP_DEFAULT_MAX_DURATION_MS.
#define ADP_INPUT_SCRIPT_TAP_MS 100u

#define ADP_INPUT_SCRIPT_NO_EVENT UINT64_MAX

typedef struct {
    port_input_t port;
    const port_clock_t *clock;
    input_event_t events[ADP_INPUT_SCRIPT_MAX_EVENTS];
    size_t count;
    size_t next;
} adp_input_script_t;

// An empty script. `clock` must outlive it.
void adp_input_script_init(adp_input_script_t *s, const port_clock_t *clock);

port_input_t *adp_input_script_port(adp_input_script_t *s);

// One event at `at_ms`. False when the script is full or `at_ms` is earlier
// than the last event's.
bool adp_input_script_push(adp_input_script_t *s, input_kind_t kind, uint16_t x, uint16_t y,
                           uint64_t at_ms);

// Parses `text` (above) and adds its taps. HAL_ERR_INVALID_ARG, with a
// sentence in `err` saying what is wrong and nothing added, for a malformed
// item, a coordinate outside `width` x `height`, a time out of order or a tap
// that begins before the last one is over, or more taps than fit.
hal_status_t adp_input_script_add_taps(adp_input_script_t *s, const char *text, uint16_t width,
                                       uint16_t height, char *err, size_t err_cap);

// When the next event is due, or ADP_INPUT_SCRIPT_NO_EVENT after the last.
uint64_t adp_input_script_next_ms(const adp_input_script_t *s);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_INPUT_SCRIPT_H
