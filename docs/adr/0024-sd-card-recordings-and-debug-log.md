# 24. Sensor recordings and the debug log on the microSD card, through `port_files`

Date: 2026-10-16

## Status

Accepted. Verified on the host: the `port_files` contract against
`adp_files_dir` and five broken wrappers it catches; unit tests of the row
format against the parser that reads it back, of the file names and the
bound, of the recorder, of the debug log's files and of the log's ring; the
parser's tests at every new boundary, every existing scenario playing as
before, and two minutes of libFuzzer on the parser and its playback; the
round trip through the sensor manager, value for value, and
through the simulator's command line, redraw for redraw. The firmware, its
BSEC variant and the test app build. **First board runs 2026-10-16**: the
first image reset itself six times at the moment `PWR_EN` rose, then mounted
a 16 GB card and recorded; a second one, with the card's task moved after the
rail, never got past that point at all. What both did there and phase 3e did
not: the log's hook writing PSRAM within microseconds of the rail switching
on. The hook is now held off PSRAM while the rail comes up ("The debug log"
below), and the third image booted at the first attempt and recorded: the
event loop's longest post into the card's queue 33 us, against the card's
slowest write of 103 ms and flush of 88 ms. The rest of what each board check is to
show is listed in `docs/hardware/board_notes.md`, open item "The microSD card
on the board (phase 3f)".

Numbered 24: 22 is the proposal on the branch `sensor-and-display-tasks`, 23
the wall clock.

## Context

