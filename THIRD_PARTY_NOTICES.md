# Third-party components

This project is licensed under Apache-2.0 (see `LICENSE`). It builds against the
following third-party components, each under its own license. Update this file
whenever a dependency is added or removed.

| Component | Version | License | Used in | Distributed in this repo |
|---|---|---|---|---|
| ESP-IDF | 5.x | Apache-2.0 | `apps/firmware_esp32`, `platform/esp32_t5s3` | No (external toolchain) |
| linenoise (inside ESP-IDF's `console` component) | ESP-IDF 5.5's copy | BSD-2-Clause | `apps/firmware_esp32/main/console.c` (the REPL's line editor) | No — part of ESP-IDF; see below |
| argtable3 (inside ESP-IDF's `console` component) | 3.2.2 | BSD-3-Clause, plus the BSD/Tcl-style notices of code it bundles | `apps/firmware_esp32/main/console.c` (command parsing, `help`) | No — part of ESP-IDF; see below |
| FatFs (ChaN, inside ESP-IDF's `fatfs` component) | R0.15 w/patch2, ESP-IDF 5.5's copy | FatFs licence (one-clause BSD-style) | `platform/esp32_t5s3/adp_sd.c` (the microSD card's filesystem, phase 3f) | No — part of ESP-IDF; see below |
| Espressif's Wi-Fi and PHY libraries (`libnet80211`, `libpp`, `libphy` and their kin, ESP-IDF's `esp_wifi/lib` and `esp_phy/lib`) | ESP-IDF 5.5's (`esp_wifi/lib` at `8a1b7bbc00e895d040c5c9a6fb9d1db2bbfc7958`) | Apache-2.0, as object code | `platform/esp32_t5s3/adp_net.c` (the Wi-Fi station, phase 4e) -- only in an image built with a network | No — part of ESP-IDF; see below |
| lwIP (inside ESP-IDF's `lwip` component) | 2.2.0, ESP-IDF 5.5's copy | BSD-3-Clause | the same: the station's TCP/IP, through `esp_netif`; since phase 4f also its SNTP client and DNS resolver (`apps/firmware_esp32/main/net_time.c`) | No — part of ESP-IDF; see below |
| wpa_supplicant (inside ESP-IDF's `wpa_supplicant` component) | ESP-IDF 5.5's copy | BSD-3-Clause | the same: WPA2/WPA3's handshakes | No — part of ESP-IDF; see below |
| Mbed TLS (inside ESP-IDF's `mbedtls` component) | 3.6.3 | Apache-2.0 OR GPL-2.0-or-later -- **used under Apache-2.0** | the same: the supplicant's cryptography; since phase 4g, in an image with a broker, also its TLS layer and X.509, linked by `esp-tls` and never run (see below) | No — part of ESP-IDF; see below |
| esp-mqtt (ESP-IDF's `mqtt` component, `components/mqtt/esp-mqtt` at `cac1552e62b0474c162547b7cce345d7cd1aecfe`) | ESP-IDF 5.5's copy | Apache-2.0 | `platform/esp32_t5s3/adp_mqtt.c` (the MQTT client, phase 4g) -- only in an image built with a broker | No — part of ESP-IDF; see below |
| http_parser (inside ESP-IDF's `http_parser` component, linked by esp-mqtt for broker URIs) | 2.7.0, ESP-IDF 5.5's copy | MIT (Joyent, Inc. and other Node contributors; based on NGINX's parser, Igor Sysoev) | the same | No — part of ESP-IDF; see below |
| TinyUSB (`espressif/tinyusb` 0.21.0~2 from Espressif's component registry: `hathach/tinyusb` as packaged by Espressif, `espressif/tinyusb` at `894ea01409e0407a7dbe0ee29b2d58a4691f9046`) | 0.21.0~2 | MIT (Copyright (c) 2012-2026, hathach, tinyusb.org) | `platform/esp32_t5s3/usb_disk.c` (the card as a USB disk, ADR 0036) -- linked into the firmware only | No — fetched as a managed component; see below |
| EPDiy (`AdaSzi/epdiy` fork, tag `2.1.3-t5s3.1`, commit `161de5275f615b58e4a4f614f24a10260430239c`) | 2.1.3 | **LGPL-3.0-or-later** | `platform/esp32_t5s3/adp_display.c` | No — fetched as a managed component (see below) |
| FastEPD | — | Apache-2.0 — **no longer a dependency, removed** | nowhere | No — see below |
| Xinyuan-LilyGO/LilyGo-EPD47 | — | **GPL-3.0 — not a dependency** | nowhere (read, never linked) | No — see below |
| Sensirion `embedded-i2c-scd4x` (tag `1.2.0`, commit `d953712213957544b5429542381fd7629cbb3786`) | 1.2.0 | BSD-3-Clause | `drivers/scd4x/scd4x_source.c` (through `drivers/scd4x/scd4x_hal.c`) | Yes — vendored unmodified in `drivers/scd4x/vendor/`; see below |
| Bosch BME68x Sensor API (tag `v4.4.8`, commit `80ea120a8b8ac987d7d79eb68a9ed796736be845`) | 4.4.8 | BSD-3-Clause | `drivers/bme68x/bme68x_source.c` (through `drivers/bme68x/bme68x_hal.c`) | Yes — vendored unmodified in `drivers/bme68x/vendor/`; see below |
| Bosch BSEC2 library (`libalgobsec.a`), its headers and the `bme680_iaq_33v_3s_4d` configuration | 2.6.1.0 | **Proprietary — Bosch Sensortec license agreement; headers and configuration marked "Confidential"** | `drivers/bme68x/bme68x_bsec_source.c`, only in a build with `CONFIG_HAC_BSEC` / `HAC_BSEC` (off by default) | **No — nothing of it, not even the headers; see below** |
| SDL2 | 2.x | Zlib | `platform/host/adp_display_sdl.c`, `platform/host/adp_input_sdl.c`, later `platform/host/sim_gui` | No (system package; optional, see below) |
| Eclipse Mosquitto client (`libmosquitto`) | 2.0.x, 2.0 or later (the build checks); run with 2.0.22 (Ubuntu 26.04) and CI's | EPL-2.0 or EDL-1.0 -- **used under EDL-1.0** (BSD-3-Clause) | `platform/host/adp_telemetry_mqtt.c`: the simulator's `--broker`, and two host tests | No -- system package, linked dynamically, host only, optional; never in a firmware image (see below) |
| Eclipse Mosquitto broker (`mosquitto`, `mosquitto_passwd`) | the same source package as the client | EPL-2.0 or EDL-1.0 | `tests/`: a private broker the `port_telemetry` contract and `tests/cli/simulator_broker.py` start | No -- run as a separate process by tests, never linked (see below) |
| Unity | 2.x | MIT | `tests/` | Vendored |
| DejaVu Sans | 2.35 | Bitstream Vera (permissive; see below) | `core/ui/fonts/font_dejavu_sans_*.generated.c` | Vendored (source TTF under `third_party/fonts/dejavu-sans/`) |
| Pillow | any recent | MIT-CMU (HPND) | `apps/tools/image_gen/image_gen.py prepare`, `apps/tools/font_gen/generate_fonts.py` — developer tools run by hand | No — not linked into anything; see below |

## Notes on Sensirion's SCD4x driver

Phase 3a (`docs/adr/0015-scd4x-driver.md`) vendors Sensirion's own C driver
for the SCD4x CO2 sensors, `Sensirion/embedded-i2c-scd4x`, at tag `1.2.0`
(commit `d953712213957544b5429542381fd7629cbb3786`, 2025-07-01), into
`drivers/scd4x/vendor/`: `scd4x_i2c.c/.h`, `sensirion_common.c/.h`,
`sensirion_config.h`, `sensirion_i2c.c/.h` and `sensirion_i2c_hal.h`, each
byte-for-byte as upstream, with upstream's `LICENSE`. Its HAL template is not
vendored; `drivers/scd4x/scd4x_hal.c`, this project's own (Apache-2.0),
implements the header. `drivers/scd4x/vendor/README.md` lists the files and
their SHA-256 hashes.

The licence is **BSD-3-Clause** (Copyright (c) 2025, Sensirion AG; some
files carry 2018 and 2019 notices of the same terms), which was checked in
the repository at that commit. It is permissive: no copyleft term reaches the
rest of the work. It adds two obligations. Source redistribution -- this
repository -- must keep the notices, which the vendored files and `LICENSE`
do untouched. Binary redistribution -- a firmware image given to someone
else -- must reproduce the copyright notice, the conditions and the
disclaimer in its documentation, as the console's linenoise and argtable3
already require (below). The third clause forbids using Sensirion's name to
promote this project.

## Notes on Bosch's BME68x Sensor API

Phase 3b (`docs/adr/0016-bme68x-driver.md`) vendors Bosch Sensortec's own C
driver for the BME680 and BME688, `boschsensortec/BME68x_SensorAPI`, at tag
`v4.4.8` (commit `80ea120a8b8ac987d7d79eb68a9ed796736be845`, 2023-05-17), into
`drivers/bme68x/vendor/`: `bme68x.c`, `bme68x.h` and `bme68x_defs.h`, each
byte-for-byte as upstream, with upstream's `LICENSE`. Bosch ships no HAL to
replace: the API takes its bus and delay as callbacks, which
`drivers/bme68x/bme68x_hal.c`, this project's own (Apache-2.0), supplies.
`drivers/bme68x/vendor/README.md` lists the files and their SHA-256 hashes.

The licence is **BSD-3-Clause** (Copyright (c) 2023 Bosch Sensortec GmbH),
checked in the repository at that commit. Its obligations are the same as
Sensirion's above: keep the notices in source, which the vendored files and
`LICENSE` do untouched; reproduce the copyright notice, conditions and
disclaimer in the documentation of a firmware image given to someone else;
and do not use Bosch's name to promote this project.

This is the open-source half of Bosch's offer and nothing more: it reads the
sensor and compensates temperature, humidity, pressure and gas resistance.
It computes no IAQ. That is BSEC's, below, linked only in a build that asks
for it; without it the firmware reports raw gas resistance, as the BSEC note
says.

The calibration set the host's BME680 model starts from
(`platform/host/i2c_models/bme680_model.c`) is a real sensor's, published in
`pimoroni/bme680-python` (MIT), `tests/conftest.py` at commit
`30fcb34efc2d19f60a63ece656b9059adb373150`, with that project's test vectors
for it. They are numbers read off a sensor, cited at their use; no code is
taken from that project.

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

## Notes on FatFs, the microSD card's filesystem

Phase 3f (`docs/adr/0024-sd-card-recordings-and-debug-log.md`) mounts the
board's microSD card through ESP-IDF's `fatfs` component, which links **FatFs**
by ChaN (`components/fatfs/src/`, "FatFs - Generic FAT Filesystem Module R0.15
w/patch2", Copyright (C) 2022, ChaN) into the firmware for the first time --
confirmed in `home_air_monitor.bin`'s size-components (`libfatfs.a`). Checked
in `~/esp/esp-idf/components/fatfs/src/ff.h`: its licence permits
redistribution and use in source and binary forms, with or without
modification, on one condition -- that **redistributions of source code**
retain the copyright notice, the condition and the disclaimer. A binary
carries no obligation under it, and nothing of FatFs is vendored here. No
copyleft term is involved. The rest of what the card pulls in -- the SD
protocol (`sdmmc`), SD over SPI (`esp_driver_sdspi`), the SPI master driver
and the VFS -- is ESP-IDF's own, under its Apache-2.0 (the first row).

## Notes on the Wi-Fi stack

Phase 4e (`docs/adr/0032-wifi-on-the-board.md`) links ESP-IDF's Wi-Fi into an
image built with a network -- `sdkconfig.credentials` naming one -- and into
no other: without a network the composition never sets the station up and the
linker drops the whole stack, which `idf.py size-components` confirms (no
`libnet80211`, `liblwip`, `libwpa_supplicant` or `libmbedcrypto` in it).
Checked in `~/esp/esp-idf/components` (ESP-IDF 5.5), 2026-10-21:

- **Espressif's Wi-Fi and PHY libraries** come as object code only.
  `esp_wifi/lib/README.rst`: "These binary libraries are provided under the
  same license as the parent esp-idf project - the Apache License 2.0 as
  provided in the file LICENSE", which is Apache-2.0's text; `esp_phy/lib`
  carries the same file.
- **lwIP** 2.2.0 (`lwip/lwip/COPYING`, Swedish Institute of Computer
  Science): BSD-3-Clause. Phase 4f (`docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md`,
  "Amendment (phase 4f)") links two more of its parts into the same image:
  its SNTP client (`lwip/src/apps/sntp/sntp.c`, Copyright (c) 2007-2009
  Frédéric Bernon and Simon Goldschmidt) and its DNS resolver
  (`lwip/src/core/dns.c`, Jim Pettinato's port of Adam Dunkels's uIP
  resolver, Copyright (c) 2002-2003 Adam Dunkels), both under the same
  three-clause BSD terms -- checked 2026-10-21 --
  so the binary notice below gains their copyright lines. ESP-IDF's
  `esp_netif_sntp` and its SNTP glue (`lwip/apps/sntp/sntp.c`) are its own,
  Apache-2.0.
- **wpa_supplicant** (`wpa_supplicant/README`, Jouni Malinen and
  contributors): "the BSD license (the one with advertisement clause
  removed)", three clauses.
- **Mbed TLS** 3.6.3 (`mbedtls/mbedtls/LICENSE`): Apache-2.0 OR
  GPL-2.0-or-later, at the user's choice -- used here under Apache-2.0.

No copyleft term is involved. lwIP's and wpa_supplicant's licences ask that
**redistributions in binary form** reproduce their copyright notices,
conditions and disclaimers in the documentation or other materials provided
with them: an image with a network, if ever distributed, carries those
obligations, as ESP-IDF's other third-party components (linenoise,
argtable3) already ask. Nothing of any of them is vendored here; the rest of
what Wi-Fi pulls in -- `esp_wifi`'s adapter code, `esp_netif`, `esp_event`,
`nvs_flash`, `esp_phy` -- is ESP-IDF's own, under its Apache-2.0 (the first
row).

## Notes on esp-mqtt, the board's MQTT client

Phase 4g (`docs/adr/0033-mqtt-on-the-board.md`) links ESP-IDF's MQTT client
into an image built with a broker -- `sdkconfig.credentials` naming one beside
the Wi-Fi network -- and into no other: without a broker the composition never
starts the client and the linker drops it (no `libmqtt` in such an image's
`idf.py size-components`). Checked in `~/esp/esp-idf/components` (ESP-IDF 5.5),
2026-10-22:

- **esp-mqtt** (`mqtt/esp-mqtt/LICENSE`): Apache-2.0, Espressif's own, the
  same terms as ESP-IDF.
- **tcp_transport** and **esp-tls**, through which esp-mqtt's plain TCP
  transport connects: ESP-IDF's own, `SPDX-License-Identifier: Apache-2.0`.
  Their plain-TCP path tears a connection down through esp-tls, which
  references Mbed TLS's TLS layer and X.509 code: about 54 KB of it is linked
  and never run, since TLS is left out (`CONFIG_MQTT_TRANSPORT_SSL` off; the
  ADR has the measurement). Mbed TLS stays under Apache-2.0, as above.
- **http_parser** 2.7.0 (`http_parser/LICENSE.txt`, `sbom.yml`): MIT --
  "Copyright Joyent, Inc. and other Node contributors", based on NGINX's HTTP
  parser by Igor Sysoev, "licensed under the same terms as NGINX". esp-mqtt
  links it to parse broker URIs; the adapter hands esp-mqtt a host and a port
  instead, but the reference stays.

No copyleft term is involved. http_parser's MIT terms ask that its copyright
and permission notice be included with copies of it: an image with a broker,
if ever distributed, carries that notice beside the others ESP-IDF's
third-party code already asks for. Nothing is vendored here.

## Notes on TinyUSB, the card's USB disk

ADR 0036 (`docs/adr/0036-the-card-as-a-usb-disk.md`) presents the microSD
card to a PC as a USB mass-storage device through TinyUSB, Espressif's
packaging of Ha Thach's `hathach/tinyusb` for ESP-IDF: the registry's
`espressif/tinyusb` 0.21.0~2, built from `espressif/tinyusb` at commit
`894ea01409e0407a7dbe0ee29b2d58a4691f9046` (the package's own
`idf_component.yml`), pinned exactly in
`apps/firmware_esp32/components/hac_platform/idf_component.yml` and fetched
into the gitignored `managed_components/`. Nothing of it is vendored here.

The licence was read in the package itself, 2026-10-22: **MIT**, "Copyright
(c) 2012-2026, hathach (tinyusb.org)". The registry labels this version's
licence "Custom" -- its file is the MIT text unchanged. Permissive: no
copyleft term reaches the rest of the work. A firmware image given to someone
else must carry the copyright notice and the permission notice, beside the
ones ESP-IDF's third-party code already asks for. TinyUSB's library sources
include the RNDIS reports under `lib/networking/` (the same MIT terms), which
the component compiles and nothing here links.

Not used: Espressif's `esp_tinyusb` (Apache-2.0), the layer above TinyUSB
that the registry pairs with it -- the ADR says why. This project's
`platform/esp32_t5s3/tinyusb/tusb_config.h` is its own, not esp_tinyusb's.

The USB IDs are Espressif's: vendor 0x303A and the product ID esp_tinyusb
gives a TinyUSB device with mass storage alone (ADR 0036, "Identity"),
suitable for a device never sold.

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

**BSEC is unaffected.** The proprietary Bosch BSEC2 binary, linked since
phase 3d in a build that asks for it (`docs/adr/0020-bsec-iaq.md`), can
coexist with LGPL-3 in one image, because only the EPDiy part must stay
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
links in a build with BSEC (see below): BSEC's license does not permit GPL-3.0
terms on the surrounding work, and GPL-3.0 does not permit a proprietary
binary in the same image. The two cannot coexist in one firmware build.

Anyone adding a "driver" dependency to this project later should check this
section before reaching for the vendor's own repository, since it is the one
that most resembles a ready-made answer and is also the one this project
must not link.

## Notes on BSEC / BSEC2

Bosch's BSEC2 is **not** in this repository, in any part, and must never be
committed (`third_party/bsec/` and `libalgobsec.a` are gitignored). It is
downloaded from Bosch Sensortec after accepting their licence agreement:

<https://www.bosch-sensortec.com/software-tools/software/bme680-software-bsec/>

Phase 3d (`docs/adr/0020-bsec-iaq.md`) was written against **BSEC 2.6.1.0**, the
archive `bsec2-6-1-0_generic_release_22102024` (its release notes dated
2024-07-29), downloaded and its terms accepted by the owner on 2026-10-16. Unpack the archive, as it comes, into `third_party/bsec/`, or
point `HAC_BSEC_DIR` at it; `cmake/bsec.cmake` names the files the builds use:

- `algo/bsec_IAQ/bin/esp/esp32_s3/libalgobsec.a` for the board, and
  `algo/bsec_IAQ/bin/gcc/Linux/m64/libalgobsec.a` for the host tests;
- `algo/bsec_IAQ/inc/` -- `bsec_datatypes.h`, `bsec_interface.h`,
  `bsec_interface_multi.h`;
- `algo/bsec_IAQ/config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.c` and `.h`,
  the configuration, compiled where it lies.

**What the licence covers, as checked in that archive.** It holds no licence
file: the terms are the agreement accepted on Bosch's site. The headers, the
configuration files and the integration examples all carry Bosch's own
header -- "Copyright (C) Bosch Sensortec GmbH. All Rights Reserved.
Confidential." -- with a disclaimer, and "It is not allowed to deliver the
source code of the Software to any third party without permission of Bosch
Sensortec." An earlier revision of this file listed "BSEC2 headers & config"
as BSD-3-Clause, in a `drivers/bme68x/bsec` that never existed. That is not
what this archive says, and it was corrected when the archive arrived: no
header, no configuration array and no example code from it is copied,
quoted or vendored here. This project's own code that calls BSEC
(`drivers/bme68x/bme68x_bsec_source.c`) is Apache-2.0 like the rest, and is
useless without the archive.

**Builds.** Off by default (`CONFIG_HAC_BSEC`, `HAC_BSEC`), so CI and a fresh
clone build without it, and then the firmware reports raw gas resistance in
place of IAQ, as the requirements recommend (§17.2): `bme68x_source` measures
it and the home screen shows it in the IAQ cell
(`docs/adr/0016-bme68x-driver.md`). With it, the BME68x is measured as BSEC
asks and the cell shows BSEC's IAQ.

**Copyleft.** None of the image's other dependencies stands in the way of
linking BSEC: EPDiy is LGPL-3.0, which constrains only the relinkability of
EPDiy itself (above); LilyGo-EPD47, GPL-3.0, is never linked (above) --
which is exactly what keeps this combination possible.

**Distribution.** A firmware image built with BSEC contains Bosch's
proprietary object code. It must not be attached to a GitHub Release, or
given to anyone, without first confirming that Bosch's terms permit that
form of redistribution.

## Notes on DejaVu Sans

DejaVu Sans is a superset/derivative of the Bitstream Vera fonts, released
under a permissive license that allows copying, modifying and embedding
(including in a commercial product) provided the copyright notice is kept and
any modified font is renamed away from "Bitstream" or "Vera" — DejaVu already
satisfies that by construction. The full text is vendored at
`third_party/fonts/dejavu-sans/LICENSE`.

`core/ui/fonts/font_dejavu_sans_<size>.generated.c` -- 16, 32, 40, 48, 72 and
160 px -- are a rasterized subset (the printable ASCII range only, at fixed
pixel sizes) produced from the vendored TTF by
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

## Notes on libmosquitto and the mosquitto broker

Phase 4c (`docs/adr/0030-telemetry-ports-and-the-reporter.md`, amendment "The
simulator on a real broker") links Eclipse Mosquitto's client library,
**libmosquitto**, into the simulator, so that it can publish to a real MQTT
broker -- a Home Assistant's -- and into the `port_telemetry` contract's host
runner. The tests also run the **mosquitto** broker itself.

**The licence, checked** in the Ubuntu 26.04 package `libmosquitto1`
2.0.22-5build1 (its `copyright` file: "Files: * ... License: EPL-2.0 or
EDL-1.0", Copyright 2009-2025 Roger A. Light) and in the upstream sources of
2.0.11, 2.0.18 and 2.0.22 (`mosquitto.org/files/source/`), whose files carry
the same dual licence (`SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause`). It is a choice: Eclipse Public License 2.0 **or**
Eclipse Distribution License 1.0. **This project uses libmosquitto under
EDL-1.0**, which is the BSD-3-Clause text with the Eclipse Foundation named
in its third clause. The EPL-2.0 alternative, a weak copyleft, is not taken,
so no copyleft term reaches anything here. Bundled in the library and named
in the same file: `uthash.h`/`utlist.h` (BSD-1-Clause) -- permissive too.

**Where it is used, and where not.** Host only. It is a system package
(`libmosquitto-dev` to build), found at configure time
(`platform/host/CMakeLists.txt`, `SIM_MQTT`, 2.0 or later) and linked
dynamically into the simulator and `test_port_telemetry_contract`. Without it
both build and run as before, with no `--broker`. Nothing of it is vendored,
and it is never part of a firmware image: the board's MQTT client is
ESP-IDF's esp-mqtt (Apache-2.0, since phase 4g; its own notes above), and
`platform/esp32_t5s3/` never includes libmosquitto.

**What EDL-1.0 obliges.** Source redistribution keeps the notices -- nothing
of its source is in this repository. Binary redistribution reproduces the
copyright notice, the conditions and the disclaimer in its documentation; it
arises only if a simulator binary is ever handed to someone, and then carries
these notices, as the console's linenoise and argtable3 do for a firmware
image (above). The third clause forbids using the Eclipse Foundation's name
to promote this project.

**The broker** (`mosquitto`, and `mosquitto_passwd` for a test with a
password file) comes from the same source package under the same dual
licence. The tests run it as a separate process on a free port of 127.0.0.1
(`tests/support/mqtt_test_broker.c`, `tests/cli/simulator_broker.py`); it is
never linked, and no obligation of it reaches anything this repository
builds. CI installs both packages (`libmosquitto-dev`, `mosquitto`) in its
host and ASan jobs; a machine without them skips those cases and says so.

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
