// SPDX-License-Identifier: Apache-2.0
//
// Contract that every port_telemetry_t implementation must satisfy
// (ports/port_telemetry.h), with a broker behind it: nothing works and
// nothing waits without a session; connect() starts one that the broker
// accepts within PORT_TELEMETRY_CONTRACT_CONNECT_MS; arguments are checked
// the same everywhere; what is published reaches a subscription -- the
// client's own, so that no second client is needed -- as published, in
// order, the payload copied before publish() returned; '+' matches one
// level; a retained message reaches a new subscription marked retained, and
// an empty one clears it; disconnect() ends the session at once and leaves
// no will; a session dropped with its network leaves its will; a full queue
// is HAL_ERR_NO_MEM, and what it accepted is delivered once the broker takes
// messages again (docs/adr/0030-telemetry-ports-and-the-reporter.md).
//
// Every topic it uses is under the runner's `topic_root`, and it clears what
// it retains there before it ends, so it can run against a real broker
// again and again.
//
// The shared body: cmake/sources_contract.cmake compiles this file,
// unmodified, into the host runner (tests/contract/test_port_telemetry_contract.c,
// against the loopback client of the broker model and libmosquitto) and into
// the ESP-IDF hac_contract component, whose runner
// (tests/contract/esp32/test_port_telemetry_contract.c) holds the board's
// MQTT client to it. No stdio, no malloc, no harness.
//
// A runner that cannot take the network away from under the session, or
// make the broker stop taking messages, leaves cut/restore or stall NULL,
// and the checks that need them are reported as not exercised.

#ifndef HAC_TESTS_CONTRACT_PORT_TELEMETRY_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_TELEMETRY_CONTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include "contract_report.h"
#include "port_clock.h"
#include "port_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

// How long one call may take on the runner's clock: it copies into a queue,
// or out of one.
#define PORT_TELEMETRY_CONTRACT_MAX_CALL_MS 10u

// How long a broker on the same network may take to accept a session, and to
// hand back a message.
#define PORT_TELEMETRY_CONTRACT_CONNECT_MS 10000u
#define PORT_TELEMETRY_CONTRACT_DELIVERY_MS 2000u

// How long a dropped session may still be reported connected.
#define PORT_TELEMETRY_CONTRACT_NOTICE_MS 10000u

// The keepalive the contract's sessions ask for: its will, after a drop,
// falls due one and a half of these later.
#define PORT_TELEMETRY_CONTRACT_KEEPALIVE_S 10u

// The most messages the full-queue check offers before it expects the queue
// to say no.
#define PORT_TELEMETRY_CONTRACT_MAX_QUEUE 256u

// The longest topic root a runner may give.
#define PORT_TELEMETRY_CONTRACT_ROOT_MAX 48u

typedef struct {
    // The clock the runner and the implementation share.
    const port_clock_t *clock;

    // Lets `ms` pass, and lets the implementation and its broker do whatever
    // they do meanwhile.
    void (*advance)(void *ctx, uint64_t ms);

    // The session's client id, and a subtree of topics the runner owns:
    // 1..PORT_TELEMETRY_CONTRACT_ROOT_MAX characters, no wildcard, no
    // trailing '/'.
    const char *client_id;
    const char *topic_root;

    // The network under the session goes, and comes back. NULL where the
    // runner cannot.
    void (*cut)(void *ctx);
    void (*restore)(void *ctx);

    // The broker stops taking messages (`on`), and takes them again. NULL
    // where the runner cannot.
    void (*stall)(void *ctx, bool on);

    void *ctx;
} port_telemetry_contract_env_t;

// Exercises `t`, which must not be connected, and reports each check through
// `report`. Leaves it disconnected. Returns the number of checks that failed
// (0 means the implementation satisfies the contract).
unsigned port_telemetry_contract_run(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                                     contract_report_fn report, void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_TELEMETRY_CONTRACT_H