The device now measures for hours on its own, but everything it learns goes
to a USB console that is there only while a PC listens. What phase 3 still
owes -- the gas sensor's burn-in, BSEC's accuracy over days, the SCD41's
warm-up, temperature against a reference (`board_notes.md`, "Phase 3c's
measurements", "BSEC on the board") -- wants long unattended runs, and the
simulator wants real episodes to replay. `docs/architecture.md` 7.1 planned
recordings over MQTT, in phase 4. The board has a microSD slot, its pins
recorded since phase 2a and unused.

## Decision

### The card now, not MQTT first

A recording on the card needs no Wi-Fi, no broker and no PC: it starts with
the boot, before any network could, and is whole whatever the network does.
It costs no radio -- whose heat is one of the things requirements 12 warns
the temperature reading against. Phase 4's MQTT publishes the present state
to Home Assistant, a different job; if it ever records too, it can write this
same format. Architecture 7.1 is updated to say the device's own recordings
come from the card.

### The slot

From the v2.4 schematic, recorded in `board_notes.md` ("The microSD slot"):
SPI mode only (DAT1 and DAT2 go nowhere), CS IO42, MOSI IO15, CLK IO11, MISO
IO16 -- `BOARD_SD_*`, unchanged; VDD from `VDD3V3`, so the card is powered
whenever the board is and not switched by `PWR_EN`; pull-ups on three lines,
none fitted on MISO (`R14` NC), so the adapter enables the S3's internal one;
card-detect goes only to a test pad, so the firmware cannot see a card arrive
or leave. IO15 and IO16 are also pins 2 and 3 of `P5`: from this phase they
belong to the card, and the sensors use only `P5`'s V and G.

### A port for append-only files

`ports/port_files.h`, because there are two implementations: FAT on the card
(`platform/esp32_t5s3/adp_sd.c`) and a directory on the host
(`platform/host/adp_files_dir.c`). Six calls: `open_append`, `append`,
`flush` (fsync: the bytes and the file's size in its directory), `close`
(always releases the handle), `list` (a directory's files with their sizes)
and `remove`. The last two are what the bound needs, and what never reusing a
name needs. Paths are one name or one directory and a name, a rule both
implementations take from the header (`port_files_path_ok()`).

**A missing medium is `HAL_ERR_NOT_READY`, a full one `HAL_ERR_NO_MEM`** --
codes `hal_status_t` already has, read that way only by this port, rather
than two new codes every `switch` on the status would have to learn.

**Reading is the runners' business, not the port's.** Nothing in the core
reads a file back yet; the on-screen history that will want to is not
designed, and a `read()` shaped now would be a guess at it. And a contract
that read back through the port under test could not tell an implementation
that put the bytes on the medium from one that only remembers them -- the
first broken wrapper below is exactly that. So the contract
(`tests/contract/port_files_contract.h`) reads back through its runner, with
the platform's own stdio: on the host the directory, on the board the card
through ESP-IDF's VFS. When the history comes, `read()` joins the port with
contract checks of its own, and the runner's reading stays the independent
witness.

**What it leaves to `port_storage`** (architecture 5.6): key-value settings
and the state that survives deep sleep -- small values written whole and
read at boot, in NVS and RTC memory, phases 5 and 6. A stream of rows on a
removable card is another thing, with another medium and other failures.

**The contract** runs on the host against `adp_files_dir` and in the test app
against `adp_sd` on a card in the slot (IGNORED, and saying so, without one).
The medium going away and coming back is a runner hook -- the host's adapter
has a switch for it, the board unmounts and mounts the card. A full medium is
a hook the host provides (a byte limit) and the board cannot -- filling a
card takes hours and wears it -- so on the board those checks report "not
exercised". Five broken wrappers -- bytes kept in memory until close, a
reopen that truncates, the path rule not enforced, a full medium reported as
an I/O error, a missing one too -- are each caught by the check meant for
them.

### The card on the board

`adp_sd.c`: ESP-IDF's SD-over-SPI driver on SPI2 at 20 MHz and its FATFS,
mounted at `/sdcard`, the files through ESP-IDF's VFS with the same POSIX
calls the host makes, a directory listed through FatFs itself (a name and a
size in one pass, where `readdir()` would want a `stat()` per file). FAT12,
16 and 32: ESP-IDF builds FatFs without exFAT (`FF_FS_EXFAT 0`), so a card
sold exFAT-formatted -- every SDXC, 64 GB and up -- has to be formatted FAT32
first. Long file names are on (`CONFIG_FATFS_LFN_HEAP`), and `adp_sd.c`
refuses to build without them, since an `sdkconfig` made before this phase
keeps `LFN_NONE` and every open would fail on the card instead. **It never
formats a card**: one the mount cannot read is reported and left alone.

**No card is a normal state.** The card's task (below) tries to mount at
start and then every `CARD_WRITER_MOUNT_RETRY_MS`, 60 s, and the console's
`sd_mount` tries at once. An automatic attempt keeps ESP-IDF's own SD lines
off the log (its tags at `NONE` for the attempt, then restored), so an empty
slot adds nothing to the log after the one line that says so; `sd_mount`
shows them, for whoever wants to know why. A card pulled out under a mounted
filesystem is found by the next call: FATFS asks the card's status first
(`disk_status_check_enable`), the adapter asks again on an I/O error, and a
card that does not answer is unmounted -- `HAL_ERR_NOT_READY` from then on,
and the retry timer looks for it again. **`sd_eject`**, not in the brief,
flushes and closes every file and unmounts, so a card can be taken to a PC
without pulling it from under a write; it holds off the automatic mount until
`sd_mount`.

### The format grows, and every new column is optional

The recordings are the scenario format of architecture 7.1, so the simulator
replays them as they are, with three columns added
(`platform/host/replay/replay_source.h`, `core/recording/record_format.h`):

- `utc`, the row's wall time (`YYYY-MM-DDTHH:MM:SSZ`), empty while the clock
  is not set (ADR 0023). Playback ignores it; the simulator starts its wall
  clock from the first row's.
- `<field>_status`, the field's own status where it differs from the row's
  `status` -- a BME68x row carries a warming gas reading beside a valid
  temperature. Interpolation then works field by field, between rows where
  that field is ok.
- `<field>_acc`, the source's accuracy verdict (ADR 0021): `unreliable`,
  `low`, `medium`, `high`, or empty. Written only for the fields the
  composition says carry one -- BSEC's IAQ -- so an SCD41's file has none.

And one status word, `unavailable`. **Optional, because every file written
before stays valid**: the bundled scenarios parse and play exactly as before
(their tests unchanged), a hand-written scenario needs none of the new
columns, and the parser stays the one reader of both. A `_status` or `_acc`
column without its field's column is refused.

Two tolerances for files that come off a card that lost power: a last line
with no line end that does not parse is dropped as a write cut short, and NUL
bytes at the very end are ignored. Anywhere else a malformed line still
rejects the file.

The writer's choices: values with a fixed number of decimals per field, at
or finer than the sensors resolve (`record_format.c`: temperature and
humidity 0.01, pressure 0.001 hPa, CO2 whole ppm, IAQ 0.01, gas whole ohms);
a row's `status` is its first field's, and a `<field>_status` cell is written
only where the field differs; a value only for a status that carries one;
`t_offset_s` to the millisecond from the file's first row. A comment line
first names the source.

### Files, days, and never reopening one

```
data/2026-10-16_scd4x.csv      a source's rows of a local day (Vilnius, ADR 0023)
data/2026-10-16_scd4x_2.csv    the next file of that day and source
data/undated-0003_bme68x.csv   rows from before the wall clock was set
log/2026-10-16.log             the debug log of a local day
log/undated-0003.log
```

A recording file is one continuous episode on one monotonic clock, its
`t_offset_s` counted from its first row. **It is never reopened once
closed**: a reboot starts that day's next file (`_2`, `_3`), as do a write
the card refused and the clock set back to a day already written. This
departs from "one file per source per day" on a day with reboots, for three
reasons: the monotonic clock restarts at boot, so a reopened file's offsets
would need its first row's time, which the port cannot read; a reboot is a
real discontinuity -- the SCD41 warms up again, BSEC starts over -- that a
replay should not interpolate across; and a file whose last write was cut
short keeps its torn line last, where the parser forgives it.

Before the wall clock is valid, rows go to an undated file numbered past
every undated file on the card, in either directory, so two boots never
collide; once the clock is set the recorder turns to the day's file. The log,
whose lines carry their own times, appends to the day's file across boots.

**The bound**: a directory's files of this form are kept within a cap --
`RECORD_DATA_DIR_CAP_BYTES`, 1 GiB, and `RECORD_LOG_DIR_CAP_BYTES`, 512 MiB,
both estimates (`core/recording/record_limits.h`) -- by removing every file of
the oldest key when a new file opens: undated boots first, as they cannot be
placed on the calendar, then the oldest day. Never a day with a file open,
never a file of another form. **A full card stops recording without a
fault**: rows are dropped and counted, the state logged once, a file nothing
reached removed again, and a file opened once more after `RECORD_RETRY_MS`.

### The recorder in the core, in two halves

`core/recording/recorder.h`. The **watch** runs on the event loop after each
poll and compares each sensor-manager slot's counters of reads and failures
with what it saw last; every change becomes an event with a copy of the
slot's sample, the time and the wall time. The **recorder** runs where the
medium may be waited on -- on the board the card's task, in the simulator the
same loop -- and formats a row, picks its file, buffers it, and appends and
flushes every `RECORD_FLUSH_INTERVAL_MS` (10 s, an estimate: what a power cut
costs at most against what each flush costs the card) and at every rotation.
The sources' tags and ids are the composition's: `scd4x` for source 1,
`bme68x` for source 2, on the board and in the simulator alike.

### The debug log: a tee that never touches the card

`esp_log_set_vprintf()` installs a hook that runs in whatever task logged
(`apps/firmware_esp32/main/log_tee.c`). It takes a slot in a ring of 1024
lines of 320 bytes in PSRAM (`log_ring.c`) under a spinlock held for a few
instructions, formats the line straight into the slot outside the lock --
no second formatting, no line buffer on the logging task's stack -- commits
it, and hands the console exactly what it printed before. The card's task
drains the ring into `log/<day>.log`, each line prefixed with its UTC time to
the millisecond, worked back from the clock at the moment it is drained so
that lines logged before the RTC set the clock get their real time; for that
it holds the log up to `CARD_WRITER_LOG_CLOCK_GRACE_MS` (15 s) after boot
while the clock is unset. **A full ring drops the new line, never an old
one,** and the file gets one line saying how many; while no card is mounted
the lines stay in the ring, so a card inserted after boot still gets the
boot.

**Held off PSRAM while the panel's rail comes up** (`log_tee_hold()`, from
before the display adapter's init until the bus is up). EPDiy logs a line in
the statement after the one that raises `PWR_EN`; the hook writing that line
into the PSRAM ring, in the rail's inrush, is the reading that fits the first
board runs -- interrupt-watchdog resets with the CPU stuck on a cache access,
at that point and nowhere else, in two images with nothing else new running
there (`board_notes.md`, open item "A reboot loop when the switched 3V3 comes
up"). While held, lines reach the console only, and the file says how many
it missed. The file's level is `CONFIG_HAC_LOG_FILE_LEVEL` (INFO by default); one
more verbose than the console's raises ESP-IDF's default level and keeps the
extra lines off the console.

Not in the file: the ROM's, the bootloader's and ESP-IDF's start-up lines
before `app_main()`, `ESP_EARLY_LOG`/`ESP_DRAM_LOG`, and the console's own
`printf` output (`I2C_SCAN`, `TIME`, `SD`, `REC` lines).

### One task owns the card

`apps/firmware_esp32/main/card_writer.c`, priority 3 on core 0, below the
event loop on core 1. It mounts, records, drains the log, keeps both within
their bounds and answers the console (`sd`, `sd_mount`, `sd_eject`, `rec`)
between its own work.

**It starts only once the switched 3V3 is up and settled**, after the I2C
bus and the RTC (`start_card()` in `main.c`), though the card runs from the
always-on `VDD3V3`. The first image started it at the top of `app_main()`,
and the card's initialisation -- and the whole-FAT scan that counting its free
space took, about 5 s on a 16 GB card -- ran across EPDiy's first register
write, which raises `PWR_EN`, and boots reset themselves there. Moving the
task did not cure that -- the cause was the log's hook, below -- but it stays
moved: the task touches PSRAM and puts the card's current on `VDD3V3`, and
neither belongs in the rail's inrush. Nothing is lost by waiting: the log's
ring holds the lines from the top of `app_main()`. **A mount does not count the free space**; `sd` asks for
it, and waits for the scan the first time after a mount. The event loop's part is `recorder_watch()` and a copy
of each event into the task's queue with no wait (`card_writer_post()`),
timed: `rec` prints the longest post beside the card's slowest write and
flush, which is what the event loop would have waited had it written itself.
The task logs its stack's low-water mark at each new low and, hourly, what it
has written.

### The simulator

`--record <dir>` runs the same recorder over `adp_files_dir`, so `<dir>/data/`
looks like a card's. `--recording <file>` (repeatable) replays a recording as
the source its file name's tag says, claiming what its header shows, under
that source's id and priority -- so a day recorded on the board plays through
the same merge -- and starts the wall clock at the earliest first row unless
`--wall-time` or `--no-wall-time` says otherwise. The simulator now gives IAQ
the BME68x's priority, as the firmware does.

## Consequences

- **Verified, and how**: `test_recording_round_trip` replays two scenarios --
  a warm-up, an error, an implausible value, a stale stretch, IAQ's accuracy
  rising from `unreliable` to `high`, a temperature below zero -- through the
  sensor manager with the recorder writing, replays the recordings through
  the same manager, and compares every poll's merged reading: statuses,
  accuracies and source ids equal, values within half the last decimal
  written. Both runs read through the same parser, so it also checks outright
  that the first run carried those statuses and accuracies; a parser that
  lost accuracy in both runs fails there. `simulator_record_replay` does the
  same through the simulator's command line and requires the same redraw
  decisions, 170 of them in half an hour of `co2_spike_meeting.csv`.
- **The parser's fuzzer was fuzzing blind, and now is not.** It linked the
  parser from `hac_platform_host`, which `-fsanitize=fuzzer` does not
  instrument, so libFuzzer saw only the harness: 4 coverage points, whatever
  the input. `tests/fuzz/CMakeLists.txt` now compiles the parser's own
  closure into the target, with UBSan beside ASan; seeded with
  `tests/fuzz/corpus/` (recordings as the device writes them) and
  `scenarios/`, two minutes reached 709 coverage points and 3,405 features in
  7.3 million runs, with no finding.
- **A replay reproduces the readings, not the sensor's failures' timing.** A
  failed read is recorded as a row of errors; replayed, it is a read that
  succeeds with error values, so the manager does not back off and re-init as
  it did on the board. The merged readings agree; the slot's life cycle does
  not.
- **Two recordings of one boot start at their own first rows**, which on the
  board are a few seconds apart; the simulator aligns both to its start, not
  to each other's `utc`.
- **Costs.** Measured in the build (`idf.py size`, `size-components`,
  against `569676d`, the same defaults): the image grew from 549,776 to
  663,232 bytes -- FATFS, the SD protocol, the SPI master driver and this
  code. Static internal RAM grew by 12,804 bytes (DIRAM 79,539 -> 92,343):
  about 7 KB of it is the SPI master driver's and the HAL's code that ESP-IDF
  places in IRAM by default (`CONFIG_SPI_MASTER_ISR_IN_IRAM` and the
  GDMA/SPI HAL; the card's transactions are polled, so turning the ISR's
  placement off is a candidate saving if phase 4's Wi-Fi makes internal RAM
  tight), about 2 KB this firmware's own statics in `main` (the status the
  console prints, the watch's events, the card's state), the rest FATFS's,
  newlib's and the SD driver's. At run time PSRAM gives 320 KB to the ring,
  about 13 KB to the recorder's and the log's buffers and FATFS its per-file
  caches; internal RAM gives the card's task its 6 KB stack and the SD
  driver and FATFS their structures when the card mounts. The board run
  measures those: the firmware logs free internal RAM and PSRAM when the card
  mounts, beside `free_internal_after_epd_init` (`board_notes.md`).
- **The estimates**: the flush interval and both caps are marked at their
  definition, to be replaced from the board's own bytes per day and flush
  times (`rec`, the hourly stats line).
- **Not done here**: reading through the port (the on-screen history's);
  recording over MQTT (phase 4, if wanted); persisting BSEC's state, which
  this card could now hold but which is phase 5's `persisted_state_t`; size-
  based rotation (a day is a few megabytes); the console's `printf` output in
  the log file.
- **`THIRD_PARTY_NOTICES.md`**: ChaN's FatFs (R0.15, inside ESP-IDF's
  `fatfs` component) is linked into the image for the first time. Its licence
  is a one-clause BSD-style one whose condition binds redistributions of
  source only.
