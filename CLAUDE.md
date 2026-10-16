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

# With Bosch's BSEC2 (ADR 0020): needs the archive in the gitignored third_party/bsec/
cmake --preset host-bsec && cmake --build build/host-bsec && ctest --test-dir build/host-bsec
HAC_TEST_VERBOSE=1 ./build/host-bsec/tests/test_bme68x_bsec_source   # what BSEC did

# Run the simulator (--sensors fake-i2c: the real drivers over device models; the footer's
# clock starts at the PC's time, or --wall-time 2026-10-25T00:58:00Z, or --no-wall-time)
./build/host-debug/apps/simulator/simulator \
    --scenario-co2 scenarios/co2_spike_meeting.csv \
    --scenario-env scenarios/normal_day.csv --time-scale 60

# Record what the sources report, as the board records to its card (ADR 0024), and replay
# a recording -- the board's, from its card's data/, or this one -- through the same merge
./build/host-debug/apps/simulator/simulator --headless --duration 3600 --record out/card \
    --scenario-co2 scenarios/co2_spike_meeting.csv --scenario-env scenarios/normal_day.csv
./build/host-debug/apps/simulator/simulator --recording out/card/data/<day>_scd4x.csv \
    --recording out/card/data/<day>_bme68x.csv --time-scale 60

# Headless run (CI path; --duration is required or it never stops)
./build/host-debug/apps/simulator/simulator --headless --frames out/ \
    --scenario-co2 scenarios/co2_spike_meeting.csv \
    --scenario-env scenarios/normal_day.csv --time-scale 60 --duration 600

# Firmware
. $HOME/esp/esp-idf/export.sh
cd apps/firmware_esp32 && idf.py build

# Firmware with BSEC2 IAQ, in its own build dir and sdkconfig (ADR 0020); then grep
# CONFIG_HAC_BSEC out of build-bsec/sdkconfig and bme680_iaq_33v_3s_4d out of the .bin
cd apps/firmware_esp32 && idf.py -B build-bsec -D SDKCONFIG=build-bsec/sdkconfig \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bsec" build
cd apps/firmware_esp32/test_app && idf.py -T hac_platform -B build-bsec \
    -D SDKCONFIG=build-bsec/sdkconfig -D HAC_BSEC=ON build

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
`CONFIG_ESP_MAIN_TASK_STACK_SIZE`; delete that build's `sdkconfig` and grep again. Phase 3f's
`CONFIG_FATFS_LFN_HEAP` (long file names for the card) is guarded: `adp_sd.c` refuses to
build with an `sdkconfig` that predates it. Its own build directories were regenerated when
the phase landed (the old files kept as `sdkconfig.old`); any other one needs the same.

## Never commit

- `sdkconfig` — it holds Wi-Fi SSID, password and broker address after `menuconfig`.
  Only `sdkconfig.defaults`, without credentials, belongs in the repository.
- Any credentials, tokens, certificates or keys. Provide `credentials.h.example` instead.
- Anything from Bosch's BSEC/BSEC2 archive: the binary (`libalgobsec.a`) ships under a
  proprietary Bosch agreement, and the headers, configurations (`bsec_iaq.c`/`.config`) and
  examples are marked "Confidential" with no redistribution of their source. Not a header,
  not a configuration array, not an excerpt. The archive lives in the gitignored
  `third_party/bsec/` (THIRD_PARTY_NOTICES.md, ADR 0020).
- Build output, `managed_components/`, `dependencies.lock`, simulator frame dumps.

This repository is public, so anything pushed stays in the git history permanently.
If a secret is ever committed, say so immediately rather than removing it in a follow-up commit.

## Licensing constraints

The project is Apache-2.0. Two specific hazards, both already investigated:

- `Xinyuan-LilyGO/LilyGo-EPD47` is **GPL-3.0**, not LGPL. It is a source of hardware facts
  (pins, orientation, panel geometry) and must never become a build dependency: linking it
  would put the whole image under GPL-3 and collide with the proprietary BSEC blob linked
  since phase 3d in a build with `CONFIG_HAC_BSEC`. See ADR 0007.
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
tried and rejected (ADR 0010). Still open from it: residue in the greys and column stripes
in white. The high-voltage rails are not off between refreshes: on this revision `PWR_EN`,
which EPDiy never clears, switches them, not `epd_poweroff()` (measured in phase 3a;
`docs/hardware/board_notes.md`, "Power rails and connectors").

Phase 2d complete and confirmed on hardware: `adp_i2c.c` drives the shared I²C bus through
ESP-IDF's `i2c_master` behind `port_i2c` (ADR 0011). The boot scan finds the RTC at `0x51`
and the touch controller at `0x5D`, where a GT911 answers — not the `0x5A` recorded before.
The `port_i2c` contract passes in the test app, including a forced hang that `recover()`
clears and a stalled SCL. Serial console on USB-Serial-JTAG: `i2c_scan`, `i2c_recover`.

