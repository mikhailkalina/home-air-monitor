# 8. Panel timing and the partial-refresh budget, measured on this unit

Date: 2026-10-13

## Status

Accepted. Replaces every estimate phase 1 put into
`platform/host/epd_emulation.h` and the partial-refresh budget phase 2a put
into `platform/esp32_t5s3/board_config.h`.

## Context

Phase 1 invented the simulator's e-paper constants -- refresh latencies quoted
as ranges, a ghosting rate, a partial-refresh budget of 20 -- and said so at
their definition. Phase 2b put an image on the glass and logged a handful of
EPDiy's own timing lines, enough to know the estimates were wrong but not
enough to replace them. `update_policy` decides against two numbers the panel
reports through `port_display`, and the simulator is only evidence about the
panel if it emulates the panel. Both needed measurements.

The ADR number is 0008 because CLAUDE.md asks for the next free number; the
phase brief named this file `0002-panel-timing.md`, which predates ADRs
0003-0007.

## Conditions

E-paper timing depends on the individual panel and on temperature, so these
travel with every figure below:

| | |
|---|---|
| Board | LILYGO T5 4.7" e-Paper S3, silkscreen **Screen-4.7-S3 v2.4, 2024-12-03** |
| Panel | ED047TC1, 960 x 540, this project's one unit |
| Ambient | **21 C**, room air at the board, by thermometer |
| Backend | EPDiy, `AdaSzi/epdiy` at `161de5275f`, waveform `epdiy_ED047TC1` (one temperature range, so `BOARD_EPD_ASSUMED_TEMPERATURE_C` = 25 selects the same tables at any value) |
| CPU | 160 MHz (the IDF default). EPDiy's difference pass is CPU-bound and is part of every figure; a faster clock would shorten that part only |
| Images | timing: calibration image `a25cae01...`; ghosting probes and residue test: `sdkconfig.calibration_*` overlays of the same code |

## Method

`apps/firmware_esp32/main/epd_calibration.c`, behind `CONFIG_HAC_EPD_CALIBRATION`
(off by default), built from `sdkconfig.calibration`. It times
`port_display->flush()` with `esp_timer` -- the call the core waits on and
the simulator emulates -- so the power bracket, the `epd_clear()` that starts
a full refresh and the adapter's own logging are all inside every figure. They
cost 2-3 ms per flush against EPDiy's own per-update total.

Five iterations. Each runs a `REFRESH_CLEAR`; partial and fast-mono refreshes
over four rectangles anchored at the origin, each driven solid black and back
to white (two samples per iteration, every row of the rectangle dirty both
times); a `REFRESH_FULL` of a solid black frame; and a `REFRESH_FULL` of
`screen_home`. Whole-panel drives are spaced by the panel's minimum interval
and the partial batches stay inside the budget the panel reported at the time.

## Results

Microseconds, `n` samples, all `failures=0`:

| Case | Mode | Rect | n | Min | Max | Mean |
|---|---|---|---|---|---|---|
| clear | `REFRESH_CLEAR` | panel | 5 | 2,455,849 | 2,455,881 | **2,455,869** |
| full_black | `REFRESH_FULL` | panel | 5 | 3,660,504 | 3,660,628 | **3,660,569** |
| full_home | `REFRESH_FULL` | panel | 7 | 3,324,753 | 3,325,213 | **3,324,863** |
| full_ramp | `REFRESH_FULL` | panel | 1 | | | 3,659,850 |
| partial | GL16 | 960 x 540 | 10 | 1,205,775 | 1,206,497 | **1,206,117** |
| partial | GL16 | 960 x 228 | 10 | 646,645 | 647,075 | **646,803** |
| partial | GL16 | 240 x 228 | 10 | 646,465 | 647,136 | **646,623** |
| partial | GL16 | 240 x 60 | 10 | 344,973 | 345,107 | **345,018** |
| fast_mono | DU | 960 x 540 | 10 | 442,792 | 443,174 | **442,912** |
| fast_mono | DU | 240 x 228 | 10 | 211,126 | 211,251 | **211,183** |
| fast_mono | DU | 240 x 60 | 10 | 86,359 | 86,831 | **86,435** |
| ghosting probe | GL16 | 49-76 rows, product rects | 40 | 327,501 | 376,082 | 330,586 |

What the table says:

1. **Partial and fast-mono refreshes cost by rows, not by area.** 960 x 228
   and 240 x 228 differ by 0.03 %. EPDiy's row output clocks every panel row
   out on every waveform pass, writing whole rows that contain a changed
   pixel and skipping the others more cheaply
   (`output_i2s/render_i2s.c`, `i2s_output_frame()`), so the width of a
   rectangle cannot matter. Phase 1's model scaled by area and was off by a
   factor of 6.6 for the 240 x 228 case (98 ms predicted, 647 measured).
