# 9. The on-target test app: one ESP-IDF project, `idf.py -T`, the firmware's own components

Date: 2026-10-13

## Status

Accepted. Supersedes the part of ADR 0005 that deferred the test app to
phase 2d; ADR 0005's boot-time runner stays, for the reason below.

## Context

`docs/architecture.md` §9.2 has every port's contract suite run on the device
as well as on the host, through an ESP-IDF unit-test app built with
`idf.py -T`. ADR 0005 put that off: in phase 2a there were two small ports and
no `port_i2c`, so the contract bodies ran once at boot from `app_main()`
instead, and the test app was left to phase 2d.

Phase 2c needed it sooner. Its brief asked for the `port_clock` suite to run
on the device "and produce the same result as on the host" -- a claim the
boot-time runner can only make by someone reading a log beside a CTest run.

Two facts about ESP-IDF 5.5 shaped what was built:

- **`-T` is not a feature of `idf.py`.** It exists only in
  `tools/unit-test-app/idf_ext.py`, an extension `idf.py` loads from the
  project directory it is run in. Outside that one app the flag is unknown.
  What *is* general is the build-system half: `tools/cmake/project.cmake`
  adds `<component>/test/` as an extra component whenever the
  `TEST_COMPONENTS`, `TEST_EXCLUDE_COMPONENTS` or `TESTS_ALL` cache variables
  are set, and links those test components with `--whole-archive` so Unity's
  self-registering `TEST_CASE`s survive.
- **The board flashes from another machine.** USB passthrough to WSL does not
  work here (CLAUDE.md, "Environment"): images are built in WSL and flashed
  from Windows, and only the log comes back. An interactive Unity menu would
  need a second round trip per run.

## Decision

**One test app, `apps/firmware_esp32/test_app/`, a second ESP-IDF project
beside the firmware.** It links the firmware's own components through
`EXTRA_COMPONENT_DIRS = ../components` -- not copies -- and chooses which
components' tests to include with `-T`:

```bash
cd apps/firmware_esp32/test_app
idf.py -T hac_platform build
```

- **Its own `idf_ext.py` provides `-T`/`-E`**, reduced from what the
  unit-test-app's does to the two options: they set the cache variables and
  nothing else. No `ut-build-*` per-config actions, which this project does
  not use.
- **Test cases live with the host runners, registered by a thin `test/`
  directory.** `components/hac_platform/test/CMakeLists.txt` registers
  `tests/contract/esp32/*.c`, so everything under `components/` stays a thin
  wrapper and the device runners sit next to the host ones they mirror. The
  contract bodies are still compiled once, from `cmake/sources_contract.cmake`
  through `hac_contract`, exactly as ADR 0005 set up.
- **It is configured from the firmware's `sdkconfig.defaults` first and its
  own second**, so the adapters are tested under the firmware's tick rate,
  console and partition table. The `delay_ms()` contract in particular is a
  statement about one tick rate (ADR 0006); a test app on IDF's default
  configuration would test a different clock. Its own overlay changes only the
  partition-table path (`../partitions.csv` -- the firmware's table, same NVS
  partition for a future `port_storage` contract) and enables Unity's
  backtrace on failure.
- **Every linked test runs once at boot, no menu**, ending in one greppable
  line: `HAC_TEST_APP_RESULT tests=<n> failures=<n> ignored=<n> OK|FAIL`. An
  image with no test cases reports `FAIL`: it almost always means `-T` was
  forgotten, and a run that tested nothing must not read as a pass.
- **Host and device print the same line per check**, through
  `tests/contract/contract_print.h`:
  `contract <port> <implementation> <check> PASS|FAIL`. "Same result as on
  the host" becomes a `diff` of those lines with the implementation column
  dropped.
- **The boot-time runner stays.** It costs about 15 ms per boot and runs on
  every normal firmware start, where nobody flashes the test app. The two
  answer different questions: the test app, "does this adapter satisfy its
  contract", on demand; the boot run, "did this particular image break it",
  every time.

## Consequences

- **Verified on hardware, 2026-10-13.** `idf.py -T hac_platform build`
  produced an image (`hac_test_app.bin`) whose run printed
  `2 Tests 0 Failures 0 Ignored`, `HAC_TEST_APP_RESULT tests=2 failures=0
  ignored=0 OK`. Its 7 `port_clock` and 10 `port_log` check lines were
  diffed against the host's `fake_clock`, `adp_clock_virtual` and
  `adp_log_console` runs with the implementation column removed: identical,
  all PASS, including `clock.delay_ms(N).advances_by_at_least_n` at the
  firmware's 100 Hz tick.
- **Not verified: the `FAIL` paths.** No image with a failing contract, and
  none built without `-T`, has been flashed. Both are short code paths in
  `test_app_main.c`, but neither has been seen to fire.
- **The test app fetches its own copy of EPDiy** (through `hac_platform`'s
  manifest) into `test_app/managed_components/`, which `.gitignore` covers
  with an anchored pattern beside the firmware's.
- **`-T` values persist in `CMakeCache.txt`**, as in the unit-test-app: a
  later `idf.py build` without `-T` keeps the previous selection.
- **`port_i2c` (phase 3) lands in a ready runner**: a new
  `tests/contract/esp32/test_port_i2c_contract.c`, one line in
  `hac_platform/test/CMakeLists.txt`, and the bus-timing and NACK checks
  ADR 0005 said a boot-time smoke check could not carry.