Side work between 2d and 3, branch `slideshow-test-app` (ADR 0012, 0013): an image
pipeline (`image_gen.py`), `gfx_blit_image()`, a slideshow test app and a photo frame, and
the panel's grey levels measured into one table the simulator and `prepare` share. Next is
still phase 3.

Phase 3a on branch `phase-3a-scd4x-driver` (ADR 0014, 0015): Sensirion's SCD4x driver
vendored under `drivers/scd4x/vendor/`, its HAL over `port_i2c`/`port_clock`, `scd4x_source`
as a `sensor_source_t` in periodic and single-shot modes, the fake bus turned into a bus of
device models with an SCD41 command-level model, the `port_sensor` contract as a shared body
— all verified on the host. On the board: the sensors are wired and the boot scan finds the
SCD4x at `0x62` and the BME68x at `0x77`, but only since the bus comes up after the display
adapter — the sensors run from the switched `3V3` that EPDiy's init raises, and before it
they hold the bus low. A software reset (`restart`) keeps them powered. Nothing composes
`scd4x_source` in the firmware yet.

Phase 3b on branch `phase-3b-bme68x-driver` (ADR 0016): Bosch's BME68x Sensor API vendored
under `drivers/bme68x/vendor/`, its callbacks over `port_i2c`/`port_clock`, `bme68x_source`
as a `sensor_source_t` in forced mode — temperature, humidity, pressure and raw gas
resistance, no IAQ without BSEC — and a BME680 register-level model whose raw values invert
the datasheet's compensation, so host tests prove the plumbing, not Bosch's arithmetic. The
home screen's IAQ cell shows gas resistance while nothing provides IAQ. All verified on the
host only. Three estimates in `drivers/bme68x/bme68x_limits.h` — heater profile, gas
burn-in, self-heating offset — are phase 3c's to replace.

Phase 3c on branch `phase-3c-sensor-manager` (ADR 0017, 0018): `core/app/sensor_manager.c`
drives every `sensor_source_t` through init/start/ready_in_ms/read on its own schedule, merges
the fields by priorities the composition passes in (temperature and humidity from the BME68x,
the SCD41 as fallback), marks a failed source's fields `VAL_ERROR` at once, and starts a source
that fails three times over with `init()` after a doubling backoff (5 s to 120 s) -- `init()`
again without `deinit()` is now part of `port_sensor`. An optional `set_ambient()` in the port
carries the BME68x's pressure to the SCD41's CO2 compensation. Both event loops wake at the
earlier of the manager's and `update_policy`'s deadlines, and age the reading against the
clock read *after* the poll (a poll takes time; before it, every fresh value looked stale).
`bringup_reading.c` and the simulator's own merge are gone. The simulator has
`--sensors replay|fake-i2c`; in fake-i2c the drivers read device models that measure
`platform/host/scenario_world.c`, played from the same CSVs, and a test holds the two modes to
the same readings. The firmware wires both sensors on the bus `adp_i2c` brings up, hands the
measured temperature to `adp_display_set_temperature()`, and logs each source's sample once a
minute. `CONFIG_HAC_DISPLAY_NULL` and the test app raise `PWR_EN` themselves
(`board_power.c`). The `i2c.master` log tag is not quietened (ADR 0017). Verified on the host
(unit, contract, integration, ASan). On the board, 2026-10-16: the test app's `port_sensor`
contract passes against both real sensors -- an SCD41 and a BME680 -- with `PWR_EN` raised by
`board_power.c`; this BME680's calibration is now the host model's default; the firmware ran
both under the manager for half an hour, and unplugging both showed `ERR` at once and recovered
25 s after the replug without a reboot. Measured: event loop stack 4,716 of 8,192 bytes used,
internal RAM after EPD init 1,352 bytes lower than before the sensors, the heater stable from
the second measurement after each init. Still owed by the board
(`docs/hardware/board_notes.md`, open item "Phase 3c's measurements"): temperature against a
reference beside the device, the gas reading's real burn-in (hours), CO2 near outdoor air, the
SCD41's warm-up.

