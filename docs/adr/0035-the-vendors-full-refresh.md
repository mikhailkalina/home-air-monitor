# 35. The vendor's full refresh: its clear, then a draw from white

Date: 2026-10-22

## Status

Accepted, at the owner's request after comparing the two on the board.
Supersedes ADR 0008's "A full refresh is `epd_clear()` plus a
content-dependent GC16 pass" and its full-refresh and clear timings, which
now describe the sequence this replaced. Verified on the host: the simulator's
panel model and window, its unit tests; the firmware builds.

**Timed on the board, 2026-10-22** (ELF `6c4244ce7...`, the BSEC image, the
owner's YAT logs of 20:19 and 22:20 local, two hours and 24 full refreshes --
22 changes of screen, the boot's, one the budget forced): the clear
1,213-1,220 ms (estimated 1,280; now the simulator's figure), EPDiy's draw
after it 432-456 ms with its difference pass 107-120 ms (the model's 448-455
for the same frames), a change of screen on the glass 1,842-1,901 ms after the
finger's lift against 3,376-3,411 ms with EPDiy's clear and GC16 the same
evening (ELF `89905d8a5...`). What the glass shows -- the flashes, any trace,
the stripes, the greys -- is the owner's eye's, not the log's (board_notes.md,
open item "The vendor's full refresh").

**Amended the same evening** (below, "Amendment: GL16 after the clear"): the
draw is `MODE_GL16`, not the vendor's `WHITE_TO_GL16` -- its greys did not
survive the partial refreshes after it. The clear stays.

## Context

The owner put LILYGO's own firmware and this one side by side on the panel,
2026-10-22. LILYGO's full refresh took about 2 s, flashed black and white
eight times quickly, and left no trace of the screen before. Ours took 3.2-3.7 s
(ADR 0008), flashed six times slowly, then drew the picture through a further
black phase, and is known to leave residue in greys (ADR 0008, "Residue that
survives a full refresh") and faint stripes in white (board_notes.md). Since
ADR 0034 every change of screen is a full refresh, so its length and its
quality are what a person tapping sees.

**What LILYGO's firmware does**, read from `Xinyuan-LilyGO/LilyGo-EPD47`,
branch `esp32s3`, commit `50224221fb81a8fcf9068532374b775af84a38d4`
(2026-09-07). Its examples refresh fully as `epd_poweron()`, `epd_clear()`,
`epd_draw_grayscale_image(epd_full_screen(), framebuffer)`, `epd_poweroff()`
(`examples/touch/touch.ino:137-164`, `wifi_sync`, `ha_calendar` alike):

- `epd_clear()` is `epd_clear_area_cycles(area, 4, 50)`
  (`src/epd_driver.c:218-248`): four cycles, each pushing the whole panel
  towards black 4 times and then towards white 4 times, every row driven for
  50 us -- 32 pushes, 8 flashes.
- `epd_draw_grayscale_image()` is `epd_draw_image(..., BLACK_ON_WHITE)`
  (`:721`, `:816`): fifteen frames that only darken, a pixel of level L
  darkened in the first 15 - L of them, with rows driven for
  `{3, 3, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5, 10, 20, 30}` us
  (`contrast_cycles_4`, `:110`).

**What we did**: EPDiy's `epd_clear()` -- 3 cycles of 10 pushes towards black,
10 towards white and 2 neutral, rows driven for 12 us: 66 pushes, 2.46 s --
then `MODE_GC16` from a white model. Read out of EPDiy's waveform for this
panel (`src/waveforms/epdiy_ED047TC1.h`), GC16 takes every pixel it draws
through 15 phases towards black first and then lightens it to its level --
white ones included, in any column holding ink -- which is the black phase
after the clear and the stripes.

**The two drivers are one lineage.** LILYGO's driver is an early EPDiy, and
this project's EPDiy fork drives this board through the same row-by-row path
(`output_i2s/render_i2s.c`): `epd_push_pixels(area, time, color)` drives each
row for `time` microseconds in both. And EPDiy ships the vendor's draw as a
mode: `MODE_EPDIY_WHITE_TO_GL16`, whose fifteen phases and their times are
exactly `contrast_cycles_4` and which only darkens from white. Nothing of the
vendor's has to be brought in.

## Decision

**`REFRESH_FULL` is the vendor's sequence, through EPDiy**
(`platform/esp32_t5s3/adp_display.c`):

- **The clear**: `clear_panel()`, a loop over EPDiy's public
  `epd_push_pixels()` -- `BOARD_EPD_CLEAR_CYCLES` (4) rounds of
  `BOARD_EPD_CLEAR_PUSHES_PER_PHASE` (4) pushes towards black and as many
  towards white, rows driven for `BOARD_EPD_CLEAR_ROW_US` (50). EPDiy's
  `epd_clear_area_cycles()` fixes ten pushes a phase inside it, so it cannot
  express four. `REFRESH_CLEAR` is the same clear. It logs `adp_display:
  panel cleared took_ms=`.
- **The draw**: `MODE_EPDIY_WHITE_TO_GL16` over the whole panel from the
  white model, instead of `MODE_GC16`. It drives nothing that stays white.

The numbers are facts about the panel's drive, taken from the vendor's
driver; its code is GPL-3.0 and none of it is in this repository (ADR 0007:
a source of hardware facts, never a dependency). `board_config.h` cites the
commit and lines.

Partial refreshes are unchanged: `MODE_GL16` over the changed rows.

## Consequences

- **A full refresh of about 1.8 s instead of 3.2-3.7 s**, measured above;
  the simulator models it (`platform/host/epd_emulation.h`) with the measured
  clear and a draw at half of GL16's measured base and per-row rate (15
  passes against 30, every row under the data bus's ~53 us floor in both),
  which the board confirmed within 4 %. The owner's stopwatch said "about 2
  s" for the vendor's. A change of screen (ADR 0034) costs that, not 3.4 s,
  and holds the event loop -- and BSEC's call -- for half as long: BSEC's
  calls at a change of screen came 0.24-1.73 s late, against 1.62-3.30 s
  with the old full refresh.
- **No black phase after the flashes, no white-to-white flash**: the stripes
  in white (board_notes.md, "Vertical stripes in white after a full refresh")
  have no cause left. ADR 0010 warned that GC16's white flash was doing real
  cleaning; the vendor's clear drives each row 1,600 us in all against
  EPDiy's 720, and is what the owner saw leave no trace.
- **Greys come by another path.** `WHITE_TO_GL16` darkens a grey from white;
  GL16, which every partial refresh still uses, and GC16, under which ADR
  0013's lightness table was measured by eye, take it through black and
  lighten it. The same level may not look the same: a grey drawn by a full
  refresh beside one a partial refresh redrew -- the footer's "updated
  HH:MM", whose changed digits a partial redraws -- may differ, and the
  slideshow's and photo frame's pictures, all full refreshes, are dithered
  for a table measured under GC16. Black and white are unaffected (black is
  all fifteen darkening phases in both, 102 us). The board's to show
  (board_notes.md, open items "The vendor's full refresh" and "How light each
  grey level really is"); if the greys disagree, `MODE_GL16` in place of
  `WHITE_TO_GL16` after the same clear keeps the clear's gain and GL16's
  greys, about 0.5 s slower -- one line in `adp_display.c`.
- **The clear runs in the event loop's task**, as EPDiy's did: its pushes are
  synchronous calls (ADR 0008, "Task context").
- **The simulator**: the window flashes four times black and four times white,
  then goes white while EPDiy works out the difference, then the picture
  darkens in -- GC16's black phase and its row and column masks are gone from
  the model. ADR 0008's measured clear (2,455,869 us) and full refreshes
  (3,660,569 and 3,324,863 us) left the model's one-percent test; the model
  is held instead to the estimate's own formula, and to coming out well under
  the old measurements.
- **To replace the estimates**: `panel cleared took_ms=` and EPDiy's own
  timing line for the draw, from any boot's first frame, into
  `EPD_SIM_CLEAR_REFRESH_MS` and `EPD_SIM_FULL_DRAW_*`; the calibration image
  (`sdkconfig.calibration`, ADR 0008) times `clear`, `full_black`,
  `full_home` and `full_ramp` through the same port.

## Amendment: GL16 after the clear (2026-10-22, the same evening)

**What the board showed.** The owner, on the image timed above: no trace of
the screen before after a full refresh -- the clear does what it was taken
for -- but after a partial refresh, lighter patches in static greys it had not
redrawn: the rules between the fields beside a changed digit, the footer's
"updated". ADR 0035 had named this risk ("Greys come by another path").

**Why.** In EPDiy's waveform for this panel GL16's entry for an unchanged grey
L -> L is not "nothing": it takes the pixel towards black for up to 14 phases
and lightens it back for as many (`epdiy_ED047TC1.h`, mode 5; black and white
get nothing). EPDiy applies it to every unchanged pixel of every row it
writes, within its global column mask -- so a partial refresh re-drives the
static greys around a changed digit to GL16's level. Drawn by GL16 or GC16 --
towards black, then lightened -- those greys come back where they were. Drawn
by `WHITE_TO_GL16` -- darkened from white -- they were darker than GL16's, and
the re-drive lightened them, patch by patch, wherever a partial reached.

**Decision.** After the vendor's clear a full refresh draws with `MODE_GL16`
from the white model. GL16 from white drives nothing that stays white -- no
white-to-white flash, so still no stripes -- and takes every grey the way
every partial refresh does. ADR 0013's lightness table, measured under GC16's
identical path, holds again for the slideshow and the photo frame. The
`REFRESH_PARTIAL` comment in `adp_display.c` no longer says "only the changed
pixels are driven".

**What it costs.** GL16's 30 passes instead of 15: the draw about 900 ms
instead of 440 -- GC16's, measured on the board the same evening at 897-910
ms for the home screen, costs the same -- so a full refresh of about 2.2 s and
a change of screen about 2.3 s, against 1.85 s with the vendor's draw and
3.4 s before ADR 0035. And the glass shows it: after the eight flashes, what
is to be grey or black goes black for the first half of the draw and then
lightens to its level -- for black text, nothing to see. The simulator models
it from measurements only: the measured clear, and GL16's measured formula
over the frame's rows, held within 1 % of ADR 0008's GC16 draws put behind the
new clear.

