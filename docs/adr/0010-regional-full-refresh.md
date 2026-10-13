# 10. A full refresh of part of the panel: tried, measured, rejected

Date: 2026-10-13

## Status

**Rejected.** Implemented on branch `experiment/regional-full-refresh`
(not merged), measured and photographed on the panel, and not adopted:
`REFRESH_FULL` stays a whole-panel refresh. This record is what the
experiment found, so that the question is not reopened without it.

## Context

Phase 2c set `max_partial_refreshes_before_full` to 10 (ADR 0008), so every
tenth redraw is a full refresh: 3.3-3.7 s, about 2.5 s of it the whole panel
flashing black and white -- every two minutes in the meeting-spike scenario.
Nearly all of those pixels never changed. The product's partial refreshes
touch a band of 50-250 rows: the CO2 headline, its level banner, the grid
values below.

EPDiy can clear and redraw a rectangle: `epd_clear_area()` makes the clear's
66 pushes skip the rows outside it and mask its columns
(`output_i2s/render_i2s.c`, `epd_push_pixels_i2s()`), and GC16 through the
highlevel API only diffs inside the area it is given.

## What was tried

- **Keeping the budget's guarantee with one counter.** `update_policy` kept
  the union of every partial refresh's rectangle since the last full one. A
  budget-forced full refresh covered that union plus the rectangle the
  current redraw changed -- every pixel with partial history, so no pixel
  went through more than ten partial refreshes without a full one -- and the
  policy refused to reset the budget if the caller flushed less. The policy
  learned nothing about the layout.
- **`port_display`**: `REFRESH_FULL` and `REFRESH_CLEAR` honoured `area`;
  adapters could widen it, never narrow it.
- **The EPDiy adapter** widened to whole bytes, ran `epd_clear_area()`,
  marked only that part of its model white, and ran GC16 over the same
  rectangle.
- **The simulator** honoured areas, flashed only the area, and checked that
  every full refresh covered all partial history.

All of it passed its host tests; the simulator showed full refreshes of
0.8-1.6 s over 49-228 rows and the panel's hourly refresh time falling from
208 s to 130 s.

## What the panel showed

**The timing held.** A clear of part of the panel costs a fixed 502 ms plus
3.607 ms per row its pushes write (the rectangle's rows, up to two no-op rows
either side, one latch row), measured with `sdkconfig.calibration_regional`:

| Clear | Written rows | Measured | Fit |
|---|---|---|---|
| top 960 x 60 | 63 | 729.6 ms | 729.6 |
| top 960 x 228 | 231 | 1,335.5 ms | 1,335.6 |
| band 908 x 77 at y 110 | 82 | 796.4 ms | 798.1 |
| whole panel | 541 | 2,455.9 ms (ADR 0008) | 2,453.8 |

A full refresh of the CO2 headline's band took 1.17 s against 3.3 s for the
whole panel, and only the band flashed. The pre-measurement estimate of the
fixed part (257 ms) had been half the real figure.

**What it looked like did not.** Two artifacts, both from how this panel and
waveform behave rather than from a bug:

1. **Grey patches and surviving outlines inside the refreshed area.** After
   twenty partial refreshes flipping the headline between three and four
   digits, a full refresh of just their rectangle left the columns between
   the unit and the step counter visibly grey, and earlier digits' outlines
   still showing, while the columns holding content came out clean. The
   cause is the same as ADR 0008's white-on-white stripes: after the clear,
   GC16 drives only dirty columns, and in this waveform GC16's white-to-white
   transition is a full flash -- 15 passes towards black, 15 back -- that
   cleans the white pixels it reaches. In a whole-panel refresh nearly every
   column is dirty, because `screen_home`'s rules run from x 24 to 936, so
   nearly every white pixel gets that extra cleaning. In a narrow band, the
   empty columns get only the clear, and what the partial refreshes left in
   them stays.
2. **The rectangle shows.** Freshly cleared white is whiter than white that
   has stood since the last full refresh. Every regional refresh leaves a
   box on the screen until the next whole-panel one -- visible at the edge
   of each cleared band in the calibration run's photographs.

The first could be fixed: making GC16 flash every pixel of the area. The
second cannot be, on this panel: only an occasional whole-panel refresh would
level the whites again, and that brings back the flashing this was meant to
remove.

## Decision

Rejected. `REFRESH_FULL` stays a whole-panel refresh, with the clear and the
whole-panel GC16 pass that makes it clean, and the budget of ten stands. A
box on the screen after every tenth redraw is worse than a 3.3 s flash every
tenth redraw.

## Consequences

- **A finding that outlives the experiment:** in a whole-panel full refresh,
  GC16's white-to-white flash in dirty columns is doing real cleaning work,
  not just costing time. ADR 0008's candidate fix for the white-on-white
  stripes -- drawing the post-clear image with GL16, whose white-to-white
  transition is empty -- would take that cleaning away from every white
  pixel. `docs/hardware/board_notes.md` now says so beside that candidate.
- **What would reopen this:** a panel or waveform whose cleared white
  matches its standing white; or a product decision that a visible box
  between whole-panel refreshes is acceptable in exchange for shorter,
  localised flashes -- in which case the experiment branch is the starting
  point, plus a GC16 pass that drives every pixel of the area.
- **The measured cost of a regional clear** (502 ms + 3.607 ms per written
  row) stays here for whoever needs `REFRESH_CLEAR` of a rectangle later; the
  port still describes full and clear refreshes as whole-panel.