2. **Within a mode the cost is linear in rows -- of two kinds.** EPDiy first
   computes the difference between the new frame and its model of the glass
   over every row of the rectangle (its own log: 106 ms for 540 rows, 45 for
   228, 12 for 60). Then each waveform pass writes the rows that hold a
   changed pixel, plus the first two rows of every run of unchanged rows,
   which it writes as no-op rows while its output pipeline drains
   (`i2s_skip_row()`'s `skipping < 2`); the rest are only clocked past. So:
   0.196 ms per rectangle row, plus, for GL16 (30 passes), 234.0 ms + 1.601 ms
   per written row; for DU (5 passes), 40.7 ms + 0.548 ms per written row --
   fitted through the 60- and 540-row means (62 and 541 written rows).
3. **A full refresh is `epd_clear()` plus a content-dependent GC16 pass.** The
   clear is a constant 2,456 ms and is the visible flashing. GC16 then drives
   only rows that are not white: 1,205 ms for a solid frame, 869 ms for
   `screen_home`.
4. **The panel is deterministic.** Every case spans less than 0.1 % between
   its fastest and slowest sample; the drive is clocked by hardware.

## Decision: the timing model

`epd_emulation.h` carries the fits, in microseconds. `epd_model.c` takes the
frame being flushed and what the emulated glass shows, and walks the rows the
way EPDiy does (`written_rows()`): which rows change, which get written. Both
host display adapters keep a glass for it, so headless and windowed runs time
the same frames identically.

| Mode | Model |
|---|---|
| `REFRESH_PARTIAL` | 196 us x rectangle rows + 233,977 us + 1,601 us x written rows |
| `REFRESH_FAST_MONO` | 196 us x rectangle rows + 40,711 us + 548 us x written rows |
| `REFRESH_CLEAR` | 2,456 ms |
| `REFRESH_FULL` | 2,456 ms + the partial formula over the whole panel, written rows = the frame's non-white rows walked as above |

When nothing in the rectangle changed, only the difference pass is charged:
EPDiy finds no difference and draws nothing.

**How far it is from the glass:**

| Against | Margin |
|---|---|
| every measured timing case (rectangles whose every row changes) | within 0.3 % |
| the ghosting probe's 40 headline refreshes (49-76 rows) | +0.2-0.3 % |
| `screen_home`'s full refresh, calibration (316 non-white rows, 334 written) | +0.2 % (3,331 vs 3,325 ms) |
| **the product's own partial refreshes**, replayed (below) | +0.7 to +1.3 % against EPDiy's own per-update total, which leaves out the 2-5 ms of power bracket and logging the model includes |
| **the product's full refresh** | GC16 part +0.8 %; the whole refresh about **+4 %**, because the clear ran 2.32 s from the event loop's task against 2.46 s from the calibration's -- see "Task context" |

The product replay: the firmware's own run (bring-up data, the event loop's
render -> diff -> flush, budget 10) was logged on the device, and the same
frames were re-rendered on the host from the logged `now_ms` of every
decision and pushed through the model. Every dirty rectangle came out
identical to the device's (`28,132 824x228`, `68,110 868x250`, ...), and the
durations as in the table. Counting rectangle height instead of written rows,
as the model first did, overstated those same refreshes by 60 % -- 646 ms
for an 824 x 228 rectangle in which 72 rows change.

`tests/unit/test_epd_model.c` holds the model to 1 % of every measured case,
including `screen_home`'s full refresh rendered from the calibration run's own
reading, and checks the row walk itself. An edit that drifts from the glass
fails there; the margin checks were seen to fail against deliberately wrong
constants before being kept.

The window flashes the refreshed area black and white for the clear's
2,456 ms -- three cycles of about 0.37 s black and 0.45 s white, the push
pattern of EPDiy's `epd_clear()` sharing out the measured total -- then the
image draws in over the rest (1,205 ms for a solid frame, 875 for
`screen_home`), which is what the glass does.

## Ghosting, and the budget

Two probes, each 40 partial refreshes of `screen_home` 6 s apart, with the
CO2 headline stepping and the step number drawn beside it, watched at about
50 cm and then close up:

- **Sawtooth**, the realistic case: CO2 walks 600 -> 1620 -> 600 ppm in 60 ppm
  steps, as the bring-up data does under `update_policy`. Nothing visible at
  50 cm through 40 steps. Close up, remnants appeared only around the "ppm"
  unit, and only from its first four-to-three-digit change (step 28, observed
  as "25-30").
