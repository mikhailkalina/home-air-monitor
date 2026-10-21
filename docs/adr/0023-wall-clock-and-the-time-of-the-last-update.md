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

**Amended 2026-10-21, phase 4f** (branch `phase-4f-sntp`; "Amendment (phase
4f): time from the network", at the end): SNTP sets the system time once
Wi-Fi is up, and after each answer the event loop measures the RTC by its
alarm's tick on INT, stamped in an interrupt, and writes it to a second's
start. Verified on the host against a model of the chip that keeps time and
raises its alarm. A first version, with a task of its own that polled the
RTC for its tick, ran on the board the same evening; what it showed is in
the amendment, and changed the design. The design as it stands ran on the
board on 2026-10-22: INT on IO9 works, the write went out on time, and the
landing measured this chip's first tick a whole second after a start.

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
  Done in phase 4f, with more changed than this foresaw (the amendment below).
- **Phase 5**: the RTC's INT on IO9 (`BOARD_RTC_INT_GPIO`, pulled up, unused
  now) is a wake-source candidate for deep sleep; the RTC keeps its time
  through deep sleep on `VCC_RTC`, while `now_ms()` restarts -- so a value's
  wall time across a sleep cannot come from `ts_ms`, which is
  `persisted_state_t`'s problem, already open in `board_notes.md`.

## Amendment (phase 4f): time from the network

Date: 2026-10-21. Branch `phase-4f-sntp`.

