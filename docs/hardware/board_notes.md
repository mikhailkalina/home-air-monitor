# Board notes — LILYGO T5 4.7" e-Paper (S3, touch)

Source of truth for `platform/esp32_t5s3/board_config.h`. Update this file
first when something about the physical board is discovered or corrected;
`board_config.h` should cite it in comments rather than duplicate the reasoning.

## Identification

- Product: LILYGO T5 4.7-inch e-Paper, ESP32-S3, touch version (vendor SKU H716)
- Revision printed on the board: **Screen-4.7-S3 v2.4, 2024-12-03**
- Purchase page: https://lilygo.cc/en-us/products/t5-4-7-inch-e-paper-v2-3
- Confirmed working: the vendor's stock Arduino example runs on this unit as received.

Note: LilyGO revises this board without always changing the product page. The
silkscreen revision above is the authority, not the page. If a second unit is
ever purchased, re-check the silkscreen before assuming it matches.

## Reference firmware

- Repository: https://github.com/Xinyuan-LilyGO/LilyGo-EPD47
- Branch: `esp32s3` — **this is an ESP-IDF component, not only an Arduino
  sketch**, which makes it a directly usable reference (or dependency) for
  `platform/esp32_t5s3/adp_display.c`, not just something to reimplement from
  scratch.
- Pin definitions live in `src/utilities.h`, **not** `src/config.h` (an
  earlier, incorrect guess in this file). The header covers several boards in
  one file, split by `CONFIG_IDF_TARGET_ESP32` vs `CONFIG_IDF_TARGET_ESP32S3`
  — read the **S3 block, lines 27–49**; the ESP32 (non-S3) block above it is
  for a different board and will give the wrong pins if copied by mistake.
  The full pin audit is still an open item below — this only records where to
  look.

## Specifications (vendor-confirmed)

| | |
|---|---|
| MCU | ESP32-S3-WROOM-1-N16R8 |
| Flash | 16 MB |
| PSRAM | 8 MB (octal) |
| Wireless | Wi-Fi, Bluetooth 5.0 |
| Display driver IC | ED047TC1 |
| Resolution | **960 × 540 px, landscape** in firmware terms — see “Display orientation” below. The vendor listing quotes 540 × 960, which is the glass orientation, not the one the driver presents |
| Gray levels | 16 (4 bpp, matches `PIXFMT_GRAY4`) |
| RTC | PCF8563, on I²C |
| Battery | onboard capacity/charge detection — chip model not yet confirmed, check schematic or reference firmware |
| Touch | present on this unit (H716 variant) — controller model not yet confirmed; GT911 is assumed in the architecture doc but must be verified against the schematic or reference firmware before writing `adp_input.c` |

## Display orientation

**Resolved 2026-09-05.** The core and the simulator use **960×540 landscape**,
matching `EPD_WIDTH` / `EPD_HEIGHT` in `src/epd_driver.h` on the `esp32s3`
branch of `Xinyuan-LilyGO/LilyGo-EPD47`, commit `391b0e2` (2026-08-19):

- `src/epd_driver.h:28` — `#define EPD_WIDTH 960`
- `src/epd_driver.h:33` — `#define EPD_HEIGHT 540`

Three findings establish this as the panel's physical scan orientation, not a
software convention that could just as easily be flipped:

- **No rotation concept exists anywhere in the reference.** Grepping the
  driver sources (`epd_driver.c/h`, `ed047tc1.c/h`, `utilities.h`,
  `touch.cpp/h`) for `rotat` returns nothing — no rotation enum, no
  `set_rotation()`, no transposing blit.
- **The framebuffer is indexed row-major with x advancing fastest across
  960.** `src/epd_driver.c:338` indexes it as
  `framebuffer[y * EPD_WIDTH / 2 + x / 2]`.
- **The panel is clocked out in 960-pixel lines.** `src/epd_driver.c:161`
  calls `epd_base_init(EPD_WIDTH)`, and the I²S row buffers are sized in units
  of `EPD_WIDTH` throughout.

The vendor's "540 × 960" is the glass's datasheet orientation, not the one the
driver presents — a future reader should not mistake our 960×540 for an error
against that listing. `docs/architecture.md` §5.5 (960×540) is correct as
written and needs no change.

## Buffer layout

`core/ui/framebuffer.c` is dimension-agnostic: `framebuffer_init()` takes
`width` and `height` as runtime arguments, and no panel size is hard-coded
anywhere in `core/`, `ports/`, `drivers/`, `platform/`, `apps/` or `tests/`.
Orientation was never a risk to phase 1a for this reason — it lands in
`board_config.h` in phase 2b and nowhere else.

Two things were checked against the reference and now line up:

- **Stride.** At width 960, our GRAY4 stride is 480 bytes
  (`ceil(960 / 2)`), exactly `EPD_WIDTH / 2`. The adapter needs no row
  padding to reconcile between the two buffers.
- **Nibble order.** The GRAY4 packing now matches the reference (odd pixel in
  the high nibble, even in the low), so `adp_display.c` can `memcpy` a
  `framebuffer_t` straight into the EPDiy buffer instead of converting on
  every flush. See `docs/adr/0003-gray4-nibble-order.md` for the two options
  considered and why matching the reference won.

## Pin assignments

**Resolved 2026-09-06, phase 2a.** Sources: `Xinyuan-LilyGO/LilyGo-EPD47`,
branch `esp32s3`, commit `391b0e25d7a39897e3a00af34053250df031d699`
(2026-08-19) -- the same commit already cited above for the panel
orientation. Every pin is corroborated by two independent places in that
repository: a driver header (with line numbers) and the vendor's own "GPIO
List" table in `README.MD` (written by LilyGO, not reverse-engineered). The
two never disagree. Carried forward as named constants in
`platform/esp32_t5s3/board_config.h`, which cites the same lines.

| Function | GPIO | Source |
|---|---|---|
| EPD `CFG_DATA` / `CFG_CLK` / `CFG_STR` (74HCT4094D shift register, sch. QP5) | 13 / 12 / **0** | `src/ed047tc1.h:48-50` (S3 block) |
| EPD `CKV` / `STH` / `CKH` | 38 / 40 / 41 | `src/ed047tc1.h:53,54,57` |
| EPD data bus `D0..D7` | 8, 1, 2, 3, 4, 5, 6, 7 | `src/ed047tc1.h:60-67` |
| I2C `SDA` / `SCL` (shared: RTC + touch + our sensors) | 18 / 17 | `src/utilities.h:39-40` |
| Touch IRQ | 47 | `src/utilities.h:41` |
| Touch I2C address | `0x5A` | `src/touch.h:10` |
| Button | 21 | `src/utilities.h:30` |
| Battery ADC | 14 | `src/utilities.h:32` |
| SD `MISO`/`MOSI`/`SCLK`/`CS` (free if no card fitted) | 16 / 15 / 11 / 42 | `src/utilities.h:34-37` |
| Genuinely free | 45, 10 (analog-in only), 48, 39 | `README.MD` "GPIO List" |

Four findings worth carrying forward, because each one corrects or narrows an
assumption made elsewhere in this repository:

- **`CFG_STR` is GPIO0**, the boot-mode strapping pin. Driving it is safe
  once the system has finished booting, but it is a reason to be deliberate
  in phase 2b about exactly when the EPD control code first touches it.
- **The touch controller is not a GT911**, which `docs/architecture.md` §6.2
  and `docs/requirements.md` §6.2 both assume. `src/touch.h:10` sets
  `TOUCH_SLAVE_ADDRESS 0x5A`, and `src/touch.cpp` reads it through a
  `0xD0`/`0xD1` register protocol returning 5 bytes per touch point. A GT911
  answers at `0x5D` or `0x14` with a completely different register map. The
  address is now settled; the actual part number is not (see open items).
- **There is no battery gauge IC on this board.** `BOARD_BATTERY_ADC_GPIO`
  (14) is a plain resistor divider read directly by the ADC
  (`examples/demo/demo.ino:331`:
  `battery_voltage = (v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0)`), and the
  divider only carries a valid reading while EPD power is on, because it
  hangs off the same 74HCT4094 rail (`demo.ino:327-330`, "When reading the
  battery voltage, POWER_EN must be turned on"). GPIO14 is an ADC2 pin on the
  S3, and ADC2 is documented as unreliable while Wi-Fi is active -- a real
  constraint for whichever phase reads battery level over Wi-Fi.
- **Panel power is not a GPIO.** It is pin 13 of the 74HCT4094 shift
  register, reached only through `CFG_DATA`/`CFG_CLK`/`CFG_STR`
  (`README.MD` FAQ 1: "software must call `epd_poweron()` to enable it").
  `epd_poweroff()` also cuts power to the Molex connector, since they share a
  rail. There is no separate status-LED GPIO either -- the board's one LED is
  on the same register (`README.MD` FAQ 4 mentions it only as something
  `epd_poweroff()` cannot turn off by itself).

## Known constraints (vendor warning, verbatim intent)

The e-paper panel must not be partially refreshed for an extended period —
doing so leaves residual ghosting and causes irreversible damage. This is the
hardware fact behind `max_partial_refreshes_before_full` in
`ports/port_display.h`; phase 2c calibrates the actual threshold.

## Ghosting observed in the simulator — a calibration starting point

**Observed 2026-09-06, phase 1c**, running `co2_spike_meeting.csv` at
`--time-scale 60` in the windowed simulator, with the unmeasured emulation
constants in `platform/host/epd_emulation.h`.

Ghosting was clearly visible as grey residue of previous readings behind the
64px CO₂ headline — the digits of earlier values remained legible underneath
the current one. The rest of the screen stayed clean. That difference is the
useful part of the observation: artifacts concentrate in the small region that
actually changes between refreshes, because that is the only region receiving
repeated partial updates. Static chrome (labels, rules, the status bar) is
never redrawn and never ghosts.

What this tells us, and what it does not:

- **It does not tell us the real threshold.** The number of partial refreshes
  before artifacts appear is currently a guess in `epd_emulation.h`, and the
  visual severity is a guess on top of a guess. Nothing here should be treated
  as a measurement.
- **It does tell us where to look on the physical panel.** During phase 2c
  calibration, watch the CO₂ headline region specifically, not the screen as a
  whole. A whole-screen impression will understate the problem, because the
  damage-relevant region is a small fraction of the panel.
- **It suggests the current guessed threshold may be too permissive.** If real
  hardware ghosts similarly at the same partial-refresh count, the value
  wants lowering. Confirm against the panel rather than tuning the emulation
  to look better.

Also worth carrying into 2c: the refresh strategy determines the ghosting
region, so a layout change that enlarges or moves the frequently-updated area
changes the wear pattern. This is an argument for calibrating after the screen
layout settles, not before.

## Open items to resolve during phase 2

- [x] Exact GPIO pin assignments for: EPD control/data bus, I²C (sensors +
      RTC + touch), SD card if present, battery ADC, user button(s), any
      status LED. Resolved 2026-09-06 (phase 2a) — see "Pin assignments"
      above. There is no separate status-LED GPIO: the board's one LED is on
      the same 74HCT4094 register as panel power.
- [x] Battery gauge / charge-detection chip model. Resolved 2026-09-06
      (phase 2a) — there is no gauge IC. Battery is a plain resistor divider
      on `BOARD_BATTERY_ADC_GPIO`, valid only while EPD power is on; see "Pin
      assignments" above.
- [x] Touch I²C address. Resolved 2026-09-06 (phase 2a): `0x5A`, and the
      controller is confirmed **not** a GT911 — see "Pin assignments" above.
- [ ] Touch controller part number. The address and register protocol
      (`0xD0`/`0xD1`, 5 bytes per point) are known and documented above; which
      physical part answers to them is still unidentified. Check the
      schematic, or the chip marking on the board itself.
- [ ] Whether the RTC (PCF8563) is wired to survive deep sleep for real-time
      timestamps, or is only used for scheduling.
- [ ] Whether `BOARD_BATTERY_ADC_GPIO` (GPIO14, ADC2) gives usable readings
      while Wi-Fi is active. The ESP32-S3 TRM documents ADC2 as unreliable
      under Wi-Fi; confirm on this board before phase 3/4 tries to read
      battery level during a Wi-Fi window, rather than assuming the divider
      alone is the whole story.
- [ ] Whether `esp_timer`-backed `now_ms()` (`platform/esp32_t5s3/adp_clock.c`)
      genuinely resets to 0 across this board's deep sleep, the way a plain
      power-on reset would, or behaves some other way. `adp_clock.c` and
      `core/app/update_policy.c` both already tolerate a reset to 0 either
      way (see `adp_clock.h`'s header comment), so nothing is blocked on this
      — it only matters for whichever phase first relies on elapsed time
      *across* a sleep cycle rather than merely surviving one.
- [ ] The `idf.py -T` test app of `docs/architecture.md` §9.2. Phase 2a runs
      the `port_clock` / `port_log` contract suites once at device boot
      instead (see `docs/adr/0005-contract-suites-run-at-boot.md`), because
      there was no test-app scaffold and no `port_i2c` yet to justify
      building one. Deferred to phase 2d, where `port_i2c` will need a real
      on-target test runner regardless — recorded here so the boot-time
      runner reads as a stated interim choice, not an unexamined shortcut.
- [ ] Measured refresh durations for each `refresh_mode_t`, and the real
      partial-refresh count at which ghosting becomes visible in the CO₂
      headline region. These replace the estimates in
      `platform/host/epd_emulation.h` and the matching guesses in
      `platform/esp32_t5s3/board_config.h` — phase 2c must update both files
      together, or the device and the simulator stop agreeing on what
      `update_policy` should do. See the ghosting section above for where to
      look and what the simulator suggests.
