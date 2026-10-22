# 25. Touch input: the GT911 behind port_input, taps, and a diagnostics screen

Date: 2026-10-17

## Status

Accepted. Verified on the host: the GT911 model against Goodix's documents,
the driver and port_input over it against the model, the `port_input`
contract against three implementations with five broken wrappers caught, the
tap classifier, the screen controller, the diagnostics screen (layout by
pixels, two golden frames), the simulator tapping through both screens from
its command line, all under ASan and UBSan. The firmware, its BSEC variant and
the test app in both variants build.

**On the board, 2026-10-17** (ELF `f63cfdf60...`, the BSEC image, two
minutes, six taps): the controller named itself -- product ID `911`,
firmware 0x1060 -- which confirms the register map; its configuration selects
a falling edge, and the interrupt fired on it. Every tap switched the screen;
each reached the event loop 1-2 ms after the lift, and the change was on the
glass 1,163-1,173 ms after it: about 80 ms of rendering and diffing, then
EPDiy's 1,068 ms for the 920 x 500 partial refresh, the panel model's 1,080
within 1.1 %. Three days unattended after that (2026-10-17 to 20): the
corners land on the panel's corners (the vendor's swap and mirror are
right), a finger during a full refresh arrives after it, `touch_sleep` and
`touch_wake` work, the diagnostics screen times out home, and a run of taps
past the budget comes out full when the interval allows -- and one fault of
this design's, found and fixed: "The controller's address", below. The
fixed image has since found the controller at `0x14` at the first attempt
and woken it from a sleep across a reset, and the test app passed against
the real controller at either address (`docs/hardware/board_notes.md`, "The
touch panel on the board", done).

One decision here goes beyond the brief and is the owner's to confirm or
undo: the bound on partial refreshes past the budget ("A person's redraw",
below).

**Superseded in part 2026-10-22 by ADR 0034**: a change of screen is a full
refresh, always, held by neither the budget nor the panel's interval -- "A
person's redraw" below, and the bound past the budget it set, which now binds
nothing at the sensors' pace.

**Amended 2026-10-21 by ADR 0031** (phase 4d): the diagnostics screen has
two lines on the network -- the Wi-Fi link's state, name, signal and address;
the MQTT session's state, messages sent and refused, and the local time of
the last -- drawn as they are when the screen is drawn, redrawn only when the
link the status bar shows changes; its title is 32px to make room.

## Context

The board has a touch panel nothing read. `ports/port_input.h` has existed
since phase 1c; the simulator turned mouse clicks into events it only
printed; the device had no implementation, and the event loop's queue had
never carried an event. The bus scan has found the controller at `0x5D` since
phase 2d, a GT911 by its address -- the part number unconfirmed. Phase 3g
reads touches on the board through the port the simulator already feeds, and
gives them one consumer, so the path from finger to glass is real before
phase 6 builds a UI on it. Phase 5 needs the controller asleep and awake
again, and its interrupt as a wake source.

What the board does to the part (`board_notes.md`, "Pin assignments",
"Connectors"): it runs from `VDD3V3`, which firmware cannot switch; its reset
is a pull-up with no line to the ESP32; its INT goes to GPIO47 with no
pull-up fitted. Nothing resets it but a power cycle, its address is whatever
it latched at power-up (`0x5D` seen, `0x14` the other), and a GT911 a previous
firmware put to sleep is still asleep after an ESP32 reset.

## Decision

### Where the GT911 code lives

As the sensor sources do (ADR 0014), **in `drivers/gt911/`**, three parts:

- `gt911.c` -- the driver: plain functions over `port_i2c` and `port_clock`,
  not a port (one controller; the port is `port_input`, one level up). It
  finds the controller at either address, wakes it, reads and checks its
  product ID, reads its configuration, reads and clears reports, and puts it
  to sleep and wakes it.
- `gt911_touch.c` -- pure: a snapshot of the points down now, against the
  previous one, becomes `INPUT_TOUCH_DOWN`/`MOVE`/`UP` by track ID; and the
  controller's frame becomes the panel's.
- `gt911_input.c` -- `port_input` over the two: when to read, what a report
  means, when a finger whose reports stopped has lifted.

So everything that decides anything is a host test against
`platform/host/i2c_models/gt911_model.c`, and the board runs the same code.
`platform/esp32_t5s3/adp_input.c` adds only what is the board's: GPIO47 as
the interrupt and as the line the driver drives to sleep and wake, the ISR,
and the task that answers it.

**Sources.** The driver and the model were written from Goodix's documents,
independently of each other, and not from any driver: `Xinyuan-LilyGO/LilyGo-EPD47`
is GPL-3.0 and was read for facts about the board only (ADR 0007) -- the
vendor's touch example's swap and mirror, and its remark that GPIO47 is not
an RTC GPIO. The GT911 datasheet Rev.10 (2015-06-25, as Waveshare hosts it)
gives the I2C transfers, the two latched addresses, the interrupt -- a pulse
in every scan cycle while touched (8.2) -- and sleep: "requires INT output low
before the command", woken by "a high level to INT pin for 2ms to 5ms", the
interval between them "longer than 58ms" (8.1 d). The register map is not in
that datasheet; it is the programming guide's -- the command register
`0x8040`, the configuration `0x8047`-`0x8100`, the product ID at `0x8140`,
the status register `0x814E` and the point records after it -- and no legible
copy of the guide was at hand. `drivers/gt911/gt911.h` says so; a product ID
of "911" read back at `0x8140` on the board is the map's own proof.

### The vendor's configuration is never written

The configuration area is the vendor's calibration for this panel -- sensor
channels, thresholds, the resolution, the interrupt's trigger -- and a GT911
keeps what is written there in flash, across power cycles. The driver reads
it (the resolution, the touch number, `Module_Switch1`'s trigger bits, the
checksum, which it checks and reports but never repairs) and has no function
that could write it. The model refuses any write into the area with a data
NACK and counts it -- a real part would accept it -- so no test can pass by
writing it, and `tests/unit/test_gt911.c` checks the count is zero after
every operation; a mutant that writes one byte fails nine checks.

### The controller's address

The GT911 latches its address from INT's level as its reset rises -- at its
own power-up, which on this board follows `VDD3V3` through the touch flex,
not the ESP32. On 2026-10-20 a cable plugged in disturbed it, and it came
back at `0x14`. The first version of this driver asked each address for a
second with full reads of the product ID; at a silent `0x5D` every read was
refused, and ESP-IDF's I2C driver logged three error lines for each, from
inside the call, on the caller's stack. That stack was app_main's, 3,584
bytes: it overflowed into the heap, and the interrupt allocation that
followed crashed on the damage -- at every boot, while the controller kept
`0x14`, which an ESP32 reset does not change; in one boot of the loop
FreeRTOS caught it first, "A stack overflow in task main has been detected"
(board_notes.md has the logs). So:

- **Found by probes.** `gt911_init()` probes both addresses in turn,
  address only, for up to a second; a refused probe costs one refused
  address and no log line. Only the address that answered is read.
  `tests/unit/test_gt911.c` counts the refused data transfers on the fake
  bus (`refused_data_transfers`): none on either path, where the first
  version made 51 at `0x14` and 102 with nothing there.
- **Found by the touch task.** `adp_input_init()` starts the task, which
  runs the first init; app_main goes on, and the first frame no longer waits
  for the touch controller -- nor its absence. The task has 6 KB: with 4 KB,
  1,408 bytes were left at its lowest from its own log lines alone, short of
  what a failed transfer's error lines take.
- **Found again.** A read that fails means the controller is no longer
  where it was: whatever was down is lifted, and it is looked for at both
  addresses at once -- the one refused read that costs is the only failed
  transfer -- and, if it is nowhere, every `GT911_INPUT_RETRY_INIT_MS`
  (60 s), as after an init that found nothing.

### Sleep and wake

`gt911_init()` wakes the controller before anything else, since nothing else
does: the INT line high for 3 ms (the middle of the datasheet's 2-5 ms), then
released, then `GT911_WAKE_SETTLE_MS` (50 ms, an ESTIMATE: Linux's driver
holds the line that long after a wake) before the first transfer, and both
addresses probed every 20 ms for up to a second -- the vendor's example waits
a whole second after waking it "otherwise the touch device cannot be
addressed".

The INT line reaches the driver as three functions (`gt911_int_line_t`):
drive it low, pulse it high for so many microseconds, release it. **The
platform times the pulse, not `port_clock`**: `delay_ms(3)` may wait past
5 ms on the board's 10 ms tick (ADR 0006). On the board the pulse is a
busy-wait with the scheduler held, so not even EPDiy's render tasks -- the
highest priority there is, on both cores -- can stretch it. A controller that
was awake drives INT itself; for the pulse's 3 ms the two outputs meet, so
the pulse is driven at the S3's weakest strength (`BOARD_TOUCH_INT_WAKE_DRIVE`,
about 5 mA).

`gt911_sleep()` drives INT low, sends the sleep command, and keeps INT low --
the interrupt masked -- until the wake, which waits out the 58 ms first.

GPIO47 is not one of the S3's RTC GPIOs (0-21): it can wake the chip from
light sleep, not from deep sleep. Phase 5 inherits that.

### From the interrupt to the screen

1. The GT911 pulses INT once per report, on the edge its configuration's
   `Module_Switch1` selects -- read at init, so the board's controller sets
   it. The ISR notifies the touch task, and for a level-triggered
   configuration masks the interrupt until the task has read the report.
2. The touch task (priority 6, above the event loop and the card's task, on
   core 0) reads the report over the shared bus -- `adp_i2c`'s mutex shares
   it with the sensors -- clears it, turns it into events stamped with
   `port_clock`'s time **of the read**, queues them, and wakes the event loop
   through `event_loop_post_input()`, a post into the loop's own queue.
3. The event loop drains the queue into `core/app/screen_controller.c` at
   the top of every pass, before the sensors.

The event loop may be inside a 3.7 s full refresh when a finger lands: the
events wait in the adapter's queue (32), with the time they happened. Two
things happen without an interrupt, because an edge can be missed and a
finger must never stay down for ever: while a finger is down the task reads
every 50 ms regardless, and a finger whose reports stop for 250 ms (an
ESTIMATE: the datasheet's slowest normal-mode cycle is 10 ms) has lifted.

### Snapshots to events

The GT911 reports which points are down now, each with a track ID that
follows a finger. A track ID new in a report is a DOWN; gone, an UP at the
last position reported; still there, a MOVE once it has gone more than
`GT911_INPUT_MOVE_THRESHOLD_PX` (8 px, about a millimetre) from where it was
last reported -- measured from the last report, so a slow drag still moves.
At most two fingers: a finger already followed keeps its place, a new one
takes the lowest free place -- the event's id, "0 for the first finger", as
the port says -- and a third is never seen, not even when a place frees,
since it would arrive as a DOWN mid-gesture. In one report every UP comes
before any DOWN. A lift and a new finger with the same track ID between two
reports the host saw look like one finger that jumped: the controller gives
nothing to tell them apart, and it is reported as a move.

**Coordinates.** Swapped, scaled from the controller's configured resolution
to the panel's, mirrored: `BOARD_TOUCH_SWAP_XY`, `_MIRROR_X`, `_MIRROR_Y`.
The values are the vendor's (its example swaps and mirrors Y), not yet seen
on this unit; the console's `touch` prints each event raw and mapped so that a
finger at each corner settles them.

### Taps

`core/app/tap.c`: one finger down and up again within
`TAP_DEFAULT_MAX_DURATION_MS` (800 ms) and `TAP_DEFAULT_MAX_TRAVEL_PX` (40 px,
about 4 mm) of where it went down, on either axis, measured on the events'
own timestamps -- so a tap made during a full refresh is still a tap when the
loop gets to it. A second finger at any time spoils it; an UP whose DOWN was
never seen, and a second DOWN of the followed finger, start over rather than
guess. CHOICEs, both.

### Two screens

`core/app/screen_controller.c` -- not `app_core`, which still does not
exist; the loops call it as they call `sensor_manager_poll()` -- holds which
screen is on the panel. A tap anywhere switches between the home screen and
a new diagnostics screen, and the diagnostics screen goes home after
`SCREEN_CONTROLLER_DEFAULT_DIAGNOSTICS_TIMEOUT_MS` (60 s) without a touch:
a screen left up would otherwise hide the readings the device exists to
show. Any touch, not only a tap, keeps it up.

The diagnostics screen (`core/ui/diagnostics_view.c`,
`core/ui/screen_diagnostics.c`) shows what the core and the composition
already know: each sensor slot's name, state ("starting", "running",
"failing", "restarting"), failures and restarts since boot; each reading's
status in the home screen's words, its accuracy where the source gives one,
and which source the merge took it from -- a temperature "from scd4x" is the
fallback at work; whether the wall clock is set, and from where; the
recorder's state. Nothing on it changes at every poll -- no read counts, no
ages, no time -- and the slot states fold IDLE and MEASURING into one word,
because a forced-mode BME68x passes through both every cycle: the screen
redraws only when what it says changes (`diag_view_equal()`), a status redraw
under the same policy as the home screen's (`update_policy_evaluate_content()`).
Trouble is black, what is well is grey.

### A person's redraw

*Superseded by ADR 0034: a change of screen is a full refresh. Kept as the
record of the first design.*

A change of screen is a redraw a person asked for:
`update_policy_request()` makes it due at once, with
`UPDATE_REASON_REQUEST`. It does not wait out `min_redraw_interval_ms` --
the sensors' one-redraw-per-cycle limit (ADR 0019) is about readings, and a
person tapping does not wait five seconds. It is not free either: a partial
refresh like any other, counted against the budget, and full when the budget
says so -- or partial past the budget while a full refresh waits for the
panel's minimum interval, as ADR 0019 has it. No separate tap feedback in
this branch: the screen changing is the feedback.

**The bound past the budget -- the owner's to confirm.** ADR 0019 left a
warning: "a faster redraw source must revisit this". Touch is that source.
In the simulator, a person tapping every 1.2 s from just after a full
refresh -- each screen change 1.08 s on the glass -- reached 40 partial
refreshes in the panel's minute, the most this panel was ever driven through
(ADR 0008), and the panel model had to force a full refresh; at that pace 55
are possible, and on the device nothing would have stopped them. So partial
refreshes past the budget now stop at the budget again
(`UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET`, 2: at most 40 between two full
refreshes for this panel's 20), and then even a request waits for the full
refresh, which it then is. Derived from the budget the panel reports, no
number of the panel's own in the core. At the sensors' pace it never binds
(23 at most, ADR 0019's own test). In that simulated run the next tap's
screen came 4.1 s after the tap instead of 1.1 s. The alternative -- a
screen change that always goes out at once -- is one line to restore.

**Latency.** Each loop logs, for a redraw that carried a tap, the time from
the finger's lift to the end of the flush: on the board `port_clock` across
the synchronous flush (`event_loop: screen changed ... latency_ms=`), in the
simulator the time the event waited for the loop plus the panel model's
flush. **Predicted by the model**: a change between the home and diagnostics
screens redraws a 920 x 500 rectangle, 455 rows written, 1,080 ms on the
glass. ADR 0008 measured DU at 40.7 ms + 0.548 ms per written row against
GL16's 234 ms + 1.601 ms: the same change in DU would be some 0.4 s, which is
phase 6's question -- a quick black-and-white acknowledgement before the
greys -- not this branch's.

### The contract

`tests/contract/port_input_contract.c`, a shared body: `poll()` never waits;
an untouched panel yields nothing; a finger yields one DOWN where it landed,
stamped between the touch and the poll, id 0 for a lone finger, MOVEs of the
same finger inside the panel and in time order, one UP of the same finger;
events wait, in order, until polled; the far corner stays inside the panel.
On the host against `gt911_input` over the model, the simulator's scripted
taps and the SDL adapter (fed synthetic mouse events, no window); five
broken wrappers -- a phantom event, a blocking poll, unmapped coordinates, an
UP for another finger, a dropped UP -- each caught by the check meant for it.
In the test app against `adp_input` and the real controller, where nobody's
finger is on the panel: the checks that need one say they were not
exercised, and a second case puts the controller to sleep and wakes it,
printing whether its address answers while it sleeps.

### The simulator and the console

The window's clicks reach the same screen controller; `--taps
"<seconds>:<x>,<y>;..."` scripts taps for headless runs, and
`tests/cli/simulator_taps.cmake` taps through both screens and the timeout in
CI. The console's `touch` prints the product ID, firmware, the configuration
the controller holds, what it has reported and the last eight events raw and
mapped; `touch_sleep` and `touch_wake` put it to sleep and wake it --
`touch_wake` on a controller never found runs a whole init again.

## Consequences

- **The firmware's cost**, from the build's object files: about 13.3 KB of
  code -- the driver 3.2 KB, port_input over it 1.5 KB, the snapshot function
  1.1 KB, the adapter 2.8 KB, the diagnostics view and screen 3.4 KB, the tap
  and the screen controller 1.3 KB; static RAM 2.7 KB -- the adapter 1.2 KB,
  the screen controller with its two diagnostics views 1.1 KB, the console's
  copy of the touch status 0.3 KB -- and from the heap the touch task's 6 KB
  stack and a 32-event queue. The init runs on the touch task, about 90 ms
  (`adp_input: touch ready took_ms=`), beside the boot rather than in front
  of the first frame.
- **Every tap that changes the screen costs a partial refresh**, a whole-
  screen one: looking at the diagnostics and back is two of the budget's
  twenty. The simulator's panel model counts them as the board will.
- **A change of screen can make BSEC's call late**: the 1.07 s flush runs on
  the event loop's task, and three of the board's first six changes delayed
  BSEC by 290-660 ms against its 187 ms tolerance -- what ADR 0020's "Timing"
  expects of any long flush, and only while someone taps.
- **The touch task's stack**: 6 KB since 2026-10-20 ("The controller's
  address"); with 4 KB, 1,408-1,488 bytes were left at its lowest from its
  own log lines, before any failed transfer.
- **EPDiy's render tasks outrank everything while the glass is driven**: the
  touch task reads between their frames, and a finger lifted before it gets
  to read is reported at the read's time, not the finger's. Whether a finger
  during a full refresh still arrives is a board check.
- **What was ASSUMED, and what the board settled**: the register map --
  confirmed by the product ID; the swap and mirrors -- confirmed by the
  corners; the interrupt's trigger -- the configuration's falling edge;
  that the controller refuses its address asleep -- confirmed (a scan after
  `touch_sleep` and a reset found nothing), and that an ESP32 reset leaves it
  asleep, so init() must wake it -- confirmed; the 50 ms settle after a wake
  -- one attempt, awake or asleep; that the lift is a report of its own --
  no timed-out lift in three days.
- **Amended together**: `update_policy` (requests, a screen that compares
  its own content, the bound past the budget -- ADR 0019 noted), the event
  loops of both compositions, `docs/architecture.md` (layout, the contract
  suites), `docs/hardware/board_notes.md` (the touch items), `CLAUDE.md`.
