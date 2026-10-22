// SPDX-License-Identifier: Apache-2.0
//
// port_telemetry on the board: esp-mqtt, ESP-IDF's MQTT client (phase 4g,
// docs/adr/0033-mqtt-on-the-board.md). Plain MQTT 3.1.1 over TCP to a broker
// on the local network; no TLS. Everything that decides anything -- the
// broker's URI, the queues, a message in pieces, which session an event
// belongs to, what to do next -- is adp_mqtt_pure.c, which the host tests.
// This adds what is the board's:
//
//   * a task of its own, the only caller of esp-mqtt. esp-mqtt's own task
//     holds its client's lock through every network wait -- the name lookup,
//     the TCP handshake and the CONNACK, each up to ADP_MQTT_NETWORK_TIMEOUT_MS,
//     and a write to a link that has gone -- and every call into the client,
//     esp_mqtt_client_enqueue() included, takes that lock first. So the event
//     loop never calls it: publish() and subscribe() copy into a ring in PSRAM
//     and wake this task, which hands each message to esp-mqtt with
//     esp_mqtt_client_enqueue(), store set -- never esp_mqtt_client_publish(),
//     which writes to the socket from its caller -- and each subscription with
//     esp_mqtt_client_subscribe_single(). connect() and disconnect() only
//     record what is asked; this task makes and starts a client, and stops
//     and destroys it, which waits for esp-mqtt's task to end. Whatever the
//     network makes esp-mqtt wait, this task waits, and the event loop does
//     not;
//   * one esp-mqtt client per session, as the simulator's libmosquitto
//     adapter has: its reconnection off, its outbox bounded
//     (ADP_MQTT_OUTBOX_LIMIT_BYTES), destroyed when the session ends -- so
//     nothing of one session is resent in the next (ports/port_telemetry.h);
//   * esp-mqtt's events, on esp-mqtt's task: the broker's answer and the
//     session's end into the session state, a received message into the
//     inbox, and the event loop woken through the waker the composition gives
//     -- event_loop_post_network() on the board;
//   * the network: with a port_net given, connect() needs it up, and a
//     session whose network has gone is over at the next call -- the port's
//     "a drop the adapter has noticed" -- without waiting for esp-mqtt's
//     keepalive to find it;
//   * what the console's `mqtt` shows, under the adapter's spinlock, which
//     guards the queues' indices and a few counters, never a copy: the
//     event loop's longest call into the port is measured.
//
// How a session ends, and what the broker makes of it:
//
//   * disconnect(): esp_mqtt_client_destroy() sends a DISCONNECT -- the
//     broker drops the will -- then closes the socket.
//   * the network gone: the same, into a dead link. The broker publishes the
//     will once it has heard nothing for one and a half keepalives -- unless
//     the link is back before both that and lwIP's IP-lost timer have run
//     out, and lwIP's retransmission delivers the DISCONNECT late: then the
//     broker drops the will for a device that is back within seconds.
//   * the broker gone, or refusing: esp-mqtt's DISCONNECTED; the session is
//     over and the log says why, each reason once until a session works.
//   * a reset or a power cut: no DISCONNECT, so the will.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adp_mqtt_limits.h"
#include "adp_mqtt_pure.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_log.h"
#include "port_net.h"
#include "port_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called from esp-mqtt's task when the session began or ended, or a message
// arrived. Must not block.
typedef void (*adp_mqtt_waker_fn)(void *ctx);

typedef struct {
    // "mqtt://<host>[:<port>]" (adp_mqtt_parse_uri()); the strings here must
    // outlive the adapter -- the image's Kconfig strings do.
    const char *broker_uri;
    // The user to log in as, or NULL or "" for none; its password, or NULL
    // or "" for none, sent only with a user name (MQTT 3.1.1, 3.1.2.9) and
    // never logged -- the boot line gives its length.
    const char *username;
    const char *password;
    const port_net_t *net;  // may be NULL: the network is taken as up
    const port_clock_t *clock;
    const port_log_t *log;
    uint32_t task_stack_bytes;  // the adapter's task
    int task_priority;
    int task_core;
} adp_mqtt_config_t;

