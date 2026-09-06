// SPDX-License-Identifier: Apache-2.0
//
// Contract that every port_log_t implementation must satisfy. See
// tests/contract/port_clock_contract.h for why this is a shared body rather
// than two hand-written suites: cmake/sources_contract.cmake compiles this
// file unmodified into both the host CTest binary and the ESP32 firmware.
//
// A logging port is unusual to contract-test because its job is to have a
// side effect (something appears somewhere), not to return a value the
// caller can assert on. What this suite actually verifies is narrower and
// still worth having: that no legal input -- an edge case a real call site
// hits, such as a NULL tag or a full field array -- crashes the adapter or
// corrupts anything past the call. It cannot verify that the *content*
// written is correct; that stays a matter of reading the log by eye.

#ifndef HAC_TESTS_CONTRACT_PORT_LOG_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_LOG_CONTRACT_H

#include "contract_report.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Exercises `log` and reports each check through `report`. Returns the
// number of checks that failed (0 means the adapter satisfies the contract).
// Every check is "the call returned without crashing"; a report callback
// that is itself called for every check is what lets the runner additionally
// confirm the call happened at all.
unsigned port_log_contract_run(const port_log_t *log, contract_report_fn report, void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_LOG_CONTRACT_H
