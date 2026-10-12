# 7. FastEPD is the e-paper backend, with a custom board definition for this board's shift register

Originally titled "EPDiy is the e-paper backend, and its LGPL-3.0 is
accepted with an escape hatch" — see "Status" below for why the title and
the decision changed. Left as struck-through history rather than rewritten,
because the corrections below are the point of this document as much as the
final decision is.

Date: 2026-09-06

## Status

**Accepted**, as of the implementation below. The original decision (EPDiy
over FastEPD and a hand-written driver, LGPL accepted with a relink note)
was approved, then withdrawn at the implementation-cost level by
"Correction after reading EPDiy's actual output-backend source," then
reopened at the choice level by "Second correction: FastEPD re-examined at
source level," which recommended switching to FastEPD. That recommendation
was not accepted outright: the next section, "Third pass: the three FastEPD
callbacks checked line by line against our own board's reference," was
requested specifically because the second correction's claim ("the same
eight config bits... this board
family") was again one level above the code — exactly the altitude at which
the first two mistakes happened. That check is done, with a per-function
verdict below. Two of the four items needed a real change, not just a pin
table; none needed a rewrite. Both changes were made in
`third_party/fastepd/fastepd_shim.c`'s board callbacks, `adp_display.c` was
written against the result, and a real `idf.py build` — full firmware, not a
syntax check — links a flashable image with this project's own code
(`platform/esp32_t5s3/*.c`) compiling clean under `-Wall -Wextra -Werror
-Wconversion`. See "Implementation" at the end of this ADR for what shipped
and what phase 2c still needs to confirm on real hardware.

## Context

Phase 2b needs a real `port_display` for the ED047TC1 on the LILYGO T5 4.7"
e-Paper S3 (SKU H716, silkscreen `Screen-4.7-S3 v2.4`). Everything else is
already in place: `board_config.h` carries the verified pins, ADR 0003 settled
the GRAY4 nibble order against the vendor reference, and phase 2a's null
adapter already reports the panel geometry and refresh limits up to
`update_policy`. What is missing is the code that clocks pixels onto glass.

Driving this panel is not a small job. The ED047TC1 has no controller of its
own: the ESP32-S3 drives the source and gate drivers directly over an 8-bit
parallel bus through the LCD_CAM peripheral, and 16 grey levels come from
running a per-temperature waveform table over several passes. Writing that
from scratch is not a weekend.

Three candidates, plus one that looks like a fourth and is not.

### The apparent fourth option: the vendor's own component

`Xinyuan-LilyGO/LilyGo-EPD47` (branch `esp32s3`) is the reference this
repository has cited since phase 1c — for the panel orientation, the GRAY4
nibble order and every pin in `board_config.h`. It is an ESP-IDF component,
it targets exactly our board, and it demonstrably works: the stock example
ran on this unit as received.

**It is GPL-3.0, not LGPL-3.0.** `LICENSE` on branch `esp32s3` is the plain
GNU GPL v3; the driver sources (`src/epd_driver.c`, `src/ed047tc1.c`) carry no
per-file header that would soften that, so the repository licence governs.
Linking it would put the entire firmware image — core, UI, reporters, all of
it — under GPL-3.0, and would collide outright with the proprietary Bosch
BSEC binary this project already plans to link (`THIRD_PARTY_NOTICES.md`).

That is a licence question, not a code-quality one, and it does not touch what
phase 2a already did: pin numbers, a panel resolution and a nibble order are
facts about hardware, not copyrightable expression. Reading that repository to
learn them was fine and stays fine. Linking it is not on the table.

Its README is nonetheless the reason the rest of this ADR is shaped the way it
is: *"The driver and sample program are from vroland/epdiy."* The vendor's own
firmware is a fork of EPDiy. Option 1 below is therefore not "a third-party
library" — it is the upstream of the code we have been reading all along.

### Option 1 — EPDiy (`vroland/epdiy`)

- **Licence:** LGPL-3.0-or-later for the driver and examples; MIT for
  utilities; CC BY-SA 4.0 for the hardware design files, which we do not use.
  Published in the ESP Component Registry as `vroland/epdiy` 2.1.3, declaring
  `targets: esp32, esp32s3` and `idf: ">=5.0"`.
- **Maturity:** the origin of the ED047TC1 driver ecosystem. `ED047TC1` is a
  first-class display definition (`src/epd_display.h`), with per-panel,
  per-temperature waveform tables. Both output paths exist: I2S for the
  original ESP32 and LCD_CAM for the S3.
- **Grayscale:** 16 levels, with distinct waveform modes rather than one
  generic "refresh" — `MODE_GC16` (flashing, any grey to any grey),
  `MODE_GL16` (the same, non-flashing), `MODE_DU` (fast, to black or white
  only), `MODE_A2` (fast monochrome), plus `MODE_EPDIY_WHITE_TO_GL16` and
  `MODE_EPDIY_BLACK_TO_GL16`. That enum maps onto our `refresh_mode_t`
  without inventing anything.
- **Rectangles:** `epd_hl_update_area(state, mode, temperature, EpdRect area)`
  takes an arbitrary rectangle, documented as "supplying a small area to
  update can speed up the update process". The high-level state additionally
  keeps `front_fb`/`back_fb`/`difference_fb` with `dirty_lines` and
  `dirty_columns`, so it narrows further on its own.
- **Buffer layout:** `epd_draw_pixel()` in `src/epdiy.c` indexes
  `framebuffer[y * epd_width() / 2 + x / 2]` and packs **odd x into the high
  nibble, even x into the low** — byte-for-byte what `core/ui/framebuffer.c`
  produces after ADR 0003. This is not luck: ADR 0003 matched LilyGo-EPD47,
  and LilyGo-EPD47 is EPDiy.

### Option 2 — FastEPD (`bitbank2/FastEPD`)

- **Licence:** Apache-2.0. Identical to ours; nothing to reconcile, ever.
- **Maturity:** newer (2024 onward), roughly 200 commits, essentially one
  author — Larry Bank, who has a long track record of display and codec
  libraries. Built for the S3 LCD path from the start, which is our path.
- **Grayscale:** `BB_MODE_1BPP` / `2BPP` / `4BPP`, 16 greys via a waveform
  matrix (`pGrayMatrix`).
- **Rectangles:** `partialUpdate()` updates **row ranges**, not arbitrary
  rectangles. Our dirty rects are genuinely two-dimensional — phase 2a
  observed `824x228` — so a row-range API discards the horizontal half of
  what `framebuffer_diff_dirty_rect()` computes.
- **Buffers:** the library owns `pCurrent`, `pPrevious` and `pTemp`
  internally. Workable, but the 4bpp packing order is undocumented and I
  could not confirm it from the source without guessing.

### Option 3 — a hand-written ED047TC1 driver

- **Licence:** ours, Apache-2.0. No question ever arises.
- **Everything else:** we would be programming LCD_CAM by hand and deriving
  16-level waveforms for a panel we own exactly one of, without being able to
  copy the vendor's GPL-3.0 tables. This is the largest and least certain
  piece of work in the whole project, and it buys a licence outcome that
  Option 2 already provides for free.

### The finding that reorders all of this

**None of the three supports this board out of the box, and the amount of work
to change that differs by an order of magnitude between them.**

Our board drives its power rails and scan direction through a 74HCT4094 shift
register on `CFG_DATA`/`CFG_CLK`/`CFG_STR` (GPIO 13/12/0 —
`docs/hardware/board_notes.md`, "Pin assignments"). That topology is the
distinguishing feature, and it is what each candidate does or does not already
know how to speak.

| | EPDiy | FastEPD |
|---|---|---|
| Nearest LilyGo board it ships | `epd_board_lilygo_t5_47` — **74HCT4094**, but ESP32 pins (CFG 23/18/0, D0–D7 33,32,4,19,2,27,21,22) | `BB_PANEL_LILYGO_T5PRO` — data pins 11,12,13,14,21,47,45,38 |
| Its other LilyGo S3 board | `lilygo_board_s3` — the **T5 S3 Pro**: PCA9555 expander, TPS65185 PMIC, CKV 48, D0–D7 5,6,7,15,16,17,18,8 | same board, same mismatch |
| Shift-register support in-tree | **yes** — `push_cfg_bit()`, `epd_config_register_t` | **no** — direct GPIO, PCA9535/9555, MCP23017, PCAL6416A only |
| Work to support our board | a new **board definition**: pin-remap existing in-tree code | a new **expander class inside the library**: fork or upstream PR |

The T5 S3 **Pro** is a different product from our T5 4.7 S3. `board_notes.md`
already established as much from the other direction — this board has no
battery gauge IC and its touch controller answers at `0x5A`, whereas the Pro
carries a BQ25896/BQ27220 and a GT911. Any documentation, blog post or board
definition that says "LilyGo T5 S3" and means the Pro does not describe our
hardware. Both libraries' S3 LilyGo definitions mean the Pro.

So the choice is not "LGPL library versus Apache library". It is:

- **EPDiy:** copy `epd_board_lilygo_t5_47.c`, swap in the S3 pins already
  named in `board_config.h`, done. The shift-register code we need is written,
  in-tree and exercised by the ESP32 variant of this same panel.
- **FastEPD:** teach the library a bus type it has never had, then carry that
  fork — or wait on an upstream PR — for as long as we ship.
- **Hand-written:** all of the above plus the waveforms.

## Decision

**Use EPDiy (`vroland/epdiy` ^2.1.3) as the phase 2b display backend, pulled
in as an ESP-IDF managed component, with a board definition for this board
written by us and living in `platform/esp32_t5s3/`.**

Accept LGPL-3.0 on the terms that actually apply today, and record the exit
explicitly rather than leaving it to be rediscovered later.

**Why the LGPL is acceptable now.** EPDiy is statically linked into the
firmware image, so LGPL-3.0 §4 requires that a recipient be able to relink the
work against a modified EPDiy. This repository is public and Apache-2.0, the
firmware builds from source with `idf.py build`, and the EPDiy version is
pinned in `idf_component.yml`. A recipient can modify EPDiy and rebuild. That
satisfies §4(d)(1) by the most ordinary route available.

Note also what LGPL does *not* do, since it is the reason the vendor's GPL-3.0
fork fails where its own upstream passes: LGPL constrains relinkability of the
LGPL part, not the licence of the rest of the work. The planned proprietary
BSEC binary can coexist with it. Under GPL-3.0 it could not.

**Why the exit matters.** A closed-source or commercially licensed build would
break this. Shipping only a stripped `.bin` gives the recipient no way to
relink, and §4's alternative — distributing linkable object files or a shared
library plus the tooling to recombine them — is awkward for a monolithic
ESP32 image, though not impossible. The obligation lands the day a binary
leaves this repository under terms that do not include its source. That day
is not today, and this ADR does not pretend to decide for it; it makes sure
the decision is cheap when it comes.

**Why it is cheap.** The backend sits behind `ports/port_display.h`. Replacing
it means rewriting `platform/esp32_t5s3/adp_display.c` and the board
definition beside it — one adapter and one pin table. `core/`, the UI, the
screens, `update_policy` and every test are untouched, because none of them
knows a backend exists. That is precisely what the hexagonal layering was for,
and this is the first time it has been asked to pay out.

If the exit is ever taken, FastEPD is the destination, not a hand-written
driver: same Apache-2.0 outcome, a fraction of the risk. The work at that
point is the shift-register expander class described above, and it is bounded.

## Consequences

- `apps/firmware_esp32/main/idf_component.yml` gains
  `vroland/epdiy: "^2.1.3"`. `managed_components/` and `dependencies.lock`
  stay gitignored, per CLAUDE.md.
- `THIRD_PARTY_NOTICES.md` already anticipates this — it lists EPDiy and
  sketches the relink note. Both get updated to the version actually pinned,
  the registry name, and the reasoning above, in the same commit as the code.
- **A board definition for this board is ours to write and ours to keep.**
  It fills in an `EpdBoardDefinition` with our GPIOs and the 74HCT4094 push
  sequence, derived from the pin facts in `board_config.h` rather than copied
  from anyone's source. It stays under Apache-2.0 in
  `platform/esp32_t5s3/`. If it is ever contributed upstream it goes under
  EPDiy's terms there; that is a separate, deliberate act.
- **The memcpy of ADR 0003 survives, and phase 2b must prove it rather than
  assume it.** EPDiy's packing matches ours because both trace to the same
  code, so `port_display.get_framebuffer()` can hand the core a buffer that
  goes to the panel with no per-pixel conversion. ADR 0003 closes by saying
  the decision "is only worth what the byte-for-byte compatibility is worth"
  and to re-check it against any non-EPDiy backend. It is being re-checked
  against an EPDiy backend, at width 960 where our stride is 480 bytes and
  EPDiy's is `epd_width() / 2` — the same 480. If the two ever disagree at
  runtime, that is a finding worth stopping over, not a conversion loop worth
  writing.
- **The rectangle contract must be verified on glass, not read off the API.**
  `epd_hl_update_area()` accepts an `EpdRect`, and EPDiy narrows further using
  its own `dirty_lines`/`dirty_columns`. But an API that accepts a rectangle
  is not proof that the panel receives one; phase 2a went to the trouble of
  making the dirty rect real (`824x228`, not `960x540`), and the ghosting
  budget in ADR 0004 is spent per refresh regardless of area. Phase 2b
  confirms on hardware that a partial flush disturbs only the given region.
- **Grayscale mode mapping is EPDiy's enum, documented at the mapping site.**
  `REFRESH_FULL` → `MODE_GC16`, `REFRESH_PARTIAL` → `MODE_GL16`,
  `REFRESH_FAST_MONO` → `MODE_DU`, `REFRESH_CLEAR` → `epd_clear()`. The
  reasoning for each belongs in `adp_display.c` next to the code, not here.
- **PSRAM cost roughly triples, and that is fine.** `epd_hl_init()` allocates
  `front_fb`, `back_fb` and `difference_fb` in PSRAM — about 760 KB — on top
  of `event_loop.c`'s own 253 KB previous-frame buffer. Against 8 MB of PSRAM
  this is nothing, and internal DIRAM (280 KB free, and the real constraint
  per `board_notes.md`) is untouched, provided the allocations really do land
  in PSRAM. Phase 2b logs which heap each one came from rather than trusting
  the flag.
- **A second, unmeasured risk arrives with EPDiy: VCOM and waveform fit.**
  EPDiy's `ED047TC1` waveform tables were characterised on EPDiy hardware, not
  on this board. Wrong VCOM shows up as washed-out or over-dark greys, not as
  a build error. This is now on phase 2c's list beside the refresh timings and
  the partial-refresh budget, and `docs/hardware/board_notes.md` should carry
  it as an open item.
- **This ADR is superseded, not amended, if the licence terms change.** A
  decision to ship closed-source is a new ADR that cites this one.

## Correction after reading EPDiy's actual output-backend source

This section is appended after the licence decision above was approved, as
soon as reading EPDiy's real source — cloned locally and read directly,
rather than through the lossy summarization this ADR's research phase used —
showed a specific claim in "Consequences" to be false. It is recorded here
rather than silently rewritten, because the claim shaped the approval: this
ADR sold EPDiy as "a new board definition: pin-remap existing in-tree code,"
and that is not what the evidence supports.

**What is confirmed, with file-and-line citations from EPDiy's own tree
(`vroland/epdiy`, default branch, cloned locally):**

- `src/output_common/render_method.c` picks the output backend with a
  compile-time `#ifdef`, keyed only on the chip target, with no board-level
  override: `CONFIG_IDF_TARGET_ESP32` selects `RENDER_METHOD_I2S`,
  `CONFIG_IDF_TARGET_ESP32S3` selects `RENDER_METHOD_LCD`, unconditionally.
  A board definition cannot choose the other one.
- `src/output_i2s/i2s_data_bus.c:6` says outright: `// the I2S driver is
  based on ESP32 registers and won't compile on the S3`, and wraps the entire
  implementation in `#ifdef CONFIG_IDF_TARGET_ESP32`. `epd_board_lilygo_t5_47`
  — the board this ADR proposed to copy and pin-remap — pairs with exactly
  this backend. On an S3 target it is dead code.
- The only backend that does compile for the S3, `output_lcd/lcd_driver.c`,
  requires `lcd_bus_config_t.leh` and `.stv` to be real, dedicated GPIO
  numbers: `leh` is routed through the GPIO matrix straight to the LCD_CAM
  peripheral's hardware hsync signal
  (`esp_rom_gpio_connect_out_signal(lcd.config.bus.leh, LCD_PERIPH_SIG(hsync_sig), ...)`),
  and `stv` is bit-banged with plain `gpio_set_level()` once per frame. Both
  calls are unconditional — there is no `-1` / `GPIO_NUM_NC` escape for
  "this signal doesn't exist as a discrete pin here."
- Our board has no such pins. This is not a gap in `board_config.h` — it was
  checked against the vendor's own working S3 firmware, cloned and read
  directly (`Xinyuan-LilyGO/LilyGo-EPD47`, branch `esp32s3`,
  `src/i2s_data_bus.c`, function `i2s_bus_init()`, the `#else` branch guarded
  by `#if !USER_I2S_REG`, which is what actually builds for the S3 target).
  It drives this exact panel through `esp_lcd_new_i80_bus()` /
  `esp_lcd_new_panel_io_i80()`: `STH` becomes the i80 bus's `dc_gpio_num`,
  `CKH` becomes its `wr_gpio_num`, `D0`–`D7` feed `data_gpio_nums` in the
  reordered sequence `{D6, D7, D4, D5, D2, D3, D0, D1}`, and `CKV` is pulsed
  independently through the RMT peripheral (`src/rmt_pulse.c`). Latching,
  scan direction and the power sequence stay exactly where
  `docs/hardware/board_notes.md` already said they were: bits pushed
  through the `CFG_DATA`/`CFG_CLK`/`CFG_STR` 74HCT4094 shift register. There
  is no separate `LEH` or `STV` trace on this board to hand to EPDiy's
  `lcd_bus_config_t` — the vendor's own firmware, which demonstrably drives
  this unit, never uses one.

**What this means for the decision above.** The licence comparison and the
choice of EPDiy over FastEPD and a hand-written driver do not depend on this
and are unaffected. What depended on it was the claim that integrating EPDiy
here is cheap. It is not: making EPDiy drive this panel means one of —

1. Patching EPDiy's own `render_method.c`/`render_method.h` and
   `output_lcd/lcd_driver.c` to add a wiring EPDiy has never shipped —
   i80-bus data path plus RMT-pulsed CKV plus a shift-register-backed
   control register, selectable independently of the chip target — then
   carrying that patch across every future EPDiy upgrade;
2. Re-examining whether FastEPD's architecture is less rigidly tied to one
   wiring convention than EPDiy's is, which "Option 2" above did not check
   because it didn't yet know this was the deciding question; or
3. Writing our own low-level line-output driver — following the wiring
   facts now confirmed above, which is the genuinely new and load-bearing
   information this correction adds — while still depending on EPDiy for
   waveform tables, grayscale rendering and the `epd_hl_*` high-level API,
   which is a real, working integration seam distinct from
   `output_common/render_method.c`'s compile-time chip switch.

None of these is "copy a file and rename some pins," and the difference
matters enough to the effort and risk estimate that it belongs in front of
the person who approved this ADR before any of the three is attempted, not
folded into a changelog after the fact.

## Second correction: FastEPD re-examined at source level

The correction above was reported back before any driver code was written,
as instructed. The response back was pointed and worth recording verbatim in
spirit: the EPDiy finding was verified by cloning the repository and reading
its actual source, but "Option 2 — FastEPD" above was written from the same
kind of lossy web summarization that had just been shown to produce a false
claim about EPDiy. A comparison built on two unequal levels of evidence is
not trustworthy just because one side happened to be checked. FastEPD earned
the same treatment: `bitbank2/FastEPD`, cloned locally and read directly,
specifically to answer one question — does its S3 output path require
dedicated GPIOs for every control line the way EPDiy's does, or does it have
an indirection point a shift register could hook into?

**It has one, and FastEPD's own author independently ran into the same wall
EPDiy's `output_lcd` does not route around.**

