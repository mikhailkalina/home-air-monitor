// SPDX-License-Identifier: Apache-2.0
//
// port_files over a directory on a PC: the simulator's --record, and the
// host side of the port's contract (tests/contract/port_files_contract.h).
// The board's implementation is FAT on the microSD card
// (platform/esp32_t5s3/adp_sd.c).
//
// POSIX open/write/fsync/close, the same calls adp_sd makes through
// ESP-IDF's VFS, so the two differ in the medium and not in the code path.
// append() writes straight through to the operating system; flush() is
// fsync().
//
// Two test hooks give the contract what a directory never does by itself:
// a medium that goes away and comes back (adp_files_dir_set_present()), and
// one that fills up (adp_files_dir_set_limit()).

#ifndef HAC_PLATFORM_HOST_ADP_FILES_DIR_H
#define HAC_PLATFORM_HOST_ADP_FILES_DIR_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_files.h"

#ifdef __cplusplus
extern "C" {
#endif

// More than PORT_FILES_MIN_OPEN: the simulator records two sources.
#define ADP_FILES_DIR_MAX_OPEN 8u
#define ADP_FILES_DIR_ROOT_MAX 256u

struct port_file_s {
    bool used;
    int fd;  // -1 once the medium went away under it
    char path[PORT_FILES_PATH_MAX];
};

typedef struct {
    port_files_t port;
    char root[ADP_FILES_DIR_ROOT_MAX];
    bool present;
    uint64_t limit_bytes;  // 0: no limit but the disk's
    uint64_t appended_bytes;
    struct port_file_s files[ADP_FILES_DIR_MAX_OPEN];
} adp_files_dir_t;

// The directory `root` is the medium's root, created if it is missing (its
// parent is not). HAL_ERR_INVALID_ARG for a root too long, HAL_ERR_IO when
// it cannot be created.
hal_status_t adp_files_dir_init(adp_files_dir_t *d, const char *root);

port_files_t *adp_files_dir_port(adp_files_dir_t *d);

// Closes whatever is still open.
void adp_files_dir_deinit(adp_files_dir_t *d);

// Test hook: the medium taken out (false) or put back (true). While it is
// out every call is HAL_ERR_NOT_READY, and a file open when it went stays
// so: only close() releases it.
void adp_files_dir_set_present(adp_files_dir_t *d, bool present);

// Test hook: the medium is full once `limit_bytes` more have been appended
// through this adapter (0 lifts the limit). An append that would pass it
// writes nothing and is HAL_ERR_NO_MEM.
void adp_files_dir_set_limit(adp_files_dir_t *d, uint64_t limit_bytes);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_FILES_DIR_H
