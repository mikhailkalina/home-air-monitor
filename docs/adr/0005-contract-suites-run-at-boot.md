# 5. Contract suites run once at device boot, not through idf.py -T

Date: 2026-09-06

## Status

Accepted. Superseded in part when phase 2d adds the test app below.

## Context

CLAUDE.md requires that "every new port needs a suite in `tests/contract/`
that runs against all of its implementations, on the host and on the
device." `docs/architecture.md` §9.2 describes the on-device half of that as
an ESP-IDF unit-test app run through `idf.py -T`: a separate build target
using the Unity test runner, flashed and executed on the target, reporting
pass/fail back over the console.

Phase 2a adds two new ports, `port_clock` and `port_log`, and their adapters
(`platform/esp32_t5s3/adp_clock.c`, `adp_log.c`). Two things are true at the
same time:

- There is no `idf.py -T` scaffold in this repository yet. Building one from
  nothing is a real piece of work: a second ESP-IDF project under
  `apps/firmware_esp32/test_apps/`, Unity integration, its own partition
  table and build configuration.
- Nothing in phase 2a needs it particularly badly. `port_clock` and
  `port_log` are both small, both have exactly one device-side
  implementation apiece, and neither depends on `port_i2c`, which is the
  port `docs/architecture.md` §5.4 identifies as the one that actually
  requires exercising real hardware behaviour (bus timing, NACK handling)
  that only a proper test app, not a boot-time smoke check, can cover well.

Building the full test-app infrastructure now, for two small ports, ahead of
the port that will actually stress it, front-loads cost onto the phase least
able to justify it.

## Decision

**The contract bodies run once, at device boot, inside `app_main()`.**
`tests/contract/port_clock_contract.c` and `port_log_contract.c` are written
as free bodies with no dependency on `stdio`, `malloc`, or the host test
harness — they report through a `contract_report_fn` callback and return a
failure count. `cmake/sources_contract.cmake` compiles that exact source,
unmodified, into both `tests/contract/test_port_clock_contract.c` /
`test_port_log_contract.c` (host CTest runners) and
`apps/firmware_esp32/components/hac_contract/` (the device build). `main.c`
supplies the third runner: it calls both `*_contract_run()` functions against
the real `adp_clock` and `adp_log`, and reports each check and the final
tally through `port_log`, into the same boot log the bring-up run is already
being read from.

**The proper `idf.py -T` test app is deferred to phase 2d**, where
`port_i2c` and its drivers arrive. `docs/hardware/board_notes.md` records
this as an open item rather than leaving it as a silent gap.

## Consequences

- Real on-device coverage exists for both ports from phase 2a onward, with no
  new build target and no change to the flash/monitor workflow the phase-2a
  plan already specifies: one `idf.py flash`, one `idf.py monitor`, one log
  to read.
- The contract body itself is the thing under version control on both sides.
  A suite that existed as two hand-written copies — one for CTest, one for
  the device — could drift apart silently; compiling the same `.c` file into
  both makes that impossible rather than merely discouraged.
- This is a narrower guarantee than a real `idf.py -T` app provides. It runs
  once per boot rather than on demand, it cannot be invoked in isolation from
  the rest of `app_main()`, and it reports through `port_log` rather than
  through a structured test-runner protocol a CI job could parse. For two
  small, single-implementation ports with no bus timing to exercise, that
  trade is acceptable; it would not be for `port_i2c`.
- When phase 2d builds the real test app, `hac_contract`'s sources do not
  change — `cmake/sources_contract.cmake` already isolates them from both
  runners. What changes is which runner phase 2a's boot-time call in
  `main.c` gets replaced by, and whether it is worth keeping the boot-time
  run alongside the proper one as a cheap continuous smoke check. That is a
  phase-2d decision, not one this ADR makes for it.
