# 28. A small offset that persists for 15 minutes redraws

Date: 2026-10-20

## Status

Accepted. Verified on the host: unit tests at the boundaries, and the owner's
recordings of 2026-10-18 and 19 replayed through the simulator before and
after. Not yet seen on the board.

## Context

Every reading redraws when it has moved by more than its delta since the
frame on the glass (requirements 9.1, ADR 0026, ADR 0027). In still air that
leaves an offset up to the delta on the glass for as long as nothing else
redraws: CO2 up to 50 ppm, temperature 0.2 C, IAQ 5 points. The owner asked
for a redraw that removes such an offset in stable conditions -- "a number of
minutes has passed and the data changed within some limits".

On the replayed days, a redraw every few minutes on average, a reading still
stayed more than half its delta from the glass for up to 20-24 minutes at a
time (CO2, temperature, humidity). In a quiet room it is unbounded.

A plain "N minutes since the last redraw, and anything differs" was weighed
and not taken: CO2 and IAQ differ in their last digit at every sample, and
temperature crosses a rounding boundary in its noise, so it would redraw
every N minutes -- an estimated +45-96 redraws a day on those days, a
periodic redraw of noise.

## Decision

A reading more than **half its delta** from the frame on the glass, for **15
minutes without a break**, redraws -- reason `settle`
(`UPDATE_POLICY_SETTLE_DELTA_FRACTION`, `update_policy_config_t.settle_after_ms`,
`UPDATE_POLICY_DEFAULT_SETTLE_AFTER_MS`). Half a delta is 25 ppm, 0.1 C,
0.5 %, 0.5 hPa and 2.5 IAQ points; "without a break" means one reading back
within it starts the 15 minutes over, so noise around the threshold does not
add up to a redraw. Both numbers are choices, the owner's.

- **It is not a timer.** It redraws only a picture that would change, and
  only once: the redraw takes the offset away. ADR 0023's "nothing on the
  screen changes with time alone" stands.
- **`update_policy_evaluate()` now records what it sees**: per reading with a
  delta, whether it is off by more than half the delta and since when. Nothing
  about the redraw itself -- the budget still moves only in
  `update_policy_commit()`, which also clears the offsets, the new frame being
  their new baseline. The same reading evaluated twice at the same time
  changes nothing. While an offset is held and nothing else is due, the
  decision's `next_deadline_ms` is when it will have held 15 minutes, so a
  loop with no new data still wakes for it.
- **Raw values, not what the screen shows**: the policy knows no display
  precision, which is the view's. So an offset of one digit on the glass with
  less than half a delta behind it -- pressure 1009.45 drawn as "1009", the air
  at 1009.55 -- is not redrawn. It is a tenth of a hPa; chasing it would
  chase every reading's noise at its rounding boundaries.

## Consequences

**Replayed** (the two days, `--recording`, headless, a day each; before is
the IAQ and pressure deltas of ADR 0026 and 0027):

| | 2026-10-18 before | after | 2026-10-19 before | after |
|---|---|---|---|---|
| redraws | 230 | 229 | 291 | 287 |
| of them `reason=settle` | | 3 | | 5 |
| full refreshes | 11 | 11 | 14 | 14 |
| longest stretch more than half a delta off: CO2 | 22 min | 15 min | 20 min | 15 min |
| ... temperature | 21 min | 15 min | 4 min | 4 min |
| ... humidity | 7 min | 5 min | 24 min | 15 min |

Fewer redraws in all, not more: a settle redraw moves the baseline, and some
later crossings of a delta do not happen. The share of the day with a reading
more than half a delta off barely moves (CO2 6.6 to 6.2 % and 13.5 to 13.1 %;
humidity on the 19th 4.6 to 6.5 %, the baselines falling elsewhere) -- on such
days the screen is redrawn often anyway. What the rule bounds is the quiet
room: no reading stays more than half a delta off the glass for longer than
15 minutes.

**On the board, still to see**: `reason=settle` decisions per day in the
card's `log/`, and the redraws in all, against the replay.
