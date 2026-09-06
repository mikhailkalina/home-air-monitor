# 6. `adp_clock`'s `delay_ms()` rounds up by a full extra tick, not to the next tick

Date: 2026-09-06

## Status

Accepted.

## Context

`tests/contract/port_clock_contract.c`'s
`clock.delay_ms(N).advances_by_at_least_n` check passed on the host's
virtual clock and failed on the ESP32-S3. The host clock advances its own
time by exactly the requested amount when asked (`docs/architecture.md`
§5.2); the device clock asks FreeRTOS's scheduler to sleep instead, and that
turned out to under-deliver in two independent ways, not one.

`platform/esp32_t5s3/adp_clock.c`'s `clk_delay_ms()` converted milliseconds
to ticks with `pdMS_TO_TICKS(ms)`, which **rounds down**. At the default
`CONFIG_FREERTOS_HZ` of 100, one tick is 10 ms. The contract's own probe,
`delay_ms(15)`, asks for 1.5 ticks; `pdMS_TO_TICKS()` truncates that to 1,
and `vTaskDelay(1)` sleeps for (at most) one tick -- 10 ms, not 15. Diagnostic
logging added to `clk_delay_ms()` (requested `ms` and the actual elapsed
`esp_timer` microseconds, at `LOG_LEVEL_DEBUG`) is what a real boot would
show confirming this exact shortfall; the arithmetic that produces it is
otherwise fully determined by `pdMS_TO_TICKS()`'s documented truncating
definition, so this is a proof rather than a guess this codebase happens to
agree with.

That truncation is the obvious bug, and rounding the tick count up instead of
down looks like the whole fix. It is not. `vTaskDelay(N)` computes a target
tick count as `(the tick count already current when the call is made) + N`,
and unblocks once the tick counter reaches it. The tick count already current
at the moment of the call can be anywhere within the tick period already
under way -- from just started to just about to roll over -- and that
partial tick is never counted towards the N ticks requested. So
`vTaskDelay(N)` only guarantees **more than `(N-1)` tick periods**, not `N`:
up to one whole tick's worth of real time is "free" to the scheduler and lost
to the caller, in the worst case. This is not specific to small `ms`; it
applies at every tick count. It is just most visible at `delay_ms(1)`, where
rounding up alone still produces `vTaskDelay(1)`, and `vTaskDelay(1)`'s
guaranteed minimum is `(1-1) * tick period = 0` -- no floor at all. Ticks
alone cannot honour a 1 ms request at 100 Hz, however the conversion is
rounded.

Three ways to close this were considered:

1. **Round up to the next whole tick, and stop there.** Fixes the truncation
   half of the bug, not the scheduling-granularity half. Still fails the
   contract at small `ms` (worst case, none of `vTaskDelay`'s wait is
   real), and, more subtly, at every `ms`: the same up-to-one-tick shortfall
   applies regardless of magnitude, just as a shrinking fraction of the
   total as `ms` grows. Rejected as incomplete, not merely imprecise.
2. **Round up, then add one more tick unconditionally.** Turns the
   guaranteed minimum from `(ticks - 1) * tick period` into
   `ceil(ms / tick period) * tick period`, which is never less than `ms` by
   construction, for every `ms` including `delay_ms(1)`. Costs up to one
   full tick period (10 ms at 100 Hz) of over-sleep on every tick-based call
   -- negligible relative to `ms` for the multi-hundred-millisecond and
   longer delays this codebase actually uses `delay_ms()` for (sensor
   warm-up, retry backoff), and irrelevant for anything shorter, since the
   existing sub-tick path (below) never reaches this branch at all.
3. **Raise `CONFIG_FREERTOS_HZ`.** A global, systemic change: every task's
   scheduling granularity shrinks, the tick ISR fires more often (a small
   but permanent power cost, working against the battery-mode duty-cycling
   `docs/requirements.md` §11 and §17 already care about), and it only
   shrinks the same worst-case shortfall proportionally -- it does not
   eliminate it. `delay_ms(1)` at 1000 Hz has the identical problem
   `delay_ms(1)` at 100 Hz has today, just with a 1 ms tick instead of a
   10 ms one. Rejected: a global, permanent cost paid to narrow, not close,
   a gap local to one function.

## Decision

**Round up to whole ticks, then add one more tick unconditionally**, for any
`ms` that reaches the tick-based path at all. The existing sub-tick path is
unchanged: when `pdMS_TO_TICKS(ms) == 0` (shorter than one tick period, the
same check the code already had), `esp_rom_delay_us(ms * 1000)` busy-waits
the exact microsecond count instead -- precise, and untouched by any of the
above, since it never calls `vTaskDelay` at all.

`clk_delay_ms()` now also logs `requested_ms` and the actual elapsed
`esp_timer` microseconds at `LOG_LEVEL_DEBUG` on every call, through a
`port_log_t*` `adp_clock_init()` now takes (borrowed, `NULL`-safe, matching
`adp_display_null_init()`'s existing pattern). This is what will confirm the
fix, and any future regression in it, directly from a boot log rather than
from re-deriving the arithmetic.

`ports/port_clock.h`'s `delay_ms` doc comment now states the "at least `ms`,
for every `ms`" contract explicitly, rather than leaving it implicit in the
contract test alone -- a future port implementation should not have to
rediscover this the way this one did.

## Consequences

- `clock.delay_ms(N).advances_by_at_least_n` holds for every `N`, not only
  for multiples of the tick period, because the guaranteed minimum
  (`ceil(ms / tick period) * tick period`) is derived to never be less than
  `ms`, independent of `CONFIG_FREERTOS_HZ`'s value.
- Every tick-based `delay_ms()` call now over-sleeps by up to one tick
  period (10 ms at the current 100 Hz). No caller in this codebase currently
  depends on tight delay precision; if one ever does, that caller should use
  `port_clock`'s `now_ms()` to poll instead of relying on `delay_ms()` for
  precision it was never specified to have.
- `adp_clock_init()`'s signature changed to take a `port_log_t*`. The only
  caller, `apps/firmware_esp32/main/main.c`, already constructs its logger
  before its clock, so this was a one-line change at the call site.
- The host's virtual clock (`platform/host/adp_clock_virtual.c`) is
  untouched: it was never subject to this bug, since it advances its own
  recorded time by exactly the requested amount rather than asking an OS
  scheduler to sleep.
