// SPDX-License-Identifier: Apache-2.0
//
// Append-only files on a medium that may not be there.
//
// Two implementations: FAT on the board's microSD card
// (platform/esp32_t5s3/adp_sd.c) and a directory on a PC
// (platform/host/adp_files_dir.c). What uses it: the sensor recordings
// (core/recording/recorder.h) and, on the board, the debug log
// (docs/adr/0024-sd-card-recordings-and-debug-log.md).
//
// Deliberately narrow. A file is opened to be appended to, appended to,
// flushed and closed; a directory can be listed with each file's size, and a
// file removed -- the two operations the recorder needs to keep a directory
// within a bound and to never reuse a name. There is no reading, no seeking
// and no rewriting: nothing in the core reads a file back yet (the on-screen
// history that will want to is not designed), and a contract that read back
// through the port itself could not tell an implementation that keeps the
// bytes from one that only remembers them. The contract
// (tests/contract/port_files_contract.h) reads back through its runner, on
// the platform's own file API, instead. Key-value settings and the state
// that survives deep sleep are port_storage's (docs/architecture.md 5.6),
// phases 5 and 6, not this port's.
//
// Paths are relative to the medium's root: one name ("notes.txt") or one
// directory and a name ("data/2026-10-16_scd4x.csv"), every component one to
// PORT_FILES_COMPONENT_MAX characters of [A-Za-z0-9_.-] not starting with a
// '.', the whole shorter than PORT_FILES_PATH_MAX. Anything else is
// HAL_ERR_INVALID_ARG, on every implementation alike (port_files_path_ok()).
// FAT does not tell case apart and a PC's directory does: names that differ
// only in case are one file on the card and two on the host, so a caller
// uses one case throughout -- this project's names are lower case.
//
// What every call may answer:
//
//   HAL_OK               done
//   HAL_ERR_INVALID_ARG  a path outside the rule above, a NULL or unknown
//                        file, data == NULL with len > 0
//   HAL_ERR_NOT_READY    no medium: none inserted, not mounted, or gone since
//                        the file was opened. A normal state, not a fault.
//   HAL_ERR_NO_MEM       the medium is full
//   HAL_ERR_BUSY         the path is already open, or every handle is in use
//                        (each implementation holds at least
//                        PORT_FILES_MIN_OPEN files open at once)
//   HAL_ERR_NOT_FOUND    remove() of a file that does not exist
//   HAL_ERR_IO           anything else the medium refused
//
// The calls block until the medium has answered -- tens of milliseconds on a
// card, longer for a flush -- so on the board only the task that owns the
// card makes them, never the event loop.

#ifndef HAC_PORTS_PORT_FILES_H
#define HAC_PORTS_PORT_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// A path's length, terminator included, and one component's length.
#define PORT_FILES_PATH_MAX 64u
#define PORT_FILES_COMPONENT_MAX 40u

// Files every implementation can hold open at once: one recording per sensor
// source, the debug log, and one to spare.
#define PORT_FILES_MIN_OPEN 4u

// An open file: the implementation's own, opaque to everyone else.
typedef struct port_file_s port_file_t;

// One regular file in a listed directory: its name (without the directory)
// and its size on the medium -- what the last flush() put there, for a file
// open now. `name` is borrowed for the duration of the call.
typedef void (*port_files_visit_fn)(void *ctx, const char *name, uint64_t size_bytes);

typedef struct port_files_s port_files_t;
struct port_files_s {
    // Opens `path` for appending, creating it -- and its directory -- when
    // missing; an existing file keeps what it holds and grows at its end.
    hal_status_t (*open_append)(port_files_t *self, const char *path, port_file_t **out);

    // Adds `len` bytes at the end. They may wait in a buffer until flush() or
    // close(); a full medium may only say so then.
    hal_status_t (*append)(port_files_t *self, port_file_t *file, const void *data, size_t len);

    // Puts everything appended so far on the medium, and the file's new size
    // in its directory, so a power cut afterwards loses none of it (fsync).
    hal_status_t (*flush)(port_files_t *self, port_file_t *file);

    // Flushes and releases the handle -- always releases it, even when the
    // flush fails or the medium has gone, and then returns the flush's
    // answer (HAL_ERR_NOT_READY for a medium that has gone).
    hal_status_t (*close)(port_files_t *self, port_file_t *file);

    // Calls `visit` once per regular file directly in the directory `dir` (a
    // single component), in no particular order. A directory that does not
    // exist lists as empty and is HAL_OK.
    hal_status_t (*list)(port_files_t *self, const char *dir, port_files_visit_fn visit, void *ctx);

    // Removes the file `path`; not one that is open (HAL_ERR_BUSY).
    hal_status_t (*remove)(port_files_t *self, const char *path);

    void *impl;
};

// --- the path rule, shared by every implementation --------------------------

static inline bool port_files_component_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
}

// True when `path` is one component, or two separated by a single '/', each
// as the header above says; with `allow_dir` false, only one component.
static inline bool port_files_path_ok_depth(const char *path, bool allow_dir)
{
    if (path == NULL) {
        return false;
    }
    size_t total = 0u;
    size_t component = 0u;
    unsigned slashes = 0u;
    for (const char *p = path; *p != '\0'; ++p, ++total) {
        if (total + 1u >= PORT_FILES_PATH_MAX) {
            return false;
        }
        if (*p == '/') {
            if (component == 0u || ++slashes > (allow_dir ? 1u : 0u)) {
                return false;
            }
            component = 0u;
            continue;
        }
        if (!port_files_component_char_ok(*p) || (component == 0u && *p == '.') ||
            ++component > PORT_FILES_COMPONENT_MAX) {
            return false;
        }
    }
    return component > 0u;
}

// A file's path: "name" or "dir/name".
static inline bool port_files_path_ok(const char *path)
{
    return port_files_path_ok_depth(path, true);
}

// A directory's path, for list(): a single component.
static inline bool port_files_dir_ok(const char *dir)
{
    return port_files_path_ok_depth(dir, false);
}

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_FILES_H
