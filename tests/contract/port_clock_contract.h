// SPDX-License-Identifier: Apache-2.0
//
// Contract that every port_clock_t implementation must satisfy (see
// CLAUDE.md: "every new port needs a suite in tests/contract/ that runs
// against all of its implementations, on the host and on the device").
//
// This file is the shared body: cmake/sources_contract.cmake compiles it,
// unmodified, into both the host CTest binary
// (tests/contract/test_port_clock_contract.c) and the ESP32 firmware
// (apps/firmware_esp32/components/hac_contract/), which runs it once at boot
// against adp_clock and reports the result through port_log. Two different
// runners, one contract -- a suite that existed twice would prove nothing,
// since the two copies could drift apart unnoticed.
//
// It therefore avoids everything a runner might not have: no stdio, no
// malloc, no test harness. Results travel out through contract_report_fn.

#ifndef HAC_TESTS_CONTRACT_PORT_CLOCK_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_CLOCK_CONTRACT_H

#include "contract_report.h"
#include "port_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

// Exercises `clock` and reports each check through `report`. Returns the
// number of checks that failed (0 means the clock satisfies the contract).
//
// Delays used internally are small (tens of milliseconds) so that running
// this at device boot costs nothing worth noticing.
unsigned port_clock_contract_run(const port_clock_t *clock, contract_report_fn report,
                                 void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_CLOCK_CONTRACT_H
