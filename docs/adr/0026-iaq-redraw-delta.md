# 26. IAQ redraws by a delta, like the other readings

Date: 2026-10-20

## Status

Accepted. Supersedes ADR 0021's "`update_policy` redraws when the band
changes". Verified on the host: unit tests at the boundaries, a CLI test over
a synthetic scenario, and the owner's recordings of 2026-10-18 and 19 replayed
through the simulator before and after. A first image of this branch, with a
different rule (below), ran on the board for 43 minutes; this rule is not yet
seen there. The day unattended it is to be checked against is still to run
(board_notes.md, open item "BSEC on the board").

## Context

ADR 0021 put IAQ's band under the number and redrew the panel whenever the
band changed, judged on the rounded value; a number moving within its band
waited for a redraw anything else caused. BSEC scales its index so that the
room's typical good air is 50, and 50 is also the upper bound of "Excellent".
The second BSEC run (2026-10-16) already showed the index crossing it back and
forth; the owner chose to watch before changing anything (board_notes.md, open
item "BSEC on the board").

Three days unattended (2026-10-17 to 20, ELF `f63cfdf60...`) settled it. From
the card's `log/` (Vilnius days):

| | 2026-10-18 | 2026-10-19 |
|---|---|---|
| redraws | 921 | 1,568 |
| of them `reason=iaq` | 884 | 1,494 |
| full refreshes | 44 | 74 |
| late BSEC calls | 167 | 292 |

Every partial refresh holds the event loop that calls BSEC (ADR 0020,
"Timing"), so the flapping cost late calls too, not only panel wear and full
refreshes.

