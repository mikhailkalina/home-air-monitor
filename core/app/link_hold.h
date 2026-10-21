// SPDX-License-Identifier: Apache-2.0
//
// What the screen says about the link, and when that changes
// (docs/adr/0031-the-network-on-the-screen.md).
//
// The link as it is now -- the network's state and whether the reporter has
// a session, reporter_link() -- changes whenever Wi-Fi reassociates, a
// broker restarts or an attempt is refused, and on a bad day every few
// seconds. Each change on the glass costs a partial refresh of the panel's
// budget, and a link that flaps would spend it, as IAQ trembling on 50 did
// (docs/adr/0026-iaq-redraw-delta.md). So the screen shows a held state: the
// link as it is now, once it has stayed so for hold_ms without a break. A
// change shorter than that never reaches the glass, and the changes that do
// are at least hold_ms apart: however the link behaves, it costs at most one
// redraw per hold_ms.
//
// The first state too. Until the link has held a state for hold_ms since
// start, the screen shows LINK_UNKNOWN -- nothing, in a place kept for the
// word -- rather than "no Wi-Fi" for the seconds an association takes and a
// minute after it is done. A link that is not configured, LINK_OFF, is shown
// at once: it is the build's or the configuration's, and does not flap.
//
// The composition feeds it after the reporter's step, as both event loops
// run the others, and puts link_hold_shown() into the screen's reading,
// where update_policy redraws on a change like any other status.
//
// Pure: no clock, no allocation, every time from the caller.

#ifndef HAC_CORE_APP_LINK_HOLD_H
#define HAC_CORE_APP_LINK_HOLD_H

#include <stdbool.h>
#include <stdint.h>

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESTIMATE: how long the link must stay in a state before the screen says
// so. Long enough to cover what is over by itself -- a Wi-Fi reassociation
// (seconds: ADP_NET_SIM_CONNECT_MS, an estimate itself), a broker restarting
// (a session again 15 s later with Home Assistant's, ADR 0030's amendment),
// the reporter's first attempts after a drop (at once, then 5 s and 10 s
// later: REPORT_BACKOFF_FIRST_MS) -- and short enough that the bar says "no
// broker" about when Home Assistant says the device is unavailable: its last
// will goes out 30 to 90 s after the device fell silent (REPORT_KEEPALIVE_S).
// It bounds what a link can cost the panel: one redraw per minute at most.
// To replace: the board's own Wi-Fi drops and reconnections, from the card's
// log, once its Wi-Fi adapter runs.
#define LINK_HOLD_DEFAULT_MS 60000u

#define LINK_HOLD_NO_DEADLINE UINT64_MAX

typedef struct {
    // A state is shown once the link has been in it this long without a
    // break; 0 shows every state at once.
    uint32_t hold_ms;
} link_hold_config_t;

typedef struct {
    link_hold_config_t cfg;
    link_status_t shown;     // what the screen says
    link_status_t seen;      // the link as it was at the last step
    uint64_t seen_since_ms;  // since when it has been `seen`, without a break
    bool started;            // stepped at least once
    uint32_t changes;        // of `shown`, since link_hold_init()
} link_hold_t;

link_hold_config_t link_hold_config_default(void);

// Shows LINK_OFF until the first step.
void link_hold_init(link_hold_t *h, const link_hold_config_t *cfg);

// The link as it is now, `now`, at `now_ms` (port_clock's). Returns when the
// shown state can next change without the link changing -- the end of a
// hold under way -- or LINK_HOLD_NO_DEADLINE. `changed`, when not NULL,
// says whether this step changed the shown state.
uint64_t link_hold_step(link_hold_t *h, link_status_t now, uint64_t now_ms, bool *changed);

// What the screen says: the last state held for hold_ms, LINK_UNKNOWN before
// any has been, LINK_OFF with no telemetry.
link_status_t link_hold_shown(const link_hold_t *h);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_APP_LINK_HOLD_H