Phase 4e gave the board a network (ADR 0032). Until now the clock was set by
hand -- `time_set` -- and drifted at the crystal's rate, the ESTIMATE above of
about a minute a month, with nothing to measure it but someone reading `time`
beside a reference weeks apart (`board_notes.md`, open item "The RTC on the
board", *Drift*).

### SNTP, from Wi-Fi's first address

`apps/firmware_esp32/main/net_time.c` registers for ESP-IDF's
`IP_EVENT_STA_GOT_IP`; at the first, the event loop starts ESP-IDF's
`esp_netif_sntp` with one server, `CONFIG_HAC_SNTP_SERVER`
(`main/Kconfig.projbuild`, `pool.ntp.org` by default: the NTP Pool's own name
suits one person's device; a product sold in numbers would ask the Pool for a
vendor zone), and an hour between answers (`NET_TIME_SYNC_INTERVAL_MS`,
ESP-IDF's default made this project's choice). From then on lwIP asks by
itself: an unanswered request again 15 s later, doubling to 150 s, through any
outage. A later address does not restart it. No server from DHCP (option 42,
which needs `CONFIG_LWIP_DHCP_GET_NTP_SRV`): the owner names one, a router's or
Home Assistant's as well as the Pool.

### lwIP's answer on lwIP's task; the rest on the event loop

lwIP hands an answer to `sntp_sync_time()` on its own tcpip task, which must
not wait on the I2C bus. ESP-IDF declares that function weak and documents
replacing it (`components/lwip/include/apps/esp_sntp.h`); `net_time.c`
replaces it. The server's time is held to `wall_clock_believable_ms()` --
2026 to 2099, this firmware's floor and the chip's last year -- because lwIP
as ESP-IDF builds it checks little of an answer (`SNTP_CHECK_RESPONSE` 0: no
leap indicator, no stratum but the kiss of death), a server that has not
synchronised itself can answer anything, and what is set here is written into
the RTC. A believable time is set at once with `settimeofday()`, as the
default does, and the sync status completed; any other is refused and nothing
is set. Either way the answer is left in a mailbox under a spinlock and the
event loop woken (`event_loop_post_time()`); nothing else happens there. Not
`esp_netif_sntp`'s `sync_cb`: it is called after the default has already set
the time, too late to refuse one. With the default replaced,
`esp_netif_sntp_sync_wait()` and the notification callback no longer fire;
nothing here uses them.

Everything else runs on the event loop, through a step hook of its own with a
deadline (`event_loop_deps_t.step`, `net_time_step()`): the answer logged
(`synced`, with how far the system time had been from it), the wall clock's
source set to `sntp` -- `time`'s `source="sntp"`, the diagnostics screen's
"Clock: set (sntp)" -- and the RTC's sync, `apps/firmware_esp32/main/rtc_sync.c`,
stepped. When ADR 0022's display task takes the panel off the event loop, the
step goes with the sensors, which keep the bus. No task of its own: see "The
first version", below.

### The RTC measured by its own tick, on INT

Reading the PCF8563 says which second it is in; where in that second it is,
only its tick says. Its alarm marks one: AF "sets ... when the time increments
to match the alarm condition", and with AIE set the open-drain INT "follows
the condition of bit AF" (datasheet Rev. 11.1, 8.6.5). With the minute alarm
alone enabled, INT falls at the tick that starts that minute. It goes to IO9
(`BOARD_RTC_INT_GPIO`, pulled up to `VDD3V3` -- the schematic's, read, not
measured), where `platform/esp32_t5s3/board_rtc_int.c` stamps each falling
edge with `esp_timer_get_time()` in an interrupt. That stamp is the
measurement, to the interrupt's latency, whatever the tasks are doing; when
the event loop looks at it does not matter.

`rtc_sync.c`, after each answer:

1. reads the RTC and arms the alarm for its next minute -- the one after, from
   second 58 on (`RTC_SYNC_ARM_LATEST_SECOND`), so that an increment between the
   reading and the arming cannot push it an hour away;
2. at the edge: the RTC's time there is that whole minute, found from a
   reading taken any time later less the time since the edge, rounded to the
   minute; the system time there is the edge's stamp carried over from
   `now_ms()`. Their difference is the RTC's offset, and from the last write's
   mark its rate (`rtc_sync_drift()`, over at least ten minutes,
   `WALL_CLOCK_DRIFT_MIN_ELAPSED_MS`). Logged, `rtc against sntp`, before the
   write. The alarm is taken back, INT let go. No edge by a second and a half
   after the latest the minute can start (`RTC_SYNC_EDGE_GRACE_MS`): "no INT
   edge", and the write goes ahead unmeasured;
3. the write -- below;
4. the alarm armed again, and the next minute's edge says where the RTC
   landed: `rtc landed residual_ms=...`, the mark the next drift is measured
   from. Measured, never predicted.

The whole takes one to three minutes of wall time, two minute boundaries; on
the event loop it is about six steps of a few I2C transfers, a millisecond
each, and the write's spin.

### The RTC set to a second's start

Writing the time registers of a running PCF8563 leaves its prescaler where it
was, so the next tick comes where the old phase puts it -- anything up to a
second off the second written. Only STOP resets the prescaler's upper stages,
and by the datasheet "the first increment of the time circuits is between
0.507813 s and 0.507935 s after the STOP bit is released" (8.10, Table 26).
`pcf8563_set()`, which `time_set` uses, writes to the second only; a boot then
reads the RTC to the second, truncated. To the minute the screen shows,
nothing; to an hour's drift of a hundred milliseconds, everything.

The write is planned for the start whose first tick falls on a second's end,
at least `RTC_SYNC_WRITE_LEAD_MS` ahead; the event loop is woken
`RTC_SYNC_SPIN_MAX_MS` (30 ms) before it, stops the clock, writes the second,
spins the rest in single milliseconds and starts it, stamping the system time
either side of the start's transfer. A wake too late for the moment -- the
loop held by a full refresh, 3.7 s today -- plans it again for a later
second, `RTC_SYNC_MAX_REARMS` (3) times; then the start goes out where the
loop is, late, and the landing measures how late. The clock stands still for
the spin at most, 30 ms an hour; a reset in it leaves the clock stopped, which
the next boot reads as "rtc: stopped" and the next answer starts. A failed
transfer never leaves it stopped either. The driver gained the writes it
takes: `pcf8563_stop()`, `pcf8563_write_time()`, `pcf8563_start()`,
`pcf8563_alarm_at_minute()`, `pcf8563_alarm_off()`.

**The delay from a start to the first tick is learned.** The write is planned
with the datasheet's 508 ms until a landing has measured this chip's: the
first tick after a start comes at the landing's tick less its offset, less
the start's stamps. A measured delay between 0.1 and 1.9 s
(`RTC_SYNC_FIRST_TICK_MIN_MS`, `_MAX_MS`) is the next write's; outside, a
measurement gone wrong, and the last believed value stays. It lives in RAM: the
first write after a boot uses the datasheet's again.

### The first version, on the board, and why it changed

The first version of this amendment gave the work a task of its own
(`hac_time`, 6 KB, priority 5 on core 0), which found the tick by polling the
RTC -- every 10 ms until its seconds changed, then every millisecond across
the next one -- and timed the write by spinning, about 3 s an hour, and
predicted the landing from the datasheet's delay before measuring it by
polling again. It ran on the board on 2026-10-21 (ELF `9fb8fd2b5...`, the BSEC
image, the owner's YAT log of 23:41 local, one sync):

- **The write landed 481 ms behind** (`rtc written ... residual_ms=-481
  uncertainty_ms=2 stopped_ms=45 start_late_ms=0 rearms=0`): started on time,
  the RTC's first tick came about 0.989 s after the start, not the
  datasheet's 0.508. One sample: the chip may be a look-alike of the
  PCF8563 -- its marking is to be read -- or behave so for another reason. The
  datasheet's delay cannot be trusted to predict a landing: hence the landing
  measured, and the delay learned.
- **The RTC had drifted -12.9 s** (`rtc_minus_sntp_ms=-12918 uncertainty_ms=1`),
  the system time from it -13.1 s (`system_minus_sntp_ms=-13148`, the boot's
  truncation the 0.23 s between). Since the `time_set` of
  2026-10-16T17:35:40Z, if nothing wrote it in between, that is -29 ppm,
  ±3 for the hand-typed second: outside the ±20 ppm ESTIMATE, some 105 ms an
  hour.
- **The task cost 8.4 KB** of internal RAM with the link up
  (`memory with the link up free_internal=66015`, against 74,419 in phase 4e's
  last image), its stack 3.2 KB used of 6.
- **Polling found the tick** to 1-2 ms on the board, but its precision hung
  on the task running when it read: a wait for the bus's lock, or EPDiy's
  feeder tasks at the top priority on both cores while a frame is drawn
  (ADR 0022), shifted a read from the moment stamped before it.

The owner's questions after the log -- why a task, what the event loop holds
up -- settled it: the interrupt's stamp makes the measurement independent of
any task's load, the start's stamps make its lateness known instead of
assumed, and with both there is nothing that needs a task of its own.

### Seen on the board

2026-10-22 (ELF `59c002862...`, the BSEC image, the owner's YAT log of 00:38
local, one sync; `board_notes.md` has the lines):

- **INT reaches IO9**: the pin high at boot, both of the sync's edges stamped.
- **The offset**, -554 ms ±2, and the write on time: `start_late_ms=0
  stopped_ms=21 rearms=0`.
- **This chip's first tick after a start comes a whole second later**:
  `rtc landed residual_ms=-492 first_tick_ms=1000 learned=true`. The polling
  version's 989 ms the evening before was its own error, the start's stamp
  taken before the wait for the bus. Learned for the next write; not yet seen
  landing.
- **The drift between the two images' writes**: 73 ms in 3,483 s, -21 ppm,
  against -29 over the five days before.
- **The cost as estimated**: 72,503 bytes of internal RAM free with the link
  up, 6.5 KB above the version with a task.
- **One error line at every boot**: the RTC's adapter installed the shared GPIO
  ISR service before the touch task, whose own install ESP-IDF then logged as
  an error. Removed after: `platform/esp32_t5s3/board_gpio_isr.c` installs it
  once, on app_main before either asks; not yet seen on the board.

### Precision

- The offset before the write: ±2 ms, from three readings truncated to the
  millisecond -- the edge's stamp, `now_ms()` and the system time; the
  interrupt's latency is microseconds. However late the edge is looked at.
- An hourly rate: about ±1 ppm from the two ticks; on top, the ESP32's own
  crystal over the minute or so between the answer and the edge (about a
  millisecond at ±10 ppm), and SNTP's own lag: lwIP as ESP-IDF builds it takes
  the server's time as the packet arrives, with no round-trip correction
  (`SNTP_COMP_ROUNDTRIP` 0), so each answer lags UTC by the packet's one-way
  trip. What varies from hour to hour is noise on each sample, which a day's
  samples average. `system_minus_sntp_ms`, hour by hour, is the ESP32's own
  drift.
- The landing: ±1-2 ms from the second write of a boot on, the delay learned;
  the first write after a boot as the datasheet's delay is right for this chip
  -- 0.49 s off on this board, whose chip first ticks a whole second after a
  start. Known either way.
- If INT does not reach IO9, no edges: "no INT edge" twice an hour, the RTC
  written with no offset measured and no landing seen, and no drift. `time`
  shows the edges counted and the pin's level.
- The GPIO interrupt sits on ESP-IDF's shared GPIO ISR service, installed once
  for it and the touch adapter without `ESP_INTR_FLAG_IRAM`
  (`board_gpio_isr.c`): while the flash's cache
  is off -- a write to flash -- it waits, and an edge then is stamped late.
  Nothing writes the flash in the steady state; the radio's calibration data
  once, at its first start.

### What does not change

The core: it already redraws when the wall time becomes known (above, "One new
trigger"), which is what a boot with nothing in the RTC sees at the first
answer, and a known clock corrected does not redraw. The boot: the RTC is read
as before, some 15 to 25 s before the first answer. The simulator: its wall
time is `adp_clock_virtual`'s, and there is no port -- one implementation
(CLAUDE.md). `adp_clock`: SNTP sets the same system time.

### Console

`time` adds `TIME_SNTP` lines: where the RTC's sync is, INT's level and edges,
the delay the next write is planned with and whether it was measured; the
server, the answers, the last one's time and age, how far the system time and
the RTC were from it, the drift, the writes and the last landing. `time_set`
says the next answer will set both again. `rtc_stop` sets the RTC's STOP bit:
a boot then finds no time in it ("rtc: stopped") -- the board check that the
network alone gives the time. Both stop a sync under way and forget the mark.

### Verified on the host

`platform/host/i2c_models/pcf8563_model.c`, a PCF8563 that keeps time and
raises its alarm, written from the datasheet independently of the driver: its
registers, a tick a second on the fake bus's clock with the calendar's
carries, STOP and the first tick after it, a write that keeps the phase, a
crystal `ppm` fast or slow, the alarm setting AF only on the increment into
its minute, INT following AF and AIE and reported with the tick's moment
(`tests/unit/test_pcf8563_model.c`). Against it, stepped as the event loop
would be (`tests/unit/test_rtc_sync.c`): offsets of 0, ±1 ms, ±300 to ±999
ms, -12.9 s, a day and an hour, each from three places in the second, found to
their stated uncertainty, and the landing on the second; the board's late
first tick, 0.989 s, landing 481 ms behind and then, learned, on the second;
the drift over an hour at -29, +20, 0 and +3 ppm and over a day at +17, each
within 1.5 ppm; the loop held 3.7 s at the start's moment, planned again; a
loop always half a second late, started late and measured; an edge looked at
20 s late, exact; no INT edge, an RTC with VL set or stopped, no wall time, a
time past 2099 and a hung bus, each said, with the clock never left stopped
nor INT low; an abort, and a second answer mid-sync. Mutations tried: the
delay not learned, the minute floored instead of rounded, the second-58 rule
dropped, the edge count taken after the arming -- each failed those tests. A
bias of a second before the rounding passed them, as it should: the rounding
absorbs it.

### What it costs

`idf.py size`, the firmware with the same network against `main`'s
(`1d7beff`), 2026-10-21: static internal RAM 131,379 -> 133,115 bytes, +1,736,
of which 1,251 is lwIP's DNS client, linked for the first time because the
server is a name (an address in `CONFIG_HAC_SNTP_SERVER` leaves it out until
MQTT needs it), 336 `net_time.c`'s, 61 the SNTP client's and 24 the
interrupt's, whose handler adds 48 bytes of IRAM; the image 1,215,056 ->
1,235,920 bytes, +20,864. No
task, no stack: the board's `memory with the link up` should read about 72 KB
free. Without a network, no SNTP and no lwIP (`size-components`); 336 bytes of
`net_time.c`'s state stay for the console's status line. Every hour some six
steps on the event loop, a few I2C transfers each, and a spin of 30 ms at most
-- inside BSEC's 187 ms tolerance; some eight transfers on the shared bus.

### Not decided here

- **A server's time that is believable and wrong** -- a router serving its
  firmware's build date after a power cut, say -- is set and written. The
  floor refuses only the unset. It shows as a large `system_minus_sntp_ms`
  and `rtc_minus_sntp_ms` in that answer's lines.
- **The drift corrected, not only measured**: the PCF8563 has no offset
  register (its successors do), and an hourly write already bounds the RTC's
  error to an hour's drift. Whether the system time should be held to a
  measured rate between answers is phase 5's question, where deep sleep makes
  the RTC the only clock.
- **The learned delay kept across boots**: in RAM only. The board's first
  measurement by INT is 1,000 ms; if the next ones hold it, it can replace the
  datasheet's 508 as the default, and a boot's first write land too.
- **INT for deep sleep**: IO9 is one of the S3's RTC GPIOs, so phase 5 can wake
  on the same alarm.

### On the board

`board_notes.md`, open item "Time from the network on the board (phase 4f)":
INT on IO9 at all, a cold boot with nothing in the RTC showing the right time,
the first landing and the learned delay, the hourly lines and the drift they
give, the cost, the RTC chip's marking, and the autumn change on 2026-10-25 if
the board runs through it.
