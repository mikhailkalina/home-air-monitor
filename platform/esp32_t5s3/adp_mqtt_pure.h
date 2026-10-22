// SPDX-License-Identifier: Apache-2.0
//
// The decisions platform/esp32_t5s3/adp_mqtt.c makes about esp-mqtt, kept
// free of ESP-IDF headers so the host build can test them
// (tests/unit/test_adp_mqtt_pure.c), as adp_net_pure.c is
// (docs/adr/0033-mqtt-on-the-board.md). Everything that calls esp-mqtt or
// FreeRTOS stays in adp_mqtt.c:
//
//   * the broker's address from the image's configuration, plain MQTT only;
//   * the queues between the event loop, the adapter's task and esp-mqtt's
//     task: rings of slots with one producer and one consumer each, whose
//     indices the adapter moves under its spinlock and whose slots it fills
//     and empties outside it, so that no copy is made with interrupts off;
//   * a received message put together from the pieces esp-mqtt hands over
//     when it is longer than its buffer;
//   * the session: which one the caller holds, whether the broker accepted
//     it, whether it is over and why -- and, from that, what the adapter's
//     task is to do with esp-mqtt's client: stop it, start one, or hand it
//     what waits. Each session has a client of its own, numbered: an event
//     from a client the caller has let go of changes nothing.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_PURE_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_PURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adp_mqtt_limits.h"
#include "hal_status.h"
#include "port_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- the broker's address --------------------------------------------------------

typedef struct {
    char host[ADP_MQTT_HOST_MAX + 1u];
    uint16_t port;
} adp_mqtt_broker_t;

// Parses "mqtt://<host>[:<port>]": a host name or an IPv4 address of
// [A-Za-z0-9.-_], 1..ADP_MQTT_HOST_MAX characters, then an optional port,
// 1..65535 (ADP_MQTT_DEFAULT_PORT without one), and nothing more. TLS
// ("mqtts://"), websockets, a user or a password in the URI, a path: refused.
// NULL when `uri` is usable, `out` filled; else a sentence saying why not,
// and `out` zeroed.
const char *adp_mqtt_parse_uri(const char *uri, adp_mqtt_broker_t *out);

// --- a ring of slots: one producer, one consumer ---------------------------------
//
// The caller keeps the slots and the lock; this keeps the indices. The
// producer reserves a slot, fills it, and commits it; the consumer peeks at
// the oldest committed slot, empties it, and releases it. Only the index
// operations need the lock: a reserved slot is the producer's alone until it
// commits, and a committed one the consumer's until it releases -- the
// reserved slot is always the one after the newest committed, wherever the
// consumer has got to.

typedef struct {
    uint16_t capacity;
    uint16_t head;   // the oldest committed slot
    uint16_t count;  // committed and not yet released
    bool reserved;   // the producer holds the slot after them
} adp_mqtt_ring_t;

void adp_mqtt_ring_init(adp_mqtt_ring_t *r, uint16_t capacity);

// The slot to fill, or -1: every slot committed, or one reserved already.
int adp_mqtt_ring_reserve(adp_mqtt_ring_t *r);
void adp_mqtt_ring_commit(adp_mqtt_ring_t *r);
void adp_mqtt_ring_cancel(adp_mqtt_ring_t *r);

// The oldest committed slot, or -1 when none is.
int adp_mqtt_ring_peek(const adp_mqtt_ring_t *r);
void adp_mqtt_ring_release(adp_mqtt_ring_t *r);

// --- what goes to esp-mqtt ---------------------------------------------------------

typedef enum {
    ADP_MQTT_OUT_PUBLISH = 0,
    ADP_MQTT_OUT_SUBSCRIBE,
} adp_mqtt_out_kind_t;

// One slot of the outgoing ring: what publish() or subscribe() accepted.
typedef struct {
    uint32_t session;  // the generation it was accepted in
    uint8_t kind;      // adp_mqtt_out_kind_t
    uint8_t qos;
    bool retain;
    uint16_t len;  // the payload's
    char topic[PORT_TELEMETRY_TOPIC_MAX];
    uint8_t payload[PORT_TELEMETRY_PAYLOAD_MAX];
} adp_mqtt_out_t;

// --- what arrives: a message in pieces ------------------------------------------

// One piece, as esp-mqtt's MQTT_EVENT_DATA hands it over: only the first
// carries the topic; `offset` is where its data starts in the payload,
// `total` the payload's whole length.
typedef struct {
    const char *topic;
    size_t topic_len;
    const uint8_t *data;
    size_t data_len;
    size_t offset;
    size_t total;
    bool retain;
} adp_mqtt_piece_t;

typedef enum {
    ADP_MQTT_PIECE_MORE = 0,  // a part of a message: more to come
    ADP_MQTT_PIECE_DONE,      // the message is whole, in the assembly's `msg`
    ADP_MQTT_PIECE_REFUSED,   // a message the port cannot hold: no topic, or one too long
    ADP_MQTT_PIECE_STRAY,     // a piece of no message under way: dropped
} adp_mqtt_piece_result_t;

typedef struct {
    telemetry_message_t msg;
    bool under_way;
    size_t next_offset;
    size_t total;
} adp_mqtt_assembly_t;

void adp_mqtt_assembly_reset(adp_mqtt_assembly_t *a);

