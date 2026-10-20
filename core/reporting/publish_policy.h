// SPDX-License-Identifier: Apache-2.0
//
// When to publish the state message
// (docs/adr/0030-telemetry-ports-and-the-reporter.md):
//
//   * at once in a new session -- the reporter says so with
//     publish_policy_reset();
//   * when a value's status changes, or IAQ's accuracy -- each is in the
//     message (mqtt_payload.h), and a sensor error must reach MQTT as it
//     reaches the screen (requirements 15.1);
//   * when a value has moved by strictly more than its delta since the last
//     message;
//   * at least once per heartbeat, which Home Assistant's expire_after counts
//     on (ha_discovery.h);
//   * at most once per sensor cycle (min_interval_ms): a message due sooner
//     after the last waits, and carries whatever else changed meanwhile.
//
// Its own module rather than a part of update_policy: the display's
// thresholds are about panel wear -- a budget of partial refreshes, a
// minimum interval between full ones -- and these are about traffic and Home
// Assistant's history. The two share a shape and, today, their deltas, not
// their reasons; architecture section 3 had them in one file, and ADR 0030
// records why not.
//
// Pure: no I/O, no allocation, every time from the caller.

#ifndef HAC_CORE_REPORTING_PUBLISH_POLICY_H
#define HAC_CORE_REPORTING_PUBLISH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PUBLISH_POLICY_NO_DEADLINE UINT64_MAX

typedef struct {
    // A value must move by strictly more than its delta to be due; 0 makes
    // any change due.
    float co2_delta_ppm;
    float temperature_delta_c;
    float humidity_delta_rh;
    float pressure_delta_hpa;
    float iaq_delta;

    uint32_t heartbeat_ms;     // at least 1
    uint32_t min_interval_ms;  // 0 lets every due message out at once
} publish_policy_config_t;

typedef enum {
    PUBLISH_REASON_NONE = 0,
    PUBLISH_REASON_SESSION,  // a new session: nothing published in it yet
    PUBLISH_REASON_STATUS,   // a value's status, or IAQ's accuracy
    PUBLISH_REASON_CO2,
    PUBLISH_REASON_TEMPERATURE,
    PUBLISH_REASON_HUMIDITY,
    PUBLISH_REASON_PRESSURE,
    PUBLISH_REASON_IAQ,
    PUBLISH_REASON_HEARTBEAT,
    PUBLISH_REASON_COUNT,
} publish_reason_t;

typedef struct {
    bool due;                 // publish now
    publish_reason_t reason;  // why -- also while held
    bool held_for_interval;   // due but for min_interval_ms; next_deadline_ms ends the hold
    // When to evaluate again without new data: now when due, the end of the
    // hold, or the heartbeat.
    uint64_t next_deadline_ms;
} publish_decision_t;

typedef struct {
    publish_policy_config_t cfg;
    air_reading_t last;  // what the last message carried
    bool have_last;      // false until the first message of a session
    uint64_t last_ms;
} publish_policy_t;

// REPORT_* of report_limits.h.
publish_policy_config_t publish_policy_config_default(void);

// Nothing published yet: the first evaluate() is due, PUBLISH_REASON_SESSION.
void publish_policy_init(publish_policy_t *p, const publish_policy_config_t *cfg);

// A new session: the next message is due at once, whatever the last one was.
void publish_policy_reset(publish_policy_t *p);

// Whether `reading` is to be published at `now_ms`. Records nothing.
void publish_policy_evaluate(const publish_policy_t *p, const air_reading_t *reading,
                             uint64_t now_ms, publish_decision_t *out);

// The message carrying `published` was accepted at `now_ms`: the baseline
// for the deltas and the heartbeat from here.
void publish_policy_commit(publish_policy_t *p, const air_reading_t *published, uint64_t now_ms);

// "none", "session", "status", "co2", "temperature", "humidity", "pressure",
// "iaq", "heartbeat"; never NULL.
const char *publish_policy_reason_name(publish_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_REPORTING_PUBLISH_POLICY_H
