# Home Air Monitor

Firmware for a home air quality monitor (CO₂, temperature, humidity, pressure, VOC/IAQ)
built on the LILYGO T5 4.7" e-Paper S3 (ESP32-S3-WROOM-1-N16R8, ED047TC1 panel,
960×540 landscape, 16 grey levels).

The codebase follows ports and adapters: the business logic builds both for the ESP32 and
natively for a PC, where the peripherals are replaced by a simulator.

## Key documents

@docs/requirements.md
@docs/architecture.md
@docs/hardware/board_notes.md

`docs/architecture.md` is the source of truth for repository layout, layering and port
interfaces. Consult it before creating new files or introducing new abstractions.

`docs/hardware/board_notes.md` is the source of truth for anything physical: board
revision, framebuffer orientation, GRAY4 packing, pin assignments. Facts recorded there
are settled — use them rather than deciding again. Decisions with lasting consequences
live in `docs/adr/`.

## Layering rules (a violation is a build failure)

- `core/` includes **only** `ports/*.h` and a subset of libc (`stdint`, `stdbool`, `string`, `math`).
  Forbidden: `stdio.h`, `time.h`, `malloc`/`free`, `esp_*`, `freertos/*`, `SDL*`.
- `ports/` is headers only — no `.c` files, no dependencies.
- `platform/<name>/` implements the ports and contains no business logic.
- `drivers/` depend only on `port_i2c.h` and `port_clock.h`, so they build on a PC.
- `apps/<name>/main.c` is the only place where dependencies are wired together.
- The core never reads time from the system: only `port_clock->now_ms()`. This is what makes it testable.
- No panel-specific or platform-specific constant may appear in `core/`. Panel limits reach
  the core as values reported by `port_display`, never as literals.
- Create a new port only when there are **two** real implementations (hardware plus simulator).
  Otherwise write a plain function.

If a task appears to require changing `core/` in order to make a platform work, stop and say
so rather than working around it. That is a sign the port abstraction is wrong, and it is
worth more than the task.

## Commands

```bash
# Static checks (no compiler needed — fastest signal, run this first)
./scripts/check_sources.sh

# Native build and tests
cmake --preset host-debug && cmake --build build/host-debug
ctest --test-dir build/host-debug --output-on-failure

# Build with sanitizers
cmake --preset host-asan && cmake --build build/host-asan && ctest --test-dir build/host-asan

# Run the simulator
./build/host-debug/apps/simulator/simulator \
    --scenario-co2 scenarios/co2_spike_meeting.csv \
    --scenario-env scenarios/normal_day.csv --time-scale 60

# Headless run (CI path; --duration is required or it never stops)
./build/host-debug/apps/simulator/simulator --headless --frames out/ \
    --scenario-co2 scenarios/co2_spike_meeting.csv \
    --scenario-env scenarios/normal_day.csv --time-scale 60 --duration 600

# Firmware
cd apps/firmware_esp32 && idf.py build
```

`idf.py monitor` never exits on its own. Use `idf.py flash && timeout 20 idf.py monitor`,
or ask me to flash and paste the log.

## Never commit

- `sdkconfig` — it holds Wi-Fi SSID, password and broker address after `menuconfig`.
  Only `sdkconfig.defaults`, without credentials, belongs in the repository.
- Any credentials, tokens, certificates or keys. Provide `credentials.h.example` instead.
- The BSEC/BSEC2 binary (`libalgobsec.a`). It ships under a proprietary Bosch agreement and
  must be downloaded separately into the gitignored `third_party/bsec/`.
- Build output, `managed_components/`, `dependencies.lock`, simulator frame dumps.

This repository is public, so anything pushed stays in the git history permanently.
If a secret is ever committed, say so immediately rather than removing it in a follow-up commit.

## Working rules

- A new core file goes into `cmake/sources_core.cmake` and **nowhere else** — that list is shared
  by the host and ESP-IDF builds. Never duplicate source lists in
  `apps/firmware_esp32/components/*/CMakeLists.txt`.
- Every new port needs a suite in `tests/contract/` that runs against all of its implementations,
  on the host and on the device.
- Pure functions (thresholds, MQTT payloads, HA Discovery, parsers, view models) get unit tests
  before being wired into the rest of the system. Cover the boundaries, not just the happy path.
- Screen changes are verified by golden tests in `tests/golden/`: raw framebuffer bytes compared
  against reference `.bin` files, each with a matching PNG so the diff is reviewable. Regenerate
  with `UPDATE_GOLDEN=1` only after confirming the change is intended.
- Values that are estimates rather than measurements must say so at their definition, naming the
  branch that will replace them. Keep them together in one place, never scattered as literals.
- Adding or removing a dependency requires updating `THIRD_PARTY_NOTICES.md` in the same commit.
  Check the new dependency's license first — copyleft components need explicit discussion.
- New files under `scripts/` must be committed with the executable bit set:
  `git update-index --chmod=+x <path>`. A plain `chmod` alone does not reach git.
- Before finishing a task, run `./scripts/check_sources.sh` and `ctest`; if `core/` or `drivers/`
  changed, also run `idf.py build`. A red CI is not the place to discover a missing SPDX header
  or a platform include that leaked into the core.
- Report honestly what could not be verified in this environment and why, rather than assuming
  it passes.
- Record significant architectural decisions in `docs/adr/NNNN-<title>.md` (context, decision,
  consequences). Anything a future reader would otherwise mistake for an arbitrary choice
  belongs there.
- Work happens on a branch per phase and lands through a pull request. `main` stays green.

## Style

- C11 for `ports/`, `core/`, `drivers/` and `platform/`. C++17 is allowed only in
  `platform/host/sim_gui/`.
- Every source and header file starts with `// SPDX-License-Identifier: Apache-2.0`
  (or the block-comment form where line comments are inappropriate). Vendored third-party
  code under `*/vendor/` and `*/third_party/` keeps its original headers untouched.
- `-Wall -Wextra -Werror -Wconversion`. Formatting is `.clang-format`, enforced in CI.
- Errors use `hal_status_t`; `esp_err_t` is mapped inside the adapter and never reaches the core.
- No magic numbers outside `core/config/`, `platform/*/board_config.h` and
  `platform/host/epd_emulation.h`.
- All code, comments, documentation and commit messages are in English.

## Current phase

Phase 1c complete: the simulator renders `screen_home` in a 960×540 window and headless,
with e-paper emulation and `update_policy` driving refreshes.

Next: phase 2a (board bring-up) — `board_config.h`, `adp_clock`, `adp_log`, a null display
adapter, and the FreeRTOS pump, running on real hardware with no panel involved.

The phase order is in section 10 of `docs/architecture.md`. Do not run ahead to later phases
unless asked. Note that phase numbers here refer to those branches; the "phase 2c" named in
`docs/hardware/board_notes.md` and in the e-paper emulation constants is the panel
calibration branch.