// Adds `p`. The payload is kept to PORT_TELEMETRY_RX_PAYLOAD_MAX bytes, the
// rest dropped and the message marked truncated (ports/port_telemetry.h).
adp_mqtt_piece_result_t adp_mqtt_assemble(adp_mqtt_assembly_t *a, const adp_mqtt_piece_t *p);

// --- the session -----------------------------------------------------------------

// Why a session ended without disconnect().
typedef enum {
    ADP_MQTT_END_NONE = 0,
    ADP_MQTT_END_NO_CONNECTION,  // no connection, or no answer: the name, the route, no broker
    ADP_MQTT_END_REFUSED,        // the broker answered the CONNECT with a refusal
    ADP_MQTT_END_LOST,           // a session that worked ended by itself
    ADP_MQTT_END_NETWORK,        // the network under it went (port_net)
    ADP_MQTT_END_NOT_STARTED,    // esp-mqtt would not make or start a client
} adp_mqtt_end_t;

typedef enum {
    ADP_MQTT_STATE_IDLE = 0,    // no session asked for: none yet, or disconnect()
    ADP_MQTT_STATE_CONNECTING,  // asked for, not yet accepted
    ADP_MQTT_STATE_CONNECTED,   // accepted
    ADP_MQTT_STATE_OVER,        // ended by itself: the caller has not let go yet
} adp_mqtt_state_t;

// The session the caller holds. Generation 0 is none: the first connect()
// makes 1, and every client esp-mqtt runs belongs to one generation.
typedef struct {
    uint32_t gen;
    bool wanted;    // connect() since, and no disconnect()
    bool accepted;  // the broker accepted it
    bool over;      // it ended by itself
    adp_mqtt_end_t end;
} adp_mqtt_session_t;

void adp_mqtt_session_init(adp_mqtt_session_t *s);

// connect()'s answer for the session state, the arguments being valid:
// HAL_ERR_BUSY while one is held and not over, HAL_ERR_NOT_READY without the
// network, else HAL_OK and a new generation.
hal_status_t adp_mqtt_session_connect(adp_mqtt_session_t *s, bool net_up);

// The caller lets go: whatever esp-mqtt still does for it is moot.
void adp_mqtt_session_disconnect(adp_mqtt_session_t *s);

// What is_connected() says with the network as it is now: accepted, held, not
// over -- and the network up. A network gone ends the session for good
// (ADP_MQTT_END_NETWORK): the port's "a drop the adapter has noticed".
bool adp_mqtt_session_usable(adp_mqtt_session_t *s, bool net_up);

// True while the caller holds generation `gen` and it is not over: what a
// received message needs to be kept.
bool adp_mqtt_session_holds(const adp_mqtt_session_t *s, uint32_t gen);

// esp-mqtt's client of generation `gen` says the broker accepted it. True
// when that is news for the session held.
bool adp_mqtt_session_accepted(adp_mqtt_session_t *s, uint32_t gen);

// ... says its connection ended, `refused` when the broker had refused it.
// True when that ends the session held: ADP_MQTT_END_LOST if it was accepted,
// else REFUSED or NO_CONNECTION.
bool adp_mqtt_session_ended(adp_mqtt_session_t *s, uint32_t gen, bool refused);

// ... could not be made or started: the session held ends, NOT_STARTED.
void adp_mqtt_session_not_started(adp_mqtt_session_t *s, uint32_t gen);

adp_mqtt_state_t adp_mqtt_session_state(const adp_mqtt_session_t *s);

// What the adapter's task does next, given the generation of the client it
// runs (`client_gen`, 0 for none) and the last generation it started a client
// for (`started_gen`): stop a client the session no longer needs; start one
// for a session that has none, once per generation; hand a client the
// session's queued messages once the broker has accepted it.
typedef enum {
    ADP_MQTT_PLAN_IDLE = 0,
    ADP_MQTT_PLAN_STOP,
    ADP_MQTT_PLAN_START,
    ADP_MQTT_PLAN_FORWARD,
} adp_mqtt_plan_t;

adp_mqtt_plan_t adp_mqtt_session_plan(const adp_mqtt_session_t *s, uint32_t client_gen,
                                      uint32_t started_gen);

// --- a session's subscriptions ----------------------------------------------------

typedef struct {
    uint32_t gen;  // the session they are for: another one starts with none
    size_t count;
    char filters[ADP_MQTT_MAX_SUBSCRIPTIONS][PORT_TELEMETRY_TOPIC_MAX];
} adp_mqtt_subs_t;

void adp_mqtt_subs_init(adp_mqtt_subs_t *subs);

// Room for `filter` in session `gen`: true when it is one already asked for
// in that session, or a new one fits -- then it is recorded.
bool adp_mqtt_subs_take(adp_mqtt_subs_t *subs, uint32_t gen, const char *filter);

// --- names, for the log and the console ----------------------------------------------

// "none", "no connection", "refused", "lost", "network", "not started".
const char *adp_mqtt_end_name(adp_mqtt_end_t end);

// "idle", "connecting", "connected", "over".
const char *adp_mqtt_state_name(adp_mqtt_state_t state);

// A CONNACK's return code (MQTT 3.1.1, 3.2.2.3): "accepted", "bad protocol
// version", "client id rejected", "server unavailable", "bad user name or
// password", "not authorised", "unknown".
const char *adp_mqtt_connack_name(int code);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_PURE_H
