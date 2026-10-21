// SPDX-License-Identifier: Apache-2.0
//
// port_telemetry over libmosquitto, to a real MQTT broker: how the simulator
// reaches a Home Assistant (docs/architecture.md 10, phase 4;
// docs/adr/0030-telemetry-ports-and-the-reporter.md, amendment "The
// simulator on a real broker"). Host only. libmosquitto is a system package,
// used under its EDL-1.0 (THIRD_PARTY_NOTICES.md); this adapter is built only
// when the library is found (platform/host/CMakeLists.txt), as the SDL window
// is, and nothing of it is ever part of a firmware image.
//
// One libmosquitto client per session, each on a network thread of its own
// that runs libmosquitto's loop (mosquitto_loop_forever()), because the port
// promises three things libmosquitto does not do by itself:
//
//   * nothing waits. connect() starts the thread and returns: the name
//     lookup, the TCP handshake and the CONNECT happen there. publish() and
//     subscribe() hand libmosquitto a copy, which the thread sends.
//   * the port never reconnects (ports/port_telemetry.h). libmosquitto's loop
//     connects again by itself after a drop; here a drop tells it to stop,
//     and the session is over. The reporter decides when to try again, and
//     the next connect() makes a new client.
//   * nothing is kept for a later session. libmosquitto resends what was not
//     acknowledged when it reconnects; a new client per session has nothing
//     to resend.
//
// What arrives -- the broker's answer, a message, the session going away --
// is put here, under a lock, by libmosquitto's callbacks on the network
// thread, and taken by the caller's next call: is_connected() for the
// session, poll() for the messages. After each, the waker is called, on that
// thread: it must be safe to call from any thread, and must not block.
//
// How a session ends, and what the broker makes of it:
//
//   * disconnect(): a DISCONNECT, so the broker drops the will. The next
//     session's CONNECT waits, on its own thread, until that has gone out
//     (at most ADP_TELEMETRY_MQTT_CLOSE_WAIT_MS), so that the broker never
//     meets the new connection before the goodbye of the old one -- it would
//     take the old session over and publish its will.
//   * the network taken away (adp_telemetry_mqtt_set_link(), the
//     simulator's --net-script): the session goes silent -- its thread is
//     stopped and the socket left open, as when Wi-Fi goes -- so the broker
//     drops it one and a half keepalives later and publishes the will; or at
//     once, when the network comes back and the old socket is closed.
//   * the broker going (a restart, a refusal): the session is over, and the
//     log says why.
//   * adp_telemetry_mqtt_destroy(): every socket closed without a
//     DISCONNECT, so the broker publishes the wills -- as when the board loses
//     its power.
//
// The broker's host name is looked up on the session's thread, at every
// connect(), so a broker that moves is followed.

#ifndef HAC_PLATFORM_HOST_ADP_TELEMETRY_MQTT_H
#define HAC_PLATFORM_HOST_ADP_TELEMETRY_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_log.h"
#include "port_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADP_TELEMETRY_MQTT_DEFAULT_PORT 1883u

// A host name's length (a DNS name's limit), and a user name's, without the
// terminator. CHOICES: longer is refused.
#define ADP_TELEMETRY_MQTT_HOST_MAX 253u
#define ADP_TELEMETRY_MQTT_USER_MAX 64u
#define ADP_TELEMETRY_MQTT_PASSWORD_MAX 128u

// CHOICES. Messages accepted and not yet acknowledged before publish() says
// HAL_ERR_NO_MEM: a session's start is 11 (online, nine configs, the state),
// all in flight together on a fast broker, so twice that and more. Received
// messages kept until polled, and subscriptions at once: past the port's
// minimums (PORT_TELEMETRY_MIN_INBOX, _MIN_SUBSCRIPTIONS).
#define ADP_TELEMETRY_MQTT_OUTBOX 32u
#define ADP_TELEMETRY_MQTT_INBOX 16u
#define ADP_TELEMETRY_MQTT_MAX_SUBSCRIPTIONS 8u

// Clients at once: the session, and earlier ones whose threads are still
// ending -- a DISCONNECT on its way, or an attempt still waiting for a TCP
// handshake the kernel gives up on only after about two minutes. CHOICE: a
// reporter gives up an attempt after 10 s and backs off to 2 min between
// them, so a few at most; past this many, connect() says HAL_ERR_IO at once.
#define ADP_TELEMETRY_MQTT_CLIENTS 8u

// libmosquitto refuses a keepalive of 1 to 4 s; a session asking for one gets
// this. 0 stays 0: never dropped for silence.
#define ADP_TELEMETRY_MQTT_MIN_KEEPALIVE_S 5u

// CHOICE: how long a new session's thread waits for the DISCONNECT of the one
// disconnect() ended before it connects anyway (above). A DISCONNECT on a
// working link is out in microseconds; one that is not out after this will
// not go.
#define ADP_TELEMETRY_MQTT_CLOSE_WAIT_MS 1000u

// Where the broker is: --broker's "mqtt://<host>[:<port>]".
typedef struct {
    char host[ADP_TELEMETRY_MQTT_HOST_MAX + 1u];
    uint16_t port;
} adp_telemetry_mqtt_url_t;

// Parses `url`: "mqtt://" -- TLS ("mqtts://") is not supported -- then a host
// name, an IPv4 address or an IPv6 one in brackets, then an optional
// ":<port>", 1..65535 (default ADP_TELEMETRY_MQTT_DEFAULT_PORT), and
// nothing more. A user or a password in it ("mqtt://user:pw@host") is
// refused: a password never goes on a command line. HAL_ERR_INVALID_ARG,
// with a sentence in `err`, for anything else.
hal_status_t adp_telemetry_mqtt_parse_url(const char *url, adp_telemetry_mqtt_url_t *out, char *err,
                                          size_t err_cap);

typedef struct {
    const char *host;  // the broker: a name or an address; copied
    uint16_t port;
    // The user name to log in with, or NULL; the password, or NULL -- sent
    // only with a user name (MQTT 3.1.1, 3.1.2.9). Both copied. The password
    // is never logged.
    const char *username;
    const char *password;
    // Why sessions fail -- no route, a refusal and its reason, a lost
    // connection -- each new reason once. May be NULL.
    const port_log_t *log;
} adp_telemetry_mqtt_config_t;

typedef struct adp_telemetry_mqtt_s adp_telemetry_mqtt_t;

// Something arrived, or a session began or ended. Called on a network
// thread.
typedef void (*adp_telemetry_mqtt_waker_fn)(void *ctx);

// Since adp_telemetry_mqtt_create().
typedef struct {
    uint32_t attempts;         // connect() calls that started one
    uint32_t sessions;         // the broker accepted
    uint32_t refused;          // the broker answered with a refusal
    uint32_t no_connection;    // no TCP connection: the name, the route, nobody listening
    uint32_t lost;             // sessions that ended by the network or the broker
    uint32_t published;        // accepted by publish()
    uint32_t acknowledged;     // of those, acknowledged by the broker (QoS 0: sent)
    uint32_t received;         // messages for a subscription
    uint32_t inbox_dropped;    // arrived with the inbox full, or with too long a topic
    uint32_t severed;          // sessions silenced by the network going
    uint32_t clients_waiting;  // clients whose threads are still ending, now
} adp_telemetry_mqtt_stats_t;

// NULL, with a sentence in `err`, when `cfg` is unusable or libmosquitto
// cannot start. Nothing connects until connect().
adp_telemetry_mqtt_t *adp_telemetry_mqtt_create(const adp_telemetry_mqtt_config_t *cfg, char *err,
                                                size_t err_cap);

// Ends every session without a DISCONNECT -- the broker publishes their wills
// -- and frees everything. NULL is fine.
void adp_telemetry_mqtt_destroy(adp_telemetry_mqtt_t *m);

port_telemetry_t *adp_telemetry_mqtt_port(adp_telemetry_mqtt_t *m);

// Set before the first connect().
void adp_telemetry_mqtt_set_waker(adp_telemetry_mqtt_t *m, adp_telemetry_mqtt_waker_fn fn,
                                  void *ctx);

// The network under the client goes (`present` false) or comes back: a
// session goes silent then (above), and connect() says HAL_ERR_NOT_READY
// while it is gone. The signature of adp_net_sim's listener as well
// (adp_telemetry_mqtt_net_listener(), the client as its context).
void adp_telemetry_mqtt_set_link(adp_telemetry_mqtt_t *m, bool present);
void adp_telemetry_mqtt_net_listener(void *client, bool present, uint64_t at_ms);

adp_telemetry_mqtt_stats_t adp_telemetry_mqtt_stats(adp_telemetry_mqtt_t *m);

// "2.0.22": the libmosquitto the program runs with.
const char *adp_telemetry_mqtt_library_version(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_TELEMETRY_MQTT_H
