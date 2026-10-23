# 34. A change of screen is a full refresh; the diagnostics title loses the link's word

Date: 2026-10-22

## Status

Accepted, at the owner's request after watching the board. Supersedes ADR
0025's "A person's redraw" (a change of screen as a partial refresh, and the
bound past the budget set for it) and amends ADR 0031's "The held link is
drawn on it too". Verified on the host: unit tests of `update_policy`, the
screen controller and the diagnostics view, the layout test by pixels, two
golden frames regenerated and looked at, and two CLI tests. Not yet seen on
the board (`docs/hardware/board_notes.md`, open item "A change of screen as
a full refresh").

**Amended the same day by ADR 0035**: the full refresh itself is now the
vendor's, about 1.9 s by estimate rather than 3.3 s, so "about 3.3 s" below
reads about 2 s. **Amended 2026-10-23 by ADR 0038** for the diagnostics
screen: its content redraws are partial without a count limit; it stays up
five minutes and redraws for its network lines. Both changes of screen stay
full.

## Context

**The change of screen.** Since ADR 0025 a tap that changes the screen has
been a partial refresh: 1.07 s on the glass for the 920 x 500 rectangle, one
of the budget's twenty. GL16 drives only the pixels that change, so a whole
new screen drawn over the old one keeps the old one's outlines where dark
pixels stayed dark -- ADR 0008's ghosting, here over almost the whole panel
at once rather than in a headline's digits. The owner, on the board: the
picture reads badly after a change of screen.

**The diagnostics title.** ADR 0031 put the status bar's word for the link
-- `online`, `no broker`, `no Wi-Fi` -- in the diagnostics screen's title
line. It was not there to be read: the screen compared the held link to
decide whether to redraw, and a redraw due for something it did not draw
came out an empty diff, which the loops skip and retry -- every second in the
simulator until the screen timed out, and on the board a backoff that would
have held a tap's redraw too. Drawing the word made every such redraw change
the picture. But the MQTT line right below already says `online` or
`offline`, so the title said it twice.

## Decision

**A change of screen is a full refresh, always.** `update_policy_request()`
-- whose only caller is the screen controller, for a tap and for the
diagnostics screen's timeout -- now makes `UPDATE_FULL` due at once,
`reason=request`:

- **neither the budget nor the panel's minimum full-refresh interval holds
  it.** That interval (60 s, `BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS`) is a
  guard against the flashing refresh coming at the sensors' pace, not a
  measured limit: no figure says how often this panel tolerates a full
  refresh. A person's taps are paced by the refresh itself -- the event loop
  flushes synchronously, 3.2-3.7 s -- so a tap during one is handled after it
  (ADR 0025: 3.4-4.4 s from a lift inside a full refresh to the next screen).
  Waiting out the interval instead would have made going to the diagnostics
  screen and straight back cost up to a minute.
- **It resets the budget**, as any full refresh does: the screen a person
  comes back to starts with twenty partial refreshes in hand.
- **The bound past the budget stays** (`UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET`):
  set for taps, it now binds nothing at the sensors' pace (ADR 0019: 23 at
  most) and stays a bound for any redraw source faster than the sensor cycle.

**The diagnostics screen draws no link word, and does not compare the held
link.** Its network lines are as of the drawing, all of them; only the
network's name and telemetry coming or going redraw them
(`diag_view_equal()`). A held link that changes while the screen is up --
which can happen once at most: the screen goes home after a minute untouched,
and the hold is a minute -- redraws nothing there; the next visit draws the
lines as they are then. The rule ADR 0031 set stands: a screen redraws only
for what it draws.

## Consequences

- **A tap costs about 3.3 s to the new screen**, against 1.1 s -- the
  clear's ~2.4 s flash and the GC16 draw (ADR 0008) -- and the panel flashes
  at every change, the timeout's included. Predicted, not yet seen on the
  board.
- **BSEC's call is late at every change of screen**: every full refresh made
  one in the board's runs (ADR 0020, "Timing"; 0.47-1.63 s against 187 ms),
  where three of six partial changes did. Only while someone taps; whether
  it moves BSEC's accuracy is the board's to show, and so far 17 late calls
  in 21 minutes did not keep it from `high`.
- **More full refreshes, fewer budget-forced ones**: two per visit to the
  diagnostics screen, each starting the budget over. A room nobody touches
  is unchanged.
- **The panel's minimum interval no longer binds every full refresh.** It
  still holds the ones the budget forces; a person can now flash the panel
  as fast as it can flash. If a figure for the panel's tolerance ever turns
  up, a request held by it -- with the latency that costs -- is the change
  to weigh.
- **Tests**: `test_update_policy` (a request full a second after a full
  refresh, again and again; full past the budget, starting it over; the bound
  past the budget reached by a screen's own content, a request still full),
  `test_screen_controller` (both changes and the timeout's full),
  `test_diagnostics_view` and `test_screen_diagnostics_layout` (the held link
  changes neither the view's comparison nor a pixel), `simulator_taps` (four
  changes of screen, every one full, one of them 15 s after the last full
  refresh), `simulator_link_flapping` (the diagnostics screen up while the
  held link changes: no redraw, nothing skipped). Seen failing with the
  request's branch taken out: `test_update_policy`, `test_screen_controller`,
  `simulator_taps`.
- **Golden frames**: `screen_diagnostics_normal` and `_trouble` regenerated
  -- the only pixels changed are the word's, x 585-708 above the title's
  rule; `_no_telemetry` had no word and is unchanged.
