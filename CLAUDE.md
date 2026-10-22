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
- `drivers/` depend only on `ports/*.h` (bus, clock, log, `port_input.h`, and `port_sensor.h`
  with the `core/domain/measurement.h` types it brings), so they build on a PC. A driver's
  `sensor_source_t` wrapper lives beside it in `drivers/`, not in `platform/` (ADR 0014), and
  so does the GT911's `port_input` implementation (ADR 0025).
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

# Taps without a window (ADR 0025): "<seconds>:<x>,<y>;..." in panel pixels -- a tap
# switches between the home and diagnostics screens; the window's clicks do the same
./build/host-debug/apps/simulator/simulator --headless --duration 200 \
    --scenario-co2 scenarios/co2_spike_meeting.csv --scenario-env scenarios/normal_day.csv \
    --taps "30:480,270;45:480,270"

# Every MQTT message, as Home Assistant would receive it (ADR 0029)
./build/host-debug/tests/mqtt_messages_dump

# The simulator in a Home Assistant, over its real broker (ADR 0030, amended): needs
# libmosquitto-dev when configuring; real time, --time-scale 1 to 60; the password from
# the environment, never the command line; Ctrl-C ends it and the will marks it unavailable
HAC_MQTT_PASSWORD=... ./build/host-debug/apps/simulator/simulator --headless --duration 0 \
    --broker mqtt://<broker>:1883 --mqtt-user <user> \
    --scenario-co2 scenarios/co2_spike_meeting.csv --scenario-env scenarios/normal_day.csv

# The reporter over a broker model in memory (ADR 0030): stdout prints every message the
# broker takes, loopback its sessions and wills and a summary; --net-script cuts the network,
# and the status bar says so a minute later -- each change a [link] line (ADR 0031)
./build/host-debug/apps/simulator/simulator --headless --duration 1800 \
    --scenario-co2 scenarios/co2_spike_meeting.csv --scenario-env scenarios/normal_day.csv \
    --telemetry loopback --net-script "600:down;1200:up"     # or --telemetry stdout --device-id x

# Firmware
. $HOME/esp/esp-idf/export.sh
cd apps/firmware_esp32 && idf.py build

# Firmware with Wi-Fi (ADR 0032): the network in the gitignored overlay, layered last over
# every build's defaults (the test app's too); it reaches only a fresh sdkconfig, and the
# build refuses to configure while the two disagree. Without it the image has no Wi-Fi.
# With CONFIG_HAC_MQTT_BROKER_URI (and _USER, _PASSWORD) in the same overlay, MQTT to
# that broker and the reporter (ADR 0033); without a broker, no MQTT. Console: mqtt.
cd apps/firmware_esp32 && cp sdkconfig.credentials.example sdkconfig.credentials  # fill in
rm -f sdkconfig && idf.py reconfigure build && grep CONFIG_HAC_WIFI_SSID sdkconfig
grep CONFIG_HAC_MQTT_BROKER_URI sdkconfig

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
- `sdkconfig.credentials` — the Wi-Fi network and its password (ADR 0032), the MQTT broker,
  its user and password (ADR 0033); the repository has `sdkconfig.credentials.example`. An image built with credentials holds them in plain
  text: never attach one, nor an sdkconfig, to an issue or a pull request.
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

