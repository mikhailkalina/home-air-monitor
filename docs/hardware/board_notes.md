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

## Per-row signal sequence (STH, CKH, CKV, LE)

**Established 2026-09-07, phase 2b.** Recorded here as a wiring fact in the
same category as the pin table, the GRAY4 nibble order and the pixel clock:
what the panel requires, not how anyone's code is arranged.

The vendor's prebuilt demo (`firmware/T5-ePaper-S3_demo_250901.bin` in
`Xinyuan-LilyGO/LilyGo-EPD47`, flashed to this unit on 2026-09-07) renders
correctly on this panel, which makes the sequence its driver produces a
known-good reference for this glass. The sequence below is what that driver
emits; it is the target any other driver for this board has to reproduce.

**STH is a per-line start pulse generated by the LCD_CAM peripheral, not by
software.** The reference reaches this by routing STH to the ESP-IDF i80
bus's **D/C** pin (`esp_lcd_i80_bus_config_t.dc_gpio_num = STH`) and issuing
each row as a transaction *with* a command phase — `lcd_cmd_bits = 10`,
`dc_levels = { idle 0, cmd 1, dummy 0, data 0 }`, and a transmit call that
passes a real command value rather than "no command". Per row, on the wire,
that produces:

1. **STH high** for one 10-bit command phase — ten CKH clocks, ≈1 µs at the
   10 MHz pixel clock.
2. **STH low** for the entire pixel-data phase that immediately follows,
   clocked out on the same CKH train with no gap.
3. **STH low** at idle between transactions.

The load-bearing part is not STH's polarity during the data — it is that
STH's falling edge sits a fixed, hardware-generated ≈1 µs ahead of the first
data clock, on the same clock train, with no software scheduling in between.

**CKH** is the i80 bus's write strobe (`wr_gpio_num`) and carries both the
command and the data phases continuously.

**CKV** is pulsed *between* the row latch and the data transfer, and is low
while the pixel data is clocked in. **LE** (latch enable) pulses through the
74HCT4094 config register after the previous row's data transfer has
completed and before CKV advances the gate — the order per row being: wait
for the previous transfer to finish, pulse LE, pulse CKV, then start the next
row's transfer.

Two consequences worth carrying forward:

- A driver that leaves STH under software control cannot reproduce step 1.
  Toggling STH with `gpio_set_level()` around the transfer yields a falling
  edge before the data, but its distance from the first data clock is a
  software-scheduling interval of unknown and variable length, not ≈1 µs of
  hardware-clocked command phase. Whether this panel tolerates that is
  **not** established by anything here — what is established is that the
  working driver does not do it that way.
- A driver that holds CKV high across the data transfer is inverted with
  respect to this sequence, which holds CKV low during data and pulses it
  before the transfer.

**Status 2026-10-12, resolved by changing backend.** This section's two
consequences were chased through four rounds of FastEPD configuration --
routing STH to the i80 D/C pin, inverting its `dc_levels`, taking CKV away
from FastEPD and pulsing it from `ROW_STEP`, parking CKH low at idle,
correcting the data-bus order -- and every round moved the measured behaviour
while the panel stayed blank. The conclusion was that a backend issuing one
software-scheduled transaction per row cannot produce what this section
describes, whatever its settings.

The display backend is now EPDiy (`docs/adr/0007-epaper-backend.md`'s
"Decision reversed"), which generates the per-row structure in hardware: the
line start and latch through the LCD_CAM peripheral and CKV through RMT. This
section stays as the record of what the panel requires -- it is a wiring
fact, not a statement about anyone's code -- but `platform/esp32_t5s3/` no
longer has its own per-row signal code to compare against it.

**First light 2026-10-12.** EPDiy's sequence satisfies the panel: the
bring-up test pattern rendered solid black, `screen_home` rendered from the
same core code the simulator runs, and `update_policy`'s partial refreshes
updated the CO2 headline region in place over several minutes. That is the
phase 2b deliverable, observed on the build that drives the panel through
EPDiy's highlevel API -- see "Panel power" in the open items below for the
one thing about that build that is not yet understood. **Confirmed the same
day** on the reviewed build, which adds the `epd_poweron()`/`epd_poweroff()`
bracket EPDiy documents and an `epd_clear()` before every full refresh: same
image, same partial refreshes, longer full refreshes (below).