- **Shift**, the worst realistic case: CO2 alternates 980 <-> 1040 every step,
  so the unit jumps one digit width back and forth. Nothing visible at 50 cm
  through 40 steps. Close up, remnants appeared **between steps 11 and 25**.

What the remnants look like matters for the model: not dark smudges where a
digit used to be, but **light outlines of an earlier digit inside the current
dark one**. EPDiy's partial refresh drives only pixels that change; dark
pixels that stay dark across several partial refreshes are never redriven and
end up lighter than freshly driven neighbours, so an earlier glyph's shape
shows through. Every full refresh removed them from the live screen.

**`max_partial_refreshes_before_full` = 10**: one below the earliest onset
observed even close up, in the worst realistic pattern. Chosen over 20 (half
of what is invisible at 50 cm) and 40 (the most tested) because a budget
errs towards full refreshes when in doubt -- ADR 0004's rule that wasted power
is recoverable and a damaged panel is not -- and because of the residue below.

The simulator reproduces the observation rather than phase 1's guess: a dark
pixel left undriven by `EPD_SIM_GHOST_VISIBLE_AFTER` (11) partial refreshes is
shown one grey step lighter, and a full refresh resets it. With a budget of
10, a policy that respects the panel shows no outlines in the window, exactly
as on the glass; one that does not shows them from the eleventh.

**Cost.** In `scenarios/co2_spike_meeting.csv`, whose CO2 climbs about
300 ppm a minute, `update_policy` issues a partial refresh roughly every 11 s,
so a full refresh -- 3.7 s, mostly flashing -- comes about every 2 minutes. A
real room changes more slowly. If the flashing proves objectionable, the
lever is `update_policy`'s redraw thresholds (`docs/requirements.md` 9.1), not
the budget.

## Grey levels

On the grey ramp the calibration run leaves on the panel, levels **0 and 1
cannot be told apart**; 14 and 15 can, and every other neighbouring pair can.
Fifteen usable greys. Nothing in `core/ui/view_model.h` uses level 1 today.
The window renders level 1 as level 0 (`EPD_SIM_DARKEST_MERGED_LEVEL`), so a
design that leans on the difference fails on the PC first.

## Residue that survives a full refresh

The first timing run's closing ramp showed faint bands across the dark bars
(levels 2-5), running along panel rows the timing batch had driven repeatedly
(its rectangles are anchored at the top of the panel), and three barely
visible lines across the white bar -- through three full refreshes, each a
three-cycle `epd_clear()` plus GC16. The shift probe's closing ramp, two full
refreshes after the probe, showed the "ppm" unit legible on the dark bars
where it had stood (bars 3-4). On white, the same screen was clean.

So a full refresh removes the outlines from the live screen but does not
return every pixel to an identical state: mid-dark greys still show where
partial refreshes concentrated. This could matter more than the outlines do.
The budget bounds how many partial refreshes run between full ones, not how
many run in total -- that is set by how often the data changes -- so if each
full refresh left something behind permanently, the budget alone would not
bound what accumulates over the panel's life.

**The residue test** (`sdkconfig.calibration_residue`, image `9d4bb69c...`)
answered whether full refreshes remove it. Eight captioned ramps, a minute
apart: a baseline; three ordinary `REFRESH_FULL`s; three "deep" events of two
extra `REFRESH_CLEAR`s back to back before the full refresh (nine clear cycles
instead of three -- what a deeper `REFRESH_FULL` would do); a closing one.
Photographed at each step, the shift probe's "ppm" stayed legible on bars 3-4
on **every one of them**: through ten full refreshes and six extra clears, 48
clear cycles in all. The deep events made it fainter, not gone.

**It does fade.** A second, identical run straight after brought the total
since the probe to 18 full refreshes and 12 extra clears -- 30 whole-panel
drives over about 20 minutes -- and at its end the imprint could only be made
out from a few centimetres. It is not permanent. What the runs cannot say is
whether drives or elapsed time erased it, because both grew together; and the
ratio is sobering either way: 40 extreme partial refreshes took on the order
of 30 whole-panel drives to undo.

Two further observations from the same run explain what kind of residue this
is:

- **It is invisible at saturation.** While `epd_clear()` flashes the panel
  between black and white, nothing shows; the imprint returns the moment
  GC16 draws the greys. The residue is not an image left on the glass but a
  change in how those pixels respond to a grey waveform -- the signature of
  stored charge from unbalanced drives offsetting the effective voltage.
  Saturated black and white are driven far enough to hide the offset;
  intermediate greys land on a different grey.
- **For the product that is mostly good news.** `screen_home` is black text on
  white, where the imprint does not show. It would show through grey
  elements (`VM_INK_SECONDARY` and the stale, warm-up and unavailable inks)
  drawn over the headline's rows.