Phase 3g on branch `phase-3g-touch-input` (ADR 0025): touch. `drivers/gt911/` -- the GT911
over `port_i2c` (found at `0x5D` or `0x14`, woken first since nothing resets it, its product
ID checked, its configuration read and never written: the host model refuses and counts any
write there), the snapshots-to-events function and `port_input` over both, all run on the
host against `platform/host/i2c_models/gt911_model.c`, written from the datasheet Rev.10
(sleep, the 2-5 ms wake pulse, the 58 ms) and the programming guide's register map, which no
legible copy confirmed. `adp_input.c` adds only GPIO47's interrupt and a task that reads the
controller and wakes the event loop (`event_loop_post_input()`). `core/app/screen_controller.c`
is port_input's one consumer: a tap (`core/app/tap.c`) switches between the home screen and
a diagnostics screen (`core/ui/screen_diagnostics.c`: sources, readings, clock, recorder),
which goes home after a minute untouched. A screen change is `update_policy_request()`: due
at once, a partial refresh counted like any other -- and partials past the budget now stop at
the budget again (`UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET`, 40 for this panel), since a
person tapping could pass the 40 the panel was tested with: the owner's to confirm (ADR 0019
amended). The `port_input` contract runs on the host against the GT911 over its model, the
simulator's `--taps` and the SDL adapter, with five broken wrappers caught; in the test app
against the real controller, with a sleep-and-wake case. Console: `touch`, `touch_sleep`,
`touch_wake`. The coordinate mapping is the vendor's (swap, Y mirrored), unconfirmed. The
panel model predicts 1,080 ms from tap to glass. Verified on the host (ASan included). On
the board, 2026-10-17 (ELF `f63cfdf60...`, BSEC): the controller named itself, `911`,
firmware 0x1060, its configuration a falling edge, 540 x 960, two points -- now the host
model's defaults; six taps switched the screen, each on the glass 1,163-1,173 ms after the
lift (80 ms rendering, 1,068 ms EPDiy; the model within 1.1 %); three of the six changes made
BSEC's call late. Then three days unattended: corners, a finger during a full refresh,
`touch_sleep`/`touch_wake` and the timeout all as designed -- and a boot loop after a cable
plugged in made the controller re-latch at `0x14`: a second of refused reads at `0x5D`, each
logged three times by ESP-IDF's I2C driver, overflowed app_main's 3.5 KB stack into the heap.
Fixed: the controller is found by address-only probes of both addresses (which log nothing),
on the touch task (6 KB, not app_main), and looked for again whenever a read fails (ADR 0025,
"The controller's address"). The fix on the board (ELF `6f8a91905...`): found at `0x14` at
the first attempt, no error lines, no panic; `touch_sleep` then `restart`: the sleeping
controller refused its address in the boot scan, and init() woke it. The test app: `9 Tests
0 Failures`, twice -- the controller at `0x14` in one run and at `0x5D` in the other -- the
`port_input` contract and the sleep-and-wake case against the real controller. Phase 3g's
board checks are done (`board_notes.md`, "The touch panel on the board").

