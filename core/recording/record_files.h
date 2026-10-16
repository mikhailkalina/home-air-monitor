// SPDX-License-Identifier: Apache-2.0
//
// Where the recordings and the debug log go on the medium, and how a
// directory is kept within its bound
// (docs/adr/0024-sd-card-recordings-and-debug-log.md):
//
//   data/2026-10-16_scd4x.csv      a sensor source's rows of a local day
//   data/2026-10-16_scd4x_2.csv    the same day's next file: a reboot, a
//                                  failed write, the clock set back
//   data/undated-0003_bme68x.csv   rows from before the wall clock was set,
//                                  from the third boot to need one
//   log/2026-10-16.log             the debug log of a local day
//   log/undated-0003.log
//
// A name's KEY is its day ("2026-10-16") or its undated boot
// ("undated-0003"); its TAG names the source; its INDEX tells files of one
// key and tag apart, 1 for the first (no suffix). Keys order the files by
// age: every undated key before every day -- an undated file cannot be put
// on the calendar, and on a board whose RTC keeps time it is the rare boot
// before time_set -- undated keys by their number, days by their date.
//
// Only names of this form count. Anything else on the medium -- a file a
// person put there, the contract suite's scratch directory -- is neither
// counted against a bound nor ever removed.

#ifndef HAC_CORE_RECORDING_RECORD_FILES_H
#define HAC_CORE_RECORDING_RECORD_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/civil_time.h"
#include "hal_status.h"
#include "port_files.h"

#ifdef __cplusplus
extern "C" {
#endif

// "2026-10-16", "undated-0003": room for either, terminator included.
#define RECORD_KEY_MAX 16u
// A source's tag in a file name: [a-z0-9] only, so it never holds the '_'
// that separates a name's parts.
#define RECORD_TAG_MAX 16u
// ".csv", ".log": room for an extension, dot and terminator included.
#define RECORD_EXT_MAX 8u

#define RECORD_UNDATED_PREFIX "undated-"
#define RECORD_UNDATED_DIGITS 4u

typedef struct {
    char key[RECORD_KEY_MAX];
    char tag[RECORD_TAG_MAX];  // "" for a name without one (the log)
    uint32_t index;            // 1 for a name without the "_<n>" suffix
    char ext[RECORD_EXT_MAX];  // ".csv"
} record_name_t;

// The key of a local day.
void record_key_for_day(char *dst, size_t cap, const civil_time_t *local_day);

// The key of the `seq`-th undated boot.
void record_key_undated(char *dst, size_t cap, uint32_t seq);

// True for a tag a name can carry: 1 to RECORD_TAG_MAX - 1 of [a-z0-9].
bool record_tag_ok(const char *tag);

// "<dir>/<key>[_<tag>][_<index>]<ext>", the index written only from 2 on.
// False, leaving `dst` empty, when the result would not fit or a part is
// not one a name may have.
bool record_name_build(char *dst, size_t cap, const char *dir, const char *key, const char *tag,
                       uint32_t index, const char *ext);

// The parts of a file name (without its directory). False for a name not of
// the form above.
bool record_name_parse(const char *name, record_name_t *out);

// < 0 when key `a` is older than `b`, 0 when equal, > 0 when newer.
int record_key_compare(const char *a, const char *b);

// The undated boot number of `key`, or 0 for a day's key.
uint32_t record_key_undated_seq(const char *key);

// --- over the medium ---------------------------------------------------------

// The next free index for files of `key`, `tag` and `ext` in `dir`: one
// more than the highest there, 1 when there is none.
hal_status_t record_next_index(port_files_t *files, const char *dir, const char *key,
                               const char *tag, const char *ext, uint32_t *out);

// One more than the highest undated boot number in `dir`, 1 when there is
// none. A boot that needs one asks once, of every directory it writes to,
// and takes the largest answer.
hal_status_t record_next_undated_seq(port_files_t *files, const char *dir, uint32_t *out);

typedef struct {
    uint64_t total_bytes;  // this module's files in the directory, after
    uint32_t files_removed;
    uint64_t bytes_removed;
    bool over_cap;  // still over: only protected keys are left
} record_retention_t;

// The most keys a caller can protect from removal at once: one per open file.
#define RECORD_PROTECTED_MAX 8u

// Keeps the files of this form in `dir` within `cap_bytes`: while they hold
// more, removes every file of the oldest key -- the oldest day first -- but
// never one of the `n_protected` keys given (the files open now). Lists the
// directory once per key removed, plus once.
hal_status_t record_enforce_cap(port_files_t *files, const char *dir, uint64_t cap_bytes,
                                const char *const *protected_keys, size_t n_protected,
                                record_retention_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_RECORDING_RECORD_FILES_H
