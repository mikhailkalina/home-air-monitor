// SPDX-License-Identifier: Apache-2.0
//
// An MQTT broker in memory, and port_telemetry as a client of it: the MQTT
// counterpart of the I2C device models (platform/host/i2c_models/), for the
// simulator and the tests (docs/adr/0030-telemetry-ports-and-the-reporter.md).
//
// The broker holds what a real one does, as MQTT 3.1.1 has it:
//
//   * retained messages, one per topic; an empty retained message clears
//     its topic (3.3.1.3). A new subscription gets the kept messages that
//     match it, marked retained; a live delivery is never marked (3.3.1.3).
//   * subscriptions with '+' and '#' (4.7), one delivery per client however
//     many of its filters match; a filter equal to one the client has
//     replaces it, and the kept messages come again (3.8.4).
//   * sessions by client id, each starting clean: a new connection with an
//     id already connected ends the old one, as if its network had failed
//     (3.1.4).
//   * the last will, published when a session ends any way but disconnect():
//     one and a half keepalives after its network went (3.1.2.10), or at
//     once when another connection takes its id.
//   * a scriptable refusal: the next attempts refused at once (a CONNACK
//     with an error: connect() says HAL_ERR_IO) or never answered (connect()
//     starts, no session ever comes); and a stall, in which the broker takes
//     no message, so the clients' queues fill.
//
// A client's queue (ADP_TELEMETRY_LOOPBACK_OUTBOX messages) is handed to the
// broker at once unless the broker is stalled or the client's network is
// gone; messages waiting when a session ends are lost, as the port allows.
// The network under a client is the composition's: adp_net_sim's listener,
// or a test's adp_telemetry_loopback_set_link().
//
// Time is the clock the broker is given: a will falls due on it, and the
// broker publishes due wills whenever any of its clients is called, or
// loopback_broker_service() is. Nothing here blocks or allocates.
//
// What the broker does is told to a tap, as it happens -- the simulator's
// --telemetry stdout prints every message, --telemetry loopback the sessions
// and the wills.

#ifndef HAC_PLATFORM_HOST_ADP_TELEMETRY_LOOPBACK_H
#define HAC_PLATFORM_HOST_ADP_TELEMETRY_LOOPBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOPBACK_BROKER_MAX_CLIENTS 4u
#define LOOPBACK_BROKER_MAX_RETAINED 32u
#define ADP_TELEMETRY_LOOPBACK_MAX_SUBSCRIPTIONS 8u
#define ADP_TELEMETRY_LOOPBACK_OUTBOX 16u
#define ADP_TELEMETRY_LOOPBACK_INBOX 16u

#define LOOPBACK_BROKER_NO_DEADLINE UINT64_MAX

// Refuse the attempts indefinitely, until loopback_broker_refuse() says
// otherwise.
#define LOOPBACK_BROKER_REFUSE_ALWAYS UINT32_MAX

typedef struct {
    char topic[PORT_TELEMETRY_TOPIC_MAX];
    uint8_t payload[PORT_TELEMETRY_PAYLOAD_MAX];
    size_t len;
    uint8_t qos;
    bool retain;
} loopback_message_t;

typedef enum {
    LOOPBACK_ACCEPT = 0,  // sessions accepted
    LOOPBACK_REFUSE,      // refused at once: connect() is HAL_ERR_IO
    LOOPBACK_SILENT,      // never answered: connect() starts, no session comes
} loopback_refusal_t;

typedef enum {
    LOOPBACK_EVENT_CONNECTED = 0,
    LOOPBACK_EVENT_DISCONNECTED,  // disconnect(): no will
    LOOPBACK_EVENT_DROPPED,       // the network under it went; a will may follow
    LOOPBACK_EVENT_TAKEN_OVER,    // another connection took its id
    LOOPBACK_EVENT_REFUSED,       // an attempt refused or left unanswered
    LOOPBACK_EVENT_MESSAGE,       // a message taken from a client
    LOOPBACK_EVENT_WILL,          // a last will published
} loopback_event_t;

// `msg` is the message for MESSAGE and WILL, NULL otherwise; `client_id` the
// client's. Both borrowed for the call.
typedef void (*loopback_tap_fn)(void *ctx, loopback_event_t event, const char *client_id,
                                const loopback_message_t *msg, uint64_t at_ms);

// Something arrived for the client, or its session went away by itself.
typedef void (*loopback_waker_fn)(void *ctx);

typedef struct adp_telemetry_loopback_s adp_telemetry_loopback_t;

