# Home Air Monitor

Firmware for a home air quality monitor (CO₂, temperature, humidity, pressure, VOC/IAQ)
built on the LILYGO T5-ePaper-S3 (ESP32-S3, ED047TC1 e-paper, 960×540, 16 grey levels).

The codebase follows ports and adapters: the business logic builds both for the ESP32 and
natively for a PC, where the peripherals are replaced by a simulator.

## Key documents

@docs/requirements.md
@docs/architecture.md

`docs/architecture.md` is the source of truth for repository layout, layering and port
interfaces. Consult it before creating new files or introducing new abstractions.

## Layering rules (a violation is a build failure)

- `core/` includes **only** `ports/*.h` and a subset of libc (`stdint`, `stdbool`, `string`, `math`).
  Forbidden: `stdio.h`, `time.h`, `malloc`/`free`, `esp_*`, `freertos/*`, `SDL*`.
- `ports/` is headers only — no `.c` files, no dependencies.
- `platform/<name>/` implements the ports and contains no business logic.
- `drivers/` depend only on `port_i2c.h` and `port_clock.h`, so they build on a PC.
- `apps/<name>/main.c` is the only place where dependencies are wired together.
- The core never reads time from the system: only `port_clock->now_ms()`. This is what makes it testable.
- Create a new port only when there are **two** real implementations (hardware plus simulator).
  Otherwise write a plain function.

## Commands

```bash
# Native build and tests
cmake --preset host-debug && cmake --build build/host-debug
ctest --test-dir build/host-debug --output-on-failure

# Build with sanitizers
cmake --preset host-asan && cmake --build build/host-asan && ctest --test-dir build/host-asan

# Run the simulator
./build/host-debug/apps/simulator/simulator \
    --scenario-co2 scenarios/co2_spike_meeting.csv \
    --scenario-env scenarios/normal_day.csv --time-scale 60

# Firmware
cd apps/firmware_esp32 && idf.py build
```

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
- Every new port needs a suite in `tests/contract/` that runs against all of its implementations.
- Pure functions (thresholds, MQTT payloads, HA Discovery, parsers, view models) get unit tests
  before being wired into the rest of the system.
- Screen changes are verified by golden tests: `simulator --headless --frames out/` compared
  against `tests/golden/`.
- Adding or removing a dependency requires updating `THIRD_PARTY_NOTICES.md` in the same commit.
  Check the new dependency's license first — copyleft components need explicit discussion.
- Before finishing a task, run `ctest`; if `core/` or `drivers/` changed, also run `idf.py build`.
- Record significant architectural decisions in `docs/adr/NNNN-<title>.md` (context, decision,
  consequences).
- Work happens on a branch per phase and lands through a pull request. `main` stays green.

## Style

- C11 for `ports/`, `core/`, `drivers/` and `platform/`. C++17 is allowed only in
  `platform/host/sim_gui/`.
- Every source and header file starts with `// SPDX-License-Identifier: Apache-2.0`
  (or the block-comment form where line comments are inappropriate).
- `-Wall -Wextra -Werror -Wconversion`. Formatting is `.clang-format`, enforced in CI.
- Errors use `hal_status_t`; `esp_err_t` is mapped inside the adapter and never reaches the core.
- No magic numbers outside `core/config/` and `platform/*/board_config.h`.
- All code, comments, documentation and commit messages are in English.

## Current phase

Phase 0 (skeleton): repository, CMake dual build, CI, `hal_status`, `port_clock`, domain types.
The phase order is in section 10 of `docs/architecture.md`. Do not run ahead to later phases
unless asked.
