// SPDX-License-Identifier: Apache-2.0
//
// When to redraw the panel.
//
// Two independent sets of inputs meet here, and keeping them separate is the
// point of the module:
//
//   * update_policy_config_t -- what the *product* wants: redraw when CO2 has
//     moved by more than 50 ppm, temperature by 0.2 C, humidity by 1 %, or
//     when two minutes have passed (docs/requirements.md 9.1);
//   * update_policy_limits_t -- what the *panel* tolerates: how often a full
//     refresh may happen and how many partial refreshes may precede it. These
//     are read out of port_display, never written down here. There is no
//     ED047TC1 constant in this file, and there must never be one: a second
//     panel reports different numbers through the same port and this logic is
//     unchanged.
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

// The thresholds of docs/requirements.md 9.1, named so that the only place
// they appear as numbers is here.
#define UPDATE_POLICY_DEFAULT_CO2_DELTA_PPM 50.0f
#define UPDATE_POLICY_DEFAULT_TEMPERATURE_DELTA_C 0.2f
#define UPDATE_POLICY_DEFAULT_HUMIDITY_DELTA_RH 1.0f
#define UPDATE_POLICY_DEFAULT_MAX_INTERVAL_MS 120000u

// Returned as next_deadline_ms when no timer is pending: only new data can
// change the answer, so there is nothing to wake up for.
#define UPDATE_POLICY_NO_DEADLINE UINT64_MAX

typedef struct {
    // A quantity must move by strictly more than its threshold to trigger a
    // redraw. A threshold of 0 therefore redraws on any change at all.
    float co2_delta_ppm;
    float temperature_delta_c;
    float humidity_delta_rh;

    // Redraw this long after the last one even if nothing moved, so the
    // "updated N ago" line stays honest. 0 disables the periodic redraw.
    uint32_t max_interval_ms;
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
// because the panel's minimum interval deferred it, so a log can say what was
// wanted and what stopped it.
typedef enum {
    UPDATE_REASON_NONE = 0,
    UPDATE_REASON_FIRST_FRAME,
    UPDATE_REASON_CO2,
    UPDATE_REASON_TEMPERATURE,
    UPDATE_REASON_HUMIDITY,
    UPDATE_REASON_STATUS,  // a value_status_t or a link flag changed
    UPDATE_REASON_PERIODIC,
} update_reason_t;

typedef struct {
    update_action_t action;
    update_reason_t reason;

    // The redraw would have been partial, and was escalated to full only
    // because the panel's partial-refresh budget is spent.
    bool full_forced_by_budget;

    // action is UPDATE_NONE only because a full refresh is due but the
    // panel's minimum full-refresh interval has not elapsed. The redraw is
    // deferred, never downgraded to a partial: downgrading is exactly the
    // behaviour the vendor warns damages the panel.
    bool deferred_by_min_interval;

    // When to call update_policy_evaluate() again in the absence of new data:
    // now_ms when a redraw is due, the deferral point when one is blocked, the
    // periodic deadline otherwise, UPDATE_POLICY_NO_DEADLINE when no timer is
    // pending.
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
} update_policy_t;

// The thresholds of docs/requirements.md 9.1.
update_policy_config_t update_policy_config_default(void);

// Reads the panel's constraints off the port. A NULL display yields limits
// that permit nothing but full refreshes, which is the safe direction.
update_policy_limits_t update_policy_limits_from_display(const port_display_t *display);

// `clock`, `cfg` and `limits` are all required and are copied (the clock by
// pointer, so it must outlive the policy). The policy starts with no rendered
// frame, so the first evaluate() returns UPDATE_FULL.
void update_policy_init(update_policy_t *p, const port_clock_t *clock,
                        const update_policy_config_t *cfg, const update_policy_limits_t *limits);

// Decides what to do with `reading` at the clock's current time. Does not
// mutate the policy: the caller flushes first and reports the outcome through
// update_policy_commit(), so a failed flush does not consume the budget.
void update_policy_evaluate(const update_policy_t *p, const air_reading_t *reading,
                            update_decision_t *out);

// Records a redraw that actually reached the panel: `rendered` becomes the
// baseline the next deltas are measured against, and the partial-refresh
// budget moves. A decision with action UPDATE_NONE is ignored.
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
