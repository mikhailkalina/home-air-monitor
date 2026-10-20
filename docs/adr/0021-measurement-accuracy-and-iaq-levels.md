# 21. A measurement's accuracy beside its status, IAQ's bands on the screen, and the CO2 unit at a fixed place

Date: 2026-10-16

## Status

Accepted. Verified on the host: unit tests, the layout test, and golden
frames looked at before they were adopted. Built into the BSEC firmware
(ELF `a3fca4872...`); not yet seen on the panel.

Amended by ADR 0023 (2026-10-16): two of the footer's notes are shorter, to
fit beside the dated "updated DD.MM HH:MM" -- "N not calibrated" for
several, and "Temp." for the temperature's label in the note. "IAQ not
calibrated" is unchanged.

Amended by ADR 0026 (2026-10-20): IAQ no longer redraws when its band
changes, but when its number moves by more than 5 points, as the other
readings do. BSEC's index rests on the Excellent/Good boundary, and on the
board it crossed it some 900-1,500 times a day -- 96 % of all redraws.

## Context

The first run of BSEC on the board (ADR 0020, 2026-10-16) showed IAQ at
BSEC's accuracy 1 -- "low", its scale not yet trimmed against the room's air
-- for the whole 21 minutes of the log, and the screen showing the number as
plainly as any other. ADR 0020 had mapped accuracy 1 to `VAL_OK` and kept
the accuracy itself in the log only, because `value_status_t` had nowhere to
put it. The owner asked, the same day:

- for IAQ's bands on the screen: 0-50 excellent, 51-100 good, 101-150
  lightly polluted, 151-200 moderately polluted, above 200 poor;
- for BSEC's accuracy on the screen, either by widening the statuses beyond
  `VAL_WARMUP` or by an optional, possibly sensor-specific, parameter on each
  measurement -- 0 shown as the warm-up it already is, 1 as "the index is
  computed, its scale not yet fitted to the room", 2 and 3 alike as
  "calibrating or calibrated";
- and for the CO2 reading's unit to move where the reading cannot draw over
  it: "ppm" followed the number, so a reading going from three digits to four
  drew over the place the letters had been and moved them, and the partial
  refreshes left them on the panel (board_notes.md, open item "Residue that
  survives full refreshes", which saw exactly that in phase 2c).

## Decision

### Accuracy is a field beside the status, on a scale of the core's own

`measurement_f32_t` gains `value_accuracy_t accuracy`: `ACC_NOT_REPORTED`
(the zero value, so every measurement built without one says nothing),
`ACC_UNRELIABLE`, `ACC_LOW`, `ACC_MEDIUM`, `ACC_HIGH`.

Not a widening of `value_status_t`. The status answers "is there a usable
number" and everything is built on that answer: the sensor manager's merge
ranks by it, ageing turns `VAL_OK` into `VAL_STALE`, the footer counts faults
by it, and MQTT will publish availability from it. Accuracy is a different
question with an independent answer -- a number can be stale and fully
calibrated, or fresh and not yet calibrated. A "low accuracy" status would
need a place in the merge's ranking, a rule for ageing (does it go stale, and
to what?) and a case in every switch on the status, and it would make "stale
and uncalibrated" impossible to say. A field travels through all of that
untouched: the merge takes it with the value it chose, ageing leaves it
alone. And it costs nothing: the struct stays 24 bytes on both targets, the
padding after `source_id` absorbs it.

A scale of the core's own, not a sensor-specific number. The core knows no
sensor's numbering, as it knows no source ids (ADR 0017): BSEC's 0-3 is
mapped onto the scale in the driver (`bme68x_bsec_iaq_accuracy()`, tested in
CI), and another source with a verdict of its own -- an SCD41's
self-calibration state, one day -- maps its own. A sensor-specific parameter
would have carried BSEC's meaning up into the view.

What the screen does with it, through `measurement_low_accuracy()` --
`ACC_UNRELIABLE` or `ACC_LOW`:

- a `VAL_OK` number with low accuracy keeps its black ink and gets a
  **question mark** where an OK number has no mark; any other status keeps its
  own mark, which says more;
- the footer says **"IAQ not calibrated"** (the field's label; "N readings
  not calibrated" for several) when there is no error, warm-up or stale data
  to report first;
- `ACC_MEDIUM`, `ACC_HIGH` and `ACC_NOT_REPORTED` look the same: the owner
  grouped 2 and 3, and a source with no verdict is not suspect;
- accuracy 0 stays a warm-up, the hourglass, as ADR 0020 has it.

`update_policy` redraws when the question mark would appear or go -- as a
status change -- and not when BSEC moves between medium and high, which on
the host it did every few minutes for the same picture.

### IAQ's bands, in the domain

`core/domain/air_quality.h`, where architecture §3 put the IAQ thresholds:
the owner's five bands, the upper bound of each inclusive. They follow the
classification Bosch gives for its IAQ, merged above 200; the classification
itself is not in the BSEC archive, which says only that 50 is the room's
typical good air and 200 its typical polluted air. A band is judged on the
number rounded as the screen rounds it, so "50" is never "Good" and "51"
never "Excellent" (a test sweeps 0-300 in steps of 0.05 against the text the
view model produces).

On the screen the band goes under the IAQ number, where the other cells have
their unit, in English -- the fonts are ASCII only -- and in a word that fits
the cell at 32px beside the question mark, 172 px: "Excellent", "Good",
"Fair", "Moderate", "Poor". "Fair" stands for "lightly polluted": Bosch's own
words are 249 px wide, and "fair" is the step between good and moderate in the
European air quality index.

`update_policy` redraws when the band changes (reason `iaq`); a number
moving within its band waits for the next redraw anything else causes. That
settles ADR 0020's open question in the form the screen needs: a band on the
glass that no longer matches the air would be worse than none, a number a few
points behind is not. (Superseded by ADR 0026: IAQ redraws when its number
moves by more than 5 points, and the band drawn is the band of the number
drawn.)

The CO2 bands of requirements 13.1 stay in `view_model.h`, where only the
view uses them.

### The CO2 unit and mark at fixed places

"CO2 ppm" and the CO2 mark now share the label's line above the reading, at
40px with the mark at 32px; nothing in that line depends on the reading, so a
change of reading drives none of its pixels. The layout test holds the
property itself: no pixel of the label, the unit or any mark lies where any
reading -- 400 to 40000, "ERR", "warm", "n/a" -- puts ink, and the line is
pixel-identical for every OK reading. It fails on the old layout.

## Consequences

- **Every golden frame changed** (the headline), and IAQ's band appears in
  two of them; a sixth frame shows IAQ not calibrated. Each was looked at
  before adoption.
- **The reading may grow wider** than "40000" at 160px would allow beside a
  unit; nothing needs it now.
- **The simulator shows no question mark**: a replay CSV has no column for
  accuracy. Add `iaq_accuracy` when a scenario needs it.
- **MQTT (phase 4)** will want the accuracy in its payload; it is in the
  reading for that.
- **The merge ranks by status alone.** With one source of IAQ that is moot;
  if two sources ever rate the same quantity, whether the better-rated should
  win among equal statuses is a question for then.
- **For the board** (board_notes.md, open item "BSEC on the board"): the
  question mark and the footer while accuracy is low, both gone at medium;
  and whether residue still gathers on "ppm" now that it never moves.
