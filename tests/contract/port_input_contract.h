// SPDX-License-Identifier: Apache-2.0
//
// Contract that every port_input_t implementation must satisfy
// (ports/port_input.h): poll() never waits, an untouched panel yields
// nothing, and a finger yields a DOWN where it landed, MOVEs as it travels
// and an UP with the same id, in panel coordinates, in order, stamped no
// later than they are delivered -- the promises screen_controller.h and
// tap.h build on (docs/adr/0025-touch-input-gt911.md).
//
// The shared body: cmake/sources_contract.cmake compiles this file,
// unmodified, into the host runner (tests/contract/test_port_input_contract.c,
// against gt911_input over the GT911 model, the simulator's scripted input,
// and adp_input_sdl where SDL is built) and into the ESP-IDF hac_contract
// component, whose on-target runner
// (tests/contract/esp32/test_port_input_contract.c) holds adp_input -- the
// real GT911 behind its interrupt -- to it. No stdio, no malloc, no harness.
//
// A runner that cannot touch the panel -- the board, with nobody's finger
// on it -- leaves press/drag/lift NULL, and the checks that need a finger
// are reported as not exercised. The taps are the board check
// (docs/hardware/board_notes.md, "The touch panel on the board").

#ifndef HAC_TESTS_CONTRACT_PORT_INPUT_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_INPUT_CONTRACT_H

#include <stdint.h>

#include "contract_report.h"
#include "port_clock.h"
#include "port_input.h"

#ifdef __cplusplus
extern "C" {
#endif

// How long one poll() may take on the clock the events are stamped with. A
// poll copies an event out of a queue; this is generous for a scheduler tick.
#define PORT_INPUT_CONTRACT_MAX_POLL_MS 10u

// How far an event may land from where the finger was put: a coordinate
// mapped from another resolution rounds.
#define PORT_INPUT_CONTRACT_POSITION_TOLERANCE_PX 2u

// An event's id is a touch point's index: no implementation follows more
// fingers than this.
#define PORT_INPUT_CONTRACT_MAX_ID 8u

typedef struct {
    uint16_t panel_width;
    uint16_t panel_height;

    // The clock the implementation stamps its events with.
    const port_clock_t *clock;

    // Lets `ms` pass, and lets the implementation do whatever it does
    // meanwhile -- read its controller, drain its window.
    void (*advance)(void *ctx, uint64_t ms);

    // A finger put down at (x, y) in panel coordinates, moved there, lifted.
    // NULL where the runner cannot.
    void (*press)(void *ctx, uint16_t x, uint16_t y);
    void (*drag)(void *ctx, uint16_t x, uint16_t y);
    void (*lift)(void *ctx);

    void *ctx;
} port_input_contract_env_t;

// Exercises `in` and reports each check through `report`. Returns the number
// of checks that failed (0 means the implementation satisfies the contract).
unsigned port_input_contract_run(port_input_t *in, const port_input_contract_env_t *env,
                                 contract_report_fn report, void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_INPUT_CONTRACT_H