Follow-up to ADR 0021 on branch `redraw-thresholds` (ADR 0026): IAQ's band flapped at 50 --
BSEC's index rests on the Excellent/Good boundary, and three days unattended made its band
changes 96 % of all redraws. IAQ now redraws as the other readings do, when its number has moved
by strictly more than `UPDATE_POLICY_DEFAULT_IAQ_DELTA`, 5 points, since the frame on the glass;
the band plays no part in the trigger, and the band drawn is the band of the number drawn. The
owner's choice, after a first image that kept the band and added a delta of 10 (43 minutes on
the board, no IAQ redraw). The noise's excursions peaked at 51.5 (median), 56.6 (p90), 58 at
most, the smallest real event at 65 (the recordings of 2026-10-18 and 19, analysed locally, never
committed). The owner's two days replayed: IAQ redraws 544 and 959 -> 215 and 253, full
refreshes 28 and 49 -> 11 and 14, the number on the glass never more than 10 from the air, every
move past a band's boundary by more than 10 on the glass within 1 s; at the board's rate an
estimated 280 and 243 IAQ redraws a day. Status and accuracy-mark changes redraw at once as
before. The simulator plays an `iaq` column from `--scenario-env` and its `[policy]` lines carry
`now_ms=`; `scenarios/iaq_flapping_at_50.csv` is synthetic, checked by
`tests/cli/simulator_iaq_flapping.cmake`. Verified on the host; the board day (`reason=iaq`, full
refreshes, late BSEC calls per day) is still to run (`board_notes.md`, open item "BSEC on the
board").

On the same branch (ADR 0027): pressure, shown in whole hPa, had no
threshold and reached the glass only when something else redrew; it now redraws when it has
moved by strictly more than `UPDATE_POLICY_DEFAULT_PRESSURE_DELTA_HPA`, 1 hPa, reason `pressure`.
On the owner's replayed days it never fires -- the other readings redrew every few minutes and
pressure never lagged more than 0.83 hPa -- so it is a bound for a room where nothing else moves.
It does not remove a one-step offset at a rounding boundary (pressure one hPa off on the glass
for up to 57 minutes on 2026-10-19).

On the same branch (ADR 0028): a reading more than half its delta from the frame on the glass for
15 minutes without a break redraws, reason `settle` (`UPDATE_POLICY_SETTLE_DELTA_FRACTION`,
`settle_after_ms`) -- an offset that persists in still air, which the deltas alone leave on the
glass for as long as nothing else redraws. Not a timer: it redraws only a reading that has moved,
once. `update_policy_evaluate()` now records which readings are off and since when (never the
budget; `update_policy_commit()` clears them), and its deadline wakes the loop for them. Raw values,
not the screen's digits: a rounding boundary's tenth of a hPa is not redrawn. The owner's two days
replayed: 3 and 5 settle redraws, 230 and 291 redraws in all -> 229 and 287, the longest stretch
of a reading more than half a delta off cut from 20-24 minutes to 15. Verified on the host.

Phase 4a on branch `phase-4a-mqtt-payload` (ADR 0029): what goes over MQTT, as pure
functions -- nothing sends yet. `core/reporting/topics.c` builds `<prefix>/<id>/state`,
`<prefix>/<id>/status` and `homeassistant/sensor/<id>/<entity>/config` from a prefix and a
device id it checks (every level `[A-Za-z0-9_-]`, HA's rule for a discovery node id; bounded
lengths), all QoS 1 and retained. `mqtt_payload.c` turns `air_reading_t` into one JSON object,
every key always present: a value only for `VAL_OK` -- stale included, `null` otherwise, since HA
stamps a state with its arrival -- and `<key>_status` beside it with the recordings' words;
`iaq_accuracy`, `rssi`, `uptime`; the recordings' decimals. `ha_discovery.c` writes one config
per entity -- measurements, and gas resistance, IAQ accuracy, RSSI and uptime as diagnostics --
with the screen's decimals as `suggested_display_precision`, `expire_after` three heartbeats,
and an empty config for what the build does not provide, which removes it; only the SCD41's
`co2` says carbon dioxide. No `object_id` (deprecated in HA 2025.10). Every name and version is
the composition's. The fixed-point text moved from `core/recording/record_text.h` to
`core/text/text_append.h`, and `core/reporting/json_writer.c` is built on it. Verified on the
host: exact text for every status and accuracy, NaN, -0.0, a buffer exactly large enough and a
byte short, illegal ids; and `mqtt_messages_parse`, which parses every message with Python's
`json` and checks that the templates name only keys the state has. Nothing seen in a Home
Assistant yet: phase 4b sends.

Phase 4b on branch `phase-4b-reporter` (ADR 0030): the reporter, its ports and a broker
model. `ports/port_net.h` as architecture 5.6 sketched it (up() asks and keeps asking; the
composition calls it once). `ports/port_telemetry.h`: connect with a last will, disconnect,
publish, subscribe, is_connected and a non-blocking `poll()` for what arrives, as port_input
has -- a departure from 5.6; nothing waits, publish() copies, `HAL_ERR_NOT_READY` without a
session, `HAL_ERR_NO_MEM` with the queue full; the port never reconnects or keeps messages
for later. `core/reporting/reporter.c` decides all that: connect when the network is up,
backoff 5 s to 120 s, give up an attempt after 10 s; every session subscribes to HA's
status, then "online", the nine discovery configs and the state; HA's live birth brings the
configs again; nothing is queued while offline (the card keeps the history); a refused
message is retried after 1 s with the newest reading; the will is "offline", keepalive 60 s.
The state carries `rssi` from port_net; the screen's reading never gets the link, so an
outage redraws nothing (the status bar still says "offline" on the board: ADR 0030, "Not
decided"). `core/reporting/publish_policy.c`, apart from update_policy (a departure from
architecture 3): at once per session, on a status or IAQ-accuracy change, past a delta, at
least every 60 s heartbeat, at most once per sensor cycle; the deltas are the display's, in
`core/reporting/report_limits.h`, sized by replaying the owner's 2026-10-18 and 19 (local
only): 1,554 and 1,597 messages a day, 8 % and 11 % over the heartbeat's 1,440. Host:
`adp_net_sim.c` (`--net-script`), `adp_telemetry_loopback.c` (a broker per MQTT 3.1.1 --
retained, `+`/`#`, wills on a drop or a takeover, refusals, a stall); the simulator's
`--telemetry stdout|loopback` and `--device-id`. Both event loops wake at the earliest of the
sensor manager's, update_policy's and the reporter's deadlines; `app_core` not yet (phase
5). Verified on the host: both contracts with six broken wrappers each, the reporter on a
fake clock, a CLI test whose outage leaves the redraw log and the recordings byte-identical.
The firmware and the test app build with `.reporter = NULL`: nothing is sent from the board.

