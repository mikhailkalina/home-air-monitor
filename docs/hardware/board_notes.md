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

## Known constraints (vendor warning, verbatim intent)

The e-paper panel must not be partially refreshed for an extended period —
doing so leaves residual ghosting and causes irreversible damage. This is the
hardware fact behind `max_partial_refreshes_before_full` in
`ports/port_display.h`; phase 2c calibrates the actual threshold.

## Open items to resolve during phase 2

- [ ] Exact GPIO pin assignments for: EPD control/data bus, I²C (sensors +
      RTC + touch), SD card if present, battery ADC or fuel-gauge I²C
      address, user button(s), any status LED.
      → source: schematic if available, otherwise reverse-engineered from the
      `esp32s3` branch of LilyGo-EPD47, `src/utilities.h` S3 block.
- [ ] Touch controller model and I²C address (assumed GT911 pending
      verification).
- [ ] Battery gauge / charge-detection chip model (onboard capability
      confirmed by the vendor; part not yet identified — check schematic or
      reference firmware).
- [ ] Whether the RTC (PCF8563) is wired to survive deep sleep for real-time
      timestamps, or is only used for scheduling.
