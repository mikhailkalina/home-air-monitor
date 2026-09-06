# 4. A due full refresh is deferred, never downgraded — and the panel enforces it too

Date: 2026-09-06

## Status

Accepted.

## Context

The vendor of the ED047TC1 states that the panel must not be partially
refreshed for an extended period: doing so leaves residual images and causes
irreversible damage (`docs/hardware/board_notes.md`). `port_display` therefore
reports two numbers up to the core — `max_partial_refreshes_before_full` and
`min_full_refresh_interval_ms` — and `core/app/update_policy.c` decides against
them.

Those two constraints can contradict each other. The partial-refresh budget
runs out, so the next redraw has to be full; but the minimum interval since the
last full refresh has not elapsed, so a full refresh is not allowed yet. Something
has to give, and the choice is not obvious:

- **Downgrade**: perform the redraw as a partial refresh anyway. The screen
  stays current, and the user sees fresh numbers.
- **Defer**: perform no redraw at all, and wait until a full refresh is
  permitted. The screen shows stale numbers for up to
  `min_full_refresh_interval_ms`.

There is a second, separate question: what should happen if some future caller
— a new screen, a bug, a phase that forgets to consult the policy — issues
partial refreshes past the budget anyway? The policy is not the only thing that
can call `port_display->flush()`.

## Decision

**The policy defers.** When a full refresh is due but the panel's minimum
interval has not elapsed, `update_policy_evaluate()` returns `UPDATE_NONE` with
`deferred_by_min_interval` set and `next_deadline_ms` at the point the full
refresh becomes legal. It never returns `UPDATE_PARTIAL` in that situation.

**The emulated panel enforces the budget independently.** `epd_model_flush()`
turns a partial refresh into a full one when the budget is spent, marks it
`forced_full`, and logs it explicitly — in the headless adapter as well as the
windowed one, so CI sees it too.

## Consequences

- Downgrading would be precisely the behaviour the vendor warns about: it
  reaches for one more partial refresh at exactly the moment the panel has had
  too many. Staleness is recoverable and is already visible on screen — the
  home screen carries an "updated N ago" line and a stale mark per quantity
  (`docs/requirements.md` §9.2). Panel damage is not recoverable. Between a
  screen that is briefly out of date and a screen with a permanent ghost burned
  into it, the trade is not close.
- A deferral is reported rather than hidden. `deferred_by_min_interval` and
  `reason` together say what was wanted and what stopped it, and the simulator
  logs it once per episode. A policy that defers constantly is a policy whose
  thresholds are wrong, and that has to be visible.
- The two enforcement points are not redundant, because they answer different
  questions. The policy asks "what should we do?"; the panel asks "what will I
  survive?". In a correct run the panel's branch is never taken — the log line
  `FORCED FULL REFRESH` appearing at all is a defect report about the caller,
  not a feature of the panel. That is why it is loud, why it names the vendor
  warning, and why the end-of-run summary counts it.
- `max_partial_refreshes_before_full == 0` means the panel offers no usable
  partial mode, so every refresh is full. Reading 0 as "unlimited" would be the
  more natural C convention and is exactly wrong here: an unconfigured or
  unknown panel must fail towards the safe behaviour, which costs power rather
  than hardware. `update_policy_limits_from_display(NULL)` returns 0 for the
  same reason.
- The core still holds no constant belonging to any particular panel. Both
  numbers arrive through `port_display`; a second panel reports different ones
  and `update_policy.c` is unchanged. The simulator's values live in
  `platform/host/epd_emulation.h` and are unmeasured estimates until phase 2c.
- The budget itself — 20 partial refreshes — is a guess. The vendor warns but
  gives no number. If phase 2c measures a much larger figure, the deferral case
  becomes rare and this decision costs almost nothing; if it measures a much
  smaller one, the decision is what keeps the panel alive. Either way the
  measurement changes one constant, not this logic.