Phase 4c on branch `phase-4c-simulator-mqtt` (ADR 0030, amendment "The simulator on a real
broker"): the simulator in front of a real MQTT broker -- a Home Assistant's.
`platform/host/adp_telemetry_mqtt.c` is port_telemetry over libmosquitto, built only when
libmosquitto 2.x is found (`SIM_MQTT`, `hac_platform_host_mqtt`, `HAC_SIM_MQTT`), as the SDL
window is: one libmosquitto client per session, each on a thread of its own running
`mosquitto_loop_forever()` -- not `mosquitto_loop_start()`, whose async connect resolves the
name in the caller and whose thread cannot be joined once it ends by itself -- so connect()
never waits, a drop stops the loop instead of reconnecting (`mosquitto_disconnect()` in
`on_disconnect`), and nothing is resent into a later session. The callbacks queue under a lock
and call a waker; the next session's CONNECT waits for the last one's DISCONNECT (at most 1 s);
the network going silences the session -- its thread cancelled, the socket left open -- so the
broker publishes the will one and a half keepalives after the last packet it heard; the end of a run closes every socket
without a DISCONNECT, so the will goes out. Simulator: `--broker mqtt://<host>[:<port>]` (no
TLS, no user or password in the URL), `--mqtt-user`, the password from `HAC_MQTT_PASSWORD`,
printed nowhere; the loop follows real time with `--broker`, headless too, at `--time-scale` 1
to 60; Ctrl-C and SIGTERM now end any run with its summary; stdout line-buffered in real time.
A board's device id is `hac-` and its MAC's twelve hex digits, fixed now in
`core/reporting/topics.h` (`mqtt_device_id_from_mac()`, `_is_from_mac()`) for the board's
composition to use, and the simulator refuses that form in front of a real broker: it never
takes a board's place in Home Assistant. libmosquitto is used under EDL-1.0 (BSD-3-Clause), one
side of its EPL-2.0/EDL-1.0 dual licence, host only, a system package, never in an image
(THIRD_PARTY_NOTICES.md). Tests: the port_telemetry contract against libmosquitto and a private
mosquitto on a free port (`tests/support/mqtt_test_broker.c`, or `HAC_TEST_MQTT_BROKER`), cut
and restore exercised, the full queue not; `tests/cli/simulator_broker.py`, with an MQTT client
of its own: the device appears (online, seven configs, two empty, the state, retained), Home
Assistant's birth brings the nine configs again, a broker restart is recovered, SIGKILL and
`--duration` leave the will, a password file refuses the wrong password ("not authorised") and
takes the right one; both say "not exercised" without a broker or libmosquitto. CI's host and
ASan jobs install `libmosquitto-dev` and `mosquitto`. Verified locally with 2.0.22 unpacked
from Ubuntu's packages (nothing could be installed): debug, ASan/UBSan and ThreadSanitizer
clean, the default build without it green. Seen in Home Assistant 2026.10.2, 2026-10-21 (a
container in WSL, Ubuntu's mosquitto 2.0.22, the board's recordings of 2026-10-18 at x1): the
device and its nine entities, Home Assistant's restart (discovery again), the broker's restart
(session 2 after 15 s -- 10 of them the reporter's connect timeout on an attempt the adapter
knew was refused: the port cannot say so, a port change to weigh with the board's adapter),
Ctrl-C (unavailable at once, the will), and an outage (`--net-script "120:down;300:up"`: the
will 34 s after the cut, 90 s after the last packet the broker had heard -- a real broker counts
the keepalive from there, MQTT 3.1.1 3.1.2.10, so a vanished device is offline 30 to 90 s later;
the loopback broker model now counts the same way, with the client's pings, where it counted
from the cut and was up to a keepalive late). History and long-term statistics seen the same
day, 2 h 37 min from a copy of Home Assistant's database: seven statistics, the hourly means
within 0.04 % of the recording's; and two simulators with one `--device-id` take each other's
session over for as long as both run, so each needs its own (its help says so). Still to see:
a broker with a password, Home Assistant on its own machine (the amendment's list).

Phase 4d on branch `phase-4d-network-on-screen` (ADR 0031): the network on the screen. The
status bar says `online` (an MQTT session -- Home Assistant's word), `no broker` (the Wi-Fi up,
no session) or `no Wi-Fi` in the stale grey, and nothing without telemetry -- in a place as wide
as the widest word, so a change of word moves nothing else. A change shows only once the link
has held it for a minute (`core/app/link_hold.c`, `LINK_HOLD_DEFAULT_MS`, an estimate), both
ways and the first state too (`LINK_UNKNOWN`: the place, no word); no telemetry at once. So a
link costs at most one redraw a minute, and one flapping faster none. `air_reading_t.link` is
the held state, which both loops feed after the reporter's step from `reporter_link()` and wake
for; `telemetry_connected` is gone, `net_connected` and the signal stay the state message's, and
`update_policy`'s status trigger fires on `link` -- drawn -- not on those (the open item now
holds `charging` alone). Not in `update_policy`: a hold there would draw the raw link whenever
anything else redrew. The diagnostics screen has two network lines -- `Wi-Fi` state (`up`,
`joining`, `failed`, `off`), SSID, signal, address; `MQTT` `online`/`offline`, `sent N` (in
thousands past 9,999), `refused N`, `last HH:MM` -- drawn as they are when the screen is drawn
and redrawn only for the held link, drawn as the bar's word in its title line, and the SSID; its
title is 32px to make room for thirteen lines. (The first version compared the held link without
drawing it: an empty diff, retried every second while the screen was up -- found by the owner in
the window, fixed, and held by a test that every change of the link changes the picture.) `port_net` gains `info()`: the SSID as configured, also while down, and the IPv4 address
while up; the contract checks it, nine broken wrappers caught; the simulator's network is
`sim-ap` at `192.0.2.10` and its `--net-script` holds 1,024 changes. The simulator prints each
change as `[link]`. CLI tests: the outage's link on the screen a minute late each way and the
same redraws until then (ADR 0030's "the same redraws throughout" replaced); a link cut 3 s in
every 10 s for half an hour -- 169 sessions -- redraws exactly as a steady one (172), one cut
90 s in 180 s shows each change a minute late and never two within a minute (20, 177 redraws).
Golden frames: `online` in the normal ones, nothing in the warm-up one, a new `no_broker`, the
diagnostics screen with the network and a new `no_telemetry`. Verified on the host (ASan, BSEC,
a real mosquitto); the firmware, its BSEC variant and the test app build. On the board, with no
reporter yet: no word where "offline" was, `Wi-Fi off` / `MQTT off` on the diagnostics screen
(`board_notes.md`, open item "The network on the screen (phase 4d)", for what the adapters are
to show).

Phase 4e on branch `phase-4e-wifi` (ADR 0032): Wi-Fi on the board.
`platform/esp32_t5s3/adp_net.c` is `port_net` over ESP-IDF's station, with a task of its own
that owns the driver -- `up()` and `down()` only record and wake it, ESP-IDF's events reach it
through a queue, and the port shows what it publishes under a spinlock, or the request itself
until the task has acted on it. Its decisions are `adp_net_pure.c`, tested on the host
(`tests/unit/test_adp_net_pure.c`), its numbers `adp_net_limits.h`: a lost link sought at
once, failed attempts after 1 s doubling to 30 s, an attempt without an address given up at
30 s; refused credentials are counted ("is the password right?") and never given up -- after
a power cut the access point and the device start together -- so `NET_FAILED` is only a
radio that will not start. All-channel scan by signal, WPA2 at least, WPA3 SAE, DHCP told the
device id `hac-<MAC>`; the signal read every 10 s. Credentials: `CONFIG_HAC_WIFI_SSID` and
`_PASSWORD` in `components/hac_platform/Kconfig`, set only in the gitignored
`apps/firmware_esp32/sdkconfig.credentials` (`.example` committed), which
`cmake/credentials_idf.cmake` layers last over every build of the firmware and the test app,
refusing to configure while it and the sdkconfig disagree; empty means no station is set up
and the linker drops the Wi-Fi stack. The driver keeps no copy (`nvs_enable = 0`, RAM
storage); NVS is opened, never erased, for the PHY's calibration data. Start order: the
station set up in `app_main` after the rail, the log's release, the card and touch; the radio
started by the event loop's new first-frame hook, once the boot's full refresh is over -- the
RF calibration neither in `PWR_EN`'s inrush nor in a panel drive. Buffers stay in internal
RAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` off: the octal PSRAM is what did not ride out
dips); lwIP pinned to core 0 beside the Wi-Fi task, core 1 left to the event loop and
EPDiy's row output; no SoftAP. Measured in the build, with a network against without:
+51,140 bytes of static internal RAM (30,184 of it Wi-Fi code in IRAM), image +501 KB to 1.2
MB. Console: `wifi`, and `vbat` -- GPIO14's ADC2 read beside Wi-Fi, for the open item. Test
app: the `port_net` contract against the station, IGNORED without credentials. No reporter
yet: the screen shows nothing of the link (the diagnostics screen says `Wi-Fi off`). Verified
on the host (unit, ASan); the firmware, its BSEC variant and the test app in both variants
build from regenerated sdkconfigs, and the firmware with a dummy overlay. On the board,
2026-10-21 (ELF `4734936f1...`, two boots): no brownout, the radio started 40-60 ms after the
first frame, the link up 6.7-6.9 s after `up()` at the first attempt, EPDiy's draw times
unchanged to the millisecond, GPIO14's ADC2 read 32 of 32 beside the link; internal RAM 56.7
KB free with the link up, 42.7 KB at the least -- Wi-Fi's run-time share 68 KB, above the
estimate. So, the same day: the Wi-Fi IRAM optimisations off (17,564 bytes of static internal
RAM back; `adp_net.c` refuses an sdkconfig with them on), and a shutdown handler so that a
`restart` no longer logs a lost link -- both seen on the board (ELF `8baab9290...`): 74.2 KB
free with the link up, 60.2 KB at the least, the link as fast as before, `restart` silent. Still
owed (`board_notes.md`, open item "Wi-Fi on the board"): many boots, recovery, the glass by eye,
a day's BSEC and heating.
The test app passed on the board (`11 Tests 0 Failures`, the `port_net` contract against the
station) once its runner called at the event loop's priority.

Phase 4f on branch `phase-4f-sntp` (ADR 0023, amendment "Time from the network"): SNTP.
`apps/firmware_esp32/main/net_time.c` starts ESP-IDF's `esp_netif_sntp` at Wi-Fi's first
address, one server, `CONFIG_HAC_SNTP_SERVER` in `main/Kconfig.projbuild` (`pool.ntp.org`),
an answer an hour (`NET_TIME_SYNC_INTERVAL_MS`); lwIP retries by itself, 15 s doubling to
150 s. lwIP's answer arrives on lwIP's task in `sntp_sync_time()`, ESP-IDF's weak function,
which `net_time.c` replaces: the server's time held to 2026-2099 (`wall_clock_believable_ms()`;
lwIP as built checks little, `SNTP_CHECK_RESPONSE` 0), set with `settimeofday()` or refused,
left in a mailbox and the event loop woken -- nothing else runs there. The rest is the event
loop's, through a new step hook with a deadline (`event_loop_deps_t.step`): the `synced` line,
the source to `sntp`, and `rtc_sync.c`'s steps. The RTC is measured by its own tick: its alarm
sets AF and pulls the open-drain INT low at the tick into the armed minute (datasheet Rev. 11.1,
8.6.5), and `platform/esp32_t5s3/board_rtc_int.c` stamps each falling edge on IO9 with
`esp_timer` in an interrupt -- the measurement, independent of any task's load. Per answer:
arm for the next minute, the edge's offset and the drift since the last write logged (`rtc
against sntp`) before the write; the write -- stopped, the second written, started so that the
first tick falls on that second's end, the loop woken 30 ms early and spinning, a missed moment
re-planned up to 3 times, stamped either side (`rtc written`); arm again, and the next edge
says where it landed (`rtc landed`), the mark for the next drift -- measured, never predicted.
The delay from a start to the first tick is learned: the datasheet's 508 ms until measured.
Driver: `pcf8563_stop()`, `_write_time()`, `_start()`, `_alarm_at_minute()`, `_alarm_off()`.
Numbers in `wall_clock_limits.h`. `time` shows `TIME_SNTP` lines (sync phase, INT's edges and
level, the learned delay, offset, drift, landing); `time_set` and the new `rtc_stop` (STOP
set: a boot finds no time) stop a sync and forget the mark. No port, no core change, no task.
Host: `platform/host/i2c_models/pcf8563_model.c`, a PCF8563 that keeps time and raises its
alarm on INT, from the datasheet; `tests/unit/test_rtc_sync.c` steps the sync against it as the
event loop would -- offsets to 2 ms, landings on the second, the board's late first tick learned,
drift over an hour and a day within 1.5 ppm, a held loop re-planned, a loop always late measured,
no INT edge, no valid RTC, no wall time, a hung bus; four mutations caught. A first version
with a task of its own that polled the RTC ran on the board on 2026-10-21 (ELF `9fb8fd2b5...`):
the RTC 12.9 s behind since the `time_set` of 10-16, **-29 ppm**; the write landed **481 ms
behind** -- this chip's first tick came about 0.989 s after a start, not 0.508 (its marking is
to be read: a look-alike?); the task cost 8.4 KB of internal RAM. Hence the design above.
Measured in the build against `main` with the same network: static internal RAM +1,736 bytes
(1,251 lwIP's DNS client, linked for the server's name), image +20,864; without a network no
SNTP and no lwIP, 336 bytes of `net_time`'s state. Verified on the host (unit, ASan); the
firmware, its BSEC variant and the test app in both variants build, and a no-network image.
On the board, 2026-10-22 (ELF `59c002862...`, one sync): INT on IO9 works, both edges
stamped; the offset -554 ms ±2; the write on time (`stopped_ms=21 rearms=0`); the landing
`residual_ms=-492 first_tick_ms=1000 learned=true` -- this chip first ticks a whole second
after a start (the 989 of the polling version was its own error); -21 ppm over the hour since
the first version's write; 72,503 bytes free with the link up. One `E gpio: ... GPIO isr
service already installed` line at every boot, the touch adapter's second install: since then
the shared GPIO ISR service is installed once (`platform/esp32_t5s3/board_gpio_isr.c`, on
app_main before either asks) -- the line gone since. A night of hourly syncs, 2026-10-22 (ELF
`ec5d8a60f...`, 9.4 hours, the last three syncs from the card's log): **the RTC loses 20.8
ppm** (±2.0 hour to hour, ~54 s a month -- the ±20 ppm ESTIMATE holds); each write lands
within ±12 ms with the learned delay, which varies 986-1,000 ms from start to start (not the
±1-2 ms first claimed); on time throughout, no warning. A power cut of 6.5 hours the same day
cost the RTC 2.6 s beyond its drift, VL clear -- the cell's slower oscillator or a moment's
stop, not yet told apart. The card's log of that night also gave ADR 0026's delta its first
board figures: about 190 redraws a day, 135 for IAQ, 8 full, 13 late BSEC calls, against
921-1,568, 884-1,494, 44-74 and 167-292 before. Still owed (`board_notes.md`, open item "Time from the network
on the board (phase 4f)"): a day and another temperature, a cold boot after `rtc_stop`, the
chip's marking, the autumn change; a default delay of ~995 ms is the owner's to choose.

Phase 4g on branch `phase-4g-board-mqtt` (ADR 0033): MQTT on the board.
`platform/esp32_t5s3/adp_mqtt.c` is `port_telemetry` over esp-mqtt (ESP-IDF 5.5's, Apache-2.0).
Its source showed what its header did not: every call into a client takes the client's lock,
`esp_mqtt_client_enqueue()` included, and esp-mqtt's task holds that lock while it waits for the
network -- the name lookup, the TCP handshake, the CONNACK, a write into a dead link, up to
its network timeout each. So the event loop calls none of it: `publish()` and `subscribe()`
copy into a ring of 16 slots in PSRAM, `connect()` and `disconnect()` record what is asked,
and a task of the adapter's own (`hac_mqtt`, 6 KB, priority 4, core 0) alone calls esp-mqtt --
`esp_mqtt_client_enqueue()` with `store` set, never `esp_mqtt_client_publish()`;
`subscribe_single()`; a client made, started, stopped and destroyed per session, its
reconnection off, its outbox bounded at 4 KB (QoS 1 waits there for PUBACK, internal RAM), so
nothing of one session is resent in the next. The ring's indices move under a spinlock, a slot
is filled and emptied outside it. esp-mqtt's events (on its task, pinned to core 0 by
`sdkconfig.defaults`) set the session's state and fill an inbox of 8, and wake the loop
(`event_loop_post_network()`); clients are numbered, and an event from one let go changes
nothing. With `port_net` given, `connect()` needs the link and a session whose network went is
over at the next call. Decisions in `adp_mqtt_pure.c`, tested on the host
(`tests/unit/test_adp_mqtt_pure.c`: the URI, the rings, a message in pieces cut at 256, the
session and what the task does next); numbers in `adp_mqtt_limits.h`. Plain MQTT on the LAN:
`mqtts://` refused, esp-mqtt's TLS and websocket transports off (`adp_mqtt.c` refuses an
sdkconfig with them); the ADR has what TLS would cost (an Mbed TLS session in internal RAM) and
what the owner accepts without it (the password in clear text on the LAN). Configuration:
`CONFIG_HAC_MQTT_BROKER_URI`, `_USER`, `_PASSWORD`, `_TOPIC_PREFIX` (`homeair`) in
`components/hac_platform/Kconfig`, set in `sdkconfig.credentials`, checked by
`cmake/credentials_idf.cmake` with the network's; empty broker, no MQTT and nothing of it
linked. `main.c`'s `start_telemetry()`: device id from the MAC, the discovery's device "Home
Air Monitor", model "LILYGO T5 4.7" e-Paper S3", hardware version the silkscreen's, software
version the app description's; the reporter goes to the event loop, so the status bar and the
diagnostics screen show the board's link (ADR 0031). The event loop logs `published in the
hour` (state, heartbeats, discovery, sessions, lost, refused) -- the card keeps INFO. Console
`mqtt`: state, broker, user, published, acknowledged, refused, discarded, queued messages and
bytes, esp-mqtt's outbox, attempts and failures, the event loop's longest call into the port,
both stacks. Test app: the `port_telemetry` contract against the owner's broker, over the
station the `port_net` case shares (`tests/contract/esp32/test_station.c`), client
`contract-<MAC>` under `hac-contract/board-<MAC>`, the drop exercised by taking the station
down, `stall` not exercised, run on core 1 at the event loop's priority; IGNORED without a
broker. Weighed and not made: a port change for "the attempt is over" (ADR 0030's amendment).
Measured in the build, network and broker against network alone: static internal RAM +3,904
bytes, image +124,500 (1.36 MB, 33 % of the partition) -- 54 KB of it Mbed TLS's TLS layer,
linked by esp-tls under the plain TCP transport and never run. Verified on the host (unit,
ASan); the firmware, its BSEC variant and the test app in both variants build from
regenerated sdkconfigs, and the firmware and the test app with a dummy broker. On the board,
2026-10-22 (ELF `6211314dd...`, BSEC; a mosquitto 2.0.21 on a Raspberry Pi Zero W with users
and an ACL, `192.168.1.141`): the first session at the first attempt, `online` and nine
configs; MQTT costs about 10 KB of internal RAM with a session (51,799 free), under the
estimate; the event loop's longest call into the port 138-204 us; stacks with margin
(esp-mqtt's 4,700 free, the adapter's 3,256); a broker restart recovered in 15 s, 10 of them
the reporter's connect timeout on an attempt esp-mqtt knew had failed -- the weighed port
change's first board figure; a `restart` left the will (`status` offline, then online 12 s
later). Still owed (`board_notes.md`, open item "MQTT on the board (phase 4g)"): Home
Assistant on that broker, its restart, Wi-Fi gone for an hour, a day unattended with HA's
long-term statistics -- requirements 16's acceptance -- and the test app.

On branch `ui-screen-change-full-refresh` (ADR 0034), at the owner's request after watching the
board: a change of screen -- a tap, or the diagnostics screen's timeout -- is a full refresh,
always, held by neither the partial-refresh budget nor the panel's minimum full-refresh interval
(a guard, not a measurement); a whole new screen drawn partially kept the old one's outlines and
read badly. It resets the budget; a tap now costs about 3.3 s to the new screen and a late BSEC
call. `UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET` stays, binding nothing at the sensors' pace. And the
diagnostics screen's title no longer carries the bar's link word -- the MQTT line says the same --
nor compares the held link, so its network lines are all as of the drawing and a redraw is never
due for something it does not draw. Verified on the host; not yet seen on the board
(`board_notes.md`, open item "A change of screen as a full refresh").

On the same branch (ADR 0035), after the owner compared LILYGO's firmware on the panel -- about
2 s, eight quick flashes, no trace -- with ours: a full refresh is now the vendor's sequence,
through EPDiy. Its clear (`BOARD_EPD_CLEAR_*`: 4 cycles of 4 pushes towards black and 4 towards
white, rows driven 50 us, from LilyGo-EPD47's `epd_clear()` -- numbers only, that code is GPL-3.0)
is a loop over EPDiy's `epd_push_pixels()` in `adp_display.c`, replacing EPDiy's 66-push
`epd_clear()` -- 1,214 ms on the board against 2,456 -- then `MODE_EPDIY_WHITE_TO_GL16`, the
vendor's fifteen darkening frames: a full refresh about 1.8 s, a change of screen 1.85 s, against
3.4 s before. Partial refreshes no longer use GL16: GL16 re-drives an unchanged grey (L -> L,
towards black and back) in every row and column a partial touches, and the static greys beside a
changed digit went lighter in patches -- with GL16 after the clear too, less. They use a mode of
this project's, "through white" (`platform/esp32_t5s3/epd_white_path.c`, pure, tested on the host,
added to EPDiy's waveform at boot under type `0x12`): a changed pixel whitened in GL16's lightening
phases and darkened in WHITE_TO_GL16's, an unchanged one never driven -- every grey by one path
(ADR 0035, both amendments). Seen on the board 2026-10-22: the owner likes it -- no serious
artifact, only the expected trace around a vanished black digit; partial draws as long as GL16's to
the millisecond, a change of screen 1,844-1,865 ms. Still to see: a day of it, greys a partial
redraws often, ghosting against the budget (`board_notes.md`, open item "The vendor's full
refresh").

Next: the board run of 4e to 4g -- a broker in the overlay, Home Assistant and the acceptance
day; the board run of phases 3d to 3f, the rest of phase 3c's measurements -- now recordable
unattended -- and this unit's own figures for the remaining estimates; the rest of the
simulator's checks in Home Assistant (ADR 0030's amendment).

The phase order is in section 10 of `docs/architecture.md`. Do not run ahead to later phases
unless asked. Note that phase numbers here refer to those branches; the "phase 2c" named in
`docs/hardware/board_notes.md` and in the e-paper emulation constants is the panel
calibration branch.