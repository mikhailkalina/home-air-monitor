// SPDX-License-Identifier: Apache-2.0
//
// The reporter: the state machine between the network (ports/port_net.h),
// an MQTT session (ports/port_telemetry.h) and the readings
// (docs/adr/0030-telemetry-ports-and-the-reporter.md). It decides everything
// the ports do not:
//
//   * when to connect: whenever the network is up and no session is, at once
//     after a session that worked drops, then after a backoff that doubles
//     while attempts fail. An attempt that has not connected within
//     connect_timeout_ms is given up. The session's last will is "offline",
//     retained, on the status topic (core/reporting/topics.h).
//   * what every session starts with: the subscription to Home Assistant's
//     status, then "online", then every discovery config (ha_discovery.h) --
//     the empty ones too, which remove what this build does not provide --
//     then the state, at once.
//   * Home Assistant restarting: its birth message ("online" on
//     <discovery prefix>/status, sent live, not from the broker's store)
//     makes every discovery config due again.
//   * when the state goes out: core/reporting/publish_policy.h.
//   * what waits: nothing published while there is no session is queued for
//     the next one -- the card keeps the history (ADR 0024), and a session
//     starts with the newest state. Within a session a refused message
//     (HAL_ERR_NO_MEM: the adapter's queue full) is tried again after
//     retry_ms, and the state then carries the newest reading, not the one
//     refused.
//
// The state message carries the reading the screen shows, with the network's
// signal strength set here from port_net (mqtt_payload.h's "rssi"): the
// screen's reading is never written to. What the screen says about the link
// is its own: reporter_link() is the link as it is now, which the
// composition holds (core/app/link_hold.h) before the screen shows it, and
// reporter_status() is what the diagnostics screen shows
// (docs/adr/0031-the-network-on-the-screen.md).
//
// The event loops call reporter_step() after each poll, as they call the
// recorder's watch, and wake at the deadline it returns among the others'.
// Not app_core (docs/architecture.md 6.1), which still does not exist: ADR
// 0030 says why not yet.
//
// Pure, like the sensor manager: no allocation, no stdio, every time from the
// caller. It logs what changes -- a session up or lost, attempts failing, a
// refusal, Home Assistant's birth -- once per change. Each state message is
// in the step's note (reporter_note_t), which the loops log as they log
// update_policy's decisions.

#ifndef HAC_CORE_REPORTING_REPORTER_H
#define HAC_CORE_REPORTING_REPORTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/measurement.h"
#include "hal_status.h"
#include "port_log.h"
#include "port_net.h"
#include "port_telemetry.h"
#include "reporting/ha_discovery.h"
#include "reporting/mqtt_payload.h"
#include "reporting/publish_policy.h"
#include "reporting/topics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REPORTER_NO_DEADLINE UINT64_MAX

typedef enum {
    REPORTER_OFFLINE = 0,  // no session: waiting for the network or the next attempt
    REPORTER_CONNECTING,   // an attempt under way
    REPORTER_ONLINE,       // a session
} reporter_state_t;

typedef struct {
    uint16_t keepalive_s;
    uint32_t connect_timeout_ms;
    uint32_t backoff_first_ms;
    uint32_t backoff_max_ms;
    uint32_t retry_ms;
    publish_policy_config_t policy;
} reporter_config_t;

typedef struct {
    port_telemetry_t *telemetry;  // required
    const port_net_t *net;        // may be NULL: no network to watch, the link is taken as up
    const port_log_t *log;        // may be NULL
    const mqtt_topics_t *topics;  // required, from mqtt_topics_init()
    // The discovery's device, origin, capabilities and has_rssi. Its topics
    // and its heartbeat are set by reporter_init() -- the topics above, the
    // policy's heartbeat -- so the configs and the messages agree. The
    // strings it points to must outlive the reporter.
    ha_discovery_t discovery;
} reporter_deps_t;

// What one step did, for the loop's log.
typedef struct {
    bool attempted;               // a connection attempt began
    hal_status_t attempt_status;  // connect()'s answer, when attempted
    bool attempt_given_up;        // an attempt reached connect_timeout_ms
    bool session_started;         // a session began
    bool session_ended;           // one ended: dropped, or the network went
    bool network_lost;            // ... because the network is not up
    uint32_t session;             // the current or last session, from 1; 0 before any
    bool birth;                   // Home Assistant's birth arrived: discovery is due again
    bool online_published;
    unsigned discovery_published;
    bool state_published;
    publish_reason_t reason;  // the state message's
    hal_status_t refused;     // HAL_OK, or what refused the last message tried
} reporter_note_t;

