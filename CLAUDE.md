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
revision, framebuffer orientation, GRAY4 packing, pin assignments, memory budget. Facts
recorded there are settled — use them rather than deciding again. Decisions with lasting
consequences live in `docs/adr/`; check the existing ones before re-opening a question.

## Environment

Development happens in WSL (Ubuntu). ESP-IDF 5.5 lives at `~/esp/esp-idf` and must be
activated per shell with `. $HOME/esp/esp-idf/export.sh` before `idf.py` is on PATH.

USB passthrough to WSL does not work here, so flashing happens from Windows with
`esptool` against COM17. Build in WSL, then tell me exactly what to flash and what
output would mean success; I flash and paste the log back. Never run `idf.py monitor` —
it does not exit on its own.

## Layering rules (a violation is a build failure)

- `core/` includes **only** `ports/*.h` and a subset of libc (`stdint`, `stdbool`, `string`, `math`).
  Forbidden: `stdio.h`, `time.h`, `malloc`/`free`, `esp_*`, `freertos/*`, `SDL*`.
- `ports/` is headers only — no `.c` files, no dependencies.
- `platform/<name>/` implements the ports and contains no business logic.
- `drivers/` depend only on `ports/*.h` (bus, clock, log, and `port_sensor.h` with the
  `core/domain/measurement.h` types it brings), so they build on a PC. A driver's
  `sensor_source_t` wrapper lives beside it in `drivers/`, not in `platform/` (ADR 0014).
  First-party code there may not include `esp_*`, `freertos/`, `driver/`, `SDL` or `stdio`;
  `scripts/check_sources.sh` enforces it. Vendored code under `drivers/*/vendor/` is exempt.
- `apps/<name>/main.c` is the only place where dependencies are wired together.
- The core never reads time from the system: only `port_clock->now_ms()`. This is what makes it testable.
- No panel-specific or platform-specific constant may appear in `core/`. Panel limits reach
  the core as values reported by `port_display`, never as literals.
- No pin number appears outside `platform/*/board_config.h`, as a `GPIO_NUM_<n>` macro or
  as a bare literal. `scripts/check_sources.sh` enforces this.
- Create a new port only when there are **two** real implementations (hardware plus simulator).
  Otherwise write a plain function.

If a task appears to require changing `core/` in order to make a platform work, stop and say
so rather than working around it. That is a sign the port abstraction is wrong, and it is
worth more than the task.

## Commands

```bash
# Static checks (no compiler needed — fastest signal, run this first)
./scripts/check_sources.sh
bash scripts/tests/test_pin_literal_check.sh
bash scripts/tests/test_driver_include_check.sh

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
. $HOME/esp/esp-idf/export.sh
cd apps/firmware_esp32 && idf.py build

# After adding or changing a Kconfig symbol — `idf.py build` alone is not enough
cd apps/firmware_esp32 && idf.py reconfigure && idf.py build

# On-target contract tests (ADR 0009): Unity app, every linked test runs at boot
cd apps/firmware_esp32/test_app && idf.py -T hac_platform build

# Slideshow test app (ADR 0012): a picture a minute, PNGs from apps/slideshow/images/
./build/host-debug/apps/slideshow/host/slideshow --time-scale 20
./build/host-debug/apps/slideshow/host/slideshow --headless --frames out/
cd apps/slideshow/esp32 && idf.py build      # -DSLIDESHOW_IMAGES_DIR=<dir> for others
python3 apps/tools/image_gen/image_gen.py prepare photo.jpg --out-dir <dir>  # needs Pillow

# Photo frame: the same app with the gitignored apps/photo_frame/images/ -- private, never commit
./build/host-debug/apps/photo_frame/host/photo_frame --time-scale 20
cd apps/photo_frame/esp32 && idf.py build

# Panel calibration image (ADR 0008), in its own build dir and sdkconfig
cd apps/firmware_esp32 && idf.py -B build-calibration -D SDKCONFIG=build-calibration/sdkconfig \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.calibration" build
```

A new symbol in `main/Kconfig.projbuild` does **not** reach `sdkconfig` from a plain
`idf.py build`: the config step is not re-run, the symbol is absent from
`build/config/sdkconfig.h` entirely, and `#if CONFIG_<NEW_SYMBOL>` therefore evaluates
to 0. Code behind it is silently compiled out — the build succeeds, nothing warns, and
the feature is simply not in the image. Observed when `CONFIG_HAC_EPD_TEST_PATTERN`
was added in phase 2b. `idf.py reconfigure` first, then verify by grepping the symbol
out of `sdkconfig` (and, for anything that must be in the image, out of
`strings build/home_air_monitor.bin`) rather than assuming the flag took effect.
The same trap applies to `sdkconfig.defaults` and overlays: defaults only fill symbols an
existing `sdkconfig` does not already hold, so a value added to an overlay never reaches a
build directory configured before it. Observed in phase 2c with
`CONFIG_ESP_MAIN_TASK_STACK_SIZE`; delete that build's `sdkconfig` and grep again.

