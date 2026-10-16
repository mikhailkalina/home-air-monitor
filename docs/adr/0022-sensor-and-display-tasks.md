# 22. The firmware in two tasks: a sensor task and a display task

Date: 2026-10-16

## Status

Proposed by the owner, 2026-10-16; not implemented. Written to be built on a
branch of its own and judged on the board against the acceptance below.
Supersedes ADR 0020's "Timing" remedy, which named this as the fix to reach
for if BSEC's late calls ever mattered.

## Context

One FreeRTOS task, `hac_pump` (`apps/firmware_esp32/main/event_loop.c`; core
1, priority 5, 8 KB of stack), does everything at each wake: it polls the
sensor manager (the drivers' transfers, BSEC's call), ages the reading,
evaluates `update_policy`, builds the view model, renders into EPDiy's
buffer, flushes to the panel, commits, and sleeps until the earlier of the
manager's and the policy's deadlines. While it flushes -- 0.3-0.6 s for a
partial refresh, about 3.4 s for a full one (`epd_clear()` then a GC16
draw) -- no sensor is polled.

What that costs, measured on the board 2026-10-16 (ELF `a3fca4872...`, 21
minutes, `docs/hardware/board_notes.md`, open item "BSEC on the board"): 17
of BSEC's calls late, 0.3-1.6 s against its 187 ms tolerance -- one at every
full refresh, and the partial refreshes that started within about 0.6 s of
a call. BSEC still reached accuracy `high` in 16 minutes, so this is not
urgent for BSEC. The reasons to do it anyway are the requirements' and the
next phases':

- §9.1 asks to "decouple sensor sampling from display updates", and §15.3
  to "split FreeRTOS tasks by responsibility";
- §15.1: today a panel that hangs inside EPDiy stops the sensors with it;
- phase 4's MQTT publishing should not wait for the panel either.

Facts that constrain the design, each checked in the tree:

- **The core is single-threaded** (architecture §6.1): `sensor_manager`,
  `update_policy`, `view_model`, `screen_home` and `framebuffer` hold state
  and take no locks. Each must be touched by exactly one task.
- **EPDiy has tasks of its own.** `epd_renderer_init()` creates two
  `epd_prep` feeder tasks, pinned to cores 0 and 1 at
  `configMAX_PRIORITIES - 1` (`managed_components/epdiy/src/render.c`), which
  the calling task notifies to prepare lines while a frame is drawn. Whether
  they keep the CPU for the whole draw or block on their line queues is not
  known.
- **`epd_clear()` runs its push loop in the calling task**
  (`epd_clear_area_cycles()` → `epd_push_pixels()`), and its duration
  depends on that task's context: 2,456 ms calibrated on core 0, about
  2,322 ms from the event loop on core 1 (ADR 0008, "Task context").
- **The I2C bus is already shared safely**: `adp_i2c` serialises transfers
  with its own lock, which the console's `i2c_scan` and `i2c_recover` rely
  on today.
- **`adp_display_set_temperature()`** writes the fields `adp_flush()` reads.
  Today both run on the pump.
- **Core 0 is reserved for Wi-Fi** in phase 4 (`main.c`), whose tasks run at
  priority 23.

## Decision (proposed)

### Two tasks, each owning its half of the core

**`hac_sensors`, the sensor task**, owns the sensor manager and every
`sensor_source_t` -- the SCD4x, the BME68x and, in a BSEC build, BSEC's
instance -- and the minute-by-minute sample log. Its loop: poll the
manager, age the merged reading, publish it, wait until the manager's next
deadline. Nothing else.

**`hac_display`, the display task**, owns `update_policy`, the view model,
`screen_home`, the framebuffer, the previous-frame copy the partial diff
needs, `port_display`, and the panel temperature
(`adp_display_set_temperature()`, from the reading it is about to draw).
Its loop: wait for a new reading or the policy's deadline, take the latest
reading, age it against its own `now`, evaluate, render, flush, commit --
with today's backoff after a failed flush -- and wait again.

**Only a copy crosses**: the merged `air_reading_t`, by value (240 bytes),
through a one-slot mailbox -- a queue of length 1 written with
`xQueueOverwrite()`. Never a pointer into the other task's state. A reading
that arrives during a 3.4 s full refresh replaces the one before it, so the
display draws the newest, never a backlog, and the sensor task never waits
for the display.

### Priorities and cores: a starting point, to be measured

