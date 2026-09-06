# Third-party components

This project is licensed under Apache-2.0 (see `LICENSE`). It builds against the
following third-party components, each under its own license. Update this file
whenever a dependency is added or removed.

| Component | Version | License | Used in | Distributed in this repo |
|---|---|---|---|---|
| ESP-IDF | 5.x | Apache-2.0 | `apps/firmware_esp32`, `platform/esp32_t5s3` | No (external toolchain) |
| EPDiy | v7 | **LGPL-3.0** (firmware); MIT (utilities); CC BY-SA 4.0 (hardware) | `platform/esp32_t5s3/adp_display.c` | No (fetched as component) |
| Sensirion SCD4x embedded driver | — | BSD-3-Clause | `drivers/scd4x` | Vendored |
| Bosch BME68x Sensor API | — | BSD-3-Clause | `drivers/bme68x` | Vendored |
| Bosch BSEC2 headers & config | 2.x | BSD-3-Clause | `drivers/bme68x/bsec` | Headers only |
| Bosch BSEC2 library binary (`libalgobsec.a`) | 2.x | **Proprietary — Bosch Sensortec license agreement** | optional IAQ backend | **No — see below** |
| SDL2 | 2.x | Zlib | `platform/host/adp_display_sdl.c`, `platform/host/adp_input_sdl.c`, later `platform/host/sim_gui` | No (system package; optional, see below) |
| Eclipse Mosquitto client (`libmosquitto`) | 2.x | EPL-2.0 / EDL-1.0 | `platform/host/adp_telemetry_mqtt.c` | No (system package) |
| Unity | 2.x | MIT | `tests/` | Vendored |
| DejaVu Sans | 2.35 | Bitstream Vera (permissive; see below) | `core/ui/fonts/font_dejavu_sans_*.generated.c` | Vendored (source TTF under `third_party/fonts/dejavu-sans/`) |

## Notes on the LGPL-3.0 display backend

EPDiy's driver code is licensed under LGPL-3.0. It is statically linked into the
firmware image. All of this project's source is publicly available, and the build
is reproducible from source, so users can rebuild the firmware against a modified
version of EPDiy.

If closed-source distribution becomes a requirement, replace the display adapter
with a permissively licensed backend. Because the driver sits behind
`ports/port_display.h`, this is a change to a single adapter file and does not
affect `core/`.

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