## Never commit

- `sdkconfig` — it holds Wi-Fi SSID, password and broker address after `menuconfig`.
  Only `sdkconfig.defaults`, without credentials, belongs in the repository.
- Any credentials, tokens, certificates or keys. Provide `credentials.h.example` instead.
- The BSEC/BSEC2 binary (`libalgobsec.a`). It ships under a proprietary Bosch agreement and
  must be downloaded separately into the gitignored `third_party/bsec/`.
- Build output, `managed_components/`, `dependencies.lock`, simulator frame dumps.

This repository is public, so anything pushed stays in the git history permanently.
If a secret is ever committed, say so immediately rather than removing it in a follow-up commit.

## Licensing constraints

The project is Apache-2.0. Two specific hazards, both already investigated:

- `Xinyuan-LilyGO/LilyGo-EPD47` is **GPL-3.0**, not LGPL. It is a source of hardware facts
  (pins, orientation, panel geometry) and must never become a build dependency: linking it
  would put the whole image under GPL-3 and collide with the proprietary BSEC blob planned
  for phase 3. See ADR 0007.
- EPDiy is LGPL-3.0 and is an accepted dependency. LGPL constrains relinkability of the
  LGPL part only, which is why BSEC can coexist with it. The exit, if a closed-source build
  is ever needed, is one adapter file plus a pin table.

Adding or removing a dependency requires updating `THIRD_PARTY_NOTICES.md` in the same
commit, after checking its license. Copyleft components need explicit discussion, never a
silent addition.

## Working rules

- A new core file goes into `cmake/sources_core.cmake` and **nowhere else** — that list is shared
  by the host and ESP-IDF builds. A new driver file goes into `cmake/sources_drivers.cmake` the
  same way, vendored files into its separate vendor list. Never duplicate source lists in
  `apps/firmware_esp32/components/*/CMakeLists.txt`.
- Vendored third-party code lives in a `vendor/` directory, byte-for-byte as upstream, with a
  `README.md` naming the tag, commit and file hashes. Never edit it; it builds with the relaxed
  options of `cmake/sources_drivers.cmake` and is included as SYSTEM.
- Every new port needs a suite in `tests/contract/` that runs against all of its implementations,
  on the host and on the device. The same source compiles into both — never a copy.
- Include what you use. Do not rely on a header arriving transitively; this has broken the
  IDF build twice where the host build was silent, because newlib's include chain is
  shorter than glibc's.
- Pure functions (thresholds, MQTT payloads, HA Discovery, parsers, view models) get unit tests
  before being wired into the rest of the system. Cover the boundaries, not just the happy path.
- Screen changes are verified by golden tests in `tests/golden/`: raw framebuffer bytes compared
  against reference `.bin` files, each with a matching PNG so the diff is reviewable. Regenerate
  with `UPDATE_GOLDEN=1` only after confirming the change is intended.
- A CI check that has never been seen to fail is not known to work. New checks get a fixture
  test under `scripts/tests/`, asserting an exact violation set so over-firing is caught too.
- Values that are estimates rather than measurements must say so at their definition, naming the
  branch that will replace them. Keep them together in one place, never scattered as literals.
- New files under `scripts/` must be committed with the executable bit set:
  `git update-index --chmod=+x <path>`. A plain `chmod` alone does not reach git.
- Before finishing a task, run `./scripts/check_sources.sh` and `ctest`; if `core/` or `drivers/`
  changed, also run `idf.py build`. A red CI is not the place to discover a missing SPDX header
  or a platform include that leaked into the core.
- Report honestly what could not be verified in this environment and why, rather than assuming
  it passes. A claim that compiles while being false is worth flagging explicitly.
- Record significant architectural decisions in `docs/adr/NNNN-<title>.md` (context, decision,
  consequences). Anything a future reader would otherwise mistake for an arbitrary choice
  belongs there. Check the highest existing number before creating one.
- Findings that matter later must land in the repository, not only in a reply to me. A caveat
  in a session summary is gone when the session ends: put it in a comment at the code site,
  in `board_notes.md` open items, or in an ADR.
