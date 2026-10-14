# 13. Grey levels: one measured lightness table, shared by the simulator and `prepare`

Date: 2026-10-14

## Status

Accepted. Levels 1-10 are measured by eye; levels 11-14 are estimates.
Confirmed on the panel 2026-10-14: photographs dithered in light towards the
table, at gamma 1.0 -- the photograph's own tones -- were judged clearly best,
over the old pipeline at its chosen 0.6 and the new one at 0.6, and kept
over 1.15, the gamma that comes closest to the old 0.6 on average. The
photo frame's pictures are prepared so.

## Context

The photo frame (`docs/adr/0012-image-pipeline.md`) looked overexposed on the
panel while its files were mid-dark -- mean level 6.7 of 15 -- and the
simulator showed them as intended. Both the simulator and
`image_gen.py prepare` assumed that level L looks like 8-bit sRGB grey L x 17,
which puts level 7 at 18 % of the way from black to white in light. Nobody had
measured it; ADR 0012 said so.

Four rounds of comparison on the panel settled on preparing the photographs
at gamma 0.6 -- one number standing in for a sixteen-entry table.

The table was then measured with a level-match chart
(`apps/slideshow/images/06_level_match.png`): each level beside fine mixes of
levels 0 and 15, read off where its patch disappears.
`docs/hardware/board_notes.md`, "How light each grey level really is", has
the readings. Levels 4-10 lie within 3 points of even steps in reflectance;
level 7 measures 50 %, not 18. The dark end is compressed: level 1 is black,
level 2 4.7 %, level 3 15.6 %. Levels 11-13 could not be matched -- sparse black
dots on white render darker than their share on e-paper, so the chart's mixes
are wrong at the light end -- and 14 matched only solid white.

## Decision

**One table, `EPD_SIM_LEVEL_LIGHTNESS_PERMILLE` in
`platform/host/epd_emulation.h`**: each level's lightness in permille from the
panel's black to its white, linear in light. Levels 11-14 are spaced evenly
between level 10 and white, as levels 4-10 are, and marked as estimates at the
definition.

**The simulator shows a level as light as the glass does.**
`epd_level_display_grey()` encodes the measured lightness as an sRGB grey, and
both the window and the headless frames use it. The panel's black is shown as
the screen's black: its contrast is not modelled. The golden-test PNGs keep
L x 17, because they document the framebuffer's levels, not the glass.
`EPD_SIM_DARKEST_MERGED_LEVEL` is gone: level 1 equals level 0 in the table,
which says the same thing as data.

**`prepare` dithers in light, towards that same array.** It reads the
array out of the header rather than keeping a copy, so the two cannot
disagree. It decodes a picture's greys as sRGB, dithers towards the measured
lightness, and carries the error at full precision -- rounded away, it would
lose faint shadows, where the step from level 0 to level 2 is 12 of the 255.
`--levels distinct` now leaves out any level that looks the same as a darker
one. `--gamma` keeps its meaning: it acts on the picture's greys before they
become light, and 1.0 now reproduces the photograph's own tones between the
panel's black and white.

**`demo` keeps the nominal greys.** Its pictures test the levels and the
dithering themselves, and ADR 0012's observations were made on them. `demo
--verify`, run by CTest, keeps the committed pictures what the generator
makes. `image_gen.py levels` prints the table `prepare` uses, and a CTest
proves it still parses.

## Consequences

- **The simulator shows the overexposure now.** A photograph prepared at
  gamma 1.0 under the old assumption looks on the PC as washed out as it did
  on the glass.
- **The chosen 0.6 does not carry over as one number.** Under the measured
  table, the old pipeline at 0.6 rendered a curve -- darker shadows, lighter
  middle -- that no single gamma in the new one reproduces: the closest, 1.13,
  differs by 15 of 255 in screen grey (RMS, weighted by the photographs'
  histogram). Which look is preferred was judged on the panel, three
  photographs at a time: the new pipeline at 1.0 won clearly over the old
  0.6 and over the new 0.6, which is far darker. The old 0.6 was a
  correction for the wrong model, and with the right one no correction is
  needed.
- **The sphere's rings have a candidate cause.** The demo sphere is dithered
  in 8-bit grey towards L x 17; on a panel whose steps are uneven in light,
  the rendered gradient changes slope at every level, which reads as bands.
  Carrying the error at full precision did not change its flat patches (4.4 %
  of the lit sphere before, 4.6 % after), so ADR 0012's other candidate is
  out. The comparison image also holds the sphere dithered in light towards
  the table: if its rings are gone, both the table and the explanation hold.
  **They are not gone** (judged on the panel, 2026-10-14): the sphere
  dithered in light still shows thin concentric rings along its shadow line,
  only further apart, and looks worse overall than the nominal one, which is
  lighter and was preferred. So the slope-change explanation is refuted, and
  the rings are where the shading crosses a level and the dither's texture
  changes -- fewer crossings in the shadow when dithering in light, whose
  dark levels lie far apart, hence wider spacing. The test says nothing for
  or against the table. **Confirmed by the next probe**: the nominal sphere
  with triangular noise of up to 8 greys added before dithering showed no
  bands at all. `prepare --noise GREYS` offers the same (off by default,
  seeded from the file name so a re-run is identical); it is not yet tried
  on photographs, whose own texture may already break the crossings up.
- **For photographs, their own tones are preferred.** Under the old
  assumption the verdict was a darkening correction, gamma 0.6; once the
  dithering is in light towards the measured table, the photograph's own
  tones won, with no correction at all. The synthetic sphere is a different question --
  it has no "right" tone -- and there the lighter nominal rendering was
  preferred.
- **Full-precision error changed `02_dither_methods` and `04_shaded_sphere`
  slightly**; they are regenerated.
- **The product's grey inks are lighter on the glass than they were chosen
  to be.** `core/ui/view_model.h` picked its levels under L x 17:
  `VM_INK_SECONDARY` is level 4, meant as a dark grey (6 % of white in that
  model) and 27 % on the panel; `VM_INK_STALE` (6) is 43 %, `VM_INK_WARMUP`
  (9) 59 %, `VM_INK_UNAVAILABLE` (11) about 72 %. That has been so on the
  panel since phase 2b; the simulator now shows it. The inks want choosing
  again against the window, which no longer flatters them -- a product
  decision for the UI's own phase, not this one.
- **Still estimates:** levels 11-14; the chart's own bias from fine
  black-and-white mixes, which may reach into levels 1-10 too; the panel's
  contrast, which the window does not show. All of it was measured on one
  unit, by eye, in room light. A RAW photograph of the solid ramp would
  replace the whole table, with nothing else to change.