`src/FastEPD.inl:1763` carries this comment on the S3 i80-bus path: *"The
code forces the use of a D/C pin, so we must assign it to an unused GPIO on
each device"* — FastEPD's own maintainer naming, in a code comment, the exact
constraint that sank EPDiy's `output_lcd` for this board: the ESP-IDF i80 bus
API insists on a GPIO for every role it defines, whether or not the board
being driven has an electrical signal to put there. FastEPD's answer is a
dedicated `ioDCDummy` field in its panel struct (`src/FastEPD.h:141`, "unused
GPIO for the LCD library to needlessly toggle") — a genuinely spare pin the
i80 driver is allowed to wiggle uselessly, not a real EPD signal.

That same struct, `BBPANELDEF` (`src/FastEPD.h:122-146`), carries what EPDiy's
`lcd_bus_config_t` does not: `ioShiftSTR`, `ioSDA`, `ioSCL` — a bit-banged
3-wire serial shift register, function `bbepSendShiftData()`
(`src/FastEPD.inl:640-655`), pushing 8 bits MSB-first. `ioCKV` and `ioSPH`
are plain `gpio_num_t` fields, toggled with ordinary `gpio_set_level()` calls
(`src/FastEPD.inl:713-756`), not routed through GPIO-matrix hardware the way
EPDiy's `leh` is. This is not a hypothetical extension point: it is already
wired up and in active use by three real panel-callback functions —
`LilyGoIOInit()`, `LilyGoEinkPower()`, `LilyGoRowControl()`
(`src/FastEPD.inl:663-756`) — which push exactly the same eight conceptual
bits (output-enable, mode, scan-direction, STV, negative and positive power
enable, power-disable, latch-enable) through that shift register that
`docs/hardware/board_notes.md` and EPDiy's own (unusable-on-S3)
`epd_board_lilygo_t5_47.c` describe for this board family, and pulse `CKV`
and `SPH` in a plain microsecond-delay loop rather than through RMT.

**Those functions are registered against `BB_PANEL_LILYGO_T5PRO`
(`src/FastEPD.h:92`), and that panel's pin table is not our board.** Its
8-pin "data bus" is `{11, 12, 13, 14, 21, 47, 45, 38}`
(`src/FastEPD.inl:226`) — GPIOs that `board_config.h` already names for
entirely different roles on our unit (12/13 are `CFG_CLK`/`CFG_DATA`, 38 is
`CKV`, 47 is the touch IRQ, 21 is the button, 14 is the battery ADC). Zero
overlap. This is the same finding `docs/hardware/board_notes.md` already
recorded from the other direction — GT911 touch, a BQ25896/BQ27220 fuel
gauge — for the **T5 S3 Pro**, a different, higher-pin-count board than the
plain T5 4.7" S3 this repository targets. FastEPD's `BB_PANEL_LILYGO_T5PRO`
table cannot be reused as-is, for exactly the reason EPDiy's `lilygo_board_s3`
could not.

**What transfers is the mechanism, not the numbers**, and `BB_PANEL_CUSTOM`
(`src/FastEPD.h:100`) exists as a named slot for supplying one. A new
`BBPANELDEF` entry built entirely from facts already verified in
`board_config.h` — width/height 960×540, the data bus as `D0`–`D7` (GPIOs 8,
1, 2, 3, 4, 5, 6, 7), `ioCKV`=38, `ioSPH`=`STH`=40, `ioCL`=`CKH`=41,
`ioSDA`/`ioSCL`/`ioShiftSTR` = `CFG_DATA`/`CFG_CLK`/`CFG_STR` = 13/12/0, and
one genuinely free GPIO (45 or 48; `GPIO10` is analog-input-only per
`board_notes.md` and a poor choice for a toggled dummy pin) for `ioDCDummy`
— paired with FastEPD's existing `LilyGoIOInit`/`LilyGoEinkPower`/
`LilyGoRowControl` callbacks, is a table row plus a review of whether those
callbacks' bit assignments and delay-loop timing suit our board, not a new
driver and not a patch to FastEPD's own files. `BBEP_DISPLAY_ED047TC1`
already selects a populated, real waveform table for this exact panel
(`u8FivePointTwoMatrix`, `src/FastEPD.inl:77` onward) — this is not a stub.
The one fact this ADR cannot get from `board_config.h` and must instead treat
as a hardware fact carried over from the reference driver — the same way
`board_notes.md` already treats the GRAY4 nibble order and the D0–D7 pin
list — is the physical bit order the row-driver shift register expects:
LilyGo's proven-working S3 driver feeds its data lines as
`{D6, D7, D4, D5, D2, D3, D0, D1}`, not `{D0..D7}` in sequence
(`ed047tc1.c`/`i2s_data_bus.c`, cited in the correction above). A `data[]`
array for our `BBPANELDEF` entry needs the same reordering applied to our own
GPIO numbers — `{6, 7, 4, 5, 2, 3, 8, 1}` — which is a statement about which
physical trace carries which shift-register bit, not a copy of anyone's
control flow.

FastEPD is actively maintained (most recent commit one week before this ADR
was revised) and Apache-2.0 throughout, confirmed by reading `LICENSE`
directly rather than trusting the registry page cited in "Option 2."

**Revised recommendation: FastEPD, not EPDiy, and not a hand-written output
driver.** This is a reversal of the original decision, not just its cost
estimate. The reasoning that chose EPDiy over FastEPD in the first place —
that FastEPD would need a wiring class (a shift-register expander) it had
never had — is now known to be backwards: FastEPD already has that class,
in active use for the closest relative of this exact board, and the thing
that actually needs writing for either library is the same size (a panel
table entry plus, possibly, small adjustments to three existing callback
functions), except that on FastEPD it is purely additive — no upstream file
is patched, no compile-time chip switch is bypassed, and no LGPL relink
obligation is taken on at all, since Apache-2.0 covers the whole thing. The
"work to support our board" table earlier in this ADR is superseded by this
section for exactly the row that mattered.

## Third pass: the three FastEPD callbacks checked line by line against our own board's reference

The second correction argued from architecture — FastEPD has a shift-register
class, EPDiy does not — without reading what the three functions that class
feeds (`LilyGoIOInit`, `LilyGoEinkPower`, `LilyGoRowControl`) actually do, or
checking them against a board that is proven to work. That is the same
altitude the first two corrections in this ADR both failed at. This pass
fixes that: `vroland/epdiy`, `Xinyuan-LilyGO/LilyGo-EPD47` (branch
`esp32s3`) and `bitbank2/FastEPD` were all cloned locally again, and every
claim below cites a line read directly, not a summary.

**Where the bit order and polarity facts below come from, stated plainly, as
asked.** The shift-register bit assignment and the initial register state are
read from `Xinyuan-LilyGO/LilyGo-EPD47`, branch `esp32s3`,
`src/ed047tc1.c` — **GPL-3.0**, and the one repository that is proven to
drive this exact board (`docs/hardware/board_notes.md`: "the vendor's stock
Arduino example runs on this unit as received"). The same bit order and
polarity also appear, independently, in `vroland/epdiy`'s
`src/board/epd_board_lilygo_t5_47.c` — **LGPL-3.0** — for the sibling ESP32
board. What is taken from both is the register layout and its reset state: a
fact about which of the 74HCT4094's eight output pins this board family
wires to which control signal, and what state each one must be in before the
panel is touched. That is a hardware fact in the same sense the D0–D7 pin
numbers and the GRAY4 nibble order already are elsewhere in this
repository (`docs/adr/0003`, `docs/hardware/board_notes.md`) — not a copy of
either project's control flow, and it is recorded here as coming from the
GPL-3.0 repository specifically so that fact is visible rather than
absorbed silently into an ADR that recommends depending on neither.

`Xinyuan-LilyGO/LilyGo-EPD47`'s `src/ed047tc1.c` sets, in `epd_base_init()`:

```c
config_reg.ep_latch_enable   = false;
config_reg.power_disable     = true;
config_reg.pos_power_enable  = false;
config_reg.neg_power_enable  = false;
config_reg.ep_stv            = true;
config_reg.ep_scan_direction = true;
config_reg.ep_mode           = false;
config_reg.ep_output_enable  = false;
```

and `push_cfg()` pushes them MSB-first in the order `ep_output_enable`,
`ep_mode`, `ep_scan_direction`, `ep_stv`, `neg_power_enable`,
`pos_power_enable`, `power_disable`, `ep_latch_enable`. `vroland/epdiy`'s
`epd_board_set_ctrl()` pushes the same eight names in the identical order.

**1. `LilyGoIOInit()` (`src/FastEPD.inl:705-722`) — verified, works
unmodified.** It sets `shift_data = 0x02 | 0x20 | 0x10` before the first
`bbepSendShiftData()`. `bbepSendShiftData()`
(`src/FastEPD.inl:640-655`) pushes bit 7 first, so FastEPD's bit map is
`0x80`=`ep_output_enable`, `0x40`=`ep_mode`, `0x20`=`ep_scan_direction`,
`0x10`=`ep_stv`, `0x08`=`neg_power_enable`, `0x04`=`pos_power_enable`,
`0x02`=`power_disable`, `0x01`=`ep_latch_enable` — matching the push order
above bit for bit, for all eight bits, not just the three set here.
Expanding FastEPD's `0x32` against that map gives `ep_scan_direction`=1,
`ep_stv`=1, `power_disable`=1, every other bit 0 — which is exactly
`ed047tc1.c`'s reset state above, field for field. Three independent
codebases, two licences, one register layout.

**2. `LilyGoEinkPower()` (`src/FastEPD.inl:663-703`) — needs a change, not a
rewrite.** The power-up sequence's shape matches `epd_poweron()`
(`ed047tc1.c:150-165`): clear `power_disable`, then set `neg_power_enable`,
then `pos_power_enable`, each separated by an escalating delay (100, 500,
100 in both, though FastEPD's are whole microseconds via
`delayMicroseconds()` and `ed047tc1.c`'s are CPU cycles via `busy_delay()` —
comparable order of magnitude, not checked further here). But
`LilyGoEinkPower()`'s very first step, before `power_disable` is even
cleared, is `shift_data |= 0x80; // OE on` — and it stays on for as long as
the panel is powered. Our board's own reference never does this: in
`epd_poweron()`, `ep_output_enable` is left untouched (false) through the
whole power-up sequence, and is instead set only inside `epd_start_frame()`
and cleared again in `epd_end_frame()` — asserted per frame, not held for
the panel's entire powered lifetime. This is a genuine divergence, not a
naming difference. Adopting `LilyGoEinkPower()` as-is would hold the source
driver's output stage enabled during every idle interval between frames,
which our own board's driver deliberately avoids. It needs the early
`OE on` step removed to match our reference, or — if there is a reason
T5-Pro's circuit wants it and ours tolerates it too — that has to be an
explicit, stated decision and checked on hardware, not an assumption carried
over silently.

**3. `LilyGoRowControl()`, `ROW_START` (`src/FastEPD.inl:727-748`) — needs
on-hardware verification, not asserted as correct.** The shape matches
`epd_start_frame()` (`ed047tc1.c:188-210`): mode on, then a CKV/STV dance
that drops `ep_stv`, pulses CKV, raises `ep_stv` again, pulses CKV again,
before output is enabled. But the timings are not the same numbers.
FastEPD's version uses `delayMicroseconds(7)`, `(10)`, `(8)`, `(10)`,
`(18)` around its CKV toggles; `ed047tc1.c`'s uses
`pulse_ckv_us(1, 1, true)`, a 240-cycle `busy_delay()`,
`pulse_ckv_us(10, 10, false)`, `pulse_ckv_us(0, 10, true)` — a different
pulse train, not the same numbers wearing different function names. E-paper
row-driver reset timing is usually tolerant of some slack, but "usually
tolerant" is not a fact this ADR can certify from source reading alone. This
one is a real open item for the board, not a paperwork item.

**4. `LilyGoRowControl()`, `ROW_STEP` (`src/FastEPD.inl:749-756`) — verified
complete, and answers a question this ADR had left open.** `ed047tc1.c`'s
per-row exposure time is *variable*: `epd_output_row(output_time_dus)`
threads a duration computed from the waveform matrix into
`pulse_ckv_ticks(output_time_dus, 50, false)` on every row, which is central
to how EPDiy/LilyGo's grayscale scheme works — one pass over the frame, per-
row timing carrying the grayscale information. `LilyGoRowControl()`'s
`ROW_STEP` takes no duration parameter at all: just `CKV off`, then a bare
latch pulse. Reading the caller
(`bbepSmoothUpdate()`, `src/FastEPD.inl:2740-2811`) resolves this rather
than leaving it a gap: FastEPD's grayscale algorithm is architecturally
different, not incomplete. It runs `iPasses = panelDef.iMatrixSize / 16`
full passes over the entire frame, and on each pass sends different pixel
*data* — looked up per pass from `pGrayMatrix` via `pGrayU`/`pGrayL` — at a
fixed per-row timing baked into `ROW_STEP` itself. Grayscale comes from
which passes a pixel appears black in, not from how long any single row is
held. Both are legitimate, established e-paper grayscale techniques;
`docs/hardware/board_notes.md`'s open item about measuring real refresh
timings and the partial-refresh budget applies to whichever one this board
ends up running, with different numbers to measure depending on which.

**One more fact worth carrying forward, not asked for but found along the
way.** `BB_PANEL_LILYGO_T5PRO`'s table sets `bus_speed` to 20 MHz
(`src/FastEPD.inl:226`) — not a number to copy. Our own board's proven pixel
clock, read directly from `ed047tc1.c`'s `esp_lcd_new_panel_io_i80()` setup
(`.pclk_hz = 10 * 1000 * 1000`, in the `#if !USER_I2S_REG` branch that
actually builds for the S3), is 10 MHz. A `BBPANELDEF` entry for this board
should carry 10,000,000, not T5-Pro's 20,000,000.

**Net verdict.** The register layout and reset state (item 1) transfer
unmodified, corroborated three ways. The data-bus reordering established in
the correction above (`{6,7,4,5,2,3,8,1}`) still stands; it was not
re-litigated here since it does not touch these three functions. Two items
— the power-on `OE` handling and the `ROW_START` timing — need a concrete
change or an on-hardware check before they can be trusted, not a full
rewrite of either function. Item 4 turned out to need nothing, once its
caller was actually read. None of this changes the licence conclusion:
FastEPD over EPDiy, Apache-2.0, additive only. It does mean `adp_display.c`
cannot simply register a new `BBPANELDEF` row and call the existing
callbacks unexamined — the fix for item 2 (delete one line) and item 3
(retime five delays and confirm on the panel) belong in the same commit as
the new panel table entry, not deferred to a follow-up.

## What would change this decision

Stated plainly, so a future reader can check the premises instead of
re-deriving them:

1. ~~FastEPD gains 74HCT4094 support.~~ **Overtaken by the second correction
   above: it already had it.** The premise this item was guarding against —
   that EPDiy's shift-register support was the scarce thing and FastEPD's
   absence of it was permanent — was itself unverified when written, and
   reading FastEPD's source showed the opposite: the shift-register class
   (`ioShiftSTR`/`ioSDA`/`ioSCL`, `LilyGoIOInit`/`LilyGoEinkPower`/
   `LilyGoRowControl`) was already there, just registered against a
   different LilyGo board's pins. Left here, struck through rather than
   deleted, as the record of which assumption this ADR got backwards.
2. **A closed-source or commercial build is decided on.** Then the relink
   obligation binds, and the exit above is taken.
3. **FastEPD's waveforms turn out not to fit this panel** in phase 2c, badly
   enough that we end up writing our own tables. At that point the borrowed
   maturity — the main thing being paid for with the choice — is gone, and
   the comparison should be run again from the start.

## Implementation

What actually shipped, for a reader who wants the current state without
replaying every correction above:

- **Vendored, not fetched.** `third_party/fastepd/` carries FastEPD
  (`bitbank2/FastEPD`, Apache-2.0, commit
  `9113bdd4d1b4174778b2b6c72fe851ba94825430`) unmodified — see
  `THIRD_PARTY_NOTICES.md` for the full file list and why vendoring, not the
  ESP-IDF component manager, is how it reaches the build (it has no
  component manifest of its own).
- **The vendor boundary is one file.** `third_party/fastepd/fastepd_shim.c`
  is the only file that `#include`s FastEPD.h, and the only file this
  project builds without `-Werror -Wconversion`
  (`apps/firmware_esp32/components/hac_fastepd_vendor/CMakeLists.txt`
  explains why in its own comment). `platform/esp32_t5s3/adp_display.c`
  never sees a FastEPD type; it calls `third_party/fastepd/fastepd_shim.h`'s
  small, hand-written C API instead, and compiles under the project's usual
  full warnings, confirmed by a real build, not inspection.
- **This board's callbacks are this project's own code**, not FastEPD's
  stock `LilyGoIOInit`/`LilyGoEinkPower`/`LilyGoRowControl` (registered
  against the T5 S3 Pro's different pins) and not copied from either
  driver's source — derived from the wiring and timing facts the "Third
  pass" section verified, with the two corrections that section called for
  actually applied: `hac_epd_eink_power()` never asserts output-enable
  during power-on (only `hac_epd_row_control()`'s `ROW_START` does, matching
  `ed047tc1.c`), and `ROW_START`'s eight delays are
  `board_config.h`'s `BOARD_EPD_ROW_START_DELAY_*` — the reference driver's
  own numbers, translated one call at a time, not FastEPD's T5-Pro guesses.
- **A fourth mismatch surfaced after the third pass, at the pixel level
  rather than the control-line level**: FastEPD's `BB_MODE_4BPP` nibble
  order is the inverse of this project's own (`docs/adr/0003`'s amendment
  covers it). `adp_display.c` keeps its own `framebuffer_t` in this
  project's usual order and nibble-swaps only the flushed rectangle into
  FastEPD's buffer at `flush()` time — bounded by the same dirty rect
  `update_policy` already computes, not a full-frame conversion.
- **The rectangle contract is half real — see the correction below.**
  `bbepFullUpdate()`'s `BB_MODE_4BPP` loop was read directly (`FastEPD.inl`,
  the pass loop around line 3140), and this bullet originally concluded that
  rows outside `[iStartRow, iEndRow]` are blanked *and* columns outside
  `[iStartCol, iEndCol]` are masked, "a genuine 2D clip". **The row half is
  correct; the column half is not** — the mask is AND-ed into the wrong half
  of the ping-pong DMA buffer. "Fourth correction" below carries the finding
  and what `adp_display.c` now does instead. `bbepFullUpdate()` is still the
  right entry point rather than `bbepPartialUpdate()`, which needs
  `BB_MODE_1BPP`.
- **Verified by a real build, not a syntax check.** `idf.py build` links a
  flashable `home_air_monitor.bin` with `CONFIG_HAC_DISPLAY_NULL` unset
  (the real display, phase 2b's actual deliverable) and, separately, with it
  set (the phase 2a null adapter, kept for headless runs per
  `main/Kconfig.projbuild`). `./scripts/check_sources.sh` and the host
  `ctest` suite both pass, unaffected — no `core/` file changed.
- **What is still unverified, on purpose.** Nothing in this implementation
  has touched real glass. The pixel clock, the `ROW_START` timings, the
  panel's actual partial-refresh budget and full-refresh interval, and
  whether FastEPD's `u8FivePointTwoMatrix` waveform table produces correct
  16-level grayscale on *this* panel rather than the one it was
  characterised on, are all phase 2c's job — `docs/hardware/board_notes.md`'s
  open items list carries each of them forward, next to the ones already
  there from phase 2a.

## First real-hardware finding: a mis-set panel flag, not a timing problem

The first flash confirmed the core, `update_policy`, the dirty-rect diff and
the rectangle handoff all work correctly on real hardware — a second
decision landed at 64.5s with dirty rect 824×228, identical to what the null
adapter reported in phase 2a. But the panel never visibly responded, and the
full and partial refreshes returned in ~20ms and ~50ms respectively — one to
two orders of magnitude faster than the "hundreds of milliseconds, multiple
waveform passes over 540 rows" a real refresh takes. That timing was the
signal that `bbepFullUpdate()` was taking an early-return path rather than
driving the panel wrongly, which narrowed the search to `FastEPD.inl` itself
rather than anything data- or timing-shaped.

**Root cause: this board's custom `BBPANELDEF` left `flags = BB_PANEL_FLAG_NONE`.**
Without `BB_PANEL_FLAG_SLOW_SPH`, `bbepIOInit()` hands `ioSPH` (this board's
STH pin) to the i80 bus setup as a **hardware chip-select**
(`s3_io_config.cs_gpio_num = pState->panelDef.ioSPH`), routed through the
GPIO matrix to the LCD_CAM peripheral — while `hac_epd_eink_power()` also
drives that same pin directly with a plain `gpio_set_level()` call, matching
how `ed047tc1.c`'s reference treats STH (a software-timed signal, not a
hardware CS). The i80 bus setup runs *after* our `pfnIOInit()` callback
(`bbepIOInit()`'s own sequence: callback first, bus setup second), so its
GPIO-matrix claim on the pin wins. `bbepWriteRow()`'s call to
`esp_lcd_panel_io_tx_color()` returns its error unchecked (the vendored
source even has the `Serial.printf` for it commented out), so a transfer
that fails fast completes its DMA descriptor and calls back into
`dma_is_done` quickly rather than hanging — consistent with the observed
"returns, loop continues, but far too fast to have driven 540 rows."

The fix confirms the third-pass verification was reading the right board:
`BB_PANEL_LILYGO_T5PRO` — the closest sibling FastEPD ships, cited
throughout the "Second correction" and "Third pass" sections above — sets
exactly this flag in its own table entry, for exactly this reason (its STH
is also software-timed, not hardware CS). This project's custom
`BBPANELDEF` had every wiring fact right and simply never copied that one
flag, because nothing in the "Third pass" review touched `BBPANELDEF.flags`
— it checked the three callback functions line by line and the pin values
field by field, and this bug lived one level structurally to the side of
both.

**Fixed in `third_party/fastepd/fastepd_shim.c`**: `paneldef.flags =
BB_PANEL_FLAG_SLOW_SPH`, with a comment at the site explaining the
GPIO-ownership conflict this avoids. Two smaller, genuinely unintentional
divergences from stock `LilyGoEinkPower()` were also found and fixed while
re-verifying the power sequence line by line as requested: one dropped
100µs settle delay in the power-on path and one in power-off, beyond the
deliberate output-enable omission `docs/adr/0007`'s "Third pass" item 2
already covers. Comprehensive entry/exit and timing instrumentation was
added alongside the fix (`hac_epd_io_init`/`hac_epd_eink_power` entry-exit
logging, once-per-pass `ROW_START` logging, a positive success confirmation
from `fastepd_shim_init()` naming every field `bbepFullUpdate()`'s
early-return paths depend on, and microsecond-precision elapsed-time logging
around every `bbepFullUpdate()` call) — a real build with the fix applied
compiles clean; **the fix itself is not yet confirmed on hardware.** A whole-panel test-pattern flush was also added at startup
(`apps/firmware_esp32/main/main.c`'s `run_epd_test_pattern()`, temporary,
removed once phase 2c confirms real content renders correctly) to separate
"does the panel respond to a drive sequence at all" from "is `screen_home`'s
rendering or the GRAY4 nibble swap correct," independent of whichever of
those questions this fix resolves. It began as a solid-black then
solid-white pair of `REFRESH_FULL` flushes two seconds apart; "Fourth
correction" below covers why that was wrong and what replaced it.


## Fourth correction: FastEPD's 4bpp column clip, and a boot diagnostic that ignored the panel's own interval

Three defects, all found by reading rather than on hardware, all reachable
from ordinary inputs, and all now handled in this project's own code rather
than by patching the vendored source.

### 1. The 4bpp column mask lands in the wrong half of the DMA buffer

`bbepFullUpdate()`'s `BB_MODE_4BPP` pass loop (`FastEPD.inl`) builds each
row into

```c
d = &pState->dma_buf[iDMAOff];      /* iDMAOff alternates 0 <-> native_width/4 */
```

and then, when a region rectangle narrows the columns, masks with

```c
dst = (uint32_t *)pState->dma_buf;  /* the start of the buffer, not d */
for (n = 0; n < pState->native_width/16; n++) dst[n] &= src[n];
```

`iDMAOff` alternates because `bbepWriteRow()` hands the row to
`esp_lcd_panel_io_tx_color()` and only waits for completion at the top of
the *next* call — the two halves ping-pong against a live transfer. So:

- **even rows** are masked in the half they were built in, correctly;
- **odd rows** are never column-masked at all and go to the panel at full
  width;
- the masking write on an odd row touches the half whose DMA transfer may
  still be in flight, which is a race on top of the wrong-region write.

The earlier "Implementation" bullet above claimed this was "a genuine 2D
clip, verified by reading `FastEPD.inl` directly". The reading happened; it
missed the `iDMAOff` indirection. Recording that explicitly, because the
lesson is about the shape of the verification, not just the outcome: reading
the clipping block was not enough without also reading where the buffer it
writes to comes from.

**What this project does instead.** `platform/esp32_t5s3/adp_display.c`'s
`partial_rect_for_fastepd()` widens every `REFRESH_PARTIAL` rect to the
panel's full width before handing it over. The masking block is guarded by
`if (iStartCol > 0 || iEndCol < native_width-1)`, so a full-width request
never reaches it — which removes the wrong-region mask and the DMA race
together. The row clip, which is correct, is kept and is where most of the
saving was: 228 of 540 rows on the one real-hardware observation so far.

The cost is that the unchanged pixels in the dirty *rows* get driven on a
partial refresh. That is the cheap side of the trade against a column clip
that is wrong on half the rows. `update_policy`'s partial-refresh budget
still governs how many such refreshes may precede a full one, so the
vendor's actual warning — prolonged partial refreshing — stays bounded by
the same mechanism as before.

**Why not patch `FastEPD.inl`.** The fix upstream is one token
(`dst = (uint32_t *)d;`). It is not applied here for the same reason
`THIRD_PARTY_NOTICES.md` already gives for `bbepSmoothUpdate()`'s
`u8Color = BBEP_WHITE` bug: this project works around FastEPD defects by not
reaching the affected code, keeping `third_party/fastepd/` byte-identical to
the pinned commit so a resync is a replace rather than a merge. If the
column clip is ever needed, that is the point to revisit — and it should be
an upstream report first.

### 2. A one-row rect leaves the panel's high-voltage rails energized

`bbepFixRect()` rejects any rectangle whose `iStartRow >= iEndRow` (height
below 2) or `iStartCol >= iEndCol` (width below 2). `bbepFullUpdate()`
reaches that check *after* it has already called `bbepEinkPower(pState, 1)`,
and returns `BBEP_ERROR_BAD_PARAMETER` without the matching
`bbepEinkPower(pState, 0)` — so the rails stay up, and no later call is
obliged to bring them down.

`framebuffer_diff_dirty_rect()` returns a one-row rectangle whenever every
differing byte falls in a single row. That is an ordinary output, not a
malformed one. Worse, `event_loop.c`'s `redraw()` deliberately skips
`update_policy_commit()` when a flush fails, so the same rect is retried on
the next tick: the failure repeats at the FreeRTOS tick rate with the rails
held on.

`partial_rect_for_fastepd()` therefore guarantees at least two rows, growing
the rect downwards where there is room and upwards against the bottom edge.
Full width already satisfies the column half. If no rectangle can satisfy
both constraints — a panel under two pixels tall, or a rect that clips away
to nothing — the adapter passes `NULL` and flushes the whole panel, which is
always safe: `bbepFullUpdate()` takes its `else` branch for a `NULL` rect
and derives the bounds from the panel size without validating anything.

The rect actually sent is logged on every partial flush, so what the backend
received is never inferred from the requested rect alone.

### 3. The boot test pattern contradicted `min_full_refresh_interval_ms`

`run_epd_test_pattern()` as first written issued two `REFRESH_FULL` flushes
two seconds apart, outside `update_policy` entirely. The policy then started
with `have_full_refresh = false`, so its own first frame was due
immediately: **three whole-panel refreshes within about two seconds against
a declared minimum interval of 60 000 ms.**

That interval is reported to the core through `port_display` and is the one
panel limit `core/app/update_policy.c` exists to enforce. A boot-time
diagnostic that ignored it was the single place in the firmware contradicting
a value the rest of the code treats as the panel's own — and the fact that
the number is still a guess pending phase 2c is a reason to measure it, not a
licence to disregard it.

It is now one whole-panel drive followed by the declared interval:

- a single `REFRESH_CLEAR` (this backend's `CLEAR_SLOW`: four whole-panel
  darken/lighten cycles, then the framebuffer's contents) on a framebuffer
  cleared to black. A stronger "does the panel respond at all" probe than the
  black/white pair it replaces, in one operation instead of two, and it
  exercises the `REFRESH_CLEAR` mapping that otherwise had no caller;
- then `clock->delay_ms(BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS)` before
  `event_loop_start()`, so the policy's first frame — `screen_home` on a
  white ground, which is what now proves the light direction — lands outside
  the interval the diagnostic consumed.

The honest cost is a minute of boot time whenever the diagnostic is enabled.
It is therefore behind `CONFIG_HAC_EPD_TEST_PATTERN`
(`main/Kconfig.projbuild`), default `y` while phase 2b is bringing the panel
up and to be set to `n` once the panel is known to work — at which point a
boot performs exactly one whole-panel refresh, `screen_home`'s own first
frame, and reaches it immediately.

### What is still unverified

`idf.py build` is the only check these three changes have had. None of them
has touched glass, and `partial_rect_for_fastepd()` — a pure function, and
exactly the kind this project's rules say gets unit tests before being wired
in — has none, because `platform/esp32_t5s3/` has no host build to put a test
in. That is the same gap `docs/hardware/board_notes.md` now records as an
open item.


## Second real-hardware finding: the board config was a dangling pointer

The first flash after the "Fourth correction" fixes produced a log that
looked like the panel was being driven correctly and still showed nothing:
`bbepFullUpdate_rc=0`, `row_steps_last_pass=539` on every pass, a sensible
shift-register byte entering each `ROW_START` (252 = OE+MODE+SCAN+STV+rails)
and a sensible one after power-off (34 = SCAN+POWER_DISABLE). Two things did
not fit. A single `REFRESH_CLEAR` took **16.4 seconds** where the arithmetic
says under a second, and the task watchdog fired three times on IDLE0 with
`main` running on CPU 0.

Decoding all three watchdog backtraces against the ELF put every one of them
inside `delayMicroseconds()` called from `hac_epd_row_control()`'s
`ROW_START` delay chain, at three *different* delays. Three independent
samples landing in a code path that should account for roughly 35 µs out of
16 s is not a coincidence — it says the delays themselves were enormous.

**Root cause: `fastepd_shim_init()` stored the caller's pointer to a stack
variable.** `adp_display_init()` builds its `fastepd_board_cfg_t` as an
ordinary local and passes its address; the shim kept it in a module-scope
`const fastepd_board_cfg_t *s_cfg`. Everything that reads `s_cfg` *during*
init is fine — `hac_epd_io_init()` runs from inside `bbepIOInit()`, while the
frame is still alive, which is exactly why the pin setup and the whole
shift-register path worked and logged plausible values. Everything that reads
it *later* read a reused stack frame:

- `hac_epd_row_control()`'s `ckv_gpio` became garbage, so **CKV was never
  toggled**. The panel's vertical shift register never advanced, every row
  landed on the same line, and the panel stayed blank — the symptom that had
  been chased through pin tables, bit orders and panel flags for two
  corrections.
- `row_start_delay_us[]` became garbage, turning each `ROW_START`'s twelve
  `delayMicroseconds()` calls into hundreds of milliseconds. That is the 370
  ms per pass the log shows, the 16.4 s total, and the watchdog: CPU 0 sat in
  `ets_delay_us()` and never let IDLE0 run.
- `hac_epd_eink_power()`'s `sph_gpio` became garbage, so STH was never raised
  at power-on either.

**The fix is a copy, not a documented requirement.** `s_cfg` is now a
`fastepd_board_cfg_t` by value, assigned `*cfg` at the top of
`fastepd_shim_init()`, with a NULL check returning `HAL_ERR_INVALID_ARG`.
`fastepd_shim.h` states the contract both ways: `cfg` is copied and need not
outlive the call, `log` is borrowed and must. Making the caller keep the
struct alive would have worked too and would have been the worse fix — the
trap would still be there for the next caller, and nothing in the signature
hinted at it.

Two things worth carrying forward from how this was found rather than from
what it was:

- **The timing instrumentation earned its place twice.** `elapsed_us` around
  `bbepFullUpdate()` was added in the first hardware finding to catch an
  early return that looked like success; here the same field caught the
  opposite failure, a call that did everything it was asked to and took forty
  times too long. Neither was visible in a return code.
- **The watchdog backtrace was the diagnosis, not the noise.** Three samples
  were enough to localise the hot path exactly, because a task watchdog
  firing repeatedly samples a blocked CPU at a fixed interval. It was worth
  decoding before changing anything.

The watchdog firing is expected to disappear with the delays corrected: 540
rows of 256 bytes at the 10 MHz pixel clock is roughly 20 ms per pass, so
`CLEAR_SLOW`'s ~40 passes should land near one second, inside the 5 s task
watchdog window. If it returns once the panel actually renders, the fix is
not to raise the timeout but to yield — FastEPD's own pass loops carry
commented-out `vTaskDelay(0)` calls at exactly the right places, and
`ROW_START` is a hook this project already owns.

Still unconfirmed, and the reason this section does not claim the panel
works: the log that produced this diagnosis also shows the device restarting
between two identical test-pattern cycles, with the boot banner and reset
reason outside the captured fragment. `CONFIG_ESP_TASK_WDT_PANIC` is not set,
so the task watchdog is not the cause. Brownout detection is enabled at the
most sensitive level (`CONFIG_BROWNOUT_DET_LVL=7`) and the panel rails were
energized continuously for 16 s, which makes a brownout plausible but
unevidenced. The next flash needs the log from the boot banner onward.


## Third real-hardware finding: timings fixed, panel still blank, STH handed to the peripheral

The flash after the dangling-pointer fix confirmed that fix completely and
left exactly one thing unexplained. `REFRESH_CLEAR` fell from 16.4 s to
1.896 s and `REFRESH_FULL` came in at 1.152 s; the task watchdog stopped
firing; there was no reset loop (the earlier restart was `rst:0x15
USB_UART_CHIP_RESET`, i.e. the flash itself, not brownout); the rails came up
(`shift_data=60`); `ROW_START` reported 539 row steps on every pass; the pass
counts were exactly right — 44 for `CLEAR_SLOW`, 28 for `CLEAR_FAST`, which
pins FastEPD's waveform matrix at 10 passes and leaves nothing unaccounted
for. The 60 s interval elapsed, `update_policy` issued its first frame, and
`screen_home` was flushed as a full refresh. **And the panel showed
nothing.**

With the data path, the pass structure, the rails, the row stepping and the
timings all accounted for, what remained were the two divergences
`docs/hardware/board_notes.md`'s "Per-row signal sequence" had already
recorded in phase 2b, from flashing the vendor's own working demo to this
unit. That section ranks them: STH's timing is "the load-bearing part", CKV's
polarity is "inverted with respect to this sequence".

**STH is now driven by the LCD_CAM peripheral.** The panel needs STH's
falling edge a fixed, hardware-generated interval ahead of the first data
clock, on the same CKH train. FastEPD bit-bangs SPH with `gpio_set_level()`
and then calls `esp_lcd_panel_io_tx_color()`, so the falling edge is
separated from the first data clock by the i80 driver's transaction-setup
time — unknown, variable, and with CKH not clocking at all in between. A
source driver sampling STH against CKH edges has nothing to sample, which is
a complete explanation for a blank panel with everything else correct.

The fix needed no change to any vendored file. `s3_io_config` is FastEPD's
own module-scope static, and because FastEPD's C mode inlines its
implementation into whichever translation unit includes `FastEPD.h`, that
static is ordinary file-scope data in `fastepd_shim.c` — the same struct
`bbepIOInit()` itself writes to a few lines later for `pclk_hz` and
`cs_gpio_num`. So the shim now, before calling `bbepIOInit()`:

- points `panelDef.ioDCDummy` at the real STH instead of a spare GPIO, so
  the pin is owned by the peripheral through the GPIO matrix;
- points `panelDef.ioSPH` at the spare, so FastEPD's per-row software SPH
  writes land on a pin connected to nothing instead of fighting the
  peripheral for the real one;
- inverts `dc_levels` from FastEPD's defaults (`idle 0, data 1`) to
  `idle 1, data 0`. The default holds D/C high for the whole data phase,
  the opposite of the reference's own `{ idle 0, cmd 1, dummy 0, data 0 }`;
- drops the `gpio_set_level(STH, 1)` from `hac_epd_eink_power()`'s power-on
  path and the `bbepPinMode(STH, OUTPUT)` from `hac_epd_io_init()`, since
  neither layer owns the pin any more.

Two honest limits on this. There is still no command phase —
`lcd_cmd_bits` is 0 and FastEPD passes no command — so the high pulse is not
the reference's ten clocks wide; what is reproduced is the falling edge's
placement, which is the part board_notes.md calls load-bearing, not the whole
waveform. And STH now idles high between refreshes with the rails down. That
is worth stating because it sounds like a new hazard and is not: FastEPD's
software arrangement already left it high between refreshes
(`bbepWriteRow()` drops it, the DMA-done callback raises it, nothing lowers
it before `bbepEinkPower(0)`), so the idle state is unchanged.

**CKV is deliberately not touched in the same step.** It is still held high
across the data transfer, because FastEPD raises it inside `bbepWriteRow()`
after this project's `ROW_STEP` callback has already returned. Fixing it
means taking the signal away from FastEPD — pointing `panelDef.ioCKV` at the
spare pin and pulsing the real one from `ROW_STEP`, which would reproduce
board_notes.md's recorded per-row order exactly: wait, pulse LE, pulse CKV,
then transfer with CKV low. That is the next step if STH alone is not enough.
It is held back for two reasons: board_notes.md ranks STH higher, and the CKV
pulse width is a number nobody has measured on this board, so doing it now
would mean adding another guessed constant to `board_config.h` in the same
change as an unverified hypothesis.

**If both fail, the decision this ADR made has to reopen.** The remaining
divergence after CKV would be the command phase itself, which cannot be
reached from this side of the vendor boundary: it needs
`esp_lcd_panel_io_tx_color()` to be called with a real command and
`lcd_cmd_bits = 10`, both inside `bbepWriteRow()`. The options at that point
are a patch to `FastEPD.inl` (against the policy stated above, and it would
have to be reported upstream), or a row loop of this project's own that uses
FastEPD only for its waveform table and grey lookup — which is most of what
choosing FastEPD was meant to avoid. Neither should be started before the
cheap experiments above are spent.


## Fourth real-hardware finding: three references agree that CKV is hardware-generated

Routing STH to the i80 D/C pin changed nothing: the panel stayed blank and
the log came back byte-for-byte equivalent (`elapsed_us` 1,896,237 against
the previous 1,896,429), with the flashed image confirmed by matching the
device's reported `ELF file SHA256` against the local build. So the search
widened to the two other implementations known to drive this glass.

### What this eliminated

Three facts from `vroland/epdiy`'s LilyGo T5 4.7 board definition and from
the Tactility project's working ESP32-S3 engine for this exact board
corroborate what this project already does, and remove three suspects for
good:

- **The shift-register bit order is right.** epdiy pushes
  `output_enable, mode, scan_direction, stv, neg_power_enable,
  pos_power_enable, power_disable, latch_enable`, MSB first -- identical to
  this project's `SHIFT_BIT_*` values. That is now a third independent
  corroboration, after `ed047tc1.c` and epdiy's own board file during the
  "Third pass".
- **The power sequences are right.** epdiy's power-on is
  `power_disable = false` / 100 us / `neg_power_enable` / 500 us /
  `pos_power_enable` / 100 us, and its power-off is
  `pos_power_enable = false` / 10 us / `neg_power_enable = false` / 100 us /
  `power_disable = true`. Both match `hac_epd_eink_power()` exactly,
  including the deliberate absence of an output-enable step.
- **The pixel clock and the pass structure are right.** Tactility's engine
  defaults to a 10 MHz pixel clock -- the same value `board_config.h` took
  from the vendor's i80 setup -- and reports a full refresh in roughly 1.2
  seconds. This project measures 1.152 s. Whatever is wrong is not the clock
  rate, the number of waveform passes, or the per-pass row count.

### What it found

**All three references generate CKV in hardware; this project generates it in
software, and had it inverted as well.**

- `vroland/epdiy`'s ESP32-S3 LCD backend drives CKV from the **RMT**
  peripheral as a free-running loop phase-locked to the line period
  (`low_time = line_length_us * 10 - ckv_high_time`), with STH connected to
  the LCD_CAM's **DE** output and LE to its **HSYNC** output through the GPIO
  matrix. The entire line structure is hardware-timed; software only toggles
  STV once per frame.
- The Tactility project's engine for this exact board -- written because
  epdiy's maintainers declined to support the S3 variant -- keeps the row
  transfer on the **LCD peripheral in i80 mode**, the same as FastEPD, but
  describes its own contents as "the shift register control and power
  sequence, **the CKV gate clock (RMT)**, the row transfer, the row by row
  update loop and the clear sequence". So a per-row i80 transfer model *is*
  workable on this board. A software-toggled CKV is the part nobody does.
- The vendor's own `ed047tc1.c` reaches CKV through `pulse_ckv_us()`, which
  `board_config.h`'s comment already recorded as RMT-timed when this project
  translated it into `gpio_set_level()` + a delay.

### What changed here

Two things, both inside `third_party/fastepd/fastepd_shim.c`, neither
touching a vendored file:

- **CKV taken away from FastEPD.** `panelDef.ioCKV` now points at the spare
  GPIO, because `bbepWriteRow()` raises CKV immediately before each
  transfer and never lowers it -- holding it HIGH across the whole data
  phase, the inversion `board_notes.md` names. The real CKV is pulsed from
  `hac_epd_row_control()`'s `ROW_STEP` instead, after the latch, and left
  low for the data: the per-row order of latch, CKV pulse, transfer that
  `board_notes.md` records from the vendor's working demo. The pulse width
  is `BOARD_EPD_ROW_CKV_PULSE_US`, 1 us, taken from `ed047tc1.c`'s smallest
  `pulse_ckv_us()` and marked in `board_config.h` as not measured here.
- **CKH idles low.** FastEPD's `s3_io_config` leaves `pclk_idle_low = 0`,
  and `esp_lcd_panel_io_i80.c` inverts that field on its way to the
  hardware (`lcd_ll_set_clock_idle_level(dev, !flags.pclk_idle_low)`,
  confirmed in the local ESP-IDF 5.5 tree rather than assumed), so the
  default parks the write strobe high between transactions. epdiy calls
  `lcd_ll_set_clock_idle_level(dev, false)`. The two agree on the active
  edge. Idling high makes the exit from idle an extra edge the source
  driver can shift on, which is the shape of the defect FastEPD's own
  `bbepIOInit()` comment describes and offers only a bit-bang fallback for;
  epdiy absorbs it with a dummy phase the esp_lcd i80 API does not expose.

### The exit, now concrete

If this does not light the panel, the remaining divergence is the mechanism
itself -- a software-scheduled CKV pulse and no dummy phase -- and that is
not reachable from this side of the vendor boundary. **The technical reason
this ADR rejected EPDiy no longer holds.** That reason was that EPDiy's only
S3-capable backend needs dedicated GPIOs for `leh` and `stv` which this board
routes through the 74HCT4094. Two things now exist that did not when that was
written: `AdaSzi/epdiy` at tag `2.1.3-t5s3.1`, a fork carrying an S3
row-by-row engine for this board, and Tactility's standalone engine, whose
scope is small and fully enumerated above. EPDiy's LGPL-3.0 is already an
accepted licence for this project (`CLAUDE.md`, "Licensing constraints"), so
the obstacle is effort, not law.

Nothing from either project has been copied. What was taken is the same
class of fact this repository has always taken from reference drivers --
a bit order, a delay, a clock rate, which peripheral generates which signal
-- and `THIRD_PARTY_NOTICES.md`'s note on EPDiy already covers reading it for
hardware facts without linking it.


## Decision reversed: EPDiy (AdaSzi fork), FastEPD removed

**Status: this supersedes the Decision section above.** FastEPD is no longer
this project's display backend. The backend is EPDiy, via `AdaSzi/epdiy` at
tag `2.1.3-t5s3.1`, fetched as an ESP-IDF managed component and pinned in
`apps/firmware_esp32/components/hac_platform/idf_component.yml`.

### Why the original reason to reject EPDiy stopped holding

This ADR rejected EPDiy on one technical ground: its only ESP32-S3-capable
render method streams whole frames through LCD_CAM and requires the latch
(LEH) and vertical start (STV) lines on dedicated GPIOs, which this board
routes through a 74HCT4094 shift register. That is still true of upstream, and
upstream's maintainers have declined to support this board for that reason.

The fork removes it. Its `src/output_common/render_method.h` gates a second S3
path behind `CONFIG_EPD_ESP32S3_I80_ROW_OUTPUT`, described in its own Kconfig
help text as being for boards that "drive LEH, STV, output enable, mode and
the panel power rails through a 74HCT4094 shift register instead" — and it
names this board's silkscreen revision, "LILYGO T5-4.7 S3 V2.3/V2.4", as the
example. It reuses EPDiy's row-by-row render path, which previously existed
only for the original ESP32's I2S peripheral, and sends each row's pixels over
LCD_CAM in i80 mode. `src/board/epd_board_lilygo_t5_47_s3.c` carries this
board's pins, and they match `platform/esp32_t5s3/board_config.h` exactly.

### Why FastEPD had to go, which is not a verdict on FastEPD

FastEPD is Apache-2.0 — the easier licence of the two — and it drove this
panel's shift register, power sequencing and waveform pass structure
correctly: the four hardware findings above confirmed the bit order, the
power delays, the pixel clock and the pass counts, several of them against
three independent references.

What it could not do is generate STH, LE and CKV from hardware on a
continuous clock train. Its S3 output model issues one
`esp_lcd_panel_io_tx_color()` transaction per row with those signals toggled
by software around it, and the gaps between them are software-scheduling
intervals of variable length during which CKH does not clock at all. Every
implementation known to drive this glass — the vendor's own `ed047tc1.c`,
upstream EPDiy, and the fork — puts that structure in hardware. That is a
property of the backend's architecture, not a setting, which is why four
rounds of configuration changes moved the measured behaviour and never the
panel.

### What the migration cost, and what it removed

The port boundary held. `core/`, the UI, `update_policy`, the framebuffer
diff and the event loop are untouched; `platform/esp32_t5s3/adp_display.c` is
the only file that knows which backend exists. What went away with FastEPD:

- `third_party/fastepd/` entirely, the `hac_fastepd_vendor` component, and
  `fastepd_shim.c` — the vendor-boundary translation unit that existed only
  because FastEPD's C mode inlines its implementation into whoever includes
  its header. EPDiy needs no such boundary, and `adp_display.c` includes
  `epdiy.h` directly under the project's full warning set.
- `swap_nibbles_into_fastepd()`, because EPDiy's nibble order and stride are
  this project's own (`docs/adr/0003`'s second amendment). The buffer is now
  passed by pointer with no copy.
- `partial_rect_for_fastepd()` and the full-width widening it forced, because
  EPDiy crops honestly on both axes.
- Most of the unmeasured surface `board_config.h` carried:
  `BOARD_EPD_PIXEL_CLOCK_HZ`, the eight `BOARD_EPD_ROW_START_DELAY_*_US`,
  `BOARD_EPD_ROW_CKV_PULSE_US`, `BOARD_EPD_LINE_PADDING_BYTES`,
  `BOARD_EPD_VCOM_MV`, `BOARD_EPD_DC_DUMMY_GPIO` and the data-bus order
  macro. Every one of them was marked "taken from the working reference, not
  derived" or outright guessed. They are EPDiy's business now.
- Two of `board_notes.md`'s open items, which existed only to track FastEPD's
  borrowed waveform table and its untestable pure helpers.

What it added: an LGPL-3.0-or-later dependency, whose obligations
`THIRD_PARTY_NOTICES.md` now sets out in full, and one `#pragma GCC
diagnostic` in `adp_display.c` suppressing `-Wsign-conversion` around the
`epdiy.h` include, because the include chain reaches ESP-IDF's own
`xtensa/core-macros.h` whose `XTHAL_COMPARE_AND_SET()` returns an `int` from a
function declared `unsigned`. That is narrower than the alternative
`cmake/warnings_idf.cmake` offers, which would drop `-Wconversion` for the
whole `hac_platform` component and so for `adp_clock.c`, `adp_log.c` and
`adp_display_null.c` as well.

### Still unverified

Nothing here has lit the panel. Both build configurations link, the host
suite and the static checks pass, and the mode mapping was derived from the
waveform table EPDiy actually ships for the ED047TC1 (five modes, of which
`MODE_GC16` and `MODE_GL16` are the two this adapter uses) rather than
guessed. Whether the panel renders, and whether the greys are right, is the
next flash.


## First EPDiy flash: a NULL waveform, and the retry storm it exposed

The panel stayed blank and `epd_draw_base()` returned `256` on every flush,
which is `EPD_DRAW_NO_PHASES_AVAILABLE`. Two separate defects, both this
project's own.

### The waveform was NULL

`epd_draw_base()`'s first line is `if (waveform == NULL) return
EPD_DRAW_NO_PHASES_AVAILABLE;`, and this adapter passed
`EPD_BUILTIN_WAVEFORM` -- which `epd_highlevel.h` `#define`s to `NULL`. The
macro means "use the display's default" only to the highlevel API, which
substitutes the default internally before calling down. Passed straight to
`epd_draw_base()` it is simply a null pointer, rejected before the mode or the
temperature is even looked at.

The adapter now names the panel once (`static const EpdDisplay_t *const
s_panel = &ED047TC1;`), hands it to `epd_init()`, and passes
`s_panel->default_waveform` to every `epd_draw_base()` call, so the two cannot
name different panels.

Worth recording rather than just fixing: the mode mapping was derived
carefully from the waveform table, the packing was verified against
`epd_draw_pixel()`, the crop behaviour was read in the render path -- and the
call still failed on an argument whose name actively suggested it was right.
A constant named `*_BUILTIN_*` that is defined to NULL only works inside the
API layer that substitutes it.

While confirming this, the temperature argument was checked too, since
`EPD_DRAW_NO_PHASES_AVAILABLE` is also what a failed temperature-range lookup
returns. It is not implicated: `waveform_temp_range_index()` returns 0 for any
temperature when `num_temp_ranges == 1`, which is the case for
`epdiy_ED047TC1` (`{ .min = 20, .max = 30 }`, one range).
`BOARD_EPD_ASSUMED_TEMPERATURE_C`'s comment is accurate as written.

### The retry storm was a real defect, not just noise

The failing flush repeated at the FreeRTOS tick rate for as long as the board
was powered -- hundreds of failures a minute, each with its own log lines.
That is a consequence of a deliberate design choice meeting a missing one:
`event_loop.c`'s `redraw()` skips `update_policy_commit()` when the flush
fails, which is correct (a failed flush must not consume the panel's
partial-refresh budget), but nothing then slowed the retry down, so the
decision stayed "due now" forever.

`event_loop.c` now counts consecutive iterations that wanted a redraw and did
not achieve one -- a failed flush, or a diff that came out empty -- and floors
the wait at one second doubling to the same 60 s ceiling the idle wait uses.
Any successful flush resets it. A transient failure still recovers within a
second; a persistent one stops drowning the log and the CPU. The failure
itself is now logged at ERROR with the consecutive count, which is what makes
a persistent fault legible in a boot log rather than inferrable from volume.

This was identified as a latent defect during the code review that preceded
the FastEPD work, when it was unreachable; the empty-diff path it shares a
shape with was noted at the time as "harmless, just wasteful". It became
reachable the moment a flush could fail persistently, and the FastEPD era
never triggered it only because those flushes all returned success.


## Second EPDiy flash: the low-level API was the wrong entry point

With the waveform passed explicitly, `epd_draw_base()` got past its NULL check
and failed one step later with `EPD_DRAW_LOOKUP_NOT_IMPLEMENTED` (2) and an
explicit message of its own: *"no output lookup method found for your mode and
LUT size!"*.

`output_common/lut.c`'s `find_lut_functions()` says why. For
`MODE_PACKING_2PPB` it selects a lookup only when the mode **also** carries
`PREVIOUSLY_WHITE` or `PREVIOUSLY_BLACK`; with neither, both branches fall
through and the function pair stays NULL. That is not an omission in EPDiy. A
whole-buffer 2 bpp-packed draw carries no information about what each pixel is
coming *from*, and an e-paper waveform needs exactly that, which is why the
ED047TC1 waveform ships `MODE_EPDIY_WHITE_TO_GL16` and
`MODE_EPDIY_BLACK_TO_GL16` (its modes 16 and 17) alongside the three obvious
ones. Those two were noticed when the mode mapping was first derived, and
dismissed as exotic. They were the clue.

**The adapter now goes through the highlevel API**, which is the layer that
holds the missing information. `epd_hl_update_area()` keeps a back buffer
describing what the glass shows, computes the difference against the front
buffer, and calls `epd_draw_base()` with `MODE_PACKING_1PPB_DIFFERENCE | mode`
plus the waveform and the dirty-line/column masks — a combination for which a
64 K lookup does exist. All three of the things that failed are handled by
that one call.

**The earlier argument against the highlevel API was wrong, and worth
recording as such.** It was rejected when this adapter was written, on the
grounds that its front/back diff would duplicate
`core/ui/framebuffer.c`'s `framebuffer_diff_dirty_rect()`. The two are not the
same question. Ours decides *whether, and over which region,* to refresh —
policy, which `core/app/update_policy.c` acts on. EPDiy's decides *how to
drive each pixel inside that region*, which requires the previous pixel value
and is waveform business. Avoiding the second one meant throwing away the
state the hardware needs.

Two things improved rather than merely moved:

- **The framebuffer is no longer allocated twice.** `get_framebuffer()` now
  returns EPDiy's highlevel front buffer directly, because its nibble order
  and `width / 2` stride are this project's own
  (`docs/adr/0003`'s second amendment). That drops a 253 KB PSRAM allocation
  and leaves no copy on the flush path at all. EPDiy's own front/back/difference
  buffers cost about 1 MB of the 8 MB pool.
- **`epd_hl_init()` memsets front and back to white**, so the model starts
  consistent with the state `adp_display_null.c` and the SDL adapter also start
  from. There is no window where the back buffer holds garbage.

`REFRESH_CLEAR` is now `epd_hl_set_all_white()` plus a `MODE_GC16` update
rather than `epd_fullclear()`, which ends in `assert(err ==
EPD_DRAW_SUCCESS)` — an abort for a condition this adapter already reports
through `hal_status_t`. Its one remaining inconsistency (it discards the
rendered screen and leaves `event_loop.c`'s previous-frame copy stale) is
documented at the site; nothing issues `REFRESH_CLEAR` today.

### The backoff earned its place on the first try

The retry storm fix added in the previous flash was exercised immediately and
behaved exactly as designed: `consecutive_failures` 1, 2, 3, 4, 5 at 1, 2, 4
and 8 second intervals, with the failure logged once per attempt at ERROR
instead of several hundred times a minute at INFO. The whole diagnosis above
was read off a log that stayed legible because of it.


## Review of the EPDiy integration after the second flash

After the highlevel-API version was built, the adapter was re-read line by
line against the EPDiy source that is actually built
(`managed_components/epdiy`, commit `161de52`) and against the fork's own
example for this board, `examples/lilygo-t5-s3/main/main.c`. Two defects and
one semantic error were found in the change described in the previous
section, all of them introduced by it.

**The panel was not powered by anything in the call path.** The previous
section's change removed the `epd_poweron()`/`epd_poweroff()` bracket around
the update, and a comment in `adp_display.c` justified that by saying
`epd_hl_update_area()` powers the panel itself. It does not: `highlevel.c`
contains no call to either function, `epd_highlevel.h` says in two places
that power must be enabled with `epd_poweron()` beforehand, and the fork's
example brackets every update explicitly. This review went further and
predicted that the version would return `EPD_DRAW_SUCCESS` on every flush
while leaving the glass blank. **That prediction was wrong** -- the version
had in fact already been flashed, and it rendered; see the next section.
The finding that stands is the narrower one: the code depended on something
EPDiy documents as the caller's job and did not do it. Every path in
`adp_flush()` now brackets its own drive, and the comment says plainly which
layer is responsible and why. `epd_poweron()`/`epd_poweroff()` were also
checked for reference counting -- they have none, each is a direct call into
the board's sequence -- so `port_display_t::power(true)` does not survive a
flush, which `adp_power()`'s comment now says rather than implying otherwise.

**`REFRESH_FULL` was not a full refresh.** `epd_hl_update_area()` builds its
difference only from pixels whose value changed and drives only those.
Mapping `REFRESH_FULL` to a bare `MODE_GC16` through it would therefore never
touch unchanged pixels, so it would not clear their accumulated ghosting --
which is the entire reason `port_display.h` defines a full refresh and
`update_policy` forces one once the partial budget is spent. `REFRESH_FULL`
is now `epd_clear()`, then the back buffer marked white (true by then, and
the only way to make every non-white pixel a difference), then `MODE_GC16`
over the whole panel. The same sequence fixes a second problem:
`epd_hl_init()` assumes the glass is white, which a bistable panel is under no
obligation to be at boot; the fork's example opens with `epd_clear()` for
exactly that reason. Because the first frame is always a full refresh,
whether or not the bring-up test pattern is enabled, the model is now
synchronised with the glass before anything is drawn on top of it.

**`REFRESH_CLEAR` erased the core's rendered screen.** It called
`epd_hl_set_all_white()`, which writes the *front* buffer -- the one core/
draws into. It is now `epd_clear()` plus marking the back buffer white,
leaving the front alone. The remaining caveat is documented at the site and
unchanged: nothing issues `REFRESH_CLEAR`, and a caller that starts to must
follow it with `REFRESH_FULL`.

Confirmed correct and left unchanged:

- **Pixel clock.** `ED047TC1`'s display definition says `.bus_speed = 20`, which
  looks like a conflict with the 10 MHz `board_notes.md` records for this
  panel. It is not one: that field serves the LCD render method, and the
  row-output i80 path this board uses has its own hard-coded
  `I80_PCLK_HZ = 10 MHz` (`output_i2s/i2s_data_bus.c`).
- **Mirroring and rotation.** `ED047TC1` is `DISPLAY_TYPE_GENERIC`, so the
  highlevel layer's in-place mirroring of the front buffer -- which would have
  mutated the core's framebuffer mid-flush -- never runs, and the default
  rotation is landscape, so its area transform is the identity.
- **Area bounds.** The highlevel path never passes our rect to
  `epd_draw_base()`'s crop check; it goes to `epd_difference_image_base()`,
  which clips the far edge to the panel. `rect_t`'s unsigned fields cannot
  produce a negative near edge, so no clamping is needed in the adapter.
- **Back-buffer consistency on partial updates.** After drawing,
  `epd_hl_update_area()` copies whole dirty *lines* front-to-back, including
  columns outside the requested area. That would desynchronise the model if
  pixels outside the area could differ -- but the area comes from
  `framebuffer_diff_dirty_rect()`, the bounding box of every changed byte, so
  outside it front, back and `event_loop.c`'s previous-frame copy agree by
  construction.

Also corrected in passing: the dependency is now pinned by commit SHA rather
than by tag, since a tag on an individual's fork can be moved and
`dependencies.lock` is not committed; and the boot log, `main.c`'s header and
both Kconfig help texts no longer say FastEPD.


## Third EPDiy flash: first light, on the build the review said would be blank

The third image flashed was the highlevel-API build of the "Second EPDiy
flash" section -- the one *before* the review above, with no power bracket,
a bare `MODE_GC16` for `REFRESH_FULL` and no `epd_clear()` anywhere. The boot
log identifies it unambiguously: its banner still read "FastEPD driving the
panel" (the review corrected that string) and its ELF SHA256 is
`320dfba08...`, not the reviewed build's.

It worked. The bring-up pattern rendered solid black, `screen_home` replaced
it after the minimum full-refresh interval, and `update_policy`'s partial
refreshes then updated the CO2 headline in place for several minutes, every
flush returning success. This is the phase 2b deliverable: the core's own
`screen_home`, rendered by the same code the simulator runs, on the physical
panel, with the core unchanged.

EPDiy's per-update timing line gives the first measurements of this panel
that describe the glass rather than a driver that could not light it:

| Update | Area | Draw | Total |
|---|---|---|---|
| `MODE_GC16`, whole panel | 960 x 540 | 1,084 ms | 1,203 ms |
| `MODE_GL16`, CO2 headline | ~250 x 228 | 353-362 ms | ~400-409 ms |
| `MODE_GL16`, wider band | ~868 x 250 | 396 ms | 447 ms |

`free_psram_after_epd_init` was 7,349,100 bytes, so EPDiy's init costs
1,037,092 bytes of PSRAM -- the "about 1 MB" estimated above, now measured.
All of this is recorded in `board_notes.md` as well, against the open items
it closes.

### What this does and does not show about the review

**The review's prediction failed, and it is worth saying which part of the
reasoning was wrong.** It assumed that without `epd_poweron()` neither the
data bus nor the high-voltage rails would be live. The bus half was simply
wrong for this variant: with `CONFIG_EPD_ESP32S3_I80_ROW_OUTPUT`,
`i2s_gpio_attach()`/`i2s_gpio_detach()` are no-ops and the i80 bus owns its
pins from `epd_init()` on, so `epd_poweron()` is not what connects it. The
rails half is not explained by anything in the source, and the source rules
out the obvious escape: `epd_renderer_init()` pushes the board's freshly
initialised register (`power_disable = true`, both rail enables false) to
the 74HCT4094 straight after the board's `init`, and every latch and
frame-start push during rendering writes those bits again. The rails were
commanded off on every register write of that build, so the shift register
cannot have been holding an earlier firmware's state. What is left is that
this revision's high-voltage supply is not gated by those bits the way both
EPDiy and LilyGo's driver assume -- with the uncomfortable corollary that
`epd_poweroff()` may not remove the rails either. `board_notes.md` carries
this as an open item with the measurement that would settle it.

**What it does not show is that the bracket is unnecessary.** EPDiy's header
states the requirement, the fork's example follows it, and a rendering
success that rests on an unexplained mechanism is not one to build on. The
reviewed build -- power bracket, `epd_clear()` before every full refresh,
`REFRESH_CLEAR` no longer touching the front buffer -- stays as the adapter.
The other two review findings are independent of power and unaffected by
this flash: a bare `MODE_GC16` through the highlevel API drives only changed
pixels, so the first-light build's full refreshes could not have cleared
ghosting from unchanged ones, however correct they looked on a freshly
drawn screen.

### Still unverified at the time

The reviewed build had not been flashed. It had to reproduce what the
first-light build showed -- black pattern, then `screen_home`, then partial
updates -- with one visible difference: each full refresh now cycles the
panel to white before drawing, which also makes it measurably longer than
the 1,203 ms above. A regression there would point at the power bracket,
the only change on the drive path.


## Fourth EPDiy flash: the reviewed build, confirmed

The reviewed build (banner "EPDiy driving the panel", ELF SHA256
`3d0eb68fb...`) reproduces everything the first-light build showed: the
black test pattern, `screen_home` after the minimum full-refresh interval,
and partial refreshes of the CO2 headline once a minute, every flush
returning `EPD_DRAW_SUCCESS`. The power bracket costs nothing visible and
nothing measurable at the 10 ms resolution of the log.

What changed is the full refresh, exactly as predicted. `epd_clear()` is
`epd_clear_area_cycles(full screen, 3, ...)`: three cycles of ten dark, ten
white and two white whole-panel pushes, 66 in all. It is the gap between
`adp_flush()` logging the refresh and EPDiy's own timing line starting:

| Full refresh | `epd_clear()` | `MODE_GC16` update | Whole flush |
|---|---|---|---|
| Test pattern (all black) | ~2.46 s | 1,204 ms (draw 1,086) | 3.67 s |
| First `screen_home` frame | ~2.33 s | 869 ms (draw 755) | 3.20 s |

Two things follow from those rows. The GC16 draw time depends on content
-- `screen_home` drives fewer pixels away from white than a solid black
fill -- so no single full-refresh figure is correct; the emulation should
take the larger. And the clear dominates: about 70 % of a full refresh is
spent getting to white, which is the price of a full refresh that actually
clears ghosting from unchanged pixels. Partial refreshes are unchanged from
the first-light build (399-448 ms in total).

Phase 2b's deliverable is met on this build: `screen_home` from the same
core code the simulator runs, on the physical panel, with the core
untouched. The open questions it leaves -- whether the panel's rails are
actually gated by the register bits, the waveform's greys on this unit, the
ghosting threshold -- are phase 2c's, and are tracked in `board_notes.md`.