- Work happens on a branch per phase and lands through a pull request. `main` stays green.
- When a task is complete and verified, end by proposing a commit message in a copyable
  block: a subject line under 72 characters in the imperative mood; a blank line; then a
  body explaining why the change was needed and what it costs. Not a list of changed files
  — git already shows those. Reference the ADR or phase when the change implements a
  recorded decision. Do not run `git commit`; I decide when to commit.
- **No Conventional Commits prefix** — no `feat:`, `fix:`, `docs:` or the rest. The subject
  is a plain imperative sentence, matching every commit in this repository's history
  (`Guarantee delay_ms() advances by at least the requested time`, `Flush only the pixels a
  partial refresh actually changed`). An earlier revision of this file asked for the prefix;
  that was never the practice here, and the one commit that followed it is the odd one out.
- **No trailers.** No `Co-Authored-By`, no `Signed-off-by`, nothing after the body.

## Style

- C11 for `ports/`, `core/`, `drivers/` and `platform/`. C++17 is allowed only in
  `platform/host/sim_gui/`.
- Every source and header file starts with `// SPDX-License-Identifier: Apache-2.0`
  (or the block-comment form where line comments are inappropriate). Vendored third-party
  code under `*/vendor/` and `*/third_party/` keeps its original headers untouched.
- `-Wall -Wextra -Werror -Wconversion -Wshadow`. Formatting is `.clang-format`, enforced in CI.
- Errors use `hal_status_t`; `esp_err_t` is mapped inside the adapter and never reaches the core.
- No magic numbers outside `core/config/`, `platform/*/board_config.h` and
  `platform/host/epd_emulation.h`.
- All code, comments, documentation and commit messages are in English.

## Current phase

Phase 2a complete and confirmed on hardware: the core runs on the ESP32-S3 with a null
display adapter, `port_clock` and `port_log` contract suites pass on device, the framebuffer
diff produces real partial-refresh rectangles, and the startup memory budget is measured
(382 KB internal, 8.38 MB PSRAM free).

Phase 2b complete and confirmed on hardware: `adp_display.c` drives the ED047TC1 through
EPDiy's highlevel API (ADR 0007, FastEPD removed), and `screen_home` renders on the physical
panel from the same core code the simulator runs, with partial refreshes of the CO₂ headline.
Measured: full refresh 3.2–3.7 s (of which `epd_clear()` ~2.4 s), partial ~0.4 s, EPDiy
~1 MB of PSRAM.

Phase 2c complete and confirmed on hardware: refresh timings measured into
`platform/host/epd_emulation.h`, partial-refresh budget set to 10 from observed ghosting,
15 usable greys (ADR 0008); the on-target test app (ADR 0009); a regional full refresh
tried and rejected (ADR 0010). Still open from it: residue in the greys, column stripes in
white, and whether the panel's high-voltage rails are really off between refreshes.

Phase 2d complete and confirmed on hardware: `adp_i2c.c` drives the shared I²C bus through
ESP-IDF's `i2c_master` behind `port_i2c` (ADR 0011). The boot scan finds the RTC at `0x51`
and the touch controller at `0x5D`, where a GT911 answers — not the `0x5A` recorded before.
The `port_i2c` contract passes in the test app, including a forced hang that `recover()`
clears and a stalled SCL. Serial console on USB-Serial-JTAG: `i2c_scan`, `i2c_recover`.

Side work between 2d and 3, branch `slideshow-test-app` (ADR 0012, 0013): an image
pipeline (`image_gen.py`), `gfx_blit_image()`, a slideshow test app and a photo frame, and
the panel's grey levels measured into one table the simulator and `prepare` share. Next is
still phase 3.

Phase 3a on branch `phase-3a-scd4x-driver`, done on the host, not yet on hardware (ADR 0014,
0015): Sensirion's SCD4x driver vendored under `drivers/scd4x/vendor/`, its HAL over
`port_i2c`/`port_clock`, `scd4x_source` as a `sensor_source_t` in periodic and single-shot
modes, the fake bus turned into a bus of device models with an SCD41 command-level model, the
`port_sensor` contract as a shared body. On the board so far: the bus scan's sensor rows and a
console `restart` command. The sensors are not wired yet; nothing composes `scd4x_source` in
the firmware yet.

Next: the SCD41 on the board once it is wired (the scan, then a composition that runs
`scd4x_source`), and phase 3b — the BME68x driver over `port_i2c`. See the open items in
`docs/hardware/board_notes.md`.

The phase order is in section 10 of `docs/architecture.md`. Do not run ahead to later phases
unless asked. Note that phase numbers here refer to those branches; the "phase 2c" named in
`docs/hardware/board_notes.md` and in the e-paper emulation constants is the panel
calibration branch.