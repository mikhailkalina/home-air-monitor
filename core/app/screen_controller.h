// SPDX-License-Identifier: Apache-2.0
//
// Which screen is on the panel, and the one consumer of port_input
// (docs/adr/0025-touch-input-gt911.md). Not app_core -- which still does not
// exist (docs/architecture.md 6.1) -- but the same shape: the event loops
// call it as they call sensor_manager_poll() and update_policy, and it says
// when it next wants to be called.
//
// Two ordinary screens: home (core/ui/screen_home.h) and diagnostics
// (core/ui/screen_diagnostics.h). A tap anywhere (core/app/tap.h) switches
// between them; the diagnostics screen goes back home on its own once
// nothing has touched the panel for diagnostics_timeout_ms -- a screen left
// up would otherwise hide the readings the device exists to show.
//
// A change of screen is a redraw a person asked for: update_policy_request()
// makes it due at once, and always as a full refresh -- a whole new screen
// drawn partially kept the old one's outlines and read badly
// (update_policy.h, docs/adr/0034-a-change-of-screen-is-a-full-refresh.md). No
// separate feedback for the tap: the screen changing is the feedback. When
// the redraw that carries it reaches the glass, screen_controller_committed()
// says so, with the time from the touch to that moment -- what the loops
// log as the latency a person sees.
//
// While a screen is up, what makes it redraw is that screen's own business:
// the home screen's is update_policy_evaluate() over the reading; the
// diagnostics screen compares the view it would draw with the one it drew
// (diag_view_equal()) and asks update_policy_evaluate_partial_content().
//
// A firmware update is modal (ui/screen_firmware_update.h): milestones
// redraw immediately, while touch and the diagnostics timeout are ignored.
// Failure unlocks touch and returns home by tap or after a short timeout.
//
// Pure: no allocation, no I/O, every time from the events or the caller.

#ifndef HAC_CORE_APP_SCREEN_CONTROLLER_H
#define HAC_CORE_APP_SCREEN_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "app/sensor_manager.h"
#include "app/tap.h"
#include "app/update_policy.h"
#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "port_input.h"
#include "ui/diagnostics_view.h"
#include "ui/framebuffer.h"
#include "ui/screen_firmware_update.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: the diagnostics screen goes home after this long untouched. Long
// enough to watch the network's states change on it while testing -- an
// outage's loss and return take minutes -- short enough that it is not what
// the room sees for long. The owner's, 2026-10-23: a minute before. The way
// home is a full refresh, which starts the partial-refresh budget over, so
// the redraws the screen makes while it is up cost the home screen nothing
// (docs/adr/0038-the-diagnostics-screen-is-a-developers.md).
#define SCREEN_CONTROLLER_DEFAULT_DIAGNOSTICS_TIMEOUT_MS 300000u

// CHOICE: leave an update failure visible long enough to read, then go home.
#define SCREEN_CONTROLLER_UPDATE_ERROR_TIMEOUT_MS 10000u

#define SCREEN_CONTROLLER_NO_DEADLINE UINT64_MAX

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_DIAGNOSTICS,
    SCREEN_FIRMWARE_UPDATE,
} screen_id_t;

typedef struct {
    tap_config_t tap;
    uint32_t diagnostics_timeout_ms;
} screen_controller_config_t;

// What the screens draw from, read together after the poll.
typedef struct {
    const air_reading_t *reading;  // required
    // port_clock's time at which reading->wall_time_ms was read
    // (view_model_build()).
    uint64_t now_ms;
    const time_zone_t *zone;          // required: the home screen's local time
    const sensor_manager_t *sensors;  // may be NULL
    diag_recorder_t recorder;
    const char *clock_source;  // may be NULL (diagnostics_view.h)
    // The reporter and its network (reporter_status()), or NULL without
    // telemetry; the held link the bar shows is the reading's.
    const reporter_status_t *network;
} screen_inputs_t;

// What a redraw that reached the panel carried.
typedef struct {
    bool screen_changed;  // it put a new screen on the glass
    bool by_tap;          // and a person asked for it, rather than the timeout
    screen_id_t screen;
    uint64_t latency_ms;  // by_tap: from the touch that asked to the end of the flush
} screen_redraw_note_t;

typedef struct {
    screen_controller_config_t cfg;
    tap_t tap;

    screen_id_t screen;  // the one to show
    uint64_t last_touch_ms;

    bool change_pending;  // a change of screen not yet on the glass
    bool change_by_tap;
    uint64_t change_asked_ms;

    // The diagnostics view last drawn, and the one being drawn now: kept here
    // rather than on the event loop's stack, which has a few kilobytes free.
    diag_view_t shown_diag;
    bool have_shown_diag;
    diag_view_t pending_diag;

    firmware_update_progress_t update;
    bool update_changed;
    uint64_t update_error_since_ms;
    uint64_t input_after_ms;  // discard queued touches from before an update ended

    uint32_t taps;
    uint32_t timeouts;
} screen_controller_t;

screen_controller_config_t screen_controller_config_default(void);

// The home screen, nothing pending.
void screen_controller_init(screen_controller_t *sc, const screen_controller_config_t *cfg);

// Reports a modal firmware update, resetting any in-flight gesture. Active
// updates ignore touch and screen timeouts. A failure unlocks touch and goes
// home by tap or after SCREEN_CONTROLLER_UPDATE_ERROR_TIMEOUT_MS.
void screen_controller_firmware_update(screen_controller_t *sc, update_policy_t *policy,
                                       const firmware_update_progress_t *progress, uint64_t now_ms);

// One event from port_input. A tap switches the screen and asks `policy` for
// the redraw; returns true when it did.
bool screen_controller_input(screen_controller_t *sc, update_policy_t *policy,
                             const input_event_t *ev);

// At `now_ms`: the diagnostics screen's timeout. Returns when it next needs
// calling without an event -- SCREEN_CONTROLLER_NO_DEADLINE on the home
// screen.
uint64_t screen_controller_tick(screen_controller_t *sc, update_policy_t *policy, uint64_t now_ms);

// Whether the screen to show is to be redrawn now, and how: `policy`'s
// decision for that screen.
void screen_controller_evaluate(screen_controller_t *sc, update_policy_t *policy,
                                const screen_inputs_t *in, update_decision_t *out);

// Draws the screen to show into `fb`.
void screen_controller_render(screen_controller_t *sc, framebuffer_t *fb,
                              const screen_inputs_t *in);

// The redraw rendered last reached the panel, at `now_ms` (after the
// flush). Fills `note` with what it carried.
void screen_controller_committed(screen_controller_t *sc, uint64_t now_ms,
                                 screen_redraw_note_t *note);

screen_id_t screen_controller_screen(const screen_controller_t *sc);

// "home", "diagnostics", "firmware_update"; never NULL.
const char *screen_controller_screen_name(screen_id_t screen);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_APP_SCREEN_CONTROLLER_H
