// SPDX-License-Identifier: Apache-2.0
//
// Contract that every port_files_t implementation must satisfy
// (ports/port_files.h): the path rule, append and flush reaching the medium,
// reopening appending at the end, PORT_FILES_MIN_OPEN files at once, list
// and remove, and a medium that goes away or fills up being a status, not a
// crash.
//
// The shared body: cmake/sources_contract.cmake compiles this file,
// unmodified, into the host runner (tests/contract/test_port_files_contract.c,
// against adp_files_dir) and into the ESP-IDF hac_contract component, whose
// on-target runner (tests/contract/esp32/test_port_files_contract.c) holds
// adp_sd to it on the board's card. No stdio, no malloc, no harness.
//
// Reading back is the RUNNER's, not the port's (docs/adr/0024): the port has
// no read, and a suite that read through the implementation under test could
// not tell bytes on the medium from bytes it merely remembers. The runner
// reads with the platform's own file API -- stdio on the host, stdio through
// ESP-IDF's VFS on the board.
//
// Everything it writes goes under "pfct/" and two names beside it, and is
// removed again at the end (the directories stay: the port removes files
// only).

#ifndef HAC_TESTS_CONTRACT_PORT_FILES_CONTRACT_H
#define HAC_TESTS_CONTRACT_PORT_FILES_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "contract_report.h"
#include "hal_status.h"
#include "port_files.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Reads the whole file `path` (relative to the medium's root), at most
    // `cap` bytes, into `buf` and its length into *len. HAL_ERR_NOT_FOUND
    // when there is no such file. Required.
    hal_status_t (*read_back)(void *ctx, const char *path, uint8_t *buf, size_t cap, size_t *len);

    // Takes the medium away (false) and puts it back (true): a card pulled
    // and reinserted, an unmount and a mount. NULL where the runner cannot;
    // the checks it would allow are then reported as not exercised.
    void (*set_present)(void *ctx, bool present);

    // Makes the medium full (true), and not (false). NULL where the runner
    // cannot -- filling a real card takes hours and wears it.
    void (*set_full)(void *ctx, bool full);

    void *ctx;
} port_files_contract_env_t;

// Exercises `files` and reports each check through `report`. Returns the
// number of checks that failed (0 means the implementation satisfies the
// contract). The medium must be present when it starts.
unsigned port_files_contract_run(port_files_t *files, const port_files_contract_env_t *env,
                                 contract_report_fn report, void *report_ctx);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PORT_FILES_CONTRACT_H