## Memory budget

**Measured 2026-09-06, phase 2a**, from `idf.py size` and `idf.py
size-components` on the phase 2a bring-up build: `adp_clock`, `adp_log`, the
null display adapter and the FreeRTOS pump, with no Wi-Fi, no MQTT, no
sensors and no real display driver linked in yet. These numbers describe
that build only — they are a baseline, not a steady-state budget.

- **App binary:** 261,197 bytes of the 4 MB app partition — 94% free.
- **DIRAM:** 61,171 of 341,760 bytes used, **280,589 free**.
- **IRAM: reported as 100% used — this figure is misleading and worth
  explaining rather than quoting on its own.** IRAM and DRAM are two views
  onto the same DIRAM pool on the ESP32-S3: whatever IRAM does not claim
  becomes ordinary data heap, so "IRAM 100%" does not mean the chip is out of
  usable RAM, only that nothing more is available for *code that must run
  from IRAM specifically*. The occupants are all ESP-IDF system components
  that are required to live there, not application code: `esp_system`
  (3,882 bytes), `esp_hw_support` (2,254), `xtensa` (2,031, including a
  427-byte vector table), `heap` (1,591), `spi_flash` (1,438). `libhac_core`
  contributes zero bytes of IRAM. **DIRAM free is the number to watch here,
  not the IRAM percentage.**

**Consequence for phase 2b.** The 960×540 GRAY4 framebuffer is 253 KB.
Against 280 KB of free internal DIRAM, that is nearly the entire remaining
budget — which is why `adp_display.c` must allocate it in PSRAM via
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, not from the default heap. An
accidental internal allocation would work fine at first and only fail later,
once Wi-Fi claims its share of DIRAM — the worst possible failure shape,
because the crash site does not point at its cause. Phase 2b should log
which heap the framebuffer actually came from at allocation time, so this is
something observed at boot rather than assumed from this budget.

Free PSRAM is a runtime-only figure (`heap_caps_get_free_size`), not
something `idf.py size` reports. It was measured at runtime in phase 2b --
see "Free PSRAM at runtime" in the open items below.

Re-run `idf.py size-components` when Wi-Fi lands in phase 4 and again when
Matter lands in phase 7: both claim their own share of IRAM and DIRAM, and
this bring-up baseline is what makes their actual cost visible.

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
- [ ] Phase 5: confirm whether `now_ms()` survives deep sleep before trusting
      `persisted_state_t`'s `last_display_update_ms` and `last_publish_ms`,
      which are stored as absolute values on that clock — if the epoch resets
      across a sleep cycle, those comparisons produce garbage and the first
      wake would misjudge every threshold. The `port_clock` contract suite
      has no assertion covering this today; phase 5 should add one.
- [ ] The `idf.py -T` test app of `docs/architecture.md` §9.2. Phase 2a runs
      the `port_clock` / `port_log` contract suites once at device boot
      instead (see `docs/adr/0005-contract-suites-run-at-boot.md`), because
      there was no test-app scaffold and no `port_i2c` yet to justify
      building one. Deferred to phase 2d, where `port_i2c` will need a real
      on-target test runner regardless — recorded here so the boot-time
      runner reads as a stated interim choice, not an unexamined shortcut.