// What the reporter and its network say now, as plain data for the
// diagnostics screen (core/ui/diagnostics_view.h), read after a step.
typedef struct {
    link_status_t link;  // reporter_link(): the link as it is now
    bool has_net;        // a network to ask; without one the next three say nothing
    net_state_t net;
    int8_t rssi_dbm;      // PORT_NET_RSSI_NONE while the network is not up
    net_info_t net_info;  // its name, and the address while up
    reporter_state_t state;
    uint32_t state_messages;     // since reporter_init(): the readings sent
    uint32_t refusals;           // messages the adapter refused (its queue full)
    bool published;              // a state message has gone out since reporter_init()
    uint64_t last_published_ms;  // and when the last one went, on port_clock
} reporter_status_t;

typedef struct {
    port_telemetry_t *telemetry;
    const port_net_t *net;
    const port_log_t *log;
    const mqtt_topics_t *topics;
    ha_discovery_t discovery;
    reporter_config_t cfg;
    publish_policy_t policy;

    reporter_state_t state;
    uint64_t retry_at_ms;          // OFFLINE: the next attempt, no sooner
    uint64_t attempt_deadline_ms;  // CONNECTING: given up at
    uint32_t backoff_ms;           // the wait after the next failure; 0 before any
    bool attempt_failure_logged;

    // Owed in this session, in this order.
    bool need_subscribe;
    bool need_online;
    uint32_t need_discovery;       // a bit per ha_entity_t
    uint64_t retry_publish_at_ms;  // after a refusal, nothing is tried before this
    bool refusal_logged;

    air_reading_t latest;  // what the next state message carries
    telemetry_message_t rx;
    char topic[MQTT_TOPIC_MAX];
    char buf[HA_DISCOVERY_CONFIG_MAX];

    // Since reporter_init().
    uint32_t attempts;
    uint32_t attempts_failed;
    uint32_t sessions;
    uint32_t sessions_lost;
    uint32_t births;
    uint32_t discovery_messages;
    uint32_t state_messages;
    uint32_t state_by_reason[PUBLISH_REASON_COUNT];
    uint32_t refusals;
    bool published;  // a state message, and when the last one went
    uint64_t last_published_ms;
} reporter_t;

// REPORT_* of report_limits.h.
reporter_config_t reporter_config_default(void);

// HAL_ERR_INVALID_ARG without a telemetry port or topics, with a heartbeat
// discovery cannot announce (1 s to HA_DISCOVERY_HEARTBEAT_MAX_S, whole
// seconds), or with a discovery block any config cannot be built from --
// checked here, once, so that no config ever fails later. Nothing is called
// on the ports until the first reporter_step().
hal_status_t reporter_init(reporter_t *r, const reporter_deps_t *deps,
                           const reporter_config_t *cfg);

// One step at `now_ms` (port_clock's, after the poll) with the merged
// reading: what arrived, the session, whatever is owed. Returns when it next
// wants to be called without anything arriving -- an attempt, a give-up, a
// retry, the heartbeat, the end of a hold -- or REPORTER_NO_DEADLINE while
// it waits for the network, whose adapter wakes the loop. `note` may be NULL.
uint64_t reporter_step(reporter_t *r, const air_reading_t *reading, uint64_t now_ms,
                       reporter_note_t *note);

// The link as it is now: LINK_NO_WIFI while the network is not up,
// LINK_NO_BROKER while it is and there is no session, LINK_ONLINE with one.
// Without a network port the network counts as up. LINK_OFF for a NULL or
// uninitialised reporter -- no telemetry.
link_status_t reporter_link(const reporter_t *r);

// Fills `out` from the reporter and its network port (state(), rssi_dbm(),
// info(): calls that never wait). A NULL or uninitialised reporter gives
// LINK_OFF and nothing else.
void reporter_status(const reporter_t *r, reporter_status_t *out);

// "offline", "connecting", "online"; never NULL.
const char *reporter_state_name(reporter_state_t state);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_REPORTING_REPORTER_H