// Since adp_mqtt_init().
typedef struct {
    uint32_t attempts;       // connect() calls that began one
    uint32_t sessions;       // the broker accepted
    uint32_t refused;        // the broker refused the CONNECT
    uint32_t no_connection;  // no TCP connection, or no answer
    uint32_t lost;           // sessions that worked and ended by themselves
    uint32_t network;        // sessions ended by the network going
    uint32_t not_started;    // esp-mqtt would not make or start a client
    uint32_t published;      // accepted by publish()
    uint32_t refused_calls;  // publish() and subscribe() said HAL_ERR_NO_MEM: the ring full
    uint32_t subscribed;     // accepted by subscribe()
    uint32_t forwarded;      // handed to esp-mqtt
    uint32_t outbox_full;    // esp-mqtt's outbox said no, and the message waited
    uint32_t acknowledged;   // the broker's PUBACKs
    uint32_t discarded;      // accepted, and dropped with their session
    uint32_t received;       // kept for poll()
    uint32_t inbox_dropped;  // arrived with the inbox full, too long a topic, or for no session
} adp_mqtt_stats_t;

// What the console's `mqtt` prints.
typedef struct {
    hal_status_t init_status;
    adp_mqtt_broker_t broker;
    const char *username;  // "" for none
    adp_mqtt_state_t state;
    adp_mqtt_end_t end;  // the session's, while over or since
    // connect() calls that began one, from 1; 0 before any. An attempt, not
    // a session: one that failed counts too -- stats.sessions are the ones the
    // broker accepted, the reporter's own session numbers.
    uint32_t attempt;
    int last_connack;  // the last refusal's return code; -1 for none
    int last_errno;    // the socket's errno at the last transport error; 0 for none
    int last_esp_err;  // esp-tls's at the same error (an esp_err_t); 0 for none
    adp_mqtt_stats_t stats;
    uint32_t queued_messages;  // accepted, not yet handed to esp-mqtt
    uint32_t queued_bytes;     // their topics and payloads
    uint32_t outbox_bytes;     // esp-mqtt's outbox, as the adapter's task last read it
    uint32_t inbox_waiting;    // received, not yet polled
    uint32_t post_max_us;      // the longest one port call kept its caller
    const char *post_max_call;
    uint32_t task_stack_free_min_bytes;  // the adapter's task; UINT32_MAX before it ran
    uint32_t mqtt_stack_free_min_bytes;  // esp-mqtt's, at its events; UINT32_MAX before any
} adp_mqtt_status_t;

// The fields are the adapter's; the struct is complete only so that the
// composition can keep one in static storage.
typedef struct {
    port_telemetry_t port;
    adp_mqtt_config_t cfg;
    adp_mqtt_broker_t broker;
    hal_status_t init_status;
    adp_mqtt_waker_fn waker;
    void *waker_ctx;
    void *task;  // TaskHandle_t
    void *mem;   // the PSRAM block: the rings' slots, the session's parameters

    // Under the adapter's spinlock.
    adp_mqtt_session_t session;
    adp_mqtt_ring_t out_ring;
    adp_mqtt_ring_t in_ring;
    uint32_t queued_bytes;
    uint32_t outbox_bytes;
    int last_connack;
    int last_errno;
    int last_esp_err;
    uint32_t refused_gen;  // the generation whose CONNECT the broker refused
    adp_mqtt_stats_t stats;
    uint32_t post_max_us;
    const char *post_max_call;
    uint32_t task_stack_free_min;
    uint32_t mqtt_stack_free_min;

    // esp-mqtt's task's alone: the generation of the client it runs, written
    // by the adapter's task before it starts that client.
    uint32_t handler_gen;

    // The adapter's task's alone.
    void *client;  // esp_mqtt_client_handle_t
    uint32_t client_gen;
    uint32_t started_gen;
    bool waiting_for_room;  // esp-mqtt's outbox refused the head of the ring
    uint32_t logged_gen;    // the last session whose end was logged
    long last_noted;        // the last reason logged, so that each is logged once
    bool memory_logged;
    uint32_t mqtt_stack_logged;
} adp_mqtt_t;

// Checks the configuration, takes its buffers and starts the adapter's task;
// nothing connects until connect(). Returns at once. Anything that fails is
// logged and leaves the port answering -- connect() says HAL_ERR_IO -- with
// its status here: HAL_ERR_INVALID_ARG for a URI or credentials it cannot
// use, HAL_ERR_NO_MEM without its PSRAM or its task. Call once per adapter.
hal_status_t adp_mqtt_init(adp_mqtt_t *m, const adp_mqtt_config_t *cfg);

port_telemetry_t *adp_mqtt_port(adp_mqtt_t *m);

// Who esp-mqtt's task wakes. Set it before the first connect().
void adp_mqtt_set_waker(adp_mqtt_t *m, adp_mqtt_waker_fn fn, void *ctx);

void adp_mqtt_status(adp_mqtt_t *m, adp_mqtt_status_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_H