**Decision: `REFRESH_FULL` is unchanged.** Tripling its clear depth -- six more
cycles at about 0.82 s each -- would take every full refresh from 3.7 s to
8.6 s of flashing for an improvement that was visible but small, and ordinary
drives erase the imprint anyway, given enough of them. Recorded, not adopted.
What remains open is the rate: whether, in steady use -- one full refresh per
ten partial ones, concentrated on the headline -- erasure keeps pace with
accumulation. Separating time from drives is the next measurement: run the
shift-probe image to the end, leave the board powered off for hours, then
flash the residue image and judge only its baseline ramp. `docs/hardware/board_notes.md` carries it as an open item.

**A separate artifact the ramps exposed: vertical stripes in white.** The
lightest bar (level 15) showed faint vertical stripes running its full height,
exactly at the columns of its own "15" label, and no other bar did. That is
EPDiy's highlevel update, not residue. After `epd_clear()` the model is white
and GC16 drives only dirty rows and dirty columns -- and the column mask is
global (`epd_populate_line_mask()`): a column is driven in every dirty row
if any pixel in it changed. In bar 15 only the label's pixels changed, so only
the label's columns were driven, in every row, because the grey bars beside
it make every row dirty. And in this waveform GC16's white-to-white
transition is not a no-op but a full flash -- 15 passes towards black, 15
back (`epdiy_ED047TC1.h`, mode 2); GL16's is empty. White after a flash and
white after a clear differ slightly, hence the stripes. In every other bar
every pixel changed, every column was driven, and the treatment is uniform.
The candidate fix -- draw the post-clear image with GL16, whose
white-to-white is empty -- changes how every full refresh renders greys, so
it is an open item to evaluate, not part of this phase.

## The minimum full-refresh interval stays a guard

`min_full_refresh_interval_ms` = 60 s is unchanged, and now says what it is at
its definition: a rate limit on the flashing refresh, not a measurement. No
stopwatch measures how often a panel "tolerates" a full refresh, the vendor
gives no figure, and nothing observed here argues for another value.

## Task context

The calibration ran on `app_main`'s task (core 0, priority 1); the product
refreshes from the event loop's task (core 1, priority 5). The clear, which
EPDiy runs in the calling task, took 2,456 ms in the first and about 2,322 ms
in the second (the device run's own log, 10 ms resolution, twice). Partial
refreshes, which EPDiy renders on its own tasks, show no such difference. The
model keeps the calibrated figure; re-measuring the clear from the product's
task is an open item.

## What the simulator still gets wrong, by how much

- Full refresh in the product: about +4 %, all of it the clear's task context.
- Partial refreshes: within about 1 % of the panel, product frames included.
- Ghost outlines: appear at exactly 11 undriven partial refreshes and never
  deepen. The panel's onset was observed as a range (11-25), and whether the
  outlines deepen beyond 40 refreshes was not observed.
- Residue surviving full refreshes (visible in greys only): not modelled.
- The white-on-white column stripes of a GC16 full refresh: not modelled.
- Temperature: one waveform range, one ambient (21 C). A cold room is a
  re-measurement, not an extrapolation.

## Confirmed on the device

The normal firmware built from `sdkconfig.defaults` alone (image `65fc3d36...`)
reported `max_partial_refreshes_before_full=10` at boot and, over 23 minutes of
bring-up data, ran exactly ten partial refreshes between full ones, the
eleventh redraw coming out `action=full ... partials=10 forced_full=true` at
668 s and again at 1,336 s, about 11 minutes apart at its one-redraw-a-minute
pace. No deferral, no forced full refresh from the panel's side.

## Consequences

- `epd_emulation.h` and `board_config.h` agree on both panel limits, and
  `tests/unit/test_panel_limits_agree.c` fails the host build when they do
  not -- the two used to be kept equal by a comment.
- `REFRESH_FAST_MONO` maps to EPDiy's `MODE_DU` in
  `platform/esp32_t5s3/adp_display.c`, so that it could be measured. DU
  drives only towards black (0x0) or white (0xF); a caller must draw nothing
  else in the area. Nothing in the product requests it.
- The calibration mode stays, off by default. Re-measuring -- a second unit,
  a cold room, a newer EPDiy -- is a build with `sdkconfig.calibration` (and
  `sdkconfig.calibration_ghost_shift` or `sdkconfig.calibration_residue` for
  the probe or the residue test alone) and a flash. It needs an 8 KB main-task
  stack, which its overlay sets and its source enforces with `#error`: the
  first image overflowed the default 3.5 KB on its first flush.
- `CONFIG_HAC_EPD_TEST_PATTERN` now defaults to off: the calibration run
  answered, for this unit, the question the boot test pattern exists to ask.