Also on this branch, at the owner's request after watching the board (ADR 0019, superseding ADR
0004's deferral and ADR 0008's budget): the partial-refresh budget is 20, past it
`update_policy` keeps refreshing partially until the panel's minimum full-refresh interval
allows a full refresh, and the next redraw due after that is full -- the screen no longer
freezes, and nothing is redrawn only because the interval elapsed. At most one redraw per 5 s
sensor cycle (`min_redraw_interval_ms`), so both sensors' readings of a cycle cost one partial
refresh: at most 20 partials and at least 105 s between full refreshes. The screen changes only
when the readings do (the periodic redraw this branch kept is gone since phase 3e, ADR 0023).
The simulator's panel model
performs and counts such partials and rescues only past the 40 the panel was tested with. And
the home screen's text is as large as its widest strings allow -- CO2 at 160px, the grid's
readings at 72px, labels and units at 32px, bar and footer at 40px, 3px rules -- with
`tests/unit/test_screen_home_layout.c` checking by pixels that the worst cases do not collide;
the gas cell's label is "Gas".

Phase 3d on branch `phase-3d-bsec-iaq` (ADR 0020): Bosch's BSEC2 2.6.1.0 behind
`CONFIG_HAC_BSEC` / `HAC_BSEC`, both off by default -- the archive is never committed, not
even its headers, which Bosch marks "Confidential" (THIRD_PARTY_NOTICES.md corrected). With it,
`drivers/bme68x/bme68x_bsec_source.c` measures the BME68x as BSEC asks: low-power mode, BSEC's
call only once its time has come (never early), its conditioning and 320 C/197 ms heater
profiles, heat-compensated temperature and humidity, IAQ with accuracy 0 as `VAL_WARMUP` and
1-3 as `VAL_OK` (`bme68x_bsec_status.c`, tested in CI), gas by BSEC's run-in, BSEC's CO2
equivalent never subscribed to. The firmware's `main.c` composes it instead of
`bme68x_source`; the IAQ cell shows IAQ. Shared bring-up moved to `bme68x_common.c`. BSEC's
state survives a sensor re-init in RAM, not a reboot (open item, phase 5/6). The host runs it
against Bosch's x86-64 library (`host-bsec` preset): unit tests and the `port_sensor` contract
over the BME680 model. Known and accepted for now: the event loop's synchronous panel flush
makes BSEC's call late at every full refresh (3.2-3.7 s against a 187 ms tolerance); each late
call is logged and counted, and the board decides (ADR 0020, "Timing"). First board run
2026-10-16, 21 minutes: BSEC's profiles as on the host, run-in and accuracy `low` after 197 s,
two late calls (459 ms at boot's full refresh, 490 ms at a partial one), accuracy still `low`
at the end. Second run, 21 minutes with breaths and airing: accuracy `high` at 955 s through 17
late calls (one per full refresh); IAQ's band redraws flap at 50, BSEC's clean-air resting value
and the Excellent/Good boundary; a brownout reset before that boot (board_notes.md, open items
"BSEC on the board" and "A reboot loop when the switched 3V3 comes up").

Also on this branch, at the owner's request after that run (ADR 0021): `measurement_f32_t`
carries `value_accuracy_t` beside its status -- a field, not new statuses; the core's own
four-step scale, BSEC's 0-3 mapped onto it in the driver -- and a `VAL_OK` number rated
`ACC_LOW` or `ACC_UNRELIABLE` gets a question mark and the footer "IAQ not calibrated";
IAQ's bands (`core/domain/air_quality.h`: 50/100/150/200, judged on the rounded number) show
under the number as "Excellent", "Good", "Fair", "Moderate", "Poor", and a change of band, or
of the question mark, redraws; and "ppm" and the CO2 mark moved to the label's line, "CO2
ppm", where no reading draws, after the moving unit left residue on the panel. The layout test
holds the last by pixels. Not yet seen on the panel.

Phase 3e on branch `phase-3e-rtc-wall-clock` (ADR 0023): the device has a wall clock. The
PCF8563 at `0x51` -- powered from `VCC_RTC`, `VDD3V3` through a diode with a rechargeable cell
behind it, so not on `PWR_EN` (`board_notes.md`, "The RTC") -- holds UTC, never local time;
`drivers/pcf8563/` reads and sets it over `port_i2c` (BCD, the VL bit, 2000-2099, a decode
failure for anything else), with no port: one RTC, and the simulator's wall time is
`adp_clock_virtual`'s. `adp_clock`'s `wall_ms()` is ESP-IDF's system time (`gettimeofday`), 0
before 2026 -- what nothing set -- so phase 4's SNTP needs nothing there; the composition sets
it from the RTC at boot when the chip says the time is valid. Console: `time` and `time_set
<YYYY-MM-DDTHH:MM:SSZ>` (UTC). `core/domain/civil_time.c` converts to local time without
`time.h`, from a zone rule the composition passes in: Vilnius, `time_zone_eu(120)`. The footer
says "updated HH:MM" -- the local minute the freshest value on screen was measured, "DD.MM
HH:MM" from an earlier day, "--:--" without a clock -- never an age, and `update_policy` has no
periodic redraw any more: nothing on the screen changes with time alone, and staleness is a
status change. Departs from requirements 9.1's two-minute example. Measured on the simulator's
panel model: no scenario's refresh count changes; a redraw in a new minute spans headline and
footer, +105 ms, +2.9 % of panel time on `co2_spike_meeting.csv`. Two footer notes shortened to
fit the dated form ("N not calibrated", "Temp. not calibrated"). Simulator: the PC's time at
start, or `--wall-time`, or `--no-wall-time`. Verified on the host. On the board, 2026-10-16:
the RTC held 2000-01-27 with VL set and was ignored; `time_set` wrote it, the footer showed
the time, nothing redrew while the readings held still, and after more than a minute with no
power at all the RTC was still valid -- the backup cell is fitted and holds. The first boot after
a power cut is not in the terminal's log: the terminal reopening the port resets the chip again
(`reason=usb`). Still owed (`board_notes.md`, open item "The RTC on the board"): `time` against a
reference, longer outages, the drift. Two logs caught brownouts at the moment `PWR_EN` rises
(open item "A reboot loop when the switched 3V3 comes up").

Phase 3f on branch `phase-3f-sd-recordings` (ADR 0024): the microSD card. The slot, from the
v2.4 schematic (`board_notes.md`, "The microSD slot"): SPI only, `VDD3V3` not `PWR_EN`, no
pull-up on MISO (the S3's internal one), card-detect only on a test pad; IO15/IO16, `P5`'s pins 2
and 3, are the card's now. `ports/port_files.h` -- open to append, append, flush (fsync), close,
list with sizes, remove; no medium `HAL_ERR_NOT_READY`, full `HAL_ERR_NO_MEM`; no read, the
contract reading back through its runner -- with `adp_sd.c` (SD over SPI2 + FATFS, never
formats, FAT32 only, long names) and `adp_files_dir.c` (a host directory), and a contract on
both, five broken wrappers caught. The scenario format grows optional columns -- `utc`,
`<field>_status` (interpolation per field), `<field>_acc` -- and the word `unavailable`; a torn
last line and a NUL tail are forgiven; every old file plays as before. `core/recording/`: the
row format, file names by Vilnius day (`data/<day>_scd4x.csv`, `_2` for the next file of a day --
a recording is never reopened -- `undated-NNNN` before the clock), the bound (1 GiB data, 512 MiB
log, estimates; the oldest day removed first), the recorder (a watch on the event loop, the
writing on the card's task, flushed every 10 s, an estimate) and the log's files. On the board
`card_writer.c` owns the card: mounts at start, every minute quietly and at `sd_mount`, records,
drains `log_tee.c`'s ring (1024 lines in PSRAM, filled by an `esp_log_set_vprintf` hook that
never touches the card) into `log/<day>.log` with UTC; console `sd`, `sd_mount`, `sd_eject`,
`rec`. Simulator: `--record <dir>`, `--recording <file>` (source by the name's tag, wall clock
from the first row). Verified on the host (unit, contract, the round trip value for value and
through the CLI redraw for redraw); the firmware, its BSEC variant and the test app build.
Measured in the build: image +113 KB, static internal RAM +12.8 KB (7 KB of it the SPI driver's
code ESP-IDF puts in IRAM). On the board, 2026-10-16: the first image reset itself at boot five
or six times in a row when `PWR_EN` rose -- interrupt watchdog with the CPU stuck on a cache
access, once a brownout -- then ran, mounted a 16 GB card and recorded; a second, with the card's
task moved after the rail, never booted. What both did there and 3e did not: `log_tee`'s hook
writing EPDiy's next log line into the octal PSRAM within microseconds of the rail switching on.
The hook is now held off PSRAM while the rail comes up (`log_tee_hold()`), and the third image
booted at the first attempt and recorded: ~53 bytes a row for the SCD41, ~83 for BSEC, the event
loop's longest post 33 us against the card's slowest write of 103 ms (`board_notes.md`, open
items "A reboot loop when the switched 3V3 comes up" and "The microSD card on the board (phase
3f)", which lists what is still to see). The lesson for phase 5: nothing touches PSRAM in the
first moments after `PWR_EN` rises.

Next: the board run of phases 3d, 3e and 3f, the rest of phase 3c's measurements -- now
recordable unattended -- and this unit's own figures for the remaining estimates.

The phase order is in section 10 of `docs/architecture.md`. Do not run ahead to later phases
unless asked. Note that phase numbers here refer to those branches; the "phase 2c" named in
`docs/hardware/board_notes.md` and in the e-paper emulation constants is the panel
calibration branch.