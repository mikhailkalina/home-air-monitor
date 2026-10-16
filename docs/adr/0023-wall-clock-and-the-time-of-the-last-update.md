# 23. A wall clock from the RTC, the time of the last update on the screen, and no periodic redraw

Date: 2026-10-16

## Status

Accepted. Verified on the host: unit tests of the calendar and the zone
rule, of the PCF8563 driver and of the firmware's RTC judgement against the
fake bus, the layout test, golden frames looked at before they were adopted,
and the simulator's panel model over every scenario file. The firmware, its
BSEC variant and the test app build. **First board run 2026-10-16** (the
BSEC image, `docs/hardware/board_notes.md`, open item "The RTC on the
board"): the RTC found with VL set and ignored, `time_set` writing it and
the system time, the footer's status redraw a second later, nothing redrawn
while the readings held still, the footer on the glass as designed, the
system time kept across a reset of the digital core, and -- after more than
a minute with no power at all -- the RTC still valid, VL clear: the backup
cell is fitted and holds. Still to see there: whether it kept counting
through the cut (`time` against a reference), longer outages, `restart`, the
drift and the autumn change.

Supersedes the periodic redraw ADR 0019 kept ("The periodic redraw only
while the data is getting older"), and departs from `docs/requirements.md`
9.1's example "Time since last update > 2 min -> update display". Shortens
two footer notes of ADR 0021 (below). Numbered 23: 22 is the proposal on the
branch `sensor-and-display-tasks`.

## Context

Nothing on the device knew the time of day. `adp_clock`'s `wall_ms()`
returned 0, `air_reading_t.wall_time_ms` was never set, and the footer said
"updated just now" or "updated N min ago" -- the second kept current by
`update_policy`'s periodic redraw, every two minutes while nothing on the
screen was fresh. That redraw was the only one the panel made because time
passed rather than because something changed.

The PCF8563 at `0x51` has answered every bus scan since phase 2d, and
nothing read it. The v2.4 schematic powers it from `VCC_RTC` -- `VDD3V3`
through a diode, with a rechargeable cell behind it -- so it is not on
`PWR_EN`, no reset stops it, and it should keep time with the board
unpowered (`board_notes.md`, "The RTC").

## Decision

### The RTC holds UTC; a driver, not a port

`drivers/pcf8563/` reads and sets the time over `port_i2c`: one burst read of
registers `0x00`-`0x08`, judged in order -- a failed transfer ("not
answering"), the VL bit ("VL set": the chip's supply fell below what its
oscillator needs since the time was written), the STOP bit ("stopped"), a
field that is not BCD or out of range ("undecodable": a day its month does
not have included) -- and a set that writes Control_status_1 to 0, running,
then `0x02`-`0x08`, which clears VL. Years are 2000-2099, from the years
register alone: the century bit's polarity differs between this chip and its
look-alikes, so it is written 0 (the datasheet's 20xx) and never read. The
weekday is written and never judged.

**UTC, never local time.** A clock holding local time is an hour wrong twice
a year until someone sets it; one holding UTC never is, and the zone is the
core's business.

**No port**: there is one RTC, on the board, and the simulator's wall time
already comes from `adp_clock_virtual` -- no second implementation for a port
to abstract over (CLAUDE.md). Like the sensor drivers it includes only
`ports/`, so the host tests it against the fake bus, whose register file is
the chip as it is.

Linux's `drivers/rtc/rtc-pcf8563.c` (GPL-2.0) was read for the register
masks, the VL check and the century bit's history; nothing of it is copied,
and it is not a dependency.

### Wall time is ESP-IDF's system time, 0 until something valid sets it

`adp_clock`'s `wall_ms()` is `gettimeofday()`, and the new
`adp_clock_set_wall_ms()` is `settimeofday()`. Phase 4's SNTP sets the same
system time through ESP-IDF's own `settimeofday()` and needs nothing here.
The port and its contract do not change: `wall_ms()` is 0 while nothing has
set the clock, which on this platform means a system time before
`ADP_CLOCK_WALL_EARLIEST_MS`, 2026-01-01T00:00:00Z -- at a chip reset
ESP-IDF's system time starts from the epoch.

At boot, once the bus is up (after the display adapter, as since phase 3a),
the composition reads the RTC (`apps/firmware_esp32/main/wall_clock.c`,
tested on the host): a valid time no earlier than the same floor sets the
system time. VL set, a stopped or undecodable clock, an implausible year --
2000-2025, which nothing of this firmware set -- or an RTC that does not
answer leave it alone, and the boot log says which (`main: wall clock
source=... rtc=...`).

**A software reset keeps the system time**, read in ESP-IDF 5.5's
`components/newlib/src/port/esp_time_impl.c`, not yet seen: ESP-IDF keeps
its "boot time" in two RTC_CNTL store registers, which `esp_restart()`, a
panic or a task watchdog does not clear, and the RTC timer under them keeps
running. So after such a reset `wall_ms()` already holds the previous boot's
time before the RTC is read. A valid RTC read replaces it; an invalid one
leaves it, and the source says "kept across a software reset". After a chip
reset -- power, RST, brownout -- the registers are cleared and only the RTC
can give a time.

**Console**: `time` prints the system time in UTC and Vilnius time and where
it came from (`rtc`, `console`, `kept across a software reset`), or, without
one, why ("rtc: VL set", "rtc: not answering", "rtc: undecodable", "rtc:
stopped", "rtc: implausible year"), and then what the RTC holds now, with its
registers. `time_set <YYYY-MM-DDTHH:MM:SSZ>` takes UTC -- no summer-time
ambiguity -- writes the RTC, which clears VL, and sets the system time. Until
phase 4 that is how the clock gets set; meanwhile it drifts at the crystal's
rate, **roughly a minute a month -- an ESTIMATE** (a typical 32.768 kHz
crystal's ±20 ppm), until phase 4's SNTP corrects it and writes the RTC back.

### Local time: a pure conversion in the core, the zone a rule passed in

`core/domain/civil_time.c` turns milliseconds since the epoch into a civil
date and time without `<time.h>`: the date from days since 1970-01-01
(Howard Hinnant's `days_from_civil`/`civil_from_days`), so every month end,
year end, leap day and century rule is one formula. Local time is UTC plus
the offset a `time_zone_t` gives for the instant: a standard offset, and
optionally summer time between two transitions, each "the nth (or last) such
weekday of such month, at such a minute UTC". The conversion holds no offset
and no date of its own. The composition passes the zone, as it passes
`update_policy`'s config: both `main.c`s build Vilnius as
`time_zone_eu(120)` -- UTC+2, and UTC+3 from 01:00 UTC on the last Sunday of
March to 01:00 UTC on the last Sunday of October (Directive 2000/84/EC).
`tests/unit/test_civil_time.c` holds it to both 2026 changes on either side
of the minute (02:59 -> 04:00, 03:59 -> 03:00), to 2027's, to a year end and
to leap days, and the same code to New York's nth-Sunday rule and Sydney's
summer across the new year.

The same module parses `YYYY-MM-DDTHH:MM:SSZ`, exactly, for `time_set` and
the simulator's `--wall-time`.

### "updated HH:MM": when the values on the screen were measured

The footer never shows an age again. With a wall time it says `updated
HH:MM`, the local time of the freshest value carrying a number on the
screen -- the reading's wall time minus that value's age, rounded down to the
minute -- with `DD.MM` in front when that is on an earlier local day than the
reading's wall time; without one, or with nothing carrying a number,
`updated --:--` in the faint ink. "just now", "N min ago", `format_age()` and
`updated_is_age` are gone. Both event loops put `clock->wall_ms()` into
`reading.wall_time_ms` at the point they read `now_ms`, after the poll (ADR
0017), and hand the view that same `now_ms`, so a value's wall time is exact.

What it means: the time the values on the screen were last brought up to
date. That stays true for exactly as long as those values are on the glass
-- and since nothing is redrawn because time passed, that is as long as the
line is shown. An age was false a minute after it was drawn, which is why
the periodic redraw existed. **Freshness is the stale marks' job**: a value
older than the sensor manager's `max_age_ms` turns `VAL_STALE`, and that is a
status change that redraws by itself, with the stale ink and the clock mark.

### No periodic redraw

`max_interval_ms`, `UPDATE_POLICY_DEFAULT_MAX_INTERVAL_MS`,
`UPDATE_REASON_PERIODIC`, `shown_age_grows()` and `periodic_deadline()` are
gone with their tests. The policy's deadlines come only from
`min_redraw_interval_ms` and the panel's minimum full-refresh interval;
otherwise it has none -- only new data changes its answer. A test keeps
staleness redrawing (`a_reading_going_stale_redraws_once`), and the full
refresh due past the budget now comes with the data going stale as it would
with any other status change.

This departs from requirements 9.1's "time since last update > 2 min ->
update display" on purpose: the requirement's aim -- a screen that does not
silently show old data as current -- is met by the time of measurement plus
the stale marks, without spending a refresh on a picture that would not
change.

**One new trigger**: whether a wall time is known is part of
`statuses_differ()`. When the clock is first set -- `time_set`, or phase 4's
SNTP -- the footer turns from `--:--` into a time at the next poll instead of
waiting for a reading to move. Only whether, never the time itself, which
changes at every poll. A known clock corrected by `time_set` does not
redraw; the next redraw shows it.

### Two footer notes, shortened

The layout test, extended with the widest `HH:MM` and `DD.MM HH:MM` this font
draws (found by measuring every one: `updated 01.01 00:04`, 417 px of ink at
40 px), found two of ADR 0021's notes too wide to share the line with the
dated form: "5 readings not calibrated" left 3 px between the halves where
24 are required, "Temperature not calibrated" overlapped. Both are now
shorter -- "5 not calibrated", "Temp. not calibrated" -- and neither occurs
on the device yet, where only BSEC rates its IAQ ("IAQ not calibrated" is
unchanged). The dated form beside a calibration note needs every shown value
fresh and the freshest measured before local midnight, so it lasts from the
first redraw after midnight until the next -- rare, but possibly all night.

## Consequences

### What the footer's minute costs, measured on the simulator's panel model

A redraw that changes both the CO2 headline and the footer's minute flushes
one rectangle spanning both. `co2_spike_meeting.csv` with `normal_day.csv`,
replay, `--time-scale 60`, two hours headless, before (main, `5e52250`) and
after (`--wall-time 2026-10-16T11:00:00Z`):

| | flushes | partial: mean | rectangle rows | written rows | panel time |
|---|---|---|---|---|---|
| before | 676 (33 full) | 460 ms | 123 | 126 | 410.2 s |
| after, `--no-wall-time` | 676 (33 full) | 460 ms | 123 | 126 | 410.2 s |
| after | 676 (33 full) | 478 ms | 170 | 132 | 422.0 s |

The 118 of 643 partial refreshes that fall in a new minute (18 %) span the
footer: 381 rectangle rows, 160 written, 565 ms against 459 for the rest --
105 ms more each, half of it EPDiy's difference pass over the 258 rows
between (`EPD_SIM_DIFF_PER_ROW_US`), half the footer's own written rows
(`EPD_SIM_PARTIAL_PER_ROW_US`). On average +18 ms a partial refresh, +2.9 %
of the time spent driving the panel; 24 hours of the same scenario: 4,891.7
-> 5,033.6 s, the same 2.9 %.

**Not split into two rectangles.** A second flush for the footer would cost
a second partial refresh -- its 234 ms base (`EPD_SIM_PARTIAL_BASE_US`) plus
about 40 written rows, some 300 ms -- and a second slot of the partial-refresh
budget, against the 105 ms the spanning rectangle costs. The numbers do not
ask for it.

### The number of refreshes never grows

| scenario (2 h unless stated) | before | after |
|---|---|---|
| `co2_spike_meeting` + `normal_day` | 676 | 676 |
| `co2_spike_meeting` alone | 676 | 676 |
| `sensor_scd41_fault` + `normal_day` | 241 | 241 |
| `sensor_scd41_fault` alone | 241 | 241 |
| `co2_spike_meeting` + `normal_day`, 24 h | 8,101 | 8,101 |
| `co2_spike_meeting` + `normal_day`, fake-i2c | 648 | 648 |
| `sensor_scd41_fault` alone, fake-i2c | 174 | 174 |
| a synthetic stale stretch (below) | 45 | 9 |

In none of the scenario files did the count drop, `sensor_scd41_fault.csv`
included, because in none of them did the periodic redraw ever fire: the
before-logs have no `reason=periodic` line. It needed nothing on the screen
fresh and something stale for two minutes, and under the sensor manager that
hardly happens: a failing source's fields turn `VAL_ERROR` at once, and a
stalled one goes `VAL_STALE` and then `VAL_ERROR` within its 30 s stall
timeout (ADR 0017); the SCD41 fault scenario shows errors, never staleness,
and the BME68x's fields stay fresh beside it. Only replayed rows marked
`stale` hold that state. A scratch pair of files that marks both sensors'
rows stale for 20 of every 30 minutes (not committed) shows the drop: 45
refreshes in two hours before -- 36 of them periodic, beside 34 periodic
redraws skipped as empty diffs -- and 9 after; 26.6 s of driving the panel
against 9.5.

The new trigger -- the wall time becoming known -- costs one refresh when
the clock is first set while the device runs; the simulator's clock is known
from the start, so it adds none there.

### On the board

To be seen (`board_notes.md`, open item "The RTC on the board"): `time`
before any `time_set` -- whether the RTC holds a valid time, and the footer
showing it or `--:--` accordingly; `time_set`, then the footer following the
redraws and nothing redrawing while the readings hold still; all power
removed, USB and battery, for some minutes, then a boot -- VL clear and the
time right means the backup cell works; `restart` with the clock set, whose
boot log should say `kept_across_reset=true` -- the system time surviving a
software reset, read from ESP-IDF and not yet seen -- before the RTC sets it
again; and, if the device is running at 01:00 UTC on 2026-10-25, the hour
step at its next redraw.

### Later phases

- **Phase 4**: SNTP sets the system time with nothing changed here; it should
  then write the RTC, so a reboot does not fall back to the crystal's drift.
- **Phase 5**: the RTC's INT on IO9 (`BOARD_RTC_INT_GPIO`, pulled up, unused
  now) is a wake-source candidate for deep sleep; the RTC keeps its time
  through deep sleep on `VCC_RTC`, while `now_ms()` restarts -- so a value's
  wall time across a sleep cannot come from `ts_ms`, which is
  `persisted_state_t`'s problem, already open in `board_notes.md`.
