// SPDX-License-Identifier: Apache-2.0
//
// Contract that every sensor_source_t implementation must satisfy
// (ports/port_sensor.h, docs/architecture.md 5.3): the life cycle the sensor
// manager relies on, whatever is behind the vtable -- a CSV replay, or a
// driver over port_i2c.
//
// The shared body: cmake/sources_contract.cmake compiles this file,
// unmodified, into the host runner (tests/contract/test_port_sensor_contract.c,
// against replay_source and against scd4x_source over the fake bus) and into
// the ESP-IDF hac_contract component, ready for an on-target runner once a
// sensor is wired to the board. No stdio, no malloc, no harness: results
// travel out through contract_report_fn, as in port_clock_contract.h.
//
// Time is the runner's: a source measures on its own clock, and only the
// runner knows how to move that clock forward -- a virtual clock jumps, a
// real one is waited for.

#ifndef HAC_TESTS_CONTRACT_PORT_SENSOR_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_SENSOR_CONTRACT_H

#include <stdint.h>

#include "contract_report.h"
#include "port_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Moves the source's clock forward by `ms`.
    void (*advance)(void *ctx, uint64_t ms);
    void *ctx;
} port_sensor_contract_env_t;

// How many times the suite waits ready_in_ms() and calls read() before it
// gives up on a source ever producing a reading -- generous, in case a source
// needs several polls (a warm-up, a sensor that answers "not yet").
#define PORT_SENSOR_CONTRACT_READ_ATTEMPTS 8u

// Exercises `src` from init() to deinit() and reports each check through
// `report`. Returns the number of checks that failed (0 means the source
// satisfies the contract). The source is deinitialised when it returns.
unsigned port_sensor_contract_run(sensor_source_t *src, const port_sensor_contract_env_t *env,
                                  contract_report_fn report, void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_SENSOR_CONTRACT_H
