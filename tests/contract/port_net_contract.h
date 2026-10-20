// SPDX-License-Identifier: Apache-2.0
//
// Contract that every port_net_t implementation must satisfy
// (ports/port_net.h): nothing waits; down until asked; up() brings the link
// up within PORT_NET_CONTRACT_UP_MS and keeps it -- a lost link is sought
// again without being asked, and is never NET_DOWN while up() is in force;
// the signal strength is a signal only while up; down() is down at once and
// stays down; both calls can be repeated
// (docs/adr/0030-telemetry-ports-and-the-reporter.md).
//
// The shared body: cmake/sources_contract.cmake compiles this file,
// unmodified, into the host runner (tests/contract/test_port_net_contract.c,
// against the simulator's network) and into the ESP-IDF hac_contract
// component, for the board's Wi-Fi adapter when phase 4c brings one. No
// stdio, no malloc, no harness.
//
// A runner that cannot take the access point away -- the board, in a room
// whose access point stays put -- leaves cut/restore NULL, and the checks
// that need it are reported as not exercised.

#ifndef HAC_TESTS_CONTRACT_PORT_NET_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_NET_CONTRACT_H

#include <stdint.h>

#include "contract_report.h"
#include "port_clock.h"
#include "port_net.h"

#ifdef __cplusplus
extern "C" {
#endif

// How long one call may take on the runner's clock: it hands a request to a
// task, or reads a state.
#define PORT_NET_CONTRACT_MAX_CALL_MS 10u

// How long a link may take to come up after up(), or after the access point
// is back: an association and an address, generously.
#define PORT_NET_CONTRACT_UP_MS 30000u

// How long a lost link may still be reported up: a Wi-Fi station notices a
// lost access point within a few missed beacons.
#define PORT_NET_CONTRACT_NOTICE_MS 10000u

typedef struct {
    // The clock the runner and the implementation share.
    const port_clock_t *clock;

    // Lets `ms` pass, and lets the implementation do whatever it does
    // meanwhile.
    void (*advance)(void *ctx, uint64_t ms);

    // The access point goes away, and comes back. NULL where the runner
    // cannot.
    void (*cut)(void *ctx);
    void (*restore)(void *ctx);

    void *ctx;
} port_net_contract_env_t;

// Exercises `net`, which must be down and never asked up, and reports each
// check through `report`. Leaves it down. Returns the number of checks that
// failed (0 means the implementation satisfies the contract).
unsigned port_net_contract_run(port_net_t *net, const port_net_contract_env_t *env,
                               contract_report_fn report, void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_NET_CONTRACT_H