Both tasks on core 1, the sensor task above the display task -- say 6 and 4,
both below EPDiy's feeders (24) and Wi-Fi's tasks (23). The sensor task
wakes about every 3 s for a few milliseconds of transfers and BSEC; the
display task does the long work and is pre-empted by it.

Two things this cannot settle on paper, and the experiment must:

1. **EPDiy's feeders may still delay the sensor task** during a draw (0.3-1 s
   per refresh), at their priority above it on both cores, unless they block
   on their queues. If late calls remain and line up with draws, the next
   steps are, in order: the sensor task at `configMAX_PRIORITIES - 1` too, so
   FreeRTOS time-slices it with the feeder at each tick; or the sensor task on
   core 0 until phase 4 brings Wi-Fi there.
2. **The sensor task now pre-empts `epd_clear()`'s push loop** for a few
   milliseconds once every 3 s. Whether a pass lengthened by that shows on
   the glass -- a band, a row driven longer -- must be looked at on the
   panel, and the clear's duration re-measured from its new task
   (`platform/host/epd_emulation.h`, ADR 0008, and the test that holds the
   simulator to the board, `test_panel_limits_agree`).

### What changes, and what does not

- `apps/firmware_esp32/main/event_loop.c` becomes `sensor_task.c` and
  `display_task.c` (one file with two tasks is also fine); `event_loop.h`'s
  deps struct splits into the two halves; `main.c` starts both, sensors first.
  The `on_reading` hook splits: its log stays with the sensors, the panel
  temperature moves to the display task.
- **Stacks**: both start at 8 KB and are cut from their low-water marks once
  measured; the sensor task carries the drivers, BSEC and float logging, the
  display task the view model and the render chain. Up to 8 KB more internal
  RAM at first (217 KB free after EPD init today).
- **Unchanged**: everything under `core/`, `port_display` and its contract,
  `update_policy`'s rule that a redraw is committed only once the flush
  succeeded, the console, the I2C adapter, the simulator.

### Later phases

- **Phase 5 (deep sleep)** needs a barrier: the display task finishes its
  flush, the sensor task suspends its sources, both report quiescent (an
  event group), and only then may anything sleep. Today one task decides
  that alone.
- **Phase 6 (watchdog)** subscribes both tasks.

## Acceptance

On the host: `ctest` unchanged -- nothing in `core/` moves -- and
`check_sources.sh`. For the board, `idf.py build` with and without
`sdkconfig.bsec`.

On the board, with BSEC, 30 minutes including breaths and airing so that
both CO2 and IAQ force redraws:

- **`BSEC called late`**: none at partial refreshes; at full refreshes,
  none, or a figure that says where the time went (the feeders, item 1
  above);
- **the panel**: no new band, stripe or uneven area after full and partial
  refreshes, photographed beside the current firmware's; the clear's
  duration from EPDiy's own timing line, against 2,322 ms;
- **both tasks' stack low-water marks** and `free_internal_after_epd_init`;
- BSEC's accuracy reaching `high` as before; `i2c_scan` at the console
  while both tasks run.

## Consequences

- BSEC's calls are on time by construction, as far as EPDiy's feeders allow,
  rather than by scheduling refreshes around them; the SCD41's reads stop
  waiting for the panel too.
- A panel hung in EPDiy no longer stops the sensors, the log or, later,
  MQTT.
- **Concurrency arrives in the firmware**, and with it bugs the host cannot
  reproduce: the firmware's loop is not host-tested today, and the
  simulator stays single-threaded, so the two loops -- already written twice
  -- drift further apart. The ownership rule above is what keeps it
  tractable, and the first review question for any later change.
- Logs from two tasks interleave; each line still carries its tag.
- Panel timings may move by a few per cent and need re-measuring.

## Alternatives considered

- **Keep one task and draw only in BSEC's free window**: a partial refresh
  waits until just after a call. Cheap, removes the partial refreshes' late
  calls (11 of 17), leaves every full refresh's.
- **A full refresh in two steps** -- the clear in one window, the draw in the
  next. Fixes the rest, but changes `port_display`'s contract and
  `update_policy`'s bookkeeping.
- **BSEC alone in a task of its own**: splits the sensor manager's ownership
  of its sources, and gains nothing over the sensor task.
- **Leave it**: the second run shows BSEC copes. Chosen until now; this ADR
  is for when the reasons above outweigh the cost.
