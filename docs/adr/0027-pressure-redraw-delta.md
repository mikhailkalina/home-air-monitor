# 27. Pressure redraws by a delta of 1 hPa

Date: 2026-10-20

## Status

Accepted. Verified on the host: a unit test at the boundaries, and the owner's
recordings of 2026-10-18 and 19 replayed through the simulator before and
after. Not yet seen on the board.

## Context

The home screen shows pressure in whole hPa, and `update_policy` had no
threshold for it: requirements 9.1 names CO2, temperature and humidity, and
ADR 0026 added IAQ. Pressure reached the glass only when something else
redrew. The weather moves it: 999.3-1008.0 hPa over 2026-10-18 and
1007.9-1012.6 over the 19th, on the board's BME680. Nothing bounded how far
the screen could fall behind if the other readings held still -- the quiet
room the owner asked about, where the screen should not keep a stale value.

## Decision

Pressure has a delta like the others (`update_policy_config_t.pressure_delta_hpa`,
`UPDATE_POLICY_DEFAULT_PRESSURE_DELTA_HPA`): a redraw, reason `pressure`, when
it has moved by strictly more than **1 hPa** since the frame on the glass --
one step of what the screen shows, as humidity's 1 %. The BME68x's noise is
far below it: about 0.01 hPa at rest, 0.2 hPa when the sensor was warmed by
hand (board_notes.md, "Phase 3c's measurements").

## Consequences

- **On the replayed days it never fires.** The other readings redrew every few
  minutes -- the longest gaps 53-86 minutes -- and pressure never fell more
  than 0.61 and 0.83 hPa behind the frame on the glass; redraws, refreshes
  and every other reason are unchanged. Alone, the delta would have redrawn 8
  and 4 times on those days. It is a bound for when nothing else moves, not a
  change on an ordinary day.
- **It does not remove a one-step offset at a rounding boundary.** The
  replayed 19th showed pressure one hPa off for up to 57 minutes with less
  than 1 hPa between the frame and the air -- 1009.45 drawn as 1009, the air
  at 1009.55. The same holds for every reading at its own delta. ADR 0028,
  the same day, redraws an offset of more than half a delta that holds for 15
  minutes -- 0.5 hPa here; a rounding boundary's tenth of a hPa stays.
