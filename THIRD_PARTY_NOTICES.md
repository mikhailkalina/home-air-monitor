# Third-party components

This project is licensed under Apache-2.0 (see `LICENSE`). It builds against the
following third-party components, each under its own license. Update this file
whenever a dependency is added or removed.

| Component | Version | License | Used in | Distributed in this repo |
|---|---|---|---|---|
| ESP-IDF | 5.x | Apache-2.0 | `apps/firmware_esp32`, `platform/esp32_t5s3` | No (external toolchain) |
| linenoise (inside ESP-IDF's `console` component) | ESP-IDF 5.5's copy | BSD-2-Clause | `apps/firmware_esp32/main/console.c` (the REPL's line editor) | No — part of ESP-IDF; see below |
| argtable3 (inside ESP-IDF's `console` component) | 3.2.2 | BSD-3-Clause, plus the BSD/Tcl-style notices of code it bundles | `apps/firmware_esp32/main/console.c` (command parsing, `help`) | No — part of ESP-IDF; see below |
| EPDiy (`AdaSzi/epdiy` fork, tag `2.1.3-t5s3.1`, commit `161de5275f615b58e4a4f614f24a10260430239c`) | 2.1.3 | **LGPL-3.0-or-later** | `platform/esp32_t5s3/adp_display.c` | No — fetched as a managed component (see below) |
| FastEPD | — | Apache-2.0 — **no longer a dependency, removed** | nowhere | No — see below |
| Xinyuan-LilyGO/LilyGo-EPD47 | — | **GPL-3.0 — not a dependency** | nowhere (read, never linked) | No — see below |
| Sensirion SCD4x embedded driver | — | BSD-3-Clause | `drivers/scd4x` | Vendored |
| Bosch BME68x Sensor API | — | BSD-3-Clause | `drivers/bme68x` | Vendored |
| Bosch BSEC2 headers & config | 2.x | BSD-3-Clause | `drivers/bme68x/bsec` | Headers only |
| Bosch BSEC2 library binary (`libalgobsec.a`) | 2.x | **Proprietary — Bosch Sensortec license agreement** | optional IAQ backend | **No — see below** |
| SDL2 | 2.x | Zlib | `platform/host/adp_display_sdl.c`, `platform/host/adp_input_sdl.c`, later `platform/host/sim_gui` | No (system package; optional, see below) |
| Eclipse Mosquitto client (`libmosquitto`) | 2.x | EPL-2.0 / EDL-1.0 | `platform/host/adp_telemetry_mqtt.c` | No (system package) |
| Unity | 2.x | MIT | `tests/` | Vendored |
| DejaVu Sans | 2.35 | Bitstream Vera (permissive; see below) | `core/ui/fonts/font_dejavu_sans_*.generated.c` | Vendored (source TTF under `third_party/fonts/dejavu-sans/`) |
| Pillow | any recent | MIT-CMU (HPND) | `apps/tools/image_gen/image_gen.py prepare`, `apps/tools/font_gen/generate_fonts.py` — developer tools run by hand | No — not linked into anything; see below |

## Notes on the console's linenoise and argtable3

Phase 2d's serial console (`docs/adr/0011-i2c-bus-adapter.md`) uses ESP-IDF's
`esp_console` REPL, which links two third-party libraries ESP-IDF ships inside
its `console` component: **linenoise** (Salvatore Sanfilippo and Pieter
Noordhuis, BSD-2-Clause) and **argtable3** (Stewart Heitmann, BSD-3-Clause;
its `LICENSE` also carries the notices of the NetBSD getopt, Tcl regex, C hash
table and Better String code it bundles, all permissive). Both were checked in
`~/esp/esp-idf/components/console/` and both are confirmed linked into
`home_air_monitor.bin` by its map file.

Nothing is vendored here and the source obligations are ESP-IDF's. What they
add is a **binary** one: a firmware image given to someone else must carry
these copyright notices and disclaimers, as the ESP-IDF image already must for
the rest of ESP-IDF's third-party code. No copyleft term is involved.

## Notes on the EPDiy display backend, and its LGPL-3.0 obligations

`docs/adr/0007-epaper-backend.md` carries the full history: EPDiy was this
project's original choice, was reversed in favour of FastEPD after reading
upstream's source, and was then chosen again after four rounds of real
hardware evidence. This section records the outcome and what the licence
obliges, not the reasoning.

**Which EPDiy.** Not `vroland/epdiy` upstream, whose maintainers declined to
support this board: its only ESP32-S3 render method streams whole frames
through the LCD_CAM peripheral and needs the latch (LEH) and vertical start
(STV) lines on real GPIOs, while this board routes LEH, STV, output enable,
mode and the panel power rails through a 74HCT4094 shift register. The
dependency is `AdaSzi/epdiy` at tag `2.1.3-t5s3.1`, a fork that adds the
row-by-row render method for the S3 — latching each row through the board's
`set_ctrl()` callback, with the row's pixels sent over LCD_CAM in i80 mode —
behind `CONFIG_EPD_ESP32S3_I80_ROW_OUTPUT`, and that carries
`src/board/epd_board_lilygo_t5_47_s3.c` for this exact board. Pinned in
`apps/firmware_esp32/components/hac_platform/idf_component.yml`; bump that pin
deliberately, since a fork's newer tag is not a routine upgrade.

**Fetched, not vendored.** EPDiy is a proper ESP-IDF component with its own
`idf_component.yml` and `CMakeLists.txt`, so the component manager fetches it
into `apps/firmware_esp32/managed_components/`, which is gitignored. No EPDiy
source is committed to this repository. The licence obligations below attach
to the **firmware image**, which does contain EPDiy's object code, not to this
repository's contents.

**LGPL-3.0-or-later, and what it requires here.** EPDiy's firmware sources are
under the GNU Lesser General Public License version 3 (its own `LICENSE` is
the full LGPL-3 text; its `idf_component.yml` declares
`LGPL-3.0-or-later`). Upstream additionally places its utilities under MIT and
its hardware designs under CC BY-SA 4.0; neither is used here.

This project stays **Apache-2.0**. No first-party file changes licence. LGPL is
a weak copyleft: it requires that *the EPDiy portion* remain replaceable, and
does not reach the rest of the work. Statically linking it makes the firmware
image a "combined work" under LGPL-3 §4, which permits conveying that image
under terms of our choosing provided the work carries prominent notice of the
library's use (this file), is accompanied by copies of the LGPL and GPL texts,
and satisfies either §4d0 (the recipient can relink against a modified
library) or §4d1 (a shared-library mechanism). For embedded firmware the route
is §4d0, and it is **already satisfied in practice**: this repository is
public, the build is reproducible from source, the dependency is pinned to an
exact commit, and anyone can rebuild the image against a modified EPDiy.

**One obligation that does not arise today and would if the device were given
away or sold.** LGPL-3 inherits GPL-3 §6's "Installation Information"
requirement for *User Products*. It is triggered by conveying the device to
someone else, not by building or using it. If this hardware is ever handed on,
the recipient must be able to install a modified EPDiy on it — which, given
that the firmware is flashed over USB with publicly available tooling, is a
documentation task rather than a design change. Recorded here so it is a known
condition rather than a discovery.

**BSEC is unaffected.** The proprietary Bosch BSEC2 binary planned for phase 3
can coexist with LGPL-3 in one image, because only the EPDiy part must stay
relinkable. This is precisely why this project accepts LGPL-3 and refuses
GPL-3 (see the LilyGo-EPD47 note below, where the whole image would be
covered and the two could not coexist).

**The exit, if a closed-source build is ever needed**, is what `CLAUDE.md`'s
licensing section has always claimed: one adapter file plus a pin table. That
is now literally true rather than aspirational — EPDiy sits behind
`ports/port_display.h`, and `platform/esp32_t5s3/adp_display.c` is the single
file that knows it exists. Nothing in `core/`, the UI, `update_policy` or any
reporter is touched by the choice.

## Notes on FastEPD — used, then removed

`bitbank2/FastEPD` (Apache-2.0) was this project's display backend through
phase 2b and is no longer a dependency. Its vendored copy under
`third_party/fastepd/`, the `hac_fastepd_vendor` component and the
`fastepd_shim.c` vendor boundary are all removed.

It was not removed over licence or quality: Apache-2.0 was the easier licence
of the two, and it drove the panel's shift register, power sequencing and
waveform passes correctly. It was removed because its ESP32-S3 output model
sends each row as an independent `esp_lcd_panel_io_tx_color()` transaction with
STH, LE and CKV toggled by software around it, and this panel needs those
signals generated by hardware on a continuous clock train. That is a
structural property of the backend rather than a setting, as four rounds of
hardware evidence established — see `docs/adr/0007-epaper-backend.md`.

Nothing of FastEPD remains in the image; `git log` is the only place it still
exists.

## Notes on LilyGo-EPD47 — read for hardware facts, never linked

`Xinyuan-LilyGO/LilyGo-EPD47` (branch `esp32s3`) is the vendor's reference
firmware for this exact board, and `docs/hardware/board_notes.md`,
`platform/esp32_t5s3/board_config.h` and the display adapter's history cite
it repeatedly — for the panel orientation, the GRAY4
nibble order (`docs/adr/0003`), every GPIO pin this project uses, and the
shift-register/ROW_START timing facts behind this board's FastEPD
callbacks. Its `LICENSE` file (branch `esp32s3`) is the plain **GNU GPL
v3**, not LGPL — its own README says as much, and its driver sources
(`epd_driver.c`, `ed047tc1.c`) carry no per-file header that would soften
that.

Reading that repository to learn pin numbers, a panel resolution, a byte
order, a shift-register bit assignment, or a working pixel-clock value is
fine and stays fine: those are facts about hardware, not copyrightable
expression, and this project has done exactly that since phase 1c. **Linking
its code, or copying its driver logic into a source file this project
compiles, is not.** GPL-3.0 is viral at the whole-image level for a
statically linked embedded firmware — pulling this repository in as a
component would put `core/`, the UI and every reporter under GPL-3.0, and it
would collide outright with the proprietary Bosch BSEC binary this project
already plans to link (see below): BSEC's license does not permit GPL-3.0
terms on the surrounding work, and GPL-3.0 does not permit a proprietary
binary in the same image. The two cannot coexist in one firmware build.

Anyone adding a "driver" dependency to this project later should check this
section before reaching for the vendor's own repository, since it is the one
that most resembles a ready-made answer and is also the one this project
must not link.

## Notes on BSEC / BSEC2

The BSEC binary library is **not** redistributed in this repository. It is only
available for download after accepting Bosch Sensortec's license agreement:

<https://www.bosch-sensortec.com/software-tools/software/bme680-software-bsec/>

To build with IAQ support, download the archive, accept the terms, and place the
static library and configuration under `third_party/bsec/` (gitignored). Without
it, the firmware falls back to reporting raw gas resistance and a simple VOC
indicator, as recommended in the requirements document.

Pre-built firmware binaries containing BSEC must not be attached to GitHub
Releases without first confirming that Bosch's terms permit that form of
redistribution.

## Notes on DejaVu Sans

DejaVu Sans is a superset/derivative of the Bitstream Vera fonts, released
under a permissive license that allows copying, modifying and embedding
(including in a commercial product) provided the copyright notice is kept and
any modified font is renamed away from "Bitstream" or "Vera" — DejaVu already
satisfies that by construction. The full text is vendored at
`third_party/fonts/dejavu-sans/LICENSE`.

`core/ui/fonts/font_dejavu_sans_16.generated.c`, `_32.generated.c` and
`_64.generated.c` are a rasterized subset (the printable ASCII range only, at
three fixed pixel sizes) produced from the vendored TTF by
`apps/tools/font_gen/generate_fonts.py`; regenerate and commit the result if the
sizes, the covered range, or the source font change.

## Notes on Pillow

Pillow (the Python imaging library) is used only by two scripts a developer
runs by hand: `font_gen` to rasterize the vendored font, and `image_gen.py
prepare` to turn a photo into a panel-ready PNG
(`docs/adr/0012-image-pipeline.md`). Neither the firmware nor the simulator
links it, and no build step needs it: `image_gen.py embed`, which the builds
run, uses the Python standard library alone. Its outputs carry no Pillow
code, so it adds no obligation to anything this repository ships.

The synthetic pictures in `apps/slideshow/images/` are drawn by
`image_gen.py demo` itself and are this project's own, under Apache-2.0.

## Notes on SDL2

SDL2 is an optional build dependency, needed only for the simulator's window
and its mouse-to-touch input. It is looked for solely when the build is
configured with `SIM_GUI=ON`; the `ci-headless` preset sets `SIM_GUI=OFF`, and
that build never calls `find_package(SDL2)` at all, so CI and any machine
without SDL2 build the whole project and run the whole test suite. Without it
the simulator still runs, headless, writing PNG frames:

```bash
cmake --preset ci-headless && cmake --build build/host-debug
./build/host-debug/apps/simulator/simulator --scenario-co2 scenarios/co2_spike_meeting.csv --headless --frames out/ --duration 600
```

SDL2 is Zlib-licensed and is linked dynamically from the system package; it is
never redistributed here, and it is never linked into the firmware.
