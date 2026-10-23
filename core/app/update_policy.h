// SPDX-License-Identifier: Apache-2.0
//
// When to redraw the panel.
//
// Two independent sets of inputs meet here, and keeping them separate is the
// point of the module:
//
//   * update_policy_config_t -- what the *product* wants: redraw when CO2 has
//     moved by more than 50 ppm, temperature by 0.2 C, humidity by 1 %
//     (docs/requirements.md 9.1), IAQ by 5 points
//     (docs/adr/0026-iaq-redraw-delta.md), pressure by 1 hPa
//     (docs/adr/0027-pressure-redraw-delta.md), when a status the screen shows
//     changes -- a number's accuracy crossing what the screen marks
//     (measurement_low_accuracy()) among them
//     (docs/adr/0021-measurement-accuracy-and-iaq-levels.md) -- when a smaller
//     offset, more than half a delta, has held for 15 minutes
//     (docs/adr/0028-redraw-a-persistent-offset.md), and no more than once
//     per sensor cycle;
//   * update_policy_limits_t -- what the *panel* tolerates: how often a full
//     refresh may happen and how many partial refreshes may precede it. These
//     are read out of port_display, never written down here. There is no
//     ED047TC1 constant in this file, and there must never be one: a second
//     panel reports different numbers through the same port and this logic is
//     unchanged.
//
// Nothing is redrawn because time passed. Nothing on the screen changes with
// time alone -- the footer says when the values on it were measured, not how
// long ago (core/ui/view_model.h) -- so a redraw on a timer would draw the
// same picture. Requirements 9.1's "time since last update > 2 min -> update
// display" is departed from on purpose, and the periodic redraw ADR 0019 kept
// is gone (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md).
// Data getting older is still shown: VAL_STALE is a status, and its arrival
// redraws like any other status change. A persistent offset is no timer
// either: it redraws only a reading that has moved, and only once
// (docs/adr/0028-redraw-a-persistent-offset.md).
//
// A redraw a person asked for -- a tap that changes the screen
// (core/app/screen_controller.h) -- goes out at once: update_policy_request()
// makes it due without waiting out min_redraw_interval_ms. It is a full
// refresh: neither the budget nor the panel's minimum full-refresh interval
// holds it, and it resets the budget as any full refresh does. A whole new
// screen drawn partially kept the old one's outlines and read badly
// (docs/adr/0034-a-change-of-screen-is-a-full-refresh.md, superseding
// docs/adr/0025-touch-input-gt911.md's "A person's redraw").
//
// The thresholds above are the home screen's. A screen that decides itself
// whether what it would draw has changed says so through
// update_policy_evaluate_content(), and the panel's rules are the same -- or
// through update_policy_evaluate_partial_content(), whose content redraws
// stay partial without a count limit: the diagnostics screen's, a developer's
// screen. Its first frame and screen changes still go out full
// (docs/adr/0038-the-diagnostics-screen-is-a-developers.md).
//
// The module is pure. It performs no I/O, allocates nothing, and takes every
// notion of "now" from the port_clock handed to update_policy_init(), which is
// what lets tests push hours through it in microseconds.

#ifndef HAC_CORE_APP_UPDATE_POLICY_H
#define HAC_CORE_APP_UPDATE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "domain/measurement.h"
#include "port_clock.h"
#include "port_display.h"

