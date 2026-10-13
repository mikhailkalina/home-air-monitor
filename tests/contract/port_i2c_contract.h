// SPDX-License-Identifier: Apache-2.0
//
// Contract that every port_i2c_t implementation must satisfy -- the rules
// written down in ports/port_i2c.h, checked rather than trusted.
//
// The shared body: cmake/sources_contract.cmake compiles this file, unmodified,
// into the host runner (tests/contract/test_port_i2c_contract.c, against the
// register-level fake) and into the on-target test app
// (tests/contract/esp32/test_port_i2c_contract.c, against adp_i2c and the
// board's RTC). No stdio, no malloc, no harness: results travel out through
// contract_report_fn, as in port_clock_contract.h.
//
// Unlike port_clock and port_log, this suite needs the world around the port
// to cooperate: a device that answers, an address nothing answers, and a way
// to break the bus on purpose. A port cannot provide any of those through its
// own vtable, so the runner supplies them in port_i2c_contract_env_t. The
// fault hooks are how "a timeout" and "recovery after a forced bus hang"
// become checks that run, rather than claims.
//
// NOT run at device boot (unlike port_clock and port_log, docs/adr/0005): it
// deliberately hangs and stalls the board's shared bus. It runs on demand in
// the test app -- see docs/adr/0011-i2c-bus-adapter.md.

#ifndef HAC_TESTS_CONTRACT_PORT_I2C_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_I2C_CONTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include "contract_report.h"
#include "port_clock.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Measures how long each call took, for the "returns within its
    // deadline" checks. On the device, the real clock; on the host, the same
    // virtual clock the fake bus advances when it models a timeout.
    const port_clock_t *clock;

    // A device that acknowledges its address and whose register pointer may
    // be set and read back. The suite never writes it more than ONE byte at a
    // time -- the register pointer -- because on the board this is the RTC,
    // and a second byte would overwrite a register.
    uint8_t present_addr;
    uint8_t present_reg;

    // An address nothing on the bus answers.
    uint8_t absent_addr;

    // Fault injection. Each returns true once the fault is in place, false if
    // it could not be set up (reported as a failed check, never skipped).
    //
    // hang_bus:     leave a slave holding SDA low mid-transfer, the state a
    //               reset in the middle of a read leaves behind. Persists
    //               until recover().
    // stall_bus:    hold SCL low, as a slave stretching the clock forever
    //               would. Persists until end_stall().
    // sda_held_low: read the SDA line, so the suite can confirm a hang is
    //               real before relying on it, and gone after recover().
    bool (*hang_bus)(void *ctx);
    bool (*stall_bus)(void *ctx);
    void (*end_stall)(void *ctx);
    bool (*sda_held_low)(void *ctx);
    void *ctx;
} port_i2c_contract_env_t;

// Exercises `bus` and reports each check through `report`. Returns the number
// of checks that failed (0 means the bus satisfies the contract). Leaves the
// bus healthy if the implementation is correct: every fault it injects is
// ended or recovered before it returns.
unsigned port_i2c_contract_run(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                               contract_report_fn report, void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_I2C_CONTRACT_H