From the card's `data/` -- BSEC's samples every 3 s, analysed locally and
**not in this repository** (the owner's home air, a public repository):

- **Band changes**: 908 and 1,527 a day, 850 and 1,480 of them across 50.
- **The index rarely rests in "Excellent"**: at or below 50 (rounded) for
  3.3 % and 8.3 % of the day, never for longer than 57 s and 147 s at a
  time; exactly 50.00 in 0.6 % and 1.7 % of the samples. At rest it sits at
  51-53 and dips to 50 for a sample or two (4 s median) about once a minute
  -- not "exactly 50.000" as the second run's minute samples suggested.
- **The noise**: 425 and 740 excursions above 50 a day, 30 s long (median),
  their peaks at 51.5 (median), 54.8 (p75), 56.6 (p90), 57.1-57.6 (p95), and
  58.1 at most from a resting index. About three quarters stay within 5 of 50.
- **Real events** -- cooking, people in the room, the evenings -- peak at 65.8
  and above, mostly 70-80, twice 110 and 179; the evening of the 18th held
  the index above 50 for six and a half hours, up to 179.

## Decision

**IAQ redraws as CO2, temperature and humidity do: when its number has moved
by strictly more than its delta since the frame on the glass** --
`update_policy_config_t.iaq_delta`, `UPDATE_POLICY_DEFAULT_IAQ_DELTA`, **5
points**; reason `iaq`. The band plays no part in the trigger. One rule for
every reading, one line in `due_reason()`.

- **Each frame stays consistent**: the band drawn is the band of the number
  drawn -- `air_quality_iaq_level()` is unchanged -- so ADR 0021's "the number
  and its band never disagree" holds on the glass. What the glass may show is
  a number up to 5 points from the air, and with it the band of that number,
  as any reading may be within its delta.
- **The trembling at 50 no longer redraws**: about three quarters of its
  excursions stay within 5. The rest cost a redraw up and one back.
- **Status changes are untouched**: a number gained or lost, and the accuracy
  mark appearing or going (`measurement_low_accuracy()`), redraw at once
  whatever the index did. So do the first frame and a person's request
  (ADR 0025).
- **5 is the owner's choice** (2026-10-20): it keeps the number on the glass
  within 5 of the air, a tenth of the resting index.

### How it got here: band and delta, then delta alone

The first version of this decision kept ADR 0021's band and added the delta
on top: a change of band redrew only once the index had also moved by more
than the delta. With 10 it cut the replayed IAQ redraws by 98 %, and a first
image ran on the board with it (below). The owner found 10 too much on the
glass, and then asked for one rule for all readings: two conditions where the
others have one, and a number free to wander within its band -- "61 Good" on
the glass while the air reads 95 -- with the panel never told. The table
compares them, on the two days (replayed through the simulator, below; and at
the board's 3 s rate, an estimate on the IAQ alone):

| rule | `reason=iaq`, replay | full refreshes, replay | IAQ redraws a day, board's rate | band on the glass differs from the reading's | number on the glass more than 10 from the air |
|---|---|---|---|---|---|
| band (ADR 0021) | 544 / 959 | 28 / 49 | 738 / 1,225 | 2.2 % / 3.8 % | 11.6 % / 2.3 % |
| band and delta 10 | 12 / 16 | 3 / 4 | 40 / 53 | 5.8 % / 24.3 % | 11.2 % / 3.0 % |
| band and delta 5 | 20 / 132 | 3 / 10 | 186 / 289 | 4.5 % / 19.5 % | 11.0 % / 2.3 % |
| **delta 5** | **215 / 253** | **11 / 14** | **280 / 243** | **4.5 % / 10.4 %** | **0.02 % / 0.02 %** |

Delta alone redraws more than band and delta: it follows the number through
the long events, a redraw every 5 points, where the band rule left the number
behind. It is short of the 90 % cut this change set out to reach: -60 % and
-74 % of the IAQ redraws in the replay, an estimated -68 % and -84 % at the
board's rate against its actual 884 and 1,494. In return the glass is the
truest of the four.

### Rejected: a sticky band in the domain

Hysteresis inside `air_quality_iaq_level()` -- the band leaves "Excellent"
only above 55 and comes back only below 45 -- would stop the flapping too, but
the band would then depend on the path, not the number: the screen could read
"52 Excellent" beside a "52 Good" a moment earlier, which is ADR 0021's
promise broken on the glass. And at this boundary it would hardly ever come
back: the index went below 45 for 36 s in the two days.

### Rejected: a dwell time

"A new band only after it has held for N seconds" would separate the dips
(the longest 147 s) from real stays, but it delays a real event by the same N,
and the goal is the next sensor cycle. It also needs the policy to remember
readings between evaluations, which it does not.

## Consequences

**Replayed through the simulator** -- `--recording` with each day's two
files, headless, `--duration 86400`, `main` before and this rule after. The
simulator's "before" is 586 and 1,010 redraws against the board's 921 and
1,568: it reads the BME68x's recording every 5 s, interpolated, not at BSEC's
3 s, and so sees fewer of the noise's peaks.

| | 2026-10-18 before | after | 2026-10-19 before | after |
|---|---|---|---|---|
| redraws | 586 | 230 | 1,010 | 291 |
| `reason=iaq` | 544 | 215 (-60 %) | 959 | 253 (-74 %) |
| partial refreshes | 558 | 219 | 961 | 277 |
| full refreshes | 28 | 11 | 49 | 14 |

At the board's rate an estimated 280 and 243 IAQ redraws a day on days like
these, and with the other readings' some 15 full refreshes and 55-60 late BSEC
calls a day (one late call per five or six redraws on the 18th and 19th),
against 44-74 and 167-292.

**Real events reach the glass in the cycle they pass the delta.** In the
replay every move of the index past a band's boundary by more than 10 -- 16
and 22 of them, in 15 and 22 events peaking above 65 -- had the glass showing
the new band within 1 s of the reading, as before.

**The band at rest.** With the index resting at 51-53 and dipping to 50, the
word under the number says which side of 50 the last frame caught: "Good"
most of the time, as the index is. After an event the glass often stays on
its last frame a few points above the resting index -- a frame in "Good" --
until the next redraw (in the replay, a frame in "Excellent" came a median 34
and 18 minutes after the index first dipped to 50 again). The band on the
glass disagrees with the reading's 4.5 % and 10.4 % of the time, against 2.2 %
and 3.8 % before.

**Is 50 the right boundary?** Not decided here: the bands are the owner's
(ADR 0021). With this room's index resting at 51-53, the "Excellent" band is
reached only in dips. Moving the boundary above the resting range (55 or 60),
or naming 0-60 one band, would make the word at rest stable; it would also
change what Bosch's classification says. BSEC's other anchor, 200, is also a
boundary; the breaths of 2026-10-16 peaked at 200-206.

**The simulator** reads an `iaq` column from `--scenario-env` files, so a
scenario can play IAQ as the BSEC firmware's BME68x does (without one the
cell still shows gas resistance), and its `[policy]` lines carry the virtual
clock, `now_ms=`, as the firmware's decision lines do -- which the replay's
timings above came from. `scenarios/iaq_flapping_at_50.csv` is synthetic,
generated with a fixed seed from the statistics above, never from the owner's
rows: 40 minutes of an index crossing 50 about once a minute -- within 5 of it
for the first 600 s, then twice past it -- with something in the room at 900
s and a breath at 1500 s. One loop makes 63 IAQ redraws with ADR 0021's rule,
15 of them in the first 600 s; 49 with this one, none there -- the rest follow
the two events five points at a time -- and both events on the glass 5 s after
they begin (`tests/cli/simulator_iaq_flapping.cmake`).

**Seen on the board with the first version** -- band and delta 10,
2026-10-20 (ELF `12ef228cf...`, the BSEC image, 43 minutes from 21:09 local,
the owner's YAT log): 41 redraws, none for IAQ -- 26 for CO2 through a breath
on the sensors at 21:14 (IAQ to 143, carried onto the glass by the CO2
redraws, which are judged first), 6 for temperature, 6 status (BSEC's accuracy
crossing low/medium); 2 full refreshes, the first frame's and one at the
budget in the breath; 6 late BSEC calls. At rest the minute samples read
49.5-51.9.

**Seen on the board with the delta of 5** -- a night from the card's `log/`
(ELF `ec5d8a60f...`, one boot, 2026-10-21 22:04Z to 2026-10-22 07:27Z, 9.4
hours): 73 redraws, 53 of them for IAQ (3 full), 9 temperature, 5 `settle`
(ADR 0028), 4 CO2, 2 status, none for pressure (ADR 0027); 5 late BSEC calls.
Quiet for seven hours -- 11 redraws -- then 51 in the two hours the room woke.
Scaled to a day: about 190 redraws, 135 for IAQ, 8 full refreshes and 13 late
calls, against 921-1,568, 884-1,494, 44-74 and 167-292 on 2026-10-18 and 19,
and under the estimate above of 243-280 IAQ redraws a day.

**On the board, still to see**: a day unattended with people in the room, by
the same counts; and how the number reads following an evening's event.

Out of scope: BSEC's timing (ADR 0020) and the CO2 bands (requirements 13.1).