typedef struct {
    char client_id[PORT_TELEMETRY_CLIENT_ID_MAX + 1u];
    loopback_message_t msg;
    uint64_t due_ms;
    bool pending;
} loopback_will_t;

typedef struct {
    const port_clock_t *clock;
    loopback_message_t retained[LOOPBACK_BROKER_MAX_RETAINED];
    size_t retained_count;
    adp_telemetry_loopback_t *clients[LOOPBACK_BROKER_MAX_CLIENTS];
    size_t client_count;
    loopback_will_t wills[LOOPBACK_BROKER_MAX_CLIENTS];

    loopback_refusal_t refusal;
    uint32_t refuse_attempts;  // still to refuse; LOOPBACK_BROKER_REFUSE_ALWAYS: for good
    bool stalled;

    loopback_tap_fn tap;
    void *tap_ctx;

    // Counters.
    uint32_t sessions;
    uint32_t refused;
    uint32_t messages;  // taken from clients
    uint32_t wills_published;
    uint32_t retained_overflow;  // a retained message with no room to keep it
} loopback_broker_t;

struct adp_telemetry_loopback_s {
    port_telemetry_t port;
    loopback_broker_t *broker;

    bool link;  // the network under it
    bool connected;
    bool attempting;  // an attempt the broker leaves unanswered
    char client_id[PORT_TELEMETRY_CLIENT_ID_MAX + 1u];
    uint16_t keepalive_s;
    bool has_will;
    loopback_message_t will;

    struct {
        char filter[PORT_TELEMETRY_TOPIC_MAX];
        uint8_t qos;
    } subs[ADP_TELEMETRY_LOOPBACK_MAX_SUBSCRIPTIONS];
    size_t sub_count;

    loopback_message_t outbox[ADP_TELEMETRY_LOOPBACK_OUTBOX];
    size_t out_head;
    size_t out_count;

    telemetry_message_t inbox[ADP_TELEMETRY_LOOPBACK_INBOX];
    size_t in_head;
    size_t in_count;

    loopback_waker_fn waker;
    void *waker_ctx;

    // Counters.
    uint32_t published;      // accepted by publish()
    uint32_t outbox_lost;    // accepted, and lost with the session
    uint32_t inbox_dropped;  // arrived with the inbox full
};

// --- the broker ----------------------------------------------------------------

// An empty broker, accepting sessions. `clock` must outlive it.
void loopback_broker_init(loopback_broker_t *b, const port_clock_t *clock);

void loopback_broker_set_tap(loopback_broker_t *b, loopback_tap_fn fn, void *ctx);

// The next `attempts` connection attempts are met `how`; LOOPBACK_ACCEPT, or
// 0 attempts, accepts again.
void loopback_broker_refuse(loopback_broker_t *b, loopback_refusal_t how, uint32_t attempts);

// Takes no message while `stalled`; when it ends, takes what waits.
void loopback_broker_stall(loopback_broker_t *b, bool stalled);

// Publishes the wills that have fallen due.
void loopback_broker_service(loopback_broker_t *b);

// When the next will falls due, or LOOPBACK_BROKER_NO_DEADLINE.
uint64_t loopback_broker_next_ms(const loopback_broker_t *b);

// What the broker keeps for `topic`, or NULL.
const loopback_message_t *loopback_broker_retained(const loopback_broker_t *b, const char *topic);

// MQTT 3.1.1's matching of a topic against a filter (4.7), with '$' topics
// out of reach of a leading wildcard (4.7.2).
bool loopback_filter_matches(const char *filter, const char *topic);

// --- a client: port_telemetry ------------------------------------------------------

// A client of `broker`, its network there, not connected. HAL_ERR_NO_MEM when
// the broker has LOOPBACK_BROKER_MAX_CLIENTS already.
hal_status_t adp_telemetry_loopback_init(adp_telemetry_loopback_t *c, loopback_broker_t *broker);

port_telemetry_t *adp_telemetry_loopback_port(adp_telemetry_loopback_t *c);

void adp_telemetry_loopback_set_waker(adp_telemetry_loopback_t *c, loopback_waker_fn fn, void *ctx);

// The network under the client goes (`present` false) or comes back, at
// `at_ms`: a session it carried is dropped then. The signature of
// adp_net_sim's listener, so a composition can hand it over as one with the
// client as its context (adp_telemetry_loopback_net_listener()).
void adp_telemetry_loopback_set_link(adp_telemetry_loopback_t *c, bool present, uint64_t at_ms);
void adp_telemetry_loopback_net_listener(void *client, bool present, uint64_t at_ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_TELEMETRY_LOOPBACK_H
