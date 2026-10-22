// SPDX-License-Identifier: Apache-2.0
//
// A publish/subscribe session with an MQTT broker
// (docs/architecture.md 5.6, docs/adr/0030-telemetry-ports-and-the-reporter.md).
//
// Implementations: the board's MQTT client over esp-mqtt
// (platform/esp32_t5s3/adp_mqtt.h, phase 4g) and, on a PC, a client of an
// in-memory broker model (platform/host/adp_telemetry_loopback.h) and one of
// a real broker over libmosquitto (platform/host/adp_telemetry_mqtt.h).
// What uses it: core/reporting/reporter.h, which owns every decision -- when
// to connect, what to publish and when to try again. The port decides none
// of them: it never reconnects on its own and never queues a message for a
// session that does not exist yet.
//
// Every call returns at once. A connection takes a TCP handshake and the
// broker's answer, a message is sent and acknowledged later; the adapter does
// that on its own task (or, on a PC, when its clock moves) and wakes the
// event loop when something happened -- a message arrived, the session came
// up or went away -- through a waker the composition gives it. So the core
// polls, as it polls port_input (ports/port_input.h): is_connected() for the
// session, poll() for what arrived. This is a departure from the sketch in
// docs/architecture.md 5.6, which had no poll(); ADR 0030 says why.
//
// What every call may answer:
//
//   HAL_OK               done, or begun
//   HAL_ERR_INVALID_ARG  a topic, filter or client id outside the rules below,
//                        a QoS above 2, a payload of PORT_TELEMETRY_PAYLOAD_MAX
//                        bytes or more, data == NULL with len > 0
//   HAL_ERR_NOT_READY    no session: never connected, still connecting, ended
//                        by disconnect() or dropped -- or, for connect(), no
//                        network to connect over. A state, not a fault.
//   HAL_ERR_NO_MEM       the adapter's queue is full: the broker has not taken
//                        what is already waiting. Nothing was accepted.
//   HAL_ERR_BUSY         connect() while connected or connecting
//   HAL_ERR_IO           connect(): the broker refused, and the adapter knew
//                        at once

#ifndef HAC_PORTS_PORT_TELEMETRY_H
#define HAC_PORTS_PORT_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// A topic or filter's length, terminator included.
#define PORT_TELEMETRY_TOPIC_MAX 128u

// A message's payload is shorter than this, on every implementation alike.
// Room for the longest discovery config (core/reporting/ha_discovery.h).
#define PORT_TELEMETRY_PAYLOAD_MAX 1024u

// A received message's payload kept by poll(); longer ones are cut and
// marked. What the core subscribes to is a word ("online").
#define PORT_TELEMETRY_RX_PAYLOAD_MAX 256u

// A client id's length, without the terminator.
#define PORT_TELEMETRY_CLIENT_ID_MAX 32u

// Every implementation holds at least this many subscriptions at once, and
// at least this many received messages until they are polled.
#define PORT_TELEMETRY_MIN_SUBSCRIPTIONS 4u
#define PORT_TELEMETRY_MIN_INBOX 8u

// What a session is opened with. Copied by connect(): nothing here needs to
// outlive the call.
typedef struct {
    // 1..PORT_TELEMETRY_CLIENT_ID_MAX characters of [A-Za-z0-9_-]. The broker
    // tells sessions apart by it: a second connection with the same id ends
    // the first.
    const char *client_id;

    // The broker drops a session it has heard nothing from for one and a half
    // times this (MQTT 3.1.1, 3.1.2.10); the adapter sends what keeps it
    // alive. 0: never dropped for silence.
    uint16_t keepalive_s;

    // The last will: what the broker publishes for the session when it ends
    // any way but disconnect() -- the network gone, the device silent past
    // its keepalive, another session taking its client id. `will_topic` NULL:
    // no will.
    const char *will_topic;
    const uint8_t *will_payload;
    size_t will_len;
    uint8_t will_qos;
    bool will_retain;
} telemetry_session_t;

// One message received for a subscription.
typedef struct {
    char topic[PORT_TELEMETRY_TOPIC_MAX];
    uint8_t payload[PORT_TELEMETRY_RX_PAYLOAD_MAX];
    size_t len;      // what `payload` holds
    bool truncated;  // the message was longer, and was cut at the end of `payload`
    // The broker sent it from its store because a subscription was just made
    // -- something published before -- rather than because it was published
    // now (MQTT 3.1.1, 3.3.1.3).
    bool retained;
} telemetry_message_t;