- [x] Free PSRAM at runtime. **Measured 2026-10-12, phase 2b**, at the top
      of `app_main()` before the display adapter allocates: **8,386,192 bytes
      free of the 8 MB pool, largest free block 8,257,536**, alongside
      375,259 bytes free internal. Both 253 KB GRAY4 buffers were then
      confirmed to land in PSRAM at runtime, not merely by intent
      (`adp_display: memory budget our_framebuffer_in_psram=true
      fastepd_buffer_in_psram=true`, 259,200 bytes each) -- which is what the
      phase-2b consequence note under "Memory budget" above asked to be
      observed rather than assumed. FastEPD allocates a third PSRAM buffer of
      its own (`pTemp`, 126 KB) plus the grey lookup tables, so the figure to
      re-measure in phase 3 and phase 4 is the one after
      `fastepd_shim_init()`, not this one. **Superseded by the EPDiy
      backend**: there is no separate adapter framebuffer any more (the core
      draws into EPDiy's highlevel front buffer). **Measured 2026-10-12**:
      `free_psram_after_epd_init=7349100` against 8,386,192 at the top of
      `app_main()`, so EPDiy's init -- its 64 K LUT, line queues, and the
      highlevel front, back and difference buffers -- costs **1,037,092
      bytes** of PSRAM. Internal RAM after init: 232,019 bytes free. These
      are the figures to re-measure in phases 3 and 4.
- [ ] Measured refresh durations for each `refresh_mode_t`, and the real
      partial-refresh count at which ghosting becomes visible in the CO₂
      headline region. These replace the estimates in
      `platform/host/epd_emulation.h` and the matching guesses in
      `platform/esp32_t5s3/board_config.h` — phase 2c must update both files
      together, or the device and the simulator stop agreeing on what
      `update_policy` should do. See the ghosting section above for where to
      look and what the simulator suggests.
      **Partial data, 2026-10-12**: with the drive sequence as it stands,
      `REFRESH_CLEAR` takes 1.896 s over 44 render passes and `REFRESH_FULL`
      1.152 s over 28, both from `fastepd_shim.c`'s own `elapsed_us` field,
      at roughly 40 ms per pass (540 rows, so ~74 us per row against ~26 us
      of actual pixel data at the 10 MHz clock -- the balance is `ROW_STEP`'s
      two bit-banged shift-register writes). The pass counts also pin
      FastEPD's waveform matrix at 10 passes: `CLEAR_SLOW` is 4x8 clear
      passes + 2 neutral + 10, `CLEAR_FAST` is 2x8 + 2 + 10. **These are not
      yet the numbers to copy into `platform/host/epd_emulation.h`**: they
      time a sequence that does not yet put an image on the glass, so they
      measure this driver rather than the panel. Re-measure once it
      renders.
      **Measured with EPDiy, 2026-10-12, on a panel that renders** (EPDiy's
      own per-update timing line): a whole-panel `MODE_GC16` update takes
      **1,084 ms to draw, 1,203 ms in total** (107 ms of that computing the
      difference); a `MODE_GL16` partial over the CO2 headline region
      (~244-262 x 228 px) takes **353-362 ms to draw, ~400-409 ms in
      total**, and over a wider ~868 x 250 px region 396 ms / 447 ms. These
      are real panel figures and can seed `platform/host/epd_emulation.h`.
      The full-refresh figure above predates `REFRESH_FULL` running
      `epd_clear()` first; **measured on the current build, 2026-10-12**:
      `epd_clear()` (66 whole-panel pushes) takes **~2.3-2.5 s**, and the
      whole `REFRESH_FULL` flush **3.20 s** for `screen_home` and **3.67 s**
      for a solid black fill -- the GC16 draw after the clear depends on how
      many pixels leave white (755 ms vs 1,086 ms). Seed the emulation with
      the larger figure. Partial refreshes are unchanged by the power
      bracket. The ghosting threshold is still unmeasured.
- [x] FastEPD board-callback timing. **Retired 2026-10-12**: the constants
      this tracked (`BOARD_EPD_PIXEL_CLOCK_HZ`, the eight
      `BOARD_EPD_ROW_START_DELAY_*_US`, `BOARD_EPD_ROW_CKV_PULSE_US`,
      `BOARD_EPD_LINE_PADDING_BYTES`, `BOARD_EPD_VCOM_MV`) are gone from
      `board_config.h` along with FastEPD. EPDiy's board definition owns the
      equivalents, and they are its own measured values for this board rather
      than numbers this project copied across engines. The pin constants
      remain in `board_config.h` as this project's record of the hardware and
      as the cross-check against EPDiy's board definition, which they match
      exactly.
- [ ] Whether EPDiy's `epdiy_ED047TC1` waveform produces correct grayscale on
      *this* unit. **Narrowed 2026-10-12, not closed.** The concern used to be
      that FastEPD's `u8FivePointTwoMatrix` was characterised for a different
      panel entirely; EPDiy instead ships a waveform named for this panel's
      driver IC, with five modes (`MODE_DU`, `MODE_GC16`, `MODE_GL16`, and the
      two `MODE_EPDIY_*_TO_GL16`), of which `adp_display.c` uses `GC16` for
      `REFRESH_FULL` and `GL16` for `REFRESH_PARTIAL`. That is a much better
      starting point, but it is still not a measurement against this
      individual unit: a mismatched waveform or VCOM shows up as washed-out or
      over-dark greys, not an error. Check visually against `screen_home`'s
      known grey levels once the panel renders, as phase 2c already plans for
      the refresh timings.
- [x] `partial_rect_for_fastepd()` and `swap_nibbles_into_fastepd()` have no
      unit test. **Retired 2026-10-12**: both functions are deleted. The
      nibble swap is unnecessary because EPDiy's packing and stride are this
      project's own (`docs/adr/0003`'s second amendment), and the rect
      constraints are unnecessary because EPDiy crops honestly on both axes.
      The underlying gap is unchanged and will return with the next pure
      function that lands there: `platform/esp32_t5s3/` is built only by
      `idf.py`, so there is nowhere on the host to put a test for it.
