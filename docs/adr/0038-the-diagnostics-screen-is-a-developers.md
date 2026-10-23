# 38. The diagnostics screen is a developer's: partial content, five minutes, the network live

Date: 2026-10-23

## Status

Accepted, at the owner's request after watching a Wi-Fi outage on the board.
Amends ADR 0031's and 0034's rule that the diagnostics network lines are as
of the drawing. Both changes of screen stay full, as ADR 0034 specifies.
Verified on the host. The owner confirmed the firmware works on the board
on 2026-10-23 after installing the BSEC build; no detailed log accompanied
that confirmation (`docs/hardware/board_notes.md`, open item "A change of
screen as a full refresh").

## Context

The owner switched the access point off twice while the diagnostics screen was
up (board_notes, open item "Wi-Fi on the board", 2026-10-23). The screen kept
saying `Wi-Fi up` and `MQTT online` for as long as it stayed up: its network
lines were drawn as of the visit, and only the network's name and telemetry
coming or going redrew them (ADR 0031, 0034). And a minute was too short to
watch a loss and a return on it, so every look meant a tap, and every tap a
full refresh's flash.

The diagnostics screen is for whoever is testing the device, not for the room:
what it says must stay live without repeated full-refresh flashes. The owner
clarified the same day: keep the full refresh on entry, and do not limit how
many partial refreshes its content may make.

## Decision

**Partial content, full screen changes.** While the diagnostics screen is up,
every content redraw is partial -- `update_policy_evaluate_partial_content()`.
Entering by tap is a full refresh at once, as before, and so is the way home
(by tap or timeout). The first frame is also full. Full refreshes start the
count over, so whatever the screen spent while it was up costs the home
screen nothing.

**No count limit for diagnostics content.** Neither the partial budget nor
`UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET` holds its content redraws or makes
them full. Past the budget they go on partially (`partial_past_budget`), even
past 40 refreshes on this panel and after the minimum full-refresh interval.
Content redraws still happen only when the view changes, held to one per
sensor cycle (`min_redraw_interval_ms`). There is no frozen state waiting for
a screen change. The home screen's budget and bound are unchanged. On a panel
with no partial mode, the normal full-refresh rules apply.

**Five minutes** (`SCREEN_CONTROLLER_DEFAULT_DIAGNOSTICS_TIMEOUT_MS`, a CHOICE,
60 s before): long enough to watch an outage's loss and return; a touch starts
it again, as before.

**The network live**: `diag_view_equal()` compares the Wi-Fi's state (`up`,
`joining`, `failed`, `off`) and its address, the MQTT session's state
(`online`, `offline`), the state messages sent, the time of the last and the
refusals, each with its trouble mark. Only the signal stays as of the
drawing: it moves by a dB or two at every read, every 10 s. `joining` covers
a whole outage, every attempt and the backoff between them, and the reporter's
`offline` too, so an outage costs a redraw at its loss and one to three at its
return; a session costs one redraw a minute or so, for `sent` and `last`. The
held link stays undrawn and uncompared (ADR 0034).

## Consequences

- **A full refresh on entry** clears the previous screen's outlines, as ADR
  0034 intended. Live content changes while diagnostics is up do not flash.
- **More partial refreshes per visit**: one a minute or so for the MQTT
  line's counts, and each change of a state. A long visit can pass the old
  bound without freezing the screen. Partial refreshes past the budget leave
  close-up outlines (ADR 0008, 0019); the full refresh home clears them.
- **BSEC late at times**, as at any partial refresh that starts near its call
  (ADR 0020, "Timing"): the screen's own redraws while it is up.
- **A screen up for five minutes** where it was up one: the room sees the
  readings later after a visit.
- **Tests**: `test_update_policy` (first frame and requests full, content
  partial for 100 consecutive redraws across the budget, former bound and
  minimum full-refresh interval; unchanged content idle; requests reset the
  count; a panel without partials full), `test_screen_controller` (both
  changes full with the count at 0), `test_diagnostics_view` (an outage's loss
  and return, a message sent, its time, an address, a refusal, a radio that
  will not start redraw; the signal and the bar's link do not),
  `simulator_link_flapping` (full entry, then one partial redraw at the loss,
  none between, one to three at the return, nothing skipped), `simulator_taps`
  (four full changes of screen; the timeout 300 s after the last tap).