#ifdef __cplusplus
extern "C" {
#endif

// The thresholds of docs/requirements.md 9.1, IAQ's
// (docs/adr/0026-iaq-redraw-delta.md) and pressure's
// (docs/adr/0027-pressure-redraw-delta.md), named so that the only place they
// appear as numbers is here.
#define UPDATE_POLICY_DEFAULT_CO2_DELTA_PPM 50.0f
#define UPDATE_POLICY_DEFAULT_TEMPERATURE_DELTA_C 0.2f
#define UPDATE_POLICY_DEFAULT_HUMIDITY_DELTA_RH 1.0f
#define UPDATE_POLICY_DEFAULT_PRESSURE_DELTA_HPA 1.0f
#define UPDATE_POLICY_DEFAULT_IAQ_DELTA 5.0f

// CHOICE: at most one redraw per this interval -- one sensor cycle
// (SENSOR_MANAGER_DEFAULT_INTERVAL_MS). The two sensors are read at different
// moments of each cycle, so without it a cycle in which both readings move
// costs two partial refreshes 0.6 s apart (phase 3c's board log); with it, the
// second waits for the first's cycle to end and both reach the glass in one.
// Half the partial refreshes for the same readings, and so twice as long
// between full ones (docs/adr/0019-partials-past-the-budget.md).
#define UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS 5000u

// CHOICE: past the panel's budget, while a full refresh waits for the
// panel's minimum interval, partial refreshes go on -- but no further than
// the budget again: at most this many times the budget between two full
// refreshes. At the sensors' pace it never binds (ADR 0019: 23 at most). It
// was set for a person tapping without pause, who would otherwise have
// reached 55 in the panel's minute, past the 40 this panel was ever driven
// through -- which is, for its budget of 20, exactly this bound
// (docs/adr/0025-touch-input-gt911.md, "A person's redraw"). Taps are full
// refreshes since ADR 0034, so it is a bound for a redraw source faster
// than the sensors' cycle, should one come. Derived from the budget the
// panel reports, never a number of its own. Diagnostics content redraws
// bypass this bound (docs/adr/0038-the-diagnostics-screen-is-a-developers.md).
#define UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET 2u

// CHOICE, the owner's (2026-10-20): a reading more than this fraction of its
// delta from the frame on the glass, for UPDATE_POLICY_DEFAULT_SETTLE_AFTER_MS
// without a break, redraws -- an offset that persists in still air, which the
// delta alone would leave on the glass for as long as nothing else redraws.
// Half a delta stays above most of the sensors' noise, so it holds only when
// the air has really settled elsewhere
// (docs/adr/0028-redraw-a-persistent-offset.md).
#define UPDATE_POLICY_SETTLE_DELTA_FRACTION 0.5f
#define UPDATE_POLICY_DEFAULT_SETTLE_AFTER_MS (15u * 60u * 1000u)

// The readings with a delta: CO2, temperature, humidity, pressure, IAQ.
#define UPDATE_POLICY_DELTA_FIELDS 5u

// Returned as next_deadline_ms when no timer is pending: only new data can
// change the answer, so there is nothing to wake up for.
#define UPDATE_POLICY_NO_DEADLINE UINT64_MAX

typedef struct {
    // A quantity must move by strictly more than its threshold to trigger a
    // redraw. A threshold of 0 therefore redraws on any change at all.
    float co2_delta_ppm;
    float temperature_delta_c;
    float humidity_delta_rh;
    float pressure_delta_hpa;
    float iaq_delta;

    // A reading more than UPDATE_POLICY_SETTLE_DELTA_FRACTION of its delta
    // from the frame on the glass for this long without a break redraws.
    // 0 never does.
    uint32_t settle_after_ms;

    // No redraw sooner than this after the last one, except the first frame:
    // a redraw that becomes due earlier waits, and goes out with whatever
    // else changed meanwhile. 0 lets every due redraw out at once.
    uint32_t min_redraw_interval_ms;
} update_policy_config_t;

// The panel's own constraints, copied out of port_display so that the policy
// can be exercised without a display at all.
typedef struct {
    uint32_t min_full_refresh_interval_ms;
    uint32_t max_partial_refreshes_before_full;
} update_policy_limits_t;

typedef enum {
    UPDATE_NONE = 0,
    UPDATE_PARTIAL,
    UPDATE_FULL,
} update_action_t;

// Why a redraw became due. Reported even when the action is UPDATE_NONE
// because the panel's minimum interval deferred it (a panel with no partial
// mode only), so a log can say what was wanted and what stopped it.
typedef enum {
    UPDATE_REASON_NONE = 0,
    UPDATE_REASON_FIRST_FRAME,
    UPDATE_REASON_CO2,
    UPDATE_REASON_TEMPERATURE,
    UPDATE_REASON_HUMIDITY,
    UPDATE_REASON_PRESSURE,
    UPDATE_REASON_IAQ,
    UPDATE_REASON_SETTLE,  // an offset of more than half a delta held settle_after_ms
    // A value_status_t, the accuracy mark, the link the bar shows, or whether
    // the wall time is known, changed -- or, on a screen that compares its
    // own content, that content.
    UPDATE_REASON_STATUS,
    UPDATE_REASON_REQUEST,  // a person asked: update_policy_request()
} update_reason_t;

typedef struct {
    update_action_t action;
    update_reason_t reason;

    // The redraw would have been partial, and was escalated to full only
    // because the panel's partial-refresh budget is spent.
    bool full_forced_by_budget;

    // action is UPDATE_PARTIAL although the budget is spent: a full refresh
    // is not allowed yet -- the panel's minimum full-refresh interval has not
    // elapsed -- and the screen is kept current meanwhile. The first redraw
    // due after the interval is full; none is made only because the interval
    // elapsed (docs/adr/0019-partials-past-the-budget.md). Diagnostics content
    // stays partial regardless of this interval or the count (ADR 0038).
    bool partial_past_budget;

    // action is UPDATE_NONE only because the last redraw was less than
    // min_redraw_interval_ms ago; next_deadline_ms is when it may go out.
    bool held_for_redraw_interval;

    // action is UPDATE_NONE only because a full refresh is due but the
    // panel's minimum full-refresh interval has not elapsed -- which happens
    // on a panel with no usable partial mode
    // (max_partial_refreshes_before_full == 0), where there is nothing to keep
    // the screen current with meanwhile, and once the partial refreshes past
    // the budget have reached UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET times it.
    bool deferred_by_min_interval;

    // When to call update_policy_evaluate() again in the absence of new data:
    // now_ms when a redraw is due, the point it may go out when it is held or
    // deferred, UPDATE_POLICY_NO_DEADLINE otherwise -- only new data can
    // change the answer then.
    uint64_t next_deadline_ms;
} update_decision_t;

typedef struct {
    const port_clock_t *clock;
    update_policy_config_t cfg;
    update_policy_limits_t limits;

    air_reading_t last_rendered;
    bool have_last_rendered;

    uint64_t last_refresh_ms;
    uint64_t last_full_refresh_ms;
    bool have_full_refresh;
    uint32_t partials_since_full;

    bool requested;  // update_policy_request() since the last redraw

    // Per reading with a delta: since when it has been more than half its
    // delta from the frame on the glass, without a break.
    bool offset_held[UPDATE_POLICY_DELTA_FIELDS];
    uint64_t offset_since_ms[UPDATE_POLICY_DELTA_FIELDS];
} update_policy_t;

// The thresholds of docs/requirements.md 9.1, IAQ's and pressure's, the
// persistent offset's interval, and the one-redraw-per-cycle interval.
update_policy_config_t update_policy_config_default(void);

// Reads the panel's constraints off the port. A NULL display yields limits
// that permit nothing but full refreshes, which is the safe direction.
update_policy_limits_t update_policy_limits_from_display(const port_display_t *display);

// `clock`, `cfg` and `limits` are all required and are copied (the clock by
// pointer, so it must outlive the policy). The policy starts with no rendered
// frame, so the first evaluate() returns UPDATE_FULL.
void update_policy_init(update_policy_t *p, const port_clock_t *clock,
                        const update_policy_config_t *cfg, const update_policy_limits_t *limits);

// Decides what to do with `reading` at the clock's current time. The only
// thing it records is what it saw: which readings have been off the frame on
// the glass by more than half their delta, and since when. Nothing about the
// redraw itself -- the caller flushes first and reports the outcome through
// update_policy_commit(), so a failed flush does not consume the budget.
// Calling it again at the same time with the same reading changes nothing.
void update_policy_evaluate(update_policy_t *p, const air_reading_t *reading,
                            update_decision_t *out);

// The same decision for a screen that compares what it would draw with what
// it drew itself: `changed` is a redraw due for UPDATE_REASON_STATUS, under
// the same interval, budget and panel rules -- or none. A request and the
// first frame are due as in update_policy_evaluate().
void update_policy_evaluate_content(const update_policy_t *p, bool changed, update_decision_t *out);

// The same for a screen whose content redraws stay partial, without a count
// limit or budget-forced full refreshes. Content is held to
// min_redraw_interval_ms as usual. Requests (screen changes) and the first
// frame are full. On a panel with no partial mode the normal full-refresh
// rules apply, as in update_policy_evaluate_content().
void update_policy_evaluate_partial_content(const update_policy_t *p, bool changed,
                                            update_decision_t *out);

// A person asked for a redraw: the screen they are looking at is to change.
// The next evaluate() says it is due now, with UPDATE_REASON_REQUEST, without
// waiting for min_redraw_interval_ms, and always as UPDATE_FULL -- neither
// the budget nor the panel's minimum full-refresh interval holds it. Pending
// until a redraw is committed.
void update_policy_request(update_policy_t *p);

// Records a redraw that actually reached the panel: `rendered` becomes the
// baseline the next deltas and offsets are measured against, the
// partial-refresh budget moves, and a request is answered. A decision with action UPDATE_NONE is
// ignored.
void update_policy_commit(update_policy_t *p, const air_reading_t *rendered,
                          const update_decision_t *decision);

// Partial refreshes performed since the last full one, for logging.
uint32_t update_policy_partials_since_full(const update_policy_t *p);

// Stable, allocation-free names for logs. Never NULL.
const char *update_policy_action_name(update_action_t action);
const char *update_policy_reason_name(update_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_APP_UPDATE_POLICY_H