- [x] Whether widening every `REFRESH_PARTIAL` to the panel's full width costs
      visible extra ghosting. **Retired 2026-10-12**: the widening existed
      only to avoid FastEPD's broken 4bpp column mask and is gone. The dirty
      rect now reaches the panel as the core computed it, so phase 2c watches
      the CO2 headline region only, as it originally planned.
- [ ] **Panel power: why the first-light build rendered at all.** The build
      that first put an image on this panel (ELF SHA256 `320dfba08...`)
      called `epd_hl_update_area()` with no `epd_poweron()` anywhere in its
      call path -- confirmed by the boot log's own strings and by its timing,
      not just by reading the source -- and it rendered and updated
      correctly for minutes. EPDiy's documentation says power must be enabled
      first (`epd_highlevel.h`, "Prior to this, power to the display must be
      enabled via `epd_poweron()`"), and nothing in the fork's source
      explains how the high-voltage rails came up without it. The board's
      `init` sets `power_disable = true` and both rail enables false;
      `epd_renderer_init()` (`render.c`) pushes that register to the
      74HCT4094 immediately afterwards via `epd_control_reg_init()`; and
      every latch and frame-start push during rendering
      (`output_i2s/render_i2s.c`) writes the same three bits again. So the
      rails were commanded *off* on every register write of that build, from
      init onwards -- which also rules out the 74HCT4094 simply holding what
      an earlier firmware latched. (The data bus needs no power-on on this
      variant -- `i2s_gpio_attach()` is a no-op, the bus is attached once at
      init -- but that does not account for the rails.)
      What remains is that, on this revision, the high-voltage supply is not
      gated by `power_disable`/`pos_power_enable`/`neg_power_enable` the way
      both EPDiy and LilyGo's own driver assume. If so, the corollary matters
      more than the curiosity: **`epd_poweroff()` may not remove the rails
      either**. The firmware now brackets every drive with
      `epd_poweron()`/`epd_poweroff()` as EPDiy documents -- confirmed on
      hardware to render identically -- so rendering no longer depends on
      the answer, but panel health and the power budget do. A current measurement at the USB input during the 60 s idle gaps,
      or a meter on VPOS/VNEG, settles it; the schematic's rail-enable
      wiring explains it.
- [ ] `update_policy`'s status trigger can fire on `charging` or
      `telemetry_connected` alone, neither of which `screen_home` draws yet,
      making the empty-diff skip in `event_loop.c`'s `redraw()`
      (`apps/firmware_esp32/main/event_loop.c`) repeat every tick with no
      visible change -- harmless, just wasteful. Unreachable today: phase
      2a's bringup reading is fabricated and never sets either flag.
      Becomes reachable for `telemetry_connected` once MQTT lands in phase
      4, and for `charging` once the battery adapter lands in phase 5.
