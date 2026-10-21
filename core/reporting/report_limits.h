// SPDX-License-Identifier: Apache-2.0
//
// The reporter's and the publish policy's numbers, together
// (docs/adr/0030-telemetry-ports-and-the-reporter.md). Each says whether it
// is a choice or an estimate, and what would replace an estimate.

#ifndef HAC_CORE_REPORTING_REPORT_LIMITS_H
#define HAC_CORE_REPORTING_REPORT_LIMITS_H

#include "app/sensor_manager.h"

// --- what is published, and when (publish_policy.h) --------------------------

// CHOICE: the heartbeat, the longest the device goes without a state message
// while it has a session: requirements 11.2 asks for a publish every 30-60 s
// on USB power. Home Assistant's expire_after is three of these
// (HA_DISCOVERY_EXPIRE_HEARTBEATS), and so is the bound on how long HA shows
// a reading the device no longer has.
#define REPORT_HEARTBEAT_MS 60000u

// CHOICE: at most one state message per sensor cycle -- the two sensors are
// read at different moments of each, and a cycle in which both move costs
// one message, not two, as it costs one redraw (update_policy.h).
#define REPORT_MIN_INTERVAL_MS SENSOR_MANAGER_DEFAULT_INTERVAL_MS

// ESTIMATES: a value that has moved by strictly more than its delta since the
// last state message goes out before the heartbeat, within a sensor cycle.
// The display's deltas (update_policy.h), so that whatever the glass redraws
// for reaches Home Assistant as soon. Every message, the heartbeat's too,
// moves the baseline, so a drift slower than a delta a minute never fires
// one: only fast moves do. Sized by replaying the owner's recorded days of
// 2026-10-18 and 19 through the simulator (ADR 0030, "The thresholds"): 1,554
// and 1,597 messages a day, 8 % and 11 % over the heartbeat's 1,440, nearly
// all for IAQ; Home Assistant's IAQ more than 10 from the air 0.02 % of the
// time, against 1 % with the heartbeat alone. Half these deltas cost three
// times the extra messages and gain little. To replace: the board's own days
// with its broker, and how Home Assistant's database grows with them.
#define REPORT_CO2_DELTA_PPM 50.0f
#define REPORT_TEMPERATURE_DELTA_C 0.2f
#define REPORT_HUMIDITY_DELTA_RH 1.0f
#define REPORT_PRESSURE_DELTA_HPA 1.0f
#define REPORT_IAQ_DELTA 5.0f
// Gas resistance has none: it is a diagnostic in Home Assistant, it drifts
// all day, and it rides along with the others and the heartbeat.

// --- the session (reporter.h) ------------------------------------------------

// CHOICE: the session's keepalive. A broker drops a session it has heard
// nothing from for one and a half keepalives, 90 s, counted from the last
// packet (MQTT 3.1.1, 3.1.2.10); the heartbeat or a ping goes out at least
// every minute, so a device that vanishes without a word is reported offline
// by its last will 30 to 90 s later -- 34 s seen on a real broker (ADR 0030's
// amendment), inside expire_after's three heartbeats -- and keeping alive
// costs nothing more.
#define REPORT_KEEPALIVE_S 60u

// ESTIMATE: how long a connection attempt may take before it is given up --
// a TCP handshake and the broker's answer on a home network take tens of
// milliseconds; a name lookup or a slow broker may take seconds. Nothing has
// measured it: the board's first session (phase 4c) does.
#define REPORT_CONNECT_TIMEOUT_MS 10000u

// CHOICE: after a failed attempt, the next waits this long, doubling to the
// ceiling while attempts keep failing -- as the sensor manager's re-inits do
// (SENSOR_MANAGER_DEFAULT_BACKOFF_*). A broker that comes back is found
// within two minutes; an absent one costs an attempt every two minutes.
// After a session that worked drops, the first attempt is at once.
#define REPORT_BACKOFF_FIRST_MS 5000u
#define REPORT_BACKOFF_MAX_MS 120000u

// CHOICE: after the adapter's queue refused a message (HAL_ERR_NO_MEM), the
// next try. A second is many acknowledgements on a working link; a link that
// takes nothing for longer ends in a dropped session anyway.
#define REPORT_RETRY_MS 1000u

#endif  // HAC_CORE_REPORTING_REPORT_LIMITS_H
