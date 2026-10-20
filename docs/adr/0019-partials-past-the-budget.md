# 19. A partial-refresh budget of 20, and partial refreshes past it until a full refresh is allowed

Date: 2026-10-16

## Status

Accepted. Supersedes the decision of ADR 0004 ("the policy defers"), narrows
its second half ("the emulated panel enforces the budget"), and replaces the
budget ADR 0008 set (10). Both changes are the owner's, made watching the
board; verified on the host, not yet on the board.

Its periodic redraw is gone since ADR 0023 (2026-10-16): the footer says
when the values were measured, not how long ago, and nothing on the screen
changes with time alone.

**Amended by ADR 0025 (2026-10-17)**, as its last consequence below asked of
a faster redraw source: partial refreshes past the budget stop at the budget
again (`UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET`, 40 for this panel), after
which even a redraw a person asked for waits for the full refresh. At the
sensors' pace it never binds; a person tapping without pause would otherwise
pass the 40 this panel was tested with.

## Context

ADR 0004 met a conflict between the panel's two limits -- the partial-refresh
budget is spent, so the next redraw has to be full, but the minimum interval
since the last full refresh has not elapsed -- by deferring: no redraw at all
until a full one was allowed. On the board (phase 3c's first run, ELF
`ea261721f...`) that froze the screen for 10-34 s at a time while the owner
warmed the sensors by hand: `event_loop: deferred: panel minimum full-refresh
interval not elapsed ... next_deadline_in_ms=33628`. The budget of 10 ran out
in about 20 s, because the redraws came faster than anyone had planned for --
two sensors are read at different moments of each 5 s cycle (the SCD41 at
x.08 s, the BME68x at x.70 s in that log), and each can trigger its own
redraw.

The owner asked for three things: keep the readings moving with partial
refreshes while the full one waits; do not make a full refresh merely
because the minute is up, only when there is something to draw; and allow 20
partial refreshes between full ones, not 10. After the second board run, a
fourth: draw both sensors' readings of one cycle in one refresh; and a
fifth: no redraw on a timer while nothing on the screen changes.

What phase 2c measured on this panel (ADR 0008, "Ghosting, and the budget")
bounds what that costs:

- nothing visible from 50 cm through **40** partial refreshes in a row, in
  either probe pattern;
- close up, faint light outlines of earlier digits from the **11th** at the
  earliest (the worst realistic pattern, 11-25; the realistic sawtooth 25-30),
  and **every full refresh removed them** from the live screen.

ADR 0008 chose 10 -- one below the earliest close-up onset -- over 20 ("half
of what is invisible at 50 cm") by erring towards full refreshes. 20 is that
rejected alternative: it accepts the close-up outlines for half as many
flashing full refreshes. And the limit the overrun trades against is the
weaker of the panel's two: `min_full_refresh_interval_ms` is "a guard, not a
measurement" (`board_config.h`), a rate limit on the 3.7 s flashing refresh
rather than anything known about the glass.

## Decision

- **The budget is 20** (`BOARD_EPD_MAX_PARTIAL_REFRESHES_BEFORE_FULL`,
  `EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL`; the two still agree, and
  `test_panel_limits_agree` still holds them to it).
- **Past the budget, while a full refresh is not yet allowed, the policy
  refreshes partially** (`update_policy_evaluate()` returns `UPDATE_PARTIAL`
  with `partial_past_budget` set).
- **The full refresh comes with the next redraw that is due anyway** once the
  interval has elapsed: a change past a threshold, a status change, or the
  periodic redraw. Nothing is redrawn only because the interval elapsed.
- **The periodic redraw only while the data is getting older.** Its job is
  the footer's "updated N ago" (requirements 9.1's "time since last update
  > 2 min", 14.4's last update time). With fresh data -- any quantity shown
  `VAL_OK` -- the footer now says "updated just now" rather than a count of
  seconds the e-paper would keep showing, so a redraw would draw the same,
  and there is no periodic redraw at all: the screen changes when the
  readings do. Once nothing shown is fresh and something is stale, the age
  grows, and the periodic redraw keeps it counting every 120 s. With nothing
  carrying a number ("--") there is nothing to count either. Asked for by
  the owner after the second board run.
- **At most one redraw per sensor cycle** (`min_redraw_interval_ms`, 5 s by
  default): a redraw that comes due sooner after the last one waits for the
  cycle to end, and goes out with whatever else changed meanwhile
  (`held_for_redraw_interval`). The first frame is never held. Added at the
  owner's request after the second board run, so the two sensors' readings in
  one cycle cost one partial refresh, not two.
- **Deferral stays for one case**: a panel with no usable partial mode
  (`max_partial_refreshes_before_full == 0`) has nothing to keep the screen
  current with, so a full refresh blocked by the interval still waits
  (`deferred_by_min_interval`).
- **The emulated panel rescues past what was tested, not past the budget.**
  `epd_model` performs partial refreshes past the budget, counts them, marks
  them `(past budget)` in its log line, and turns a partial refresh into a
  full one only past `EPD_SIM_MOST_PARTIALS_TESTED` (40, the most this panel
  has been driven through without a full refresh) -- still reported as a
  defect of the caller. Its ghosting emulation shows outlines from the 11th
  partial refresh, as the glass did close up.

## Consequences

- **At the firmware's pace the budget is never passed.** With one redraw per
  5 s cycle and readings that move in every one, the budget of 20 is spent
  100 s after a full refresh -- past the 60 s interval -- and the 21st redraw,
  at 105 s, is full: **20 partial refreshes and at least 105 s between full
  ones** (`tests/unit/test_update_policy.c`,
  `at_the_firmwares_pace_the_budget_is_never_passed`). Past-the-budget
  partials remain for redraws faster than the cycle: without the limit, at a
  redraw every 2.5 s, they go to 23
  (`past_the_budget_is_bounded_by_the_interval`).
  With nothing to draw after the interval, the panel keeps the partial
  refreshes it has until the next change, however long that takes -- at the
  firmware's pace never more than the budget.
- **The price**, from ADR 0008's probes: close up, faint light outlines inside
  digits that stayed dark through more than ten partial refreshes, until the
  next full refresh removes them -- now in ordinary use, not only past the
  budget. Nothing from viewing distance; the most ever driven, 40, stays a
  ceiling the simulator enforces.
- **Seen on the board** (2026-10-16, ELF `9c8d96f20...`, before the
  one-per-cycle limit): no freeze -- the owner warmed the BME680 by hand, the
  temperature moved every 5 s for 75 s, and the 20 partial refreshes were
  followed by a full one at 152 s, the interval long elapsed, so nothing went
  past the budget. Still to see: how often a full refresh comes in ordinary
  use with the limit in place.
- **A faster redraw source must revisit this.** Touch or a menu redrawing more
  often than the sensors are read could take the count well past 23 within one
  interval. `epd_model`'s rescue at 40 would flag it in the simulator; on the
  device nothing would.
- **The redraws themselves** are what spends the budget: one per cycle at
  most now, and only when a reading crosses a threshold. ADR 0008's lever
  still stands: `update_policy`'s thresholds (`docs/requirements.md` 9.1) --
  the 0.2 C temperature step is the one the board's logs show firing most.