typedef struct port_telemetry_s port_telemetry_t;

struct port_telemetry_s {
    // Begins a session: never waits. HAL_OK once an attempt is under way;
    // is_connected() says when the broker has accepted it. An attempt that
    // never succeeds -- the broker unreachable, or refusing in a way the
    // adapter learns only later -- simply leaves is_connected() false: the
    // caller gives up on it with disconnect() after a time of its own.
    hal_status_t (*connect)(port_telemetry_t *self, const telemetry_session_t *session);

    // Ends the session, or gives up an attempt: never waits; is_connected()
    // is false once it returns. A session ended this way has no last will
    // published. What was accepted and not yet delivered may be lost. HAL_OK
    // also when there is nothing to end.
    hal_status_t (*disconnect)(port_telemetry_t *self);

    // Accepts one message for the broker: never waits, and copies the topic
    // and the payload before it returns. Accepted messages are delivered in
    // the order accepted while the session lasts; one still waiting when it
    // drops may be lost. An empty payload is a message -- retained, it
    // clears what the broker keeps for the topic.
    hal_status_t (*publish)(port_telemetry_t *self, const char *topic, const uint8_t *payload,
                            size_t len, uint8_t qos, bool retain);

    // Asks for the messages that match `filter`, for the rest of this
    // session -- and, at once, for the ones the broker keeps that match it,
    // marked retained. A filter equal to one already asked for replaces it,
    // and the kept messages come again. A subscription does not outlive its
    // session: after a connect, subscribe again. HAL_ERR_NO_MEM past the
    // adapter's subscriptions.
    hal_status_t (*subscribe)(port_telemetry_t *self, const char *filter, uint8_t qos);

    // True from the broker accepting the session until disconnect(), or a
    // drop the adapter has noticed.
    bool (*is_connected)(const port_telemetry_t *self);

    // Non-blocking: fills *out with the oldest message received and not yet
    // polled and returns true, or returns false when there is none. Messages
    // received before a drop may still be polled after it. Call it in a loop
    // until it returns false.
    bool (*poll)(port_telemetry_t *self, telemetry_message_t *out);

    void *impl;
};

// --- the rules, shared by every implementation -------------------------------

// A topic to publish to: 1..PORT_TELEMETRY_TOPIC_MAX - 1 bytes, no wildcard.
// (MQTT allows empty levels, and so does this.)
static inline bool port_telemetry_topic_ok(const char *topic)
{
    if (topic == NULL || topic[0] == '\0') {
        return false;
    }
    size_t n = 0u;
    for (const char *p = topic; *p != '\0'; ++p) {
        if (*p == '+' || *p == '#' || ++n >= PORT_TELEMETRY_TOPIC_MAX) {
            return false;
        }
    }
    return true;
}

// A filter to subscribe with: a topic whose levels may also be '+' -- one
// whole level -- or end in '#', a whole last level (MQTT 3.1.1, 4.7.1).
static inline bool port_telemetry_filter_ok(const char *filter)
{
    if (filter == NULL || filter[0] == '\0') {
        return false;
    }
    size_t n = 0u;
    for (const char *p = filter; *p != '\0'; ++p) {
        if (++n >= PORT_TELEMETRY_TOPIC_MAX) {
            return false;
        }
        const bool starts_level = (p == filter) || (p[-1] == '/');
        const bool ends_level = (p[1] == '\0') || (p[1] == '/');
        if (*p == '+' && !(starts_level && ends_level)) {
            return false;
        }
        if (*p == '#' && !(starts_level && p[1] == '\0')) {
            return false;
        }
    }
    return true;
}

// A client id: 1..PORT_TELEMETRY_CLIENT_ID_MAX characters of [A-Za-z0-9_-].
static inline bool port_telemetry_client_id_ok(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    size_t n = 0u;
    for (const char *p = id; *p != '\0'; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || ++n > PORT_TELEMETRY_CLIENT_ID_MAX) {
            return false;
        }
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_TELEMETRY_H
