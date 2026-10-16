// SPDX-License-Identifier: Apache-2.0
//
// The recordings' and the debug log's numbers, together
// (docs/adr/0024-sd-card-recordings-and-debug-log.md). Each says whether it
// is a choice or an estimate, and what would replace an estimate.

#ifndef HAC_CORE_RECORDING_RECORD_LIMITS_H
#define HAC_CORE_RECORDING_RECORD_LIMITS_H

// The directories on the medium, and the files' extensions.
#define RECORD_DATA_DIR "data"
#define RECORD_LOG_DIR "log"
#define RECORD_DATA_EXT ".csv"
#define RECORD_LOG_EXT ".log"

// ESTIMATE: how long rows wait in RAM before they are put on the medium
// with a flush (fsync) -- what a power cut can cost at most. Shorter costs
// more card writes (each flush rewrites a data sector, a FAT sector and a
// directory sector), longer loses more. Ten seconds is two SCD41 rows and
// about three of BSEC's; nothing has measured the wear or the time a flush
// takes on this board yet: the first board run (phase 3f) gives the time,
// phase 5's power work the energy, and either may move it.
#define RECORD_FLUSH_INTERVAL_MS 10000u

// The same for the debug log, which has the same trade.
#define RECORD_LOG_FLUSH_INTERVAL_MS 10000u

// ESTIMATE: the most the recordings may hold on the medium before the oldest
// day is removed. A day is about 1 MB of SCD41 rows at 5 s and 2.5 MB of
// BSEC's at 3 s (~60 and ~85 bytes a row, computed, not yet measured on a
// card), so 1 GiB is about 300 days. Replace from the board run's bytes per
// day (phase 3f, `rec`).
#define RECORD_DATA_DIR_CAP_BYTES (1024ull * 1024ull * 1024ull)

// ESTIMATE: the same for the debug log. Its volume is not known at all: the
// first unattended run of phase 3f measures it (`rec` prints the log's
// bytes since boot). Together with the recordings, 1.5 GiB: room on a 2 GB
// card, a small share of a common 16 or 32 GB one.
#define RECORD_LOG_DIR_CAP_BYTES (512ull * 1024ull * 1024ull)

// CHOICE: how long a recorder that met a full or failing medium waits before
// it opens a file again. Opening creates a file, so retrying on every row
// would litter the card with empty ones; a minute bounds that to a few an
// hour while the condition lasts, and a medium that recovers is used again
// within a minute.
#define RECORD_RETRY_MS 60000u

#endif  // HAC_CORE_RECORDING_RECORD_LIMITS_H
