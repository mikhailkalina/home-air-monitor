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
| RTC | PCF8563, on I²C at `0x51`, with a rechargeable backup cell -- see "The RTC" below |
| Battery | charger HX6610S (`U9`, schematic v2.4), with a red "charging" and a green "full" LED of its own; no gauge IC -- the level is a plain divider on GPIO14 (see "Pin assignments" and "Power rails and connectors" below) |
| Touch | present on this unit (H716 variant) — a GT911 at I²C `0x5D` (INT low at reset): seen by the phase 2d bus scan, 2026-10-13, and named by its product ID, `911`, firmware 0x1060, on 2026-10-17 (phase 3g) |

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
| Touch I2C address | **`0x5D`** (GT911; `0x14` is its other latch address) | observed by the phase-2d bus scan; `examples/touch/touch.ino:109,113` |
| Button | 21 -- since ADR 0036 the USB disk's: held 2 s asks for it, pressed ends it | `src/utilities.h:30` |
| USB `D-` / `D+` (the USB-C) | 19 / 20 -- the S3's own USB pins, no bridge: USB-Serial-JTAG (the console, COM17) or USB-OTG (the USB disk, ADR 0036) on its one internal PHY, never both | ESP32-S3 datasheet; every console session since phase 2a |
| Battery ADC | 14 | `src/utilities.h:32` |
| microSD `MISO`/`MOSI`/`SCLK`/`CS` -- the card's since phase 3f ("The microSD slot") | 16 / 15 / 11 / 42 | `src/utilities.h:34-37` |
| Genuinely free | 45, 10 (analog-in only), 48, 39 | `README.MD` "GPIO List" |

Four findings worth carrying forward, because each one corrects or narrows an
assumption made elsewhere in this repository:

- **`CFG_STR` is GPIO0**, the boot-mode strapping pin. Driving it is safe
  once the system has finished booting, but it is a reason to be deliberate
  in phase 2b about exactly when the EPD control code first touches it.
- **~~The touch controller is not a GT911~~ -- corrected 2026-10-13 (phase
  2d): it answers where a GT911 does.** This finding first read: "which
  `docs/architecture.md` §6.2 and `docs/requirements.md` §6.2 both assume.
  `src/touch.h:10` sets `TOUCH_SLAVE_ADDRESS 0x5A`, and `src/touch.cpp`
  reads it through a `0xD0`/`0xD1` register protocol". That header is real,
  but nothing in the reference repository at this commit uses it; the
  vendor's S3 examples drive a GT911 at `0x5D` or `0x14`, and the phase-2d
  bus scan found a device at `0x5D` and nothing at `0x5A`. The architecture
  and requirements documents' GT911 assumption was right after all. See
  "Touch controller" in the open items for what is and is not established.
- **There is no battery gauge IC on this board.** `BOARD_BATTERY_ADC_GPIO`
  (14) is a plain resistor divider read directly by the ADC
  (`examples/demo/demo.ino:331`:
  `battery_voltage = (v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0)`), and the
  divider only carries a valid reading while EPD power is on, because it
  hangs off the same 74HCT4094 rail (`demo.ino:327-330`, "When reading the
  battery voltage, POWER_EN must be turned on"). GPIO14 is an ADC2 pin on the
  S3, and ADC2 is documented as unreliable while Wi-Fi is active -- a real
  constraint for whichever phase reads battery level over Wi-Fi.
  **Narrowed 2026-10-13 by the v2.4 schematic, not measured:** the divider is
  `R29`/`R32`, 100 kOhm 1 % each, from the battery straight to ground with
  10 nF on the tap. Nothing switched sits in it, so on this revision it
  should read correctly with `PWR_EN` low too -- the demo's comment may
  describe an earlier board -- and it draws about 20 uA from the battery all
  the time. Confirm when the battery adapter lands (phase 5).
- **~~`epd_poweroff()` also cuts power to the Molex connector~~ -- corrected
  2026-10-13 (phase 3a).** Pin 13 of the 74HCT4094 is `PWR_EN` on the
  schematic: it switches the `3V3` rail that feeds the white 4-pin
  connectors, the blue LED and the panel's high-voltage boost, not "panel
  power" alone, and EPDiy keeps it set through `epd_poweroff()` -- on this
  board the rail stays up between refreshes. See "Power rails and
  connectors" below. This finding first read: "Panel power is not a GPIO. It
  is pin 13 of the 74HCT4094 shift register, reached only through
  `CFG_DATA`/`CFG_CLK`/`CFG_STR` (`README.MD` FAQ 1: 'software must call
  `epd_poweron()` to enable it'). `epd_poweroff()` also cuts power to the
  Molex connector, since they share a rail. There is no separate status-LED
  GPIO either -- the board's one LED is on the same register (`README.MD`
  FAQ 4 mentions it only as something `epd_poweroff()` cannot turn off by
  itself)." The board has three LEDs, in fact: the blue one on that rail,
  the only one firmware influences, and the charger's red and green ones.

## Power rails and connectors

**Read from the schematic and measured 2026-10-13, phase 3a**, to decide
where the sensors take their power. Sources:

- The schematic `schematic/Screen-4.7-S3-V2.4 24-12-03.pdf` in
  `Xinyuan-LilyGO/LilyGo-EPD47`, branch `esp32s3`, last changed by commit
  `e3c0a1aab2575d0f6bc6dcf15b0649b410a4f25d` (2025-12-12), SHA-256
  `dc38d925ff17...`; its revision matches this unit's silkscreen. It was
  read from a rendering, not a netlist: designators and values are as
  printed, and the few that are hard to read say so.
- LilyGO's board pinmap for the capacitive-touch V2.4,
  `https://wiki.lilygo.cc/products/t5-series/t5-e-paper/index/image/t5-e-paper-pin-en.jpg`
  (2025-02-27 in its metadata), which photographs the connectors with their
  silkscreen.
- Measurements on this unit with this project's EPDiy firmware, by eye and
  with a multimeter.

### Rails

| Net | Source | Present | Feeds |
|---|---|---|---|
| `VBUS` | USB-C, before the Schottky `D8` | only with USB plugged in | the charger; pins 2 and 4 of the 40-pin header |
| `5V` | `VBUS` through `D8`, or the battery through P-MOSFET `Q5` while `VBUS` is absent (3.7-4.2 V then) | whenever the board is powered | the LDO |
| `VDD3V3` | LDO `U13` (ME6217-3V3) from `5V` | whenever the board is powered | the ESP32-S3 module, the RTC (through `D3`, as `VCC_RTC` -- "The RTC" below), the 74HCT4094 (`VCC` and `OE`), the touch connector `P2`, the I2C pull-ups |
| `3V3` | `VDD3V3` through P-MOSFET `Q4`, switched by `PWR_EN` (NPN `Q2` pulls the gate) | while `PWR_EN` is high | pin 1 ("V") of the 4-pin connectors `P4`/`P5`, the blue LED `LED7` (through 2 kOhm), and the boost `U2` (LT1945): its `VIN`, and `SHDN1`/`SHDN2` through 100 kOhm |
| +22 V, -20 V | boost `U2` | while `3V3` is up -- see below | the panel; ±15 V derived from them by `U1`/`U6` (78L15/79L15) |

A second P-MOSFET on `PWR_EN`'s gate net, `Q3`, connects the battery to the
LDO input; its role was not traced further.

`VBUS` is never a sensor supply: it is 5 V, absent on battery, and a sensor
module's pull-ups would put 5 V on the ESP32's I2C pins.

### The 74HCT4094 (`U3`)

Matched against the order `epd_board_set_ctrl()` shifts the bits out in
EPDiy's `src/board/epd_board_lilygo_t5_47_s3.c` (the first bit pushed ends
in `QP7`):

| Output (pin) | Net | Through | EPDiy bit |
|---|---|---|---|
| `QP0` (4) | `EP_LE` | 0 Ohm | `ep_latch_enable` |
| `QP1` (5) | `SMPS_CTRL` | resistor marked **NC** | `power_disable` |
| `QP2` (6) | `POS_CTRL` | resistor marked **NC** | `pos_power_enable` |
| `QP3` (7) | `NEG_CTRL` | resistor marked **NC** | `neg_power_enable` |
| `QP4` (14) | `EP_STV` | `R33`, 0 Ohm | `ep_stv` |
| `QP5` (13) | `PWR_EN` | direct | `ep_scan_direction` |
| `QP6` (12) | `EP_MODE` | direct | `ep_mode` |
| `QP7` (11) | `EP_OE` | direct | `ep_output_enable` |

The three resistors on `QP1`-`QP3` are `R6`, `R7` and `R21`, all printed NC;
which designator sits on which output is not legible, and does not matter.
Three consequences:

- **`PWR_EN` is the bit EPDiy calls `ep_scan_direction`**, which its board
  file describes as "repurposed as power enable of the touch controller /
  display supply. Kept set, as in LILYGO's driver": set at init, left set by
  `epd_poweroff()`. This is pin 13 of LilyGO's `README.MD` FAQ 1.
- **`power_disable`, `pos_power_enable` and `neg_power_enable` drive
  nothing on this revision.** `SMPS_CTRL`, `POS_CTRL` and `NEG_CTRL` run only
  to the 40-pin header, and the boost's shutdown pins are pulled up to `3V3`.
  So the panel's high-voltage rails are up whenever `3V3` is, and EPDiy's
  power sequencing does not switch them -- which is the open item "Panel
  power" below, explained. Read from the schematic, then measured on the
  LT1945 2026-10-15 ("Measured" below).
- **`OE` is tied to `VDD3V3`**, so the outputs are always driven, and after
  power-up they hold whatever the register happens to contain until the
  first latch.

The schematic also carries an unfitted option, in dashed boxes, that drives
`EP_LE` and `EP_STV` straight from IO48 and IO45 through resistors marked
NC. LilyGO's pinmap labels "STV IO45" and "LE IO48" describe that option;
on this unit EPDiy drives both through the register, and IO45 and IO48 stay
free, as the pin table above says.

### Connectors

| Ref | What it is | Pins |
|---|---|---|
| `P2` | 6-pin 0.5 mm FPC socket beside the RST button: **the touch panel's**, occupied by its flex | 1 GND, 2 INT (IO47), 3 SCL (IO17), 4 SDA (IO18), 5 `VDD3V3`, 6 NC. The only connector carrying `VDD3V3`. Pull-ups beside it: see the open item on pull-ups |
| `P6` | 8-pin FPC footprint with a `T_RST` line, in a dashed option box | not fitted on this revision (absent from LilyGO's photographs) |
| `P5` | the white 4-pin connector, fitted | 1 `3V3` (switched), 2 IO15, 3 IO16, 4 GND -- LilyGO's "G 16 15 V". IO15/IO16 are the microSD card's MOSI/MISO, and **from phase 3f the card's**: the sensors use only pins 1 and 4 ("The microSD slot") |
| `P4` | a 4-hole footprint beside `P5`, not fitted | 1 `3V3` (switched), 2 IO48, 3 IO45, 4 GND. **IO45 is a strapping pin** (it selects the flash supply voltage at reset): never put a pull-up on it |
| `P8` | the 40-pin header, "Raspberry Pi compatible" (`P3` is a second footprint for the same signals) | 1 NC; 2, 4 `VBUS`; 3 IO18 (SDA); 5 IO17 (SCL); 6, 9 GND; 7 IO47; 8, 10 UART0 RX/TX; 19, 21, 23, 24 MOSI/MISO/SCLK/CS; 33, 35, 37 `NEG_CTRL`/`POS_CTRL`/`SMPS_CTRL` (undriven, above); the rest NC or GND. **No 3.3 V pin** |

**Discrepancy on the 40-pin header.** LilyGO's pinmap labels pin 3
"SCL(17)" and pin 5 "SDA(18)". The schematic puts IO18 on pin 3 and IO17 on
pin 5 (through `R17`/`R20`, 0 Ohm), and so does the board's own silkscreen,
legible in the same pinmap's close-up ("18" beside pin 3, "17" beside pin
5). The schematic and silkscreen win. A swap is harmless to test: a bus
scan simply finds nothing.

### Measured: the switched `3V3`

- **It comes up together with the blue LED and stays up** -- between
  refreshes as during them -- on `P5` pin 1, with this project's EPDiy
  firmware. Measured 2026-10-13.
- **Pressing RST turns the blue LED off**, so the rail drops with an ESP32
  reset, and comes back when the firmware's EPDiy init writes the register
  again. The likely mechanism, not verified: `epd_board_set_ctrl()` leaves
  `CFG_STR` (GPIO0) high, so the 74HCT4094's storage register is transparent,
  and in reset `CFG_CLK`/`CFG_DATA` (GPIO12/13) float. What a software reset
  (panic, watchdog, `esp_restart()`) does is an open item below.
- **The high-voltage boost runs whenever the rail is up.** On the LT1945
  (`U2`), pins 1, 2, 4 and 5 read -1.23, 2.78, 2.78 and +1.21 V; they appear
  together with the blue LED and do not change between refreshes. The far
  ends of the 100 kOhm resistors on pins 2 and 4 read 3.2 V -- the switched
  `3V3` itself. Pins 1 (`NFB1`) and 5 (`FB2`) sit at the converters'
  feedback reference, so both regulate: +22 V and -20 V, and the ±15 V
  derived from them, are present from the first register write to the next
  reset, refreshing or idle. The 0.4 V across each 100 kOhm is about 4 uA
  into each `SHDN` pin, which still leaves them at a firm logic high.
  Measured 2026-10-15.

### The sensor wiring this implies

Phase 3's sensors take power from `P5` ("V" and "G") and the bus from the
40-pin header's pin 3 (SDA), pin 5 (SCL) and a GND pin. `P2` is the only
connector with `VDD3V3`, and the touch panel occupies it. What follows for
the firmware:

- **Sensor power follows `PWR_EN`.** It is undefined from power-up until
  EPDiy's init first writes the register, so the sensors are initialised
  after the display adapter, allowing each sensor's power-up time after
  that. A `CONFIG_HAC_DISPLAY_NULL` build, and the on-target test app, never
  run EPDiy's init; since phase 3c they write the register themselves
  (`platform/esp32_t5s3/board_power.c`: the bits `epd_poweroff()` leaves,
  `PWR_EN` set, output enable and start pulse low) before anything touches
  the bus.
- **A hardware reset power-cycles the sensors**: every such boot starts the
  SCD41 from idle, warm-up included, and the BME68x unconfigured. A software
  reset may or may not do the same, so the SCD4x driver must not assume
  either -- it stops periodic measurement first.
- **Phase 5**: clearing `PWR_EN` switches off the sensors, the panel's
  high-voltage rails and the blue LED together, and deep sleep that does not
  hold GPIO0/12/13 probably drops the rail as a reset does.

### The sensor modules

From the enclosure research notes, kept outside this repository
(`mechanical/common/research/sensors.md`, 2026-09-17), which identify the
modules from the seller's photographs:

- **BME68x**: a purple T-shaped breakout with pads VCC, GND, SCL, SDA, SDO
  and CS and a level converter, matching TinyTronics SKU 003658
  (BME680LVLCNV, the CJMCU-680 outline). Its address, `0x76` or `0x77`,
  depends on how SDO is strapped: a wiring fact the bus scan settles.
  Whether the chip is a BME680 or a BME688 is not established -- see the
  open items.
- **SCD4x**: a blue breakout marked "SCD41 CO2 IIC", Banggood product
  2038457, option 6334266, with pads GND, VDD, SCL and SDA. The same listing
  sells a purple SCD40, which has no single-shot mode and a narrower range,
  so the variant is read back from the sensor rather than assumed.

### The RTC

**From the v2.4 schematic, the owner's reading of it, 2026-10-16** (phase
3e, `docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md`); not
measured:

- **Supply**: the PCF8563's `VDD` is `VCC_RTC`, which is `VDD3V3` through
  `D3`, a 1N4148, with an MS412FE rechargeable lithium cell behind it on the
  RTC's side of the diode. So the RTC runs whenever the board is powered, is
  **not on `PWR_EN`** -- no reset, software or hardware, stops it, and
  clearing `PWR_EN` for deep sleep would not either -- and the cell should
  keep it running with the board unpowered, charged through the diode while
  it is powered. **Seen 2026-10-16**: on this unit the cell is fitted and
  kept VL clear through more than a minute with no power at all; longer
  outages are untested ("The RTC on the board" in the open items).
- **INT** goes to IO9 with a 10 kOhm pull-up to `VDD3V3`
  (`BOARD_RTC_INT_GPIO`): the open-drain alarm and timer interrupt. Since
  phase 4f the alarm marks the RTC's tick on it, each falling edge stamped in
  an interrupt (`platform/esp32_t5s3/board_rtc_int.c`) -- read from the
  schematic, **seen working 2026-10-22**: the pin high at boot, both of a
  sync's edges stamped (open item "Time from the network on the board"). IO9
  is an RTC GPIO: a wake source for phase 5's deep sleep.
- **CLKOUT** is pulled up and goes nowhere the firmware uses.
- The bus: the shared I2C at `0x51` (`BOARD_RTC_I2C_ADDRESS`), found by
  every boot scan since phase 2d.

The firmware keeps **UTC** in it, never local time, reads it once at boot
after the bus is up, and sets the system time from it when it is valid --
answering, VL clear, running, decodable, and no earlier than 2026
(`apps/firmware_esp32/main/wall_clock.c`, ADR 0023). The console's `time`
says what it holds; `time_set` writes it. Since phase 4f an image with a
network writes it from each SNTP answer, an hour apart, to the start of a
second: stopped, the second written, started so that its first tick falls on
that second's end -- a write without STOP leaves the prescaler's phase, up to
a second off (datasheet Rev. 11.1, 8.10). Before each write, and after it, it
finds the chip's own tick by its alarm on INT, stamped on IO9 in an
interrupt: how far it had drifted, and where the write landed. The delay from
a start to the first tick is measured and learned, since this chip's is not
the datasheet's 0.508 s but a whole second: 1,000 ms by its INT on
2026-10-22, about 989 by polling the day before (ADR 0023, "Amendment (phase
4f)"; open item "Time from the network on the board").

### The microSD slot

**From the v2.4 schematic, the owner's reading of it, 2026-10-16** (phase
3f, `docs/adr/0024-sd-card-recordings-and-debug-log.md`); not measured:

- **SPI mode only.** The slot's DAT1 and DAT2 are not connected, so the
  card's 4-bit SD mode is out of reach; it runs over SPI. CS is IO42,
  MOSI/CMD IO15, CLK IO11, MISO/DAT0 IO16 -- the `BOARD_SD_*` constants, as
  recorded since phase 2a, unchanged.
- **Supply**: the card's VDD is `VDD3V3`, so it is powered whenever the
  board is -- **not switched by `PWR_EN`**. The card can be mounted before
  EPDiy's init, a reset does not power-cycle it, and phase 5's clearing of
  `PWR_EN` would not either.
- **Pull-ups**: 10 kOhm on three of the four lines; the one on MISO, `R14`,
  is printed NC. `platform/esp32_t5s3/adp_sd.c` enables the S3's internal
  pull-up (~45 kOhm) on IO16 instead, so MISO does not float with no card
  driving it.
- **Card-detect** (CD#) goes only to test pad `TP72`: nothing the firmware
  can read. A card's arrival or removal is not seen; the firmware tries to
  mount at boot, every minute while none is mounted, and at the console's
  `sd_mount`, and finds a card pulled out at its next access.
- **IO15 and IO16 are also `P5`'s pins 2 and 3** ("Connectors" above).
  From phase 3f they belong to the card; the sensors take only `P5`'s V and
  G, as they always have.

The controller is SPI2 (`BOARD_SD_SPI_HOST`; SPI0/1 run the flash and the
PSRAM, the panel is on LCD_CAM), at 20 MHz (`BOARD_SD_SPI_MAX_KHZ`, ESP-IDF's
default for SD over SPI, a choice). The filesystem is FAT12/16/32: ESP-IDF
builds FatFs without exFAT, so a card sold exFAT-formatted -- every SDXC, 64
GB and up -- must be formatted FAT32 on a PC first. The firmware never
formats a card.

## Sensors on the bus

**Seen on the board 2026-10-16** (boot scan, phase 3a, ELF SHA256
`c59289da4...`, the sensors wired to `P5` and the 40-pin header):

```
I2C_SCAN bus sda_gpio=18 scl_gpio=17 scl_hz=100000, idle lines sda=high scl=high
I2C_SCAN 0x14 absent      touch: GT911, if INT was high at reset (BOARD_TOUCH_ALT_I2C_ADDRESS)
I2C_SCAN 0x51 found       rtc: PCF8563 (BOARD_RTC_I2C_ADDRESS)
I2C_SCAN 0x5D found       touch: GT911, INT low at reset (BOARD_TOUCH_I2C_ADDRESS)
I2C_SCAN 0x62 found       co2: SCD4x, its only address (BOARD_SCD4X_I2C_ADDRESS)
I2C_SCAN 0x76 absent      env: BME68x, if SDO is low (BOARD_BME68X_ALT_I2C_ADDRESS)
I2C_SCAN 0x77 found       env: BME68x, SDO high (BOARD_BME68X_I2C_ADDRESS)
I2C_SCAN_RESULT answered=4 missing=0 unexpected=0 scan=complete scan_ms=27
```

| Device | Address | Where it comes from | Seen |
|---|---|---|---|
| SCD4x | `0x62` | the part's only address (Sensirion SCD4x datasheet v1.7, Table 8); nothing on a module changes it | found |
| BME68x, SDO high | `0x77` | Bosch BME680 datasheet, "I2C interface" | found: this module straps SDO high |
| BME68x, SDO low | `0x76` | the same; kept named, as the GT911's second address is | absent |

That boot ran on USB power alone. A scan sees the sensors only once
`PWR_EN` is up: before it, they sit on the bus unpowered and hold both
lines low (open item "A reboot loop when the switched `3V3` comes up"), so
the firmware brings the bus up and scans after the display adapter. The
address answers but does not name the parts: the SCD4x driver reads its
variant back, the BME68x driver its `variant_id` (open item "Which sensor
variants are fitted").

**Bus speed: 100 kHz stays** (decided in phase 3a, the decision
`BOARD_I2C_SCL_HZ`'s comment used to defer). Nothing needs more: the SCD4x
moves about a dozen bytes every 5 s. And nothing yet shows the bus could take
fast mode's 300 ns rise time: the board's pull-ups come to about 20 kOhm per
line (the open item on pull-ups below, unconfirmed), each module adds its own
to the switched `3V3`, and the sensors hang on wires from the 40-pin header.
An estimate, not a measurement: 20 kOhm in parallel with a module's 10 kOhm
(a common value, unconfirmed here) and 100 pF of wiring rise in about
0.6 us -- inside standard mode's 1000 ns, twice fast mode's limit. Raise the
rate only with the pull-ups read off the modules and the rise time seen on a
scope.

**How the SCD4x driver starts, given the power facts above**
(`drivers/scd4x/scd4x_source.h`, `docs/adr/0015-scd4x-driver.md`): it waits
the sensor's 30 ms power-up time first, because the switched `3V3` may have
risen only when EPDiy's init wrote the register; it stops periodic
measurement and reinitialises whatever the reset was; and it logs the serial
number, the variant and `found_measuring` -- whether the sensor was still
measuring, which after a reset that dropped `PWR_EN` it cannot be. Since
phase 3c the firmware runs it, under the sensor manager
(`docs/adr/0017-sensor-manager.md`).

**How the BME68x driver starts** (`drivers/bme68x/bme68x_source.h`,
`docs/adr/0016-bme68x-driver.md`, phase 3b): it waits the sensor's 2 ms
start-up time first, for the same reason; Bosch's `bme68x_init()` then
soft-resets the sensor whatever the reset was, so one a software reset left
configured or measuring starts from its power-on state; it checks the sensor
is asleep before anything goes through Bosch's unbounded wait for sleep mode;
and it logs the chip id, the variant (`BME680`, `BME688` or `unknown`, from
register `0xF0`) and the measurement time, 143 ms by Bosch's estimate with
the 100 ms heater; the first init() also logs the calibration NVM, in five
`calibration (n/5)` lines. Since phase 3c the firmware runs it too.

**The order the firmware brings them up in** (phase 3c,
`apps/firmware_esp32/main/main.c`): the display adapter -- EPDiy's init
raises `PWR_EN` and the blue LED lights -- or, in a
`CONFIG_HAC_DISPLAY_NULL` build, `board_power_switched_3v3_on()`; then
`BOARD_SWITCHED_3V3_SETTLE_MS` (50 ms); then the bus and the boot scan; then
the event loop, whose first poll initialises each sensor, each driver
waiting its own power-up time once more before its first transfer: 30 ms
for the SCD4x (its datasheet v1.7, Table 7, "power-up time", maximum),
2 ms for the BME68x (BME680 datasheet, Table 1, `t_startup`). Nothing
touches the bus while `PWR_EN` is low.

**Checks still to do on the board, in order:**

1. ~~`restart` at the console, watching the blue LED.~~ Done 2026-10-16:
   it stays lit -- open item "What a software reset does to `PWR_EN`".
2. ~~Wire the sensors and scan.~~ Done 2026-10-16, above.
3. Run `scd4x_source` on the board -- the phase-3c firmware does: its first
   log line names the variant, and its first minutes of readings replace the
   warm-up estimate (`SCD4X_WARMUP_MS`).
4. Run `bme68x_source` beside it: its first log line names the variant
   (open item "Which sensor variants are fitted"); its first readings say
   whether the gas is valid and heater-stable with the 100 ms profile, and
   whether a measurement is ever later than Bosch's 143 ms; and its first
   hour of readings, beside the SCD41's temperature and a reference
   thermometer, gives the three estimates of the open item "The BME68x's
   estimates" their first measured values.
5. ~~The `port_sensor` contract in the test app.~~ **Done 2026-10-16**
   (`idf.py -T hac_platform build`, ELF SHA256 `6ab6ea91b...`):
   `6 Tests 0 Failures`, every `contract port_sensor` line PASS -- the SCD4x
   in periodic and single-shot mode and the BME68x, each through init, a
   measurement, `set_ambient()`, init again without deinit and a second
   measurement. What the log shows beyond PASS:
   - **`board_power_switched_3v3_on()` works**: with the sensors wired and
     EPDiy never run, every `adp_i2c: bus up` line read both lines high,
     and the `port_i2c` contract passed in full on the shared bus.
   - **The parts**: an SCD41 (serial `0x2103c5073b73`) and a BME680
     (`chip_id=0x61`, `variant_id=0x00`) -- open item "Which sensor
     variants are fitted".
   - **The SCD41 refuses `get_serial_number` while it measures**: the
     periodic suite's second init() met it measuring (`found_measuring=true`),
     and the one refused transfer cost three `i2c.master` lines. Whether the
     address or the command byte was refused, the log does not say -- see
     the open item "What the SCD41 does where its datasheet is silent".
   - **Single shots were done by 5 s on the board's clock**: both of them,
     read at the first attempt after 5000 ms, with no refused transfer.
   - **This BME680's first two measurements after init came back
     `gas_valid=true heat_stab=false`** -- the heater did not reach 300 C in
     the 100 ms profile, from cold. Whether it does once warm is the
     firmware run's to show (open item "The BME68x's estimates").
   - **This BME680's calibration**, now the host model's
     (`BME680_MODEL_BOARD_CALIBRATION`).
   - In passing: the `port_i2c` contract's forced hang took five clocks to
     free this time, two on 2026-10-13 -- the RTC's position in its byte
     when the injection stops differs between runs.
6. ~~On the running firmware, unplug the sensors.~~ **Done 2026-10-16**
   (ELF `ea261721f...`, the owner's log of 02:28-02:59; boot at about
   02:28:51 by the log's own clock): both sensors, on one connector, out at
   02:41:45 and back at 02:42:45. Within the poll after the unplug both
   failed (`scd4x ... step=read status=not_found`, `bme68x ... step=start
   status=not_found`), the panel showed `ERR` in every sensor field in the
   next partial refresh, and the display adapter went back to its assumed
   25 C. Three failures in a row each, then re-init attempts 5, 10, 20 and
   40 s apart; the one 25 s after the replug found both (`inits=5
   after_failure=true`), the SCD41 power-cycled by the unplug
   (`found_measuring=false`) and so warming up again, the BME680 burning in
   again. No reboot; the bus only ever refused addresses, never timed out.
   Each attempt cost 12 `i2c.master` lines for the two sensors, three per
   refused transfer. Not yet tried: one sensor alone, which this wiring
   cannot do without separating the connectors.
7. The measurements phase 3c owes (open item "Phase 3c's measurements";
   the first run's are recorded there).

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
EPDiy's highlevel API -- see "Panel power" in the open items below for why
that build rendered without `epd_poweron()` (resolved 2026-10-15: the
panel's supply was already up). **Confirmed the same
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
this bring-up baseline is what makes their actual cost visible. **Wi-Fi's, in
the build, 2026-10-21** (phase 4e, `docs/adr/0032-wifi-on-the-board.md`): an
image with a network against the same tree without one, +51,140 bytes of
static internal RAM -- 30,184 of it Wi-Fi code in IRAM (ESP-IDF's default
`CONFIG_ESP_WIFI_IRAM_OPT` and `_RX_IRAM_OPT`), 13,136 `.bss`, 7,820 `.data`
-- and +501 KB of image, 1,215,097 bytes in all, 29 % of the app partition. The
run-time share is the board's to measure (open item "Wi-Fi on the board").
Measured on the board the same day, 68 KB; with that, the IRAM optimisations
were turned off: static internal RAM 131,379 bytes with a network, Wi-Fi's
share 33,576.
**MQTT's, in the build, 2026-10-22** (phase 4g,
`docs/adr/0033-mqtt-on-the-board.md`): an image with the network and a broker
against the same tree with the network alone, +3,904 bytes of static internal
RAM (the reporter, the topics, the adapter's state) and +124,500 bytes of
image, 1,363,697 in all, 33 % of the app partition -- about 54 KB of it Mbed
TLS's TLS layer, which esp-tls links for the plain TCP transport and nothing
runs. The adapter's buffers, about 27 KB, are in PSRAM. Its run-time share --
two task stacks, esp-mqtt's client and buffers, the socket, the outbox -- is
an ESTIMATE of 20-25 KB of internal RAM (open item "MQTT on the board").

## Known constraints (vendor warning, verbatim intent)

The e-paper panel must not be partially refreshed for an extended period —
doing so leaves residual ghosting and causes irreversible damage. This is the
hardware fact behind `max_partial_refreshes_before_full` in
`ports/port_display.h`. **Set from observation in phase 2c: 10**
(`docs/adr/0008-panel-timing.md`, "Ghosting, and the budget"); **20 since
2026-10-16**, the owner's choice (`docs/adr/0019-partials-past-the-budget.md`),
accepting close-up outlines from the 11th for fewer full refreshes. Partial
refreshes may also go past it while a full refresh waits for the minimum
interval -- never at the firmware's pace, which draws at most once per 5 s
sensor cycle (20 partials, at least 105 s between full refreshes) -- and the
next redraw after the interval is full. Past it by the budget again at most,
40 in all, the most this panel has been driven through: a person tapping
between screens could otherwise have taken it further (ADR 0025) -- until a
change of screen became a full refresh, held by neither the budget nor the
minimum interval (ADR 0034).

## Ghosting observed in the simulator — a calibration starting point

**Superseded by the panel, 2026-10-13 (phase 2c).** What the glass actually
does is in `docs/adr/0008-panel-timing.md`, and it is not what this section's
simulator showed: nothing at all is visible from 50 cm through 40 partial
refreshes; close up, the artifact is light outlines of an earlier digit
*inside* the current dark one (undriven dark pixels drifting lighter), not
dark residue behind it, appearing after 11-25 partial refreshes in the worst
realistic pattern. The section below is kept as the record of what was
expected and why the probe watched the headline.

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
- [x] **Touch controller: `0x5A`, or a GT911?** **Resolved 2026-10-13
      (phase 2d) on the board: `0x5D`.** The boot scan
      (`I2C_SCAN_RESULT answered=2 missing=1 unexpected=0 scan=complete`,
      image ELF SHA256 `cd89f6e27...`) found the RTC at `0x51` and a device at
      `0x5D`, and nothing at `0x5A` or `0x14` -- a GT911 that latched INT low
      as it left reset. `BOARD_TOUCH_I2C_ADDRESS` is now `0x5D`;
      `BOARD_TOUCH_ALT_I2C_ADDRESS` keeps `0x14`, which this unit has not
      shown but which the latch could in principle produce at another
      power-up, and which the vendor probes too. The history below is how
      the question arose.

      Reopened 2026-10-13 (phase 2d); it was marked resolved on 2026-09-06
      as "`0x5A`, confirmed **not** a GT911". That came from `src/touch.h:10` in `Xinyuan-LilyGO/LilyGo-EPD47`
      (branch `esp32s3`, commit `391b0e2`) -- but nothing in that repository
      at that commit includes `src/touch.h` or uses its `TouchClass`. The
      vendor's own S3 examples drive a **GT911** through SensorLib instead
      (`examples/touch/touch.ino:31` and `examples/demo/demo.ino:32` include
      `TouchDrvGT911.hpp`), and find it by probing `0x14` and `0x5D`
      (`touch.ino:109,113`; `demo.ino:239,243`, inside the
      `CONFIG_IDF_TARGET_ESP32S3` block at `:208`): a GT911 latches `0x5D` if
      its INT line is low as it leaves reset and `0x14` if high, and on this
      board its reset is a hardware pull-up, so the vendor scans for both.
      `touch.ino` probes before `epd_poweron()`, so the touch is reachable
      with the panel unpowered. The phase-2d boot scan (`I2C_SCAN` lines)
      reports `0x5A` as required and both GT911 addresses as candidates;
      correct `BOARD_TOUCH_I2C_ADDRESS` and the "Pin assignments" finding
      above from what it shows. If no touch address answers at all, cut the
      board's power completely and scan again before concluding anything: the
      vendor examples drive INT high first "assuming that the previous touch
      was in sleep state", and a GT911 a previous firmware put to sleep stays
      asleep across a reset of the ESP32 alone.
- [x] Touch controller part number. **Resolved 2026-10-17 on the board**
      (phase 3g, ELF `f63cfdf60...`): `gt911: found address=93 (0x5D)
      product_id=911 firmware=4192 (0x1060)` -- a GT911, and the
      programming guide's register map confirmed by it. Its configuration:
      version 0x42, X/Y maximum 540 x 960 (portrait, the frame the vendor's
      swap implies), two touch points, `Module_Switch1` 0x05 (bits 1:0 = 01, a
      falling edge; bit 2 set, its meaning not established), checksum
      right. Now the host model's defaults. **The address latch is not
      stable** (2026-10-20): `0x14` after a power glitch, `0x5D` again after
      the next power cycle -- the firmware takes either (open item "The touch
      panel on the board"). The history follows. Narrowed 2026-10-13 (phase 2d): it
      answers at a GT911 address, and the vendor's S3 examples drive a GT911,
      so it is very probably one -- but an address-only probe cannot name a
      part. Reading the GT911's product ID (16-bit register `0x8140`, four
      bytes, `"911"` and a NUL) closes it; that is the touch driver's first
      transaction anyway, so it belongs to whichever phase writes
      `adp_input.c`. Also open: whether the address latch is stable across
      power-ups. One unit, one power-up seen so far. **Built in phase 3g**
      (`docs/adr/0025-touch-input-gt911.md`): every boot's `gt911: found`
      line names the product ID and firmware, `touch` at the console prints
      them, and the test app's `port_input` case checks for `"911"` -- the
      first run on the board closes this item (open item "The touch panel on
      the board").
- [x] **Phase 2d on the board.** **Confirmed 2026-10-13.** The boot scan:
      the bus came up with both lines idle-high, the scan completed, the RTC
      answered at `0x51` and the touch controller at `0x5D` (the item above),
      and `app_main` returned normally on its 3,584-byte stack. The test app
      (ELF SHA256 `5f61e780d...`), run twice: `3 Tests 0 Failures`, every
      `contract port_i2c adp_i2c` check PASS. The forced hang was real (the
      RTC acknowledged and held SDA low) and `recover()` freed it in two
      clocks; a stalled SCL ended on the 100 ms software deadline after about
      110-120 ms, never on the S3's hardware SCL timeout. Details, and a
      transfer on the hung bus that the driver's own clear-bus rescued, in
      `docs/adr/0011-i2c-bus-adapter.md`, "Consequences".
- [ ] Whether the I2C bus has external pull-ups. `adp_i2c.c` enables the
      S3's internal ones (~45 kOhm) either way. Without external ones, rise
      times at 100 kHz are marginal on a bus with several devices and a
      cable to the sensors -- relevant in phase 3, when the sensors are
      wired. The schematic answers it; an idle-line reading cannot.
      **Answered by the schematic 2026-10-13, values unconfirmed:** beside
      the touch connector `P2`, `N6` and `N7` (10 kOhm) pull SCL and SDA up
      to a node fed from `VDD3V3` through `N8`, printed 10 kOhm; `N5` (on
      INT) and `N9` (that node to the switched `3V3`) are NC. If `N8` really
      is 10 kOhm in series, each line sees about 20 kOhm. With the board
      unpowered, SDA to SCL (40-pin header pins 3 and 5) reads about 20 kOhm
      if `N6` and `N7` are fitted. The sensor modules add their own
      pull-ups, to the switched `3V3`. Phase 3a kept the bus at 100 kHz
      until these values and a rise time are known ("Sensors on the bus").
- [x] **What a software reset does to `PWR_EN`.** **Answered for
      `esp_restart()` 2026-10-16:** the owner typed `restart` at the console
      (ELF SHA256 `c59289da4...`) and the blue LED stayed lit -- by eye, so a
      dip shorter than the eye can see is not ruled out, but the rail is not
      switched off. A software reset keeps the sensors powered: an SCD41 that
      was measuring is still measuring when the firmware comes back, and a
      BME68x keeps whatever configuration it had. Both drivers must reset or
      stop their part at init rather than assume a power-on state --
      `scd4x_source` does (stop, reinit), and so does `bme68x_source` since
      phase 3b (Bosch's soft reset, then a check that the sensor is asleep;
      on the host only). It matches the reading below. Still only read,
      not seen: the panic and task/interrupt-watchdog reboots, which go
      through the same `esp_restart_noos()`; the RTC watchdog and brownout,
      which reset the whole digital system.
      The question as it stood: Pressing RST drops the
      switched `3V3` and with it the sensors' power ("Power rails and
      connectors", "Measured"). Whether a panic, the watchdog or
      `esp_restart()` does the same is not observed, and it decides whether
      the SCD41 is found idle or still measuring after such a reboot. A
      console `restart` command and an eye on the blue LED answer it.
      **The command exists since phase 3a** (`esp_restart()`, in
      `apps/firmware_esp32/main/console.c`); "Sensors on the bus" says what
      to watch. **Read from ESP-IDF 5.5, not observed:** `esp_restart()` --
      and the reboot after a panic or a task/interrupt watchdog, which goes
      through the same `esp_restart_noos()` -- resets the CPUs only
      (`esp_rom_software_reset_cpu()`; reset reason 12, `RTC_SW_CPU_RST`),
      plus a list of peripherals in
      `components/esp_system/port/soc/esp32s3/system_internal.c` that does
      not include GPIO. So GPIO0/12/13 may keep their levels across it, and
      with them the 74HCT4094 and `PWR_EN`: the rail would stay up and the
      SCD41 keep measuring -- unlike RST, a chip reset. A reset that reaches
      the whole digital system (the RTC watchdog, brownout) is another case
      again. Once a composition runs `scd4x_source`, its `found_measuring`
      log field answers the same question from the sensor's side -- after a
      `restart` with the source running, it should now read true. The driver
      stops periodic measurement at init either way.
- [x] **A reboot loop when the switched `3V3` comes up.** **Gone
      2026-10-16, mechanism not established** -- see the last paragraph.
      Seen 2026-10-15
      with the phase-3a firmware (ELF SHA256 `52329ece6...`) and the sensors
      wired to `P5` and the 40-pin header, on USB power alone. Every boot:
      `adp_i2c: bus up ... sda_high=false scl_high=false` -- both lines
      **low** at idle, where phase 2d saw both high -- then the scan stops at
      its first address (`I2C_SCAN stopped at 0x08: timeout`), then
      `epdiy-i80: i80 row output ready`, and the chip resets. That line is
      the last thing EPDiy logs before it first writes the 74HCT4094, which
      raises `PWR_EN` and with it the switched `3V3`. The reset reason did
      not reach the log: the ROM prints it, but a chip reset drops the
      USB-Serial-JTAG device and the terminal misses the line. The one that
      arrived, `rst:0x15 (USB_UART_CHIP_RESET)`, is a reset from the USB host,
      probably the terminal reopening the port. Since 2026-10-15 `app_main`
      logs `previous reset reason=... rom_code=...` itself.
      Reading, not yet confirmed: (1) the lines are low because the sensor
      modules sit on the bus unpowered -- their pull-ups go to a dead rail,
      and their chips clamp SDA and SCL through their protection diodes --
      for as long as `PWR_EN` is low, which is the whole boot until EPDiy's
      init; the RTC and the touch controller lose the bus with them. (2) The
      reset is the modules' capacitance and inrush, or a part latched up by
      that back-powering, pulling `VDD3V3` -- which `Q4` connects straight to
      the switched rail -- below the 2.44 V brownout threshold. Checks, each
      decisive: boot with the sensors' `V` wire off `P5`; flash the phase-2d
      image with the wiring unchanged (a loop there too clears the firmware);
      check that `V` and `G` are not swapped (LilyGO prints the connector
      "G 16 15 V") and that `P5` pins 1 and 4 are not shorted with the board
      unpowered. Whatever the cause, (1) means the boot scan, and anything
      else on the bus, must wait until `PWR_EN` is up while the sensors take
      their power from the switched rail.
      **Narrowed the same day by the owner's checks:** `V` and `G` are not
      swapped. With the battery connected as well as USB the board stops
      rebooting, but the boot scan still finds nothing; with the sensors
      disconnected it finds the RTC and the touch controller again. So (1)
      holds: unpowered sensors take the whole bus down. And (2) is the
      supply -- a short or a latched part would not care about the battery.
      The likely reading, not measured: the battery, behind `Q5`, holds the
      `5V` node up when the USB supply, through its cable and `D8`, sags under
      the inrush -- the switched rail feeds the panel's boost as well as the
      sensors, and both start at the same register write. Not yet known:
      which module draws the inrush, and the reset reason by name (the
      `previous reset` line). **Firmware since 2026-10-15:** the bus comes up
      and the boot scan runs after the display adapter, once EPDiy has raised
      `PWR_EN`, `BOARD_SWITCHED_3V3_SETTLE_MS` later.
      **And with that the loop is gone, on USB power alone** (2026-10-16,
      ELF SHA256 `c59289da4...`; the owner's check, and the boot scan in
      "Sensors on the bus"). So (2) was not the supply alone: the same rail
      switching on, with the same modules on the same USB supply, no longer
      resets the chip once nothing has touched the bus before it. What did
      it is not established. Candidates: the state the ESP32's own I2C pins
      were in when the rail rose -- the controller configured, its internal
      pull-ups on, a probe just timed out against lines held low -- or the
      extra current that put into the unpowered sensors' protection diodes.
      The battery's margin hid it, which is why it looked like the supply.
      **A brownout again, 2026-10-16** (ELF `a3fca4872...`, the BSEC
      firmware): the boot of 16:48 reported `previous reset reason=BROWNOUT
      rom_code=3`, once; that boot then ran 21 minutes. **USB power alone --
      no battery** (the owner). What the board was doing when the supply
      sagged -- starting, raising `PWR_EN`, a full refresh -- the log does
      not show: it begins at the boot after. If it recurs, keep the terminal logging
      across the reset: ESP-IDF prints "Brownout detector was triggered"
      before it restarts.
      **Caught, 2026-10-16** (ELF `dd1e81a7b...`, the phase-3e BSEC firmware,
      the owner's log of 20:25 local, right after flashing): a boot that itself
      reported `previous reset reason=BROWNOUT` ran to `epdiy-i80: i80 row
      output ready` and then printed `BOD: Brownout detector was triggered`,
      and the ROM restarted it (`rst:0x3 (RTC_SW_SYS_RST)`); the boot after
      that ran normally. So at least two brownouts in a row, **each at the
      same point as the 2026-10-15 loop**: after EPDiy's last log line before
      it first writes the 74HCT4094 -- which raises `PWR_EN`, and with it the
      switched `3V3` feeding the panel's boost and the sensors -- and before
      its next one (`epd: Space used for waveform LUT`). The bus is not
      involved: nothing touches it before that point since 2026-10-15. So
      the log points back to reading (2) above for these resets -- the
      switched rail's inrush pulling `VDD3V3` under the brownout threshold,
      some boots and not others -- placed by the order of the log lines, not
      measured. USB alone, no battery (the owner). **Again at 20:54 local**:
      after a power cut, a boot reset by the terminal reopening the port
      (`previous reset reason=usb`) browned out at the same line; the one
      after it ran. Both runs began with a reset from the USB host --
      esptool's after flashing, the terminal's after a power cut -- and
      browned out in the boots right after it, but the terminal's reset at
      20:36 was followed by a boot that ran: too few to call a pattern. What
      would settle it: the 3.3 V rail on a scope at `PWR_EN`'s rise; the
      cheap mitigations to weigh, in phase 5's power work at the latest: a
      slower rise of the switched rail, or more bulk capacitance on
      `VDD3V3`.
      **Worse with the microSD card, 2026-10-16 22:36** (ELF `f19605034...`,
      the first phase-3f BSEC image, the owner's YAT log, a 16 GB card in
      the slot): six boots in a row reset themselves at the same point --
      each after `epdiy-i80: i80 row output ready`, about 0.9 s later -- before
      the seventh ran. Five were the interrupt watchdog (`rst:0x8
      (TG1WDT_SYS_RST)`, "PRO/APP CPU has been reset by WDT", `previous reset
      reason=interrupt_watchdog`) with no panic printed, the saved PC in
      `panic_enable_cache` (`panic_handler.c:285`) or `xt_highint4`: the
      watchdog's interrupt entered the panic handler, which then stalled
      re-enabling the cache until the second stage reset the chip. One was a
      brownout (`BOD: Brownout detector was triggered`, saved PC in
      `rtc_brownout_isr_handler`). **On the battery as well as USB, the same**
      (the owner): "a couple of resets, then it boots". What was new at that
      point: the card's task, started at the top of `app_main()`, was
      initialising the card and then reading its whole FAT to count the free
      space -- `card mounted` came at 5,780 ms in the boot that ran -- across
      the moment `PWR_EN` rose. The reading, not measured: the switched
      rail's inrush through `Q4` dips `VDD3V3` itself, behind the LDO, which
      is why the battery does not help; the card's current on the same rail
      deepens the dip, sometimes past the brownout threshold and more often
      to a level the octal PSRAM -- a 3 V part on `VDD3V3` -- does not ride
      out, while the detector stays quiet: a CPU stalled on a cache access is
      what the stuck PC shows. **Changed the same evening**: the card's task
      starts only after `PWR_EN` is up, `BOARD_SWITCHED_3V3_SETTLE_MS` has
      passed and the RTC has been read (`start_card()` in `main.c`), and a
      mount no longer counts the free space (`sd` does, on demand). To see on
      the board: several boots of the new image with the card in, and one
      without -- and if the resets persist with no card at all, the card's
      timing was not the cause.
      **That was not it, 23:32 the same evening** (ELF `f2929013f...`, the
      card started after the rail, battery and USB both connected, the card
      in): the board never got through -- some twenty boots in under 40 s,
      every one reset at the same point, interrupt watchdog (saved PC
      `xt_highint4` each time) or brownout (four), and in none of them had
      anything of the card run: the `adp_sd` line comes later in the boot. So
      the card's mount was not the cause. What the two phase-3f images share,
      and phase 3e did not, at that point: `log_tee.c`'s hook. EPDiy raises
      `PWR_EN` in `epd_control_reg_init()` and logs `epd: Space used for
      waveform LUT` in the next statement (`render.c:245-266` in the pinned
      component); in phase 3e that line went to the USB console alone, its
      LUT in internal RAM, while in 3f the hook writes it into the ring in the
      octal PSRAM -- within microseconds of the rail switching on, in the
      inrush. The reading, not measured: a PSRAM transaction in that dip
      hangs the memory interface and the CPU with it, short of the brownout
      detector's threshold; the brownouts are the same dip going deeper, as
      in phase 3e. **Changed**: `main.c` holds the hook off PSRAM
      (`log_tee_hold()`) from before the display adapter's init until the
      rail has settled, after the bus comes up; the lines of that window go to
      the console only, and the file says how many it missed. To see on the
      board: boots with the card in, on USB alone and with the battery. If
      the interrupt-watchdog resets go and only the occasional brownout of
      phase 3e is left, the reading holds; if they stay, it does not, and a
      scope on `VDD3V3` at `PWR_EN`'s rise is the next step. In any case the
      octal PSRAM not riding out that dip is a hardware weakness firmware can
      only step around: anything that touches PSRAM in the first moments
      after `PWR_EN` rises is exposed (EPDiy's own framebuffers are
      allocated some 20 ms later), and phase 5's raising of `PWR_EN` after a
      deep sleep inherits the same rule.
      **And with that the resets are gone, 23:45** (ELF `7acd662da...`, the
      BSEC image, the card in): the boot after flashing ran through at the
      first attempt -- `epd: Space used for waveform LUT` straight after
      `epdiy-i80: i80 row output ready`, as in phase 3e -- and the owner saw
      no reset since. One boot in the log; the reading holds for now, not
      proven. The four lines held off the file in that window are counted in
      `rec`'s `4 dropped`.
      **Since Wi-Fi, from the card, 2026-10-20 to 22**: eight boots reported
      `previous reset reason=BROWNOUT`, every one the boot after one the card
      has nothing of -- the pattern of a brownout at `PWR_EN`'s rise, before the
      card mounts, in the first boot of a freshly flashed image or after the
      terminal's reset. One may be another kind: 13:56:30Z on 2026-10-22 followed
      a `power_on` boot that had run 10 s, its last line on the card at 10.1 s,
      the Wi-Fi associated and DHCP under way. Up to 10 s of lines never reach
      the card before a reset (its flush interval), so whether the supply sagged
      at run time -- the radio's current on `VDD3V3` -- or the terminal reopening
      the port reset the chip into a brownout at the rail's rise, the card cannot
      say. To see: a terminal attached through a cold boot, and `BOD:` after
      `adp_net` lines rather than after `epdiy-i80: i80 row output ready`.
      **What follows, until the mechanism is known:** nothing may drive the
      bus while `PWR_EN` is low and the sensors are on the switched rail.
      That binds phase 5 above all: clearing `PWR_EN` for deep sleep, or
      waking with it low, must leave the I2C controller deinitialised until
      the rail is back. Powering the sensors from the always-on `VDD3V3`
      instead would remove the whole question, at the cost of switching
      them off by command rather than by rail.
- [ ] **Phase 3c's measurements.** The firmware logs what each needs. Each
      replaces an estimate or sets a comparison point, and lands here and at
      the estimate's definition. **First run 2026-10-16** (ELF
      `ea261721f...`, 30 minutes from boot at about 02:28:51; the owner
      warmed the BME680 by hand and breathed on both sensors in the first ten
      minutes, unplugged both at 02:41:45-02:42:45 and aired the room
      02:45-02:53; a reference sensor in the next room read 21.6 C, 48 %,
      1006 hPa):
      - *Event loop stack*: `stack_free_min_bytes=3476` at 4.6 s, and never
        lower through the unplug and the recovery: 4,716 bytes used of
        8,192. Kept at 8,192; 6,144 would leave 1.4 KB.
      - *Internal RAM after EPD init*: 223,115 against 224,467 before the
        sensors: 1,352 bytes, the drivers' and the manager's static state.
        The event loop's stack, 2,048 bytes larger since phase 3c, comes out
        of the heap after that line.
      - *The heater*: `heat_stab=false` at the first measurement after each
        init, and never again in 30 minutes -- the 300 C / 100 ms profile
        reaches stability once the plate has cycled.
      - *Gas*: 12.0 kOhm at the first valid reading (300 s), 30.3 kOhm at
        12 minutes; after the replug's power cycle 45.7 kOhm at 6 minutes and
        57.8 kOhm at 16, still rising about 1 kOhm a minute. The 300 s
        burn-in is far short of where the reading settles; how far needs
        hours, without breath on the sensor.
      - *Temperature*: over 02:45-02:59 the SCD41 read 0.23-0.54 C (about
        0.45) above the BME680 with its 0.3 C self-heating offset taken off;
        both 0.5-1.8 C above the reference -- but the reference was in
        another room, so neither offset is settled. Next: the reference
        beside the device.
      - *Humidity*: BME680 46.3-48.1 %, SCD41 47.1-49.2 %, reference 48 %.
      - *Pressure*: 1006.20-1006.30 hPa against the reference's 1006 --
        agreement within its resolution. Seen in the minute samples: a dip
        of 0.2 hPa each time the sensor was warmed by hand to about 30 C
        (02:31:52, 02:39:05) -- the die's temperature, not the air's
        pressure; flat at 1006.28-1006.30 (+-0.01) while the window was open;
        and 0.07 hPa (7 Pa) lower within a minute of closing it, which fits a
        room going from open to the outside to slightly depressurised. Nothing
        at the unplug. One-minute samples cannot resolve faster events.
      - *CO2*: about 1,100 ppm in the closed room, 5,423 at the peak of
        breathing on the sensor; eight minutes of airing brought it from
        1,118 to 939, nowhere near outdoor air, so nothing about the
        calibration follows. The warm-up cannot be measured from these logs:
        a value in warm-up is withheld (`warmup`), so where the readings stop
        drifting after a cold start does not show.
      **Second run 2026-10-16** (ELF `9c8d96f20...`, the larger type,
      ~6 minutes): the event loop's stack low-water mark 3,588 bytes free --
      the 160px font did not deepen it; partial refreshes now draw for
      ~320-630 ms (larger glyphs, more rows) and a full one ~1,020 ms after
      the clear, against 275-450 and 875 before; the gas's first valid
      reading, at 6 minutes, was 78.8 kOhm against 12.0 the day before -- the
      burn-in goes on across power cycles.
      What each item needs:
      - **Temperature and humidity, three ways**: the BME68x's and the
        SCD41's (`bme68x: sample ...` and `scd4x: sample ...`, once a
        minute each, the SCD41's logged even while the BME68x's is shown)
        beside a reference thermometer, after an hour of running. Gives
        `HAC_BME68X_SELF_HEATING_C` its first measured value
        (requirements 13.2), and says whether the SCD41's own offset
        (4 C by default, inside the sensor) suits this board.
      - **Pressure** against a local weather report's sea-level pressure,
        corrected for this room's altitude.
      - **CO2 after airing the room**: an open window brings it towards the
        outdoor ~420 ppm. With the warm-up seen from a cold start, it
        replaces `SCD4X_WARMUP_MS`; whether the reading moves with the
        pressure handed over (`sensor_manager: ambient handed over`) is a
        separate question, ADR 0018.
      - ~~**This unit's BME68x calibration.**~~ **Done 2026-10-16**, from
        the test app's five `bme68x: calibration (n/5)` lines: the host
        model's default since then (`BME680_MODEL_BOARD_CALIBRATION`,
        `platform/host/i2c_models/bme680_model.c`), and the simulator's
        fake-i2c mode runs against it.
      - **Free internal RAM after EPD init** (`adp_display: memory budget
        ... free_internal_after_epd_init=`) against 224,467 at the same
        line before the sensors (2026-10-13, "Free PSRAM at runtime").
      - **The event loop's stack**: `event_loop: stack low-water mark
        stack_free_min_bytes=...` at its lowest after the drivers have run
        a while -- an init, failures and a recovery included, so the unplug
        test (checks, item 6) belongs in the same run. The task has
        8192 bytes since phase 3c.
- [x] **Which sensor variants are fitted.** **Resolved 2026-10-16** on the
      board (the test app's `port_sensor` contract, ELF `6ab6ea91b...`): an
      **SCD41**, serial `0x2103c5073b73`, so single-shot mode is available;
      and a **BME680**, `chip_id=0x61 variant_id=0x00` -- the part the host
      model is, so the driver's gas path is the modelled one. The history
      follows. BME680 or BME688 (the BME68x's
      `variant_id` register), and SCD40 or SCD41 (the seller lists both;
      read the variant back): the drivers log both, phase 3a/3b.
      **SCD4x side done in phase 3a, on the host:** `scd4x_source`'s init
      logs `variant=SCD40|SCD41|SCD43|unknown` and the serial number, and
      refuses single-shot mode on an SCD40 (`docs/adr/0015-scd4x-driver.md`).
      **BME68x side done in phase 3b, on the host:** `bme68x_source`'s init
      logs `chip_id=0x61 variant=BME680|BME688|unknown variant_id=0x..`;
      Bosch's code handles both parts, so the driver does not refuse either
      (`docs/adr/0016-bme68x-driver.md`). The host's model is a BME680 only:
      if the board says BME688, the driver's gas path (its own registers and
      formula) has met no model, and its first readings are the check.
- [ ] **The BME68x's estimates (phase 3c).** Five numbers in phase 3b's
      driver and model are not measured, each marked at its definition:
      the heater profile, 300 C for 100 ms (the datasheet's quick-start
      example, `HAC_BME68X_HEATER_*`); the gas burn-in, 300 s of `warm`
      (Pimoroni's example, `HAC_BME68X_GAS_BURN_IN_MS`); the self-heating
      offset, 0.3 C (a back-of-envelope, `HAC_BME68X_SELF_HEATING_C`); the
      heater's ambient temperature, 25 C (`HAC_BME68X_HEATER_AMBIENT_C`); and
      the conversion time, which no datasheet gives and which the model
      takes from Bosch's own estimate, so no host test can show it wrong.
      The host model's calibration is this unit's since 2026-10-16 (the
      test app's log). With BSEC linked, its own heater profile and heat
      compensation replace the first and third. **First evidence on the
      heater, 2026-10-16**: the two forced measurements the test app made
      right after init both came back `gas_valid=true heat_stab=false` --
      300 C was not reached in 100 ms from a cold plate. Whether it is once
      the plate has cycled for a while, the firmware's minute-by-minute
      `bme68x: sample ... gas_ohm=` shows: a number, or `error`.
- [ ] **BSEC on the board (phase 3d).** Built 2026-10-16 with BSEC 2.6.1.0
      (`docs/adr/0020-bsec-iaq.md`), verified on the host only. The firmware
      with `sdkconfig.bsec` logs what each question needs; the first run
      answers them:
      - *It runs at all*: the banner says `BSEC2 IAQ`, and `bme68x_bsec:
        sensor ready ... bsec=2.6.1.0 config=bme680_iaq_33v_3s_4d
        state=fresh`; then `heater profile` lines -- on the host 380 C,
        150 C, 200 C for 1000 ms each, then 320 C for 197 ms -- and `iaq
        accuracy from=- to=unreliable`, then `to=low` within about a minute
        (17 measurements on the host) and `gas sensor status ...
        run_in_done=true`. A board that stays at `unreliable` is the timing
        question below, or the sensor.
      - *How late the calls come*: every `BSEC called late late_ms=...
        late_calls=...` line. Expected: one per full refresh at least, and
        some partial refreshes'. BSEC tolerates 187 ms. The ADR's
        "Timing" section says what follows if IAQ never reaches `medium` or
        `high`, or flickers with the refreshes: the flush off the event
        loop's task.
      - *Accuracy*: `medium` and `high` need BSEC to have seen the air both
        clean and polluted -- an open window, then breath or a marker pen
        near the sensor. Note how long each took, and that every reboot
        starts again from `unreliable` (the item below).
      - *Temperature*: BSEC's heat-compensated temperature, in the
        `bme68x_bsec: sample` line once a minute, beside the SCD41's and a
        reference thermometer. The difference gives `HAC_BSEC_DEVICE_HEAT_C`
        (`drivers/bme68x/bme68x_bsec_limits.h`, an ESTIMATE of 0) its first
        value, as phase 3c's temperature item does for the forced-mode
        driver's offset.
      - *The module's supply*: the configuration assumes 3.3 V at the chip.
        A meter on the module's VCC pad and, if its regulator's output is
        reachable, there: a reading near 1.8 V would call for the `18v`
        configuration (`cmake/bsec.cmake`).
      - *Cost*: the `memory budget` line (BSEC adds 1,476 bytes of instance
        and state and a 4,096-byte work buffer, all static) and the event
        loop's `stack low-water mark`, now with BSEC's calls on that stack.
      - *The test app*: `idf.py -T hac_platform -B build-bsec -D
        SDKCONFIG=build-bsec/sdkconfig -D HAC_BSEC=ON build` adds `contract
        port_sensor bme68x_bsec_source+BME68x` lines to the run.
      **First run 2026-10-16** (ELF `6d984c914...`, 21 minutes from a USB
      reset, the owner's log of 16:11, no reference thermometer, nothing done
      to the air):
      - *It runs*: banner and `sensor ready ... bsec=2.6.1.0
        config=bme680_iaq_33v_3s_4d state=fresh` as expected; the heater
        profiles at the same calls as on the host -- 380 C from the first,
        150 C at 13.9 s, 200 C at 16.9 s, 320 C for 197 ms from 22.9 s.
      - *Run-in*: `run_in_done=true` and accuracy `low` together at the 66th
        measurement (197 s), against the 17th on the host's model: the real
        element took four times as long. Accuracy stayed `low` to the end of
        the log; `medium` and `high` were not tried for (no polluted air).
      - *Late calls*: two in 21 minutes. 459 ms, the boot's first full
        refresh (3.4 s); 490 ms, a 588 ms partial refresh that started just
        before a call. No full refresh came after boot -- 14 partial ones,
        under the budget of 20 -- so the steady state's full refreshes are
        still to be seen.
      - *IAQ at accuracy `low`*: 48-51 for five minutes after the run-in,
        then 55-81 for the next twelve, while the gas resistance went from
        153 kOhm to a 164.5 kOhm peak (8 min) and back to 158-162, humidity
        from 51 to 54 %, and CO2 fell from 790 to 734 ppm. An index scaled
        to the cleanest air BSEC has seen since boot, not an absolute one.
      - *Gas resistance* with BSEC's 320 C / 197 ms: 153-165 kOhm, against
        46-79 kOhm on the forced-mode driver's 300 C / 100 ms in earlier runs
        -- a different heater profile, so not comparable.
      - *Temperature*: once the SCD41 had warmed up (9 min on), BSEC's
        heat-compensated value read 0.2-0.35 C above the SCD41's, where the
        forced-mode driver with its 0.3 C offset read about 0.45 C below it.
        Which is nearer the air needs the reference. Humidity: BSEC
        51.4-54.5 %, SCD41 52.6-54.5 %.
      - *Cost*: `free_internal_after_epd_init=217499`, 5,616 bytes under phase
        3c's 223,115 -- the 5,572 bytes of instance, state and work buffer,
        and a little code's data. The event loop's stack low-water mark did not
        move from 3,476 bytes free: BSEC's calls are shallower than the
        deepest path already there.
      Still open from this list: `medium`/`high` (an open window, then
      breath or a marker pen), a steady-state full refresh's lateness, the
      reference thermometer, the module's supply, the test app.
      **Since ADR 0021** (ELF `a3fca4872...`, same day, not yet flashed) the
      screen shows it too: IAQ's band under the number, a question mark
      beside it and "IAQ not calibrated" in the footer while the accuracy is
      `low`, both gone at `medium`. That is the first thing to see on the
      panel.
      **Second run 2026-10-16** (ELF `a3fca4872...`, 21 minutes, the owner's
      logs of 16:48 and 17:09 -- one boot, 38 s lost between the files; the
      owner breathed on the sensors at about 4 and 9 minutes and aired the
      room):
      - *Accuracy reaches `high` despite late calls*: `low` at 120 s (the
        40th measurement), `medium` at 796 s, `high` at 955 s, then once
        back to `medium` and up again. Two breaths of polluted air and the
        clean air after them were enough -- not the half hour of each that
        Bosch's text suggests.
      - *Late calls*: 17 in 21 minutes, 310-1,630 ms. Every full refresh
        made one (6, the boot's included, 0.47-1.63 s); the other 11 were
        partial refreshes that began within about 0.6 s of a call. This run
        redrew 112 times -- 60 for CO2, which the breaths moved by hundreds
        of ppm a cycle -- far more than a quiet room. That calibration got
        to `high` regardless answers ADR 0020's "Timing" question for this
        rate of lateness: nothing to change for BSEC's sake yet.
      - *The screen (ADR 0021) as designed*: a status redraw when the
        number came (unreliable to low, 120 s) and when the question mark
        went (low to medium, 798 s); none for the medium/high back and
        forth. The IAQ followed the breaths: 200 at the first, 206 at the
        second, back to 50-60 within three or four minutes.
      - *IAQ's band redraws too often*: 20 of the 112 redraws were for a
        change of band, several 6 s apart (419, 425, 431, 443 s; 1070,
        1082, 1094 s) while the index sat at 50-60. BSEC's resting value in
        clean air is exactly 50.000 -- seen in the minute samples and at
        every accuracy change -- and 50 is the boundary between "Excellent"
        and "Good", so the index crosses it back and forth. (**Corrected
        2026-10-20** by three days of 3 s samples: at rest the index sits at
        51-53 and dips to 50; exactly 50.00 in 1-2 % of samples -- ADR 0026.) (The log gives
        no IAQ at each redraw; the cadence and the minute samples make
        flapping the likely reading.) BSEC's other anchor, 200, is also a
        boundary. Not changed; a candidate is a hysteresis -- a band change
        redraws only once the index has also moved some points since the
        last frame. The owner's decision, 2026-10-16: not yet -- test longer
        first, with this as the thing to watch (ADR 0021).
      - *Temperature*: in the quiet last eight minutes BSEC's read 0.3-0.65 C
        above the SCD41's (0.2-0.35 C in the first run), and the breaths
        warmed the BME68x by up to 1.5 C where the SCD41 moved 0.3 C -- the
        BME68x's module is the more exposed. Humidity: BSEC 1.5-3 % below
        the SCD41. Still no reference.
      - *Gas*: 119-127 kOhm in clean air, against 153-165 kOhm in the first
        run on the same heater profile, at 2-3 % higher humidity.
      - *Stack*: the low-water mark stayed at 3,476 bytes free.
      **Third run 2026-10-16** (ELF `dd1e81a7b...`, phase 3e, 10 minutes,
      the owner's log of 20:25 local): IAQ's band flapping at 50 again, and
      more of it -- 13 of the first 19 redraws (98-238 s from boot) were
      `reason=iaq` while the index sat at 50-58, so the partial-refresh budget
      was spent in 5 minutes and the 21st redraw, at 325 s, came out full.
      Still the owner's call (ADR 0021: watch it before adding a hysteresis).
      **The flapping, narrowed 2026-10-20** (`docs/adr/0026-iaq-redraw-delta.md`):
      three days unattended made it 96 % of all redraws (884 and 1,494 of
      921 and 1,568 on 2026-10-18 and 19, 44 and 74 full refreshes, 167 and
      292 late BSEC calls -- open item "The touch panel on the board", "In
      passing"). IAQ no longer redraws when its band changes but, as the
      other readings, when its number has moved by more than 5 points since
      the frame on the glass -- the owner's choice, after a first image that
      kept the band and added a delta of 10. The noise's excursions above 50
      peaked at 51.5 (median), 56.6 (p90), 58 at most, the smallest real
      event at 65; about three quarters stay within 5. Replayed through the
      simulator, those two days' IAQ redraws fell from 544 and 959 to 215
      and 253, the full refreshes from 28 and 49 to 11 and 14, and every move
      past a band's boundary by more than 10 was on the glass within 1 s, as
      before; the number on the glass was never more than 10 from the air
      (it was for 11.6 % of the 18th). At the board's 3 s rate an estimated
      280 and 243 IAQ redraws a day on such days. The band on the glass
      disagrees with the reading's 4.5 % and 10.4 % of the time (2 % and 4 %
      before) -- at rest, the word under the number says which side of 50
      the last frame caught. Whether 50 is the right boundary for a room
      whose index rests at 51-53 is the owner's (ADR 0021's bands).
      **The first image on the board, band and delta 10, 2026-10-20** (ELF
      `12ef228cf...`, 43 minutes from 21:09 local, the owner's YAT log, after
      a boot that reported `previous reset reason=BROWNOUT`): 41 redraws,
      none for IAQ -- 26 for CO2 through a breath at 21:14 (IAQ to 143, drawn
      by the CO2 redraws), 6 temperature, 6 status; 2 full refreshes; 6 late
      BSEC calls; IAQ at rest 49.5-51.9 in the minute samples.
      Still to see on the board with the delta of 5, a day unattended with
      the card in: from `log/`, `decision ... reason=iaq` lines, `decision
      action=full` lines and `BSEC called late` lines per day, against
      884-1,494, 44-74 and 167-292 before; and the redraws of ADR 0027 and
      0028 on the same branch, `reason=pressure` (none in the replay) and
      `reason=settle` (3-5 a day in the replay).
      **The delta of 5 on the board, a night from the card** (ELF
      `ec5d8a60f...`, one boot, 2026-10-21 22:04Z to 2026-10-22 07:27Z, 9.4
      hours, the card's `log/` copied off by the owner): 73 redraws -- 53 for
      IAQ (3 of them full), 9 temperature, 5 `settle`, 4 CO2, 2 status, none
      for pressure -- and 5 late BSEC calls, 480-710 ms. Quiet until about 05Z:
      11 redraws in the first seven hours, 51 in the two after, when the room
      woke (CO2 to 865 ppm, IAQ to 176 at the power cut). Scaled to a day, about
      190 redraws, 135 for IAQ, 8 full and 13 late calls, against 921-1,568,
      884-1,494, 44-74 and 167-292 on 2026-10-18 and 19, and the replay's 243-280
      IAQ redraws a day estimated for such days: the delta does on the board
      what the replay said, or better. `settle` fired 5 times (the replay: 3-5
      a day). A night, not yet a day with people in the room.
      - *A brownout before this boot*: `previous reset reason=BROWNOUT
        rom_code=3` -- ESP-IDF's brownout handler resetting the chip -- after
        the image was flashed. The brownout itself is not in the logs. See the
        item "A reboot loop when the switched 3V3 comes up".
- [ ] **BSEC's state restarts at every boot.** BSEC learns the room: its
      IAQ accuracy rises from 0 as its background calibration sees the air
      clean and polluted, and its state holds that. Since phase 3d the state
      survives a re-init of a failing sensor -- the source keeps it in RAM --
      but nothing keeps it across a reboot or a deep sleep: IAQ accuracy
      starts from `unreliable` at every boot, and the scale from the
      library's defaults. Keeping it is `persisted_state_t`'s (phase 5,
      `docs/architecture.md` 4.2, 180 bytes for BSEC 2.6.1.0) and NVS's
      (phase 6), not phase 3d's. Bosch's integration example saves it every
      few hundred samples; the cost to weigh then is flash wear against
      calibration lost. The configuration's four-day history
      (`bme680_iaq_33v_3s_4d`) was chosen with this in mind; revisit it
      when the state persists.
- [x] **Which addresses the sensors answer at.** **Resolved 2026-10-16**
      by the boot scan: the SCD4x at `0x62`, the BME68x at `0x77` (SDO high
      on this module), nothing at `0x76`, nothing unexpected ("Sensors on
      the bus").
- [ ] **What the SCD41 does where its datasheet is silent.** The host model
      (`platform/host/i2c_models/scd41_model.h`) assumes, and the driver relies
      on, three behaviours the datasheet does not state: the address is
      refused while a command executes; a command not allowed in the current
      state is refused with a data NACK; a reading still buffered when
      measurement stops is discarded. The first matters most -- it is how
      `scd4x_source` tells "busy" from "gone". All three show in the first
      logs of the driver on the board (and in the `i2c.master` driver's own
      NACK lines); correct the model if the sensor disagrees.
      **First evidence, 2026-10-16** (the test app, ELF `6ab6ea91b...`):
      the second, refused: `get_serial_number` sent to a measuring SCD41 was
      refused on the write (`i2c_master_multi_buffer_transmit ... I2C
      transaction failed`), but whether at the address or at the command
      byte the log does not say -- `adp_i2c` logs its classification at
      `DEBUG` only. The first was not exercised: the driver never addresses
      the sensor during a single shot, and both shots were ready by 5 s. The
      third was not exercised. To settle the first two: raise the
      `adp_i2c` tag to `DEBUG` (its `transfer failed ... status=` line names
      `NOT_FOUND` for the address, `IO` for a data byte), or a scope.
- [x] Whether the RTC (PCF8563) is wired to survive deep sleep for real-time
      timestamps, or is only used for scheduling. **Answered by the
      schematic 2026-10-16** ("The RTC"): it runs from `VCC_RTC`, not from
      `PWR_EN`'s rail, with a backup cell, so deep sleep does not stop it;
      since phase 3e it is the device's wall clock (ADR 0023). Whether the
      cell holds it with the board unpowered is the item below.
- [ ] **The RTC on the board (phase 3e).** Built 2026-10-16 (ELF
      `e26c8ec48...`, and `dd1e81a7b...` with BSEC), verified on the host only
      (`docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md`).
      The boot log's `main: wall clock source=... rtc=... rtc_unix_ms=...
      kept_across_reset=...` line and the console's `time` answer each check:
      - *What the RTC holds before anything here set it*: `time` at the first
        boot -- `rtc: VL set` and a 2000s date if the chip lost power since
        the vendor's firmware last wrote it, `rtc: implausible year` if it
        kept a time this firmware rejects, a valid time if the factory demo
        set one and the cell kept it. The footer shows that time or
        `updated --:--` accordingly.
      - *Setting it*: `time_set <YYYY-MM-DDTHH:MM:SSZ>` in UTC, then `time`:
        `source="console"`, the RTC `valid` and within a second. The footer
        turns from `--:--` into the time at the next poll (a status change),
        follows the redraws after that, and nothing redraws while the
        readings hold still.
      - *The backup cell*: remove all power -- USB and battery -- for some
        minutes, then boot. `rtc: valid` with the time right means the cell
        is fitted and holds; `rtc: VL set` means it is not, or is flat (it
        charges while the board is powered, so try again after a day on
        USB). Record either outcome here.
      - *A software reset*: `restart` with the clock set; the next boot's line
        should read `kept_across_reset=true` (ESP-IDF keeps its system time in
        RTC registers a CPU reset does not clear -- read, not seen) and then
        `source=rtc`.
      - *Drift*: an ESTIMATE of about a minute a month (a 32.768 kHz
        crystal's ±20 ppm) until phase 4's SNTP corrects it. `time` against a
        reference a week or two after `time_set` gives this unit's figure.
        **Since phase 4f** the network measures it every hour, with nobody
        watching: open item "Time from the network on the board". Its first
        answer, 2026-10-21, found the RTC 12.9 s behind: -29 ppm since the
        `time_set` of 2026-10-16 (±3, the hand-typed second), if nothing wrote
        it in between. **Measured by the network over a night, 2026-10-22:
        -20.8 ppm** (nine hourly samples, ±2.0 hour to hour), about 54 s a month
        -- the ESTIMATE holds; the -29 was its own assumptions'. A power cut of
        6.5 hours the same day cost 2.6 s more than that (open item "Time from
        the network on the board").
      - *The autumn change*: 2026-10-25 at 01:00 UTC; if the device runs
        through it, its next redraw shows the hour step, 03:59 -> 03:00 in
        Vilnius.
      **First run 2026-10-16** (ELF `dd1e81a7b...`, the BSEC image, the
      owner's log of 20:25 local):
      - *What the RTC held*: `rtc: VL set`, 2000-01-27T05:06:30Z at boot,
        registers `08 00 A5 07 05 27 05 01 00` -- VL set, Control_status_1
        at its power-on value (TESTC), nothing ever written. `wall clock
        source=none`, so the footer had no time. And it was **running**: a
        minute later `time` read 05:07:25. Read back, not established: a
        power-on of the chip on about **2026-09-20 at 12:19 UTC** -- 26 days
        5 h 6 min before this boot, if it started from 2000-01-01 00:00 with
        weekday 0 (the register's 5 is that plus 26 days; the calendar's
        2000-01-27 was a Thursday, 4) -- and the chip counting ever since.
        If the board was ever without USB and battery after that date, the
        backup cell carried it through; if it never was, nothing follows.
      - *Setting it*: `time_set` with a bad argument was refused
        (`"2026-10-16T17:28:402>" is not YYYY-MM-DDTHH:MM:SSZ`); the next,
        `time_set 2026-10-16T17:35:40Z`, gave `rtc=ok system=ok`, local
        20:35:40 UTC+03:00, `source="console"`, and the registers `00 00 40
        35 17 16 05 10 26` -- VL and TESTC clear, weekday 5, a Friday. The
        footer's status redraw followed 1 s later (`reason=status`), its
        rectangle 381 rows tall, from the headline's top to the footer:
        EPDiy's difference pass took 75 ms, 196 us a row as
        `EPD_SIM_DIFF_PER_ROW_US` has it, and the whole flush 648 ms.
      - *Quiet readings, no redraws*: none between 329 and 566 s of uptime,
        nor between the time_set's redraw and the next reset, 37 s later.
        No `reason=periodic` anywhere.
      - *A short unplug*: the owner unplugged USB (the only supply; no
        battery) and plugged it back, within the 37 s after the time_set's
        redraw. The first boot in the log after it is **not the power-on
        one**: it reports `previous reset reason=usb rom_code=21`, a reset
        from the USB host -- the terminal reopening the port, as on
        2026-10-15 -- so the power-on boot before it ran unlogged, while the
        terminal was reconnecting. That boot read the RTC and set the system
        time, and the reset after it kept it (`kept_across_reset=true`, the
        system time in RTC-domain registers a digital-core reset does not
        clear, as read in ESP-IDF's source). Then `source=rtc rtc=rtc:
        valid`, 17:36:19Z; `time` a few seconds later: system and RTC
        17:36:25Z, agreeing to the second. (Read at first, wrongly, as an
        unplug that did not cut the power; the owner confirmed USB alone.)
        The SCD41 came back `found_measuring=false`.
      - *Cost*: `free_internal_after_epd_init=217247`, 252 bytes under
        phase 3d's 217,499; the event loop's stack low-water mark unchanged at
        3,476 bytes free.
      - *On the glass* (the owner, by eye): as expected -- "updated --:--"
        before time_set, the time after.
      **Second run 2026-10-16, the backup cell** (same ELF, the owner's log
      of 20:52 local): USB -- the only supply, no battery -- out for more
      than a minute, then back. The log again starts after the power-on
      boot: a boot reset by the terminal (`previous reset reason=usb`) that
      browned out when `PWR_EN` rose ("A reboot loop ..." in the open items),
      then one after the brownout that ran: `wall clock source=rtc rtc=rtc:
      valid rtc_unix_ms=1792173275000 kept_across_reset=true`, 17:54:35Z.
      **The RTC came through the power cut with VL clear**, so its supply
      never fell below what its oscillator needs: the cell is fitted and
      held it, for one to two minutes at least. Twice confirmed: the
      unlogged power-on boot found it valid too, or nothing would have set
      the system time that the later boots kept. Not shown by the log: that
      it kept *counting* -- the RTC is the only clock across the gap. VL
      clear makes a stopped oscillator unlikely; `time` against a reference
      clock settles it, and is the drift's first data point. One reference
      before the cut agrees: the log file's name, 20:52:05 local, against
      17:52:06Z worked out for its first line from the RTC-set clock.
      Still open: `time` against a reference (a stopped clock during the cut
      would show as the outage's length behind; otherwise, the drift since
      time_set); a longer outage -- hours -- for the cell's capacity; the
      cell's charge after days unpowered; the autumn change. **`restart`
      itself seen 2026-10-20** (phase 3g's image): `previous reset
      reason=software`, then `wall clock source=rtc rtc=rtc: valid
      kept_across_reset=true` -- the system time survived the CPU reset, as
      read in ESP-IDF's source, and the RTC agreed. A tip for these: YAT's own time stamps on each line
      give the log an outside clock.
- [ ] **The microSD card on the board (phase 3f).** Built 2026-10-16,
      verified on the host only (`docs/adr/0024-sd-card-recordings-and-debug-log.md`).
      What each check is to show, and the lines that show it:
      - *Boot without a card*: one `card_writer: no card: recording and the
        log file are off until one is mounted state=no_card` line, and
        nothing about the card after it -- no ESP-IDF `sdmmc_*` or
        `vfs_fat_sdmmc` lines, though a quiet mount is tried every minute.
        Everything else as before; `sd` says `SD no_card`, `rec` says
        `REC off (no_medium)` with rows dropped and counted.
      - *Insert a card and `sd_mount`* (FAT32): `SD_MOUNT ok`, then `card
        mounted card=... capacity_mb=... spi_khz=... free_internal=...
        free_psram=...`; `recorder: recording
        path=data/<day>_scd4x.csv` and `..._bme68x.csv` at the next rows;
        `rec` shows both files growing. The log file gets the boot's lines,
        kept in the ring since boot, unless more than 1024 were logged
        meanwhile -- then a `log: N lines lost before this one` line. Without
        `sd_mount` the automatic attempt finds it within a minute.
      - *The cost*, from the `card mounted` line and `adp_display: memory
        budget ... free_internal_after_epd_init=`: internal RAM against
        217,247 (phase 3e's BSEC image, the last measured) and PSRAM against
        7,347,640 after EPD init (phase 2d) minus the ring's 327,680 bytes;
        the event loop's stack low-water mark against 3,476 bytes free; the
        card's task's own (`card_writer: stack low-water mark`, of 6,144).
      - *No stall on the loop*: `rec`'s `the longest post` (microseconds,
        expected tens) beside `slowest write` and `slowest flush` (the
        card's milliseconds the event loop did not wait); the hourly
        `recording stats` line carries both.
      - *Hours unattended*: the bytes per day (`rec`, `sd`), for the caps
        (`RECORD_DATA_DIR_CAP_BYTES`, `RECORD_LOG_DIR_CAP_BYTES`, estimates)
        and the log's volume, which nothing has measured.
      - *A power cut in the middle of a write*: the card mounts at the next
        boot, every file opens on a PC and in the simulator, and at most one
        flush interval (`RECORD_FLUSH_INTERVAL_MS`, 10 s, an estimate) is
        missing before the cut; the new boot writes the day's next file
        (`_2`).
      - *A card pulled without `sd_eject`*: `the card stopped answering:
        unmounted, recording off state=lost`, and recording resumes when it
        is back and mounted.
      - *On a PC*: a day's `data/<day>_scd4x.csv` opens in a spreadsheet,
        and `simulator --recording` with both of a day's files plays it,
        `[sim] recording ... plays source id 1` and `2`, the wall clock at
        the first row.
      - *The supply*: an SD card draws tens of milliamps while it writes, on
        `VDD3V3` -- the rail behind the brownouts of the open item "A reboot
        loop when the switched 3V3 comes up". A brownout at a flush, or at
        boot with a card inserted, belongs there.
      - *The test app*: `idf.py -T hac_platform build` with a card in the
        slot adds `contract port_files adp_sd` lines; without one the case
        is IGNORED and says why.
      **First run 2026-10-16 22:36** (ELF `f19605034...`, the BSEC image,
      a 16 GB card, `SD16G`, 14,772 MB, at 20 MHz): six boots reset
      themselves when `PWR_EN` rose -- read first as the card's mount, then,
      after a second image (23:32) that never booted at all, as the log's
      hook writing PSRAM in the rail's inrush; the open item "A reboot loop
      when the switched 3V3 comes up" has both and the change made for
      each. The seventh ran: `card mounted` at
      5,780 ms -- the mount had spent about 5 s counting the free space, a
      whole-FAT scan, since dropped from the mount -- `free_internal=184043
      free_psram=6711076`; both recordings opened at 6.9 s
      (`data/2026-10-16_bme68x.csv`, `..._scd4x.csv`), the card's task's
      stack low-water mark 2,664 bytes free of 6,144, the event loop's 3,412
      (3,476 before this phase). `free_internal_after_epd_init=195983`,
      21,264 under phase 3e's 217,247: the 12.8 KB of static RAM this phase
      adds, the card's task's stack and queues -- then allocated before the
      display -- and the SPI bus.
      **Second run, 23:45** (ELF `7acd662da...`, the BSEC image with the
      log held off PSRAM while the rail rises; no reset): `card mounted` at
      973 ms, with no free-space count; `free_internal_after_epd_init=204327`,
      12,920 under phase 3e's 217,247 -- the 12.8 KB of static RAM this phase
      adds, as the build said -- and `free_psram_after_epd_init=7021416`;
      after the mount `free_internal=184551 free_psram=6711432`, the event
      loop's 8 KB stack and its 253 KB diff buffer included. The card's task's
      stack low-water mark 2,688 bytes free of 6,144; the event loop's 3,412.
      Recording into `data/2026-10-16_scd4x_5.csv` and `..._bme68x_5.csv` --
      the day's fifth files, the boots before having each started their own,
      as designed. `rec` after four minutes: 44 SCD41 rows, 2,316 bytes (53 a
      row, about 0.9 MB a day at 5 s), 75 BSEC rows, 6,195 bytes (83 a row,
      about 2.4 MB a day at 3 s), nothing dropped; the log 85 lines, 10,424
      bytes, the 4 held lines its only loss; the ring 16 lines at most, never
      full. **The event loop's longest post into the card's queue: 33 us;
      the card's slowest write 102.6 ms and slowest flush 87.8 ms** -- what
      the loop would have waited had it written itself. `sd`: 14,763 MB
      free, answered within a quarter second (the card's FSINFO held the
      count this time); `data/` 10 files, 122 KB, `log/` one file, 112 KB.
      **Six days unattended, from the card copied off on 2026-10-22**
      (`log/2026-10-16` to `-22`, `data/` alike, analysed locally): the night of
      2026-10-21 to 22 recorded 17,260 rows with none dropped, the slowest flush
      112 ms, the event loop's longest post 38 us. Two things seen for the first
      time:
      - *A power cut* (all power gone at about 07:27Z on 2026-10-22, open item
        "Time from the network on the board"): the card mounted at the next
        boot, every file of the day read, the last rows 07:27:03Z and 07:27:05Z
        -- within the 10 s flush interval of the cut -- and the boot wrote the
        day's next files, as designed.
      - *A card pulled without `sd_eject`* (phase 4g's image, about 14:26Z):
        found at the next write, 14:27:22Z -- two `diskio_sdmmc: Check status
        failed (0x107)` lines, `recorder: recording stopped state=no_medium
        ... rows_dropped=37`, and `card_writer: no card ... state=no_card`, not
        the `state=lost` this item expected; `log: 28 lines lost while the card
        could not be written`; the card's task's stack low-water mark at its
        lowest yet, 2,048 bytes free of 6,144. The card back in, the minute's
        quiet mount found it at 14:29:57Z and recording went on in the day's
        tenth files. No reboot, nothing else held up.
      **The slowest flush yet, 2026-10-22** (ELF `6c4244ce7...`, the owner's
      YAT log, `card_writer: recording stats`): 270 ms in the second hour of a
      boot, against 160 ms at most over the three days before; the event
      loop's longest post 35 us all the same -- the card's task waited, nothing
      else did.
      Still to see: a day on a PC and in the simulator, the test app.
- [x] **The touch panel on the board (phase 3g).** **Done 2026-10-20**:
      every check below seen, the test app's included; one fault found and
      fixed on the way (the controller's address, below). Built 2026-10-17,
      verified on the host only (`docs/adr/0025-touch-input-gt911.md`): the
      GT911 driver and `port_input` over it against a model written from
      Goodix's documents, the firmware and the test app built. What each
      check is to show, and the lines that show it:
      - *The part*: the boot's `gt911: found address=... product_id=911
        firmware=... attempts=... trigger=... config_checksum_ok=` and
        `configuration (read, never written) ... x_max= y_max=
        touch_number= module_switch1=`; `touch` prints the same. Closes the
        open item "Touch controller part number" with the product ID and
        firmware. The register map these come from is the programming
        guide's, not checked against a legible copy: "911" read back is its
        proof. `attempts` above 1 says the 50 ms settle after the wake pulse
        (`GT911_WAKE_SETTLE_MS`, an ESTIMATE) was short.
      - *The cost of init*: `adp_input: touch ready status=0 took_ms=` --
        about 90 ms with the controller awake, on the touch task since
        2026-10-20 (before that `main: touch status=0 took_ms=`, in front of
        the first frame).
      - *The four corners*: a finger at each corner of the glass, then
        `touch`: its `TOUCH event` lines give each event raw and mapped. The
        mapped ones should be near (0,0), (959,0), (0,539), (959,539). If
        not, correct `BOARD_TOUCH_SWAP_XY` and `_MIRROR_X`/`_MIRROR_Y` in
        `board_config.h` -- now the vendor's choice, swap and Y mirrored --
        and record here what the raw corners were.
      - *A tap*: the diagnostics screen comes up, `event_loop: tap next=
        diagnostics`, then `screen changed screen=diagnostics by=tap
        latency_ms=... action=partial`. The latency is the time from the
        finger's lift to the end of the flush; the panel model predicts
        about 1,080 ms for the change of screen. A second tap goes home; a
        minute untouched goes home too (`by=timeout`).
      - *A finger during a full refresh*: tap while the panel flashes (the
        first frame, or a full refresh after the budget). The tap should
        still switch the screen once the refresh is done; its `latency_ms`
        then includes the wait. EPDiy's render tasks outrank the touch task
        on both cores while they drive the glass.
      - *Sleep, a reset, and init waking it*: `touch_sleep`, then `touch`
        (`ASLEEP`), then `restart`; the next boot's `gt911: found` line
        should come with no error -- `init()` woke it -- and taps work. Then
        `touch_sleep` and `touch_wake` without a reset: `TOUCH_WAKE ok in
        ... ms`.
      - *The bus shared*: while tapping and dragging, the sensors keep their
        once-a-minute `sample` lines and the sensor manager logs no new
        `source failed`; no new `i2c.master` error lines. `touch`'s counts:
        `failed` reads should stay 0, `lifts timed out` too -- a count there
        means the controller's report of a lift was not seen and the 250 ms
        timeout (`GT911_INPUT_LIFT_TIMEOUT_MS`, an ESTIMATE) lifted the
        finger.
      - *The test app*: `idf.py -T hac_platform build` adds `contract
        port_input adp_input+GT911` lines -- the name, the configuration,
        the untouched checks, the finger checks marked not exercised -- and
        a sleep-and-wake case that prints `while asleep the address answers`
        or `is refused`: the host model assumes refused.
      - *For phase 5*: GPIO47, the touch INT, is not one of the S3's RTC
        GPIOs (0-21), so it can wake the chip from light sleep only, not from
        deep sleep -- the vendor's example says the same ("The touch
        interrupt uses non-RTC-IO"). Read, not tried.
      **First run 2026-10-17** (ELF `f63cfdf60...`, the BSEC image, the
      owner's log of 08:06 local, two minutes, six taps):
      - *The part*: as the item "Touch controller part number" records --
        a GT911, `attempts=1` (the 50 ms settle after the wake pulse was
        enough for a controller that was awake; a sleeping one is still to
        try), the interrupt on GPIO47 on the falling edge its configuration
        selects. Init took 86 ms (`main: touch status=0 took_ms=86`), against
        75 estimated.
      - *Taps*: all six switched the screen, home and diagnostics in turn.
        Each reached the event loop 1-2 ms after the finger's lift (the
        decision's `now_ms` against the tap's `up_ms`), and the screen
        changed `latency_ms=1163` to `1173` after it: about 80 ms of
        rendering and diffing on the event loop, then EPDiy's 1,068 ms for
        the 920 x 500 partial refresh (draw 959, diff 99) -- the panel
        model's 1,080 within 1.1 %. The simulator's latency leaves out the
        80 ms of rendering.
      - *The diagnostics screen* redrew once on its own while it was up, an
        88 x 24 px status word (281 ms): it redraws only what changed.
      - *The budget*: each change a partial refresh, as designed -- six taps
        took the count from 5 to 12.
      - *BSEC*: three of the six changes made its call late (430, 660 and
        290 ms against 187): the 1.07 s flush runs on the event loop's task,
        as ADR 0020's "Timing" expects of any long flush. Touch adds such
        flushes only while someone taps.
      - *The bus shared*: no `i2c.master` lines, no `source failed`, the
        sensors' samples on time through the taps -- two minutes only.
      - *The cost*: `free_internal_after_epd_init=201375`, 2,952 under phase
        3f's 204,327 (the static RAM); after the card's mount 176,143, 8,408
        under 3f's 184,551 (with the touch task's 4 KB stack and the queue);
        the event loop's stack low-water mark 3,384 bytes free (3,412 in 3f).
        **The touch task's own: 1,488 bytes free of 4,096**, reached within
        70 ms of boot, before any finger -- its low-water mark's own log line
        (an ESP_LOG with fields, through the log tee) is the deepest thing it
        does. Kept at 4 KB: 1.5 KB of margin.
      - *Not explained yet*: the touch task ran once about 50 ms after its
        interrupt was enabled, before any finger. Read as an edge latched
        while the line was the driver's, an empty read; `touch`'s counts
        (`empty`) will say.
      - *The coordinates*: the six taps landed at (939,81), (154,293),
        (223,259), (133,264), (190,201), (158,318) -- plausible, but where
        the finger was is not recorded: the corner check is still to do.
      Still to see: the four corners with `touch`; a finger during a full
      refresh; `touch_sleep`, `restart` and `init()` waking it;
      `touch_sleep`/`touch_wake`; the diagnostics screen's timeout; the
      test app; an hour of touches beside the sensors.
      **Three days unattended, 2026-10-17 to 20** (the same image, from the
      card's `log/` and `data/`, 2026-10-17 05:06Z to 2026-10-20 12:23Z, and
      the owner's YAT log of 2026-10-20 15:33 local):
      - *Seen working*: the corners -- four taps at (9,23), (948,6),
        (925,513), (53,502) in turn, top left, top right, bottom right,
        bottom left (2026-10-19 17:03Z): the vendor's swap and Y mirror are
        right, `BOARD_TOUCH_*` stay. A finger during a full refresh -- at boot,
        `up_ms=1083` inside the first frame's flush, the change on the glass
        `latency_ms=4440` after it; another inside a budget's full refresh,
        3,404 ms. `touch_sleep` then `touch_wake` 15 s later: `woken
        attempts=1`, taps working at once. The diagnostics screen's timeout,
        twice, 60.0 s after the tap. Taps faster than the screen changes: each
        change waits for the one before (1,395-2,309 ms), the 21st redraw
        after a full one comes out full (`action=full`, 3,402 ms); never
        past the budget. 45 taps in all.
      - *The bus shared*: three days, no `i2c.master` line, no `source
        failed`, no timed-out lift.
      - **A panic, and a boot loop, 2026-10-20 -- the firmware's fault,
        fixed.** The owner plugged a cable in (USB and battery connected,
        a connector or the touch flex disturbed). The touch controller, on
        `VDD3V3` through the flex at `P2`, came back from that with **the
        other address, 0x14** -- the latch at its power-up, which the ESP32
        never saw. The running image panicked (12:23:23Z, no terminal
        attached, no backtrace), and from then on every boot did: `gt911:
        found address=20 (0x14) ... attempts=36`, then a `LoadProhibited` in
        `gpio_install_isr_service()` -> `esp_intr_alloc()`, `EXCVADDR`
        0x00060c23 -- a saved PS register value where an interrupt
        descriptor's pointer should be -- three boots running, until a USB
        reset found it at 0x5D again. The cause: for a second the driver read
        the product ID at 0x5D, a data transfer each time, and ESP-IDF's I2C
        driver logged three error lines for every refusal from inside the
        call -- on app_main's stack, 3,584 bytes, through the log tee's
        `vsnprintf`. It overflowed into the heap, and the next allocation of
        an interrupt met the damage. **Confirmed** by the same image in the
        same state at 16:32 local the same day (the owner's YAT log): a loop
        of eleven boots, the controller still at 0x14 -- an ESP32 reset does
        not reach its latch -- each crashing in `esp_intr_alloc()` after the
        storm of `i2c.master` lines, and one of them stopped first by
        FreeRTOS's own check: `***ERROR*** A stack overflow in task main has
        been detected`. The first panic is most likely the same
        on the touch task's 4 KB (1,408 bytes left at its lowest from its own
        log lines), at the first failed read after the address changed; not
        proven. **Changed** (`docs/adr/0025-touch-input-gt911.md`, "The
        controller's address"): the controller is found by address-only
        probes of both addresses in turn -- a refused probe logs nothing; the
        first init runs on the touch task, not app_main, and the task has 6 KB;
        a failed read looks for the controller again at once, at both
        addresses, and every minute while it is nowhere. To see on the board:
        a boot with the controller at 0x14 (`found address=20`), and the
        touch task's low-water mark after a failed read (`touch`).
      - *The address latch is not stable*: 0x5D at every boot before, 0x14
        after the glitch, 0x5D again after the next. The GT911 latches from
        INT's level as its reset rises, at its own power-up; GPIO47 floats
        then. An option not taken: the S3's pull-down on GPIO47 would make
        it 0x5D whenever the ESP32 is running, for about 70 uA while a
        falling-edge INT idles high. The firmware takes either address now.
      - **The fix on the board, 2026-10-20 16:35 local** (ELF `6f8a91905...`,
        the owner's YAT logs of 16:35): the controller still at 0x14 from the
        glitch, and found there at the first attempt -- `gt911: found
        address=20 ... attempts=1`, `adp_input: touch ready status=0
        took_ms=79`, not one `i2c.master` line from the search, no panic, the
        first frame no longer behind it. Then `restart` (a software reset,
        `previous reset reason=software`): found at 0x14 again at once,
        `took_ms=89`. The touch task's low-water mark 2,288 bytes free of
        6,144 at boot. The one `i2c.master` NACK in the second boot is the
        SCD41's, by design: after a software reset it is still measuring
        (`found_measuring=true`) and refuses the serial number the driver asks
        for first.
      - **Sleep, a reset, and `init()` waking it -- seen 2026-10-20 16:39
        local** (the same image, the owner's YAT log): two taps, 1,164 and
        1,177 ms to the glass; `touch_sleep` (`gt911: asleep: INT held low
        until woken`); `restart` 16 s later. The boot's scan found nothing at
        0x14 or 0x5D (`answered=3`): **asleep, the GT911 refuses its address**
        -- what the host model assumed -- and **an ESP32 reset does not wake
        it**. Then the touch task's init: the wake pulse, `gt911: found
        address=20 ... attempts=1` -- the 50 ms settle is enough after a real
        sleep too -- `touch ready took_ms=90`, and taps working at once.
      - **The test app, 2026-10-20 16:43 local** (ELF `0192cce1e...`, the
        owner's YAT log): `9 Tests 0 Failures 0 Ignored`, twice, 246
        contract lines PASS and none FAIL. `port_input` against the real
        controller: it names itself `911`, nothing to poll while untouched,
        `poll()` never waits; the finger checks say they were not exercised.
        The sleep-and-wake case: `while asleep the address is refused (3)`
        (`HAL_ERR_NOT_FOUND`), the wake 61 ms, its address answering after.
        The first run met the controller at 0x14, the second -- after a
        reset from outside the log, a power cycle by the address -- at 0x5D:
        both paths pass. The touch task's low-water mark 2,508 bytes free of
        6,144.
      - *In passing*, from the same days: IAQ's band changes were 2,378 of
        the 2,489 redraws of 2026-10-18 and 19 (96 %), the flapping at 50
        that ADR 0021 left to watch -- and with them most of those days' 44
        and 75 full refreshes (addressed by ADR 0026: open item "BSEC on the
        board"); BSEC's accuracy reached `high` and moved between `high` and
        `medium` 234 times; 640 late BSEC calls in 3.3 days. Recording:
        151,323 rows, none dropped, the slowest flush 160 ms, the event loop's
        longest post 38 us.
- [ ] Whether `BOARD_BATTERY_ADC_GPIO` (GPIO14, ADC2) gives usable readings
      while Wi-Fi is active. The ESP32-S3 TRM documents ADC2 as unreliable
      under Wi-Fi; confirm on this board before phase 3/4 tries to read
      battery level during a Wi-Fi window, rather than assuming the divider
      alone is the whole story. **Readable since phase 4e**: the console's
      `vbat` reads it 32 times through ESP-IDF's oneshot driver, which
      arbitrates ADC2 with Wi-Fi and refuses what it cannot read -- the open
      item "Wi-Fi on the board" says what to compare.
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
- [x] The `idf.py -T` test app of `docs/architecture.md` §9.2. **Resolved
      2026-10-13, phase 2c** (`docs/adr/0009-on-target-test-app.md`):
      `apps/firmware_esp32/test_app/`, `idf.py -T hac_platform build`; the
      `port_clock` and `port_log` contract lines on the device diff identical
      to the host's. Phase 2a runs
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
      are the figures to re-measure in phases 3 and 4. **Re-measured
      2026-10-13, phase 2d** (I2C bus and serial console added): internal RAM
      free at the top of `app_main()` 366,451 (was 375,259), after EPD init
      **218,039** (was 232,019); PSRAM after EPD init 7,347,640 (was
      7,349,100). So the I2C driver, the `esp_console` REPL (its 4 KB task
      stack, the USB-Serial-JTAG driver, linenoise) and this project's
      adapter and scan cost about **14 KB of internal RAM**, some 9 KB of it
      static. Not itemised further. **Mind the measuring point**: those
      figures came from builds that started the console before the display.
      It now starts last (ADR 0011), so the same log line reads 224,467
      (2026-10-13, ELF `c56d499ce...`) -- the console's ~6.4 KB of runtime
      allocations now land after it. The total is unchanged; compare phase 3
      and 4 against 224,467 at that line.
- [x] Measured refresh durations for each `refresh_mode_t`, and the real
      partial-refresh count at which ghosting becomes visible in the CO₂
      headline region. **Resolved 2026-10-13, phase 2c** -- see
      `docs/adr/0008-panel-timing.md` for the table, the conditions (this
      unit, 21 C) and the fits. In short: clear 2,456 ms; full 3,325 ms
      (`screen_home`) to 3,661 ms (solid content); partial and fast mono
      cost by rows, never by area -- a difference pass over the
      rectangle's rows plus a per-pass cost for the rows EPDiy writes
      (changed rows, plus two per run of unchanged ones); every case
      repeatable to 0.1 %, and the model within about 1 % of the product's
      own refreshes. Ghosting:
      nothing visible from 50 cm through 40 partial refreshes; close up,
      light outlines of earlier digits after 11-25 in the worst realistic
      pattern; budget set to 10. `epd_emulation.h` and `board_config.h` were
      updated together, and `tests/unit/test_panel_limits_agree.c` now keeps
      them so. The history below is how the figures were reached. These replace the estimates in
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
- [x] Whether EPDiy's `epdiy_ED047TC1` waveform produces correct grayscale on
      *this* unit. **Resolved by eye 2026-10-13, phase 2c**: on the
      calibration run's 16-level ramp every neighbouring pair is
      distinguishable except **levels 0 and 1**, which look identical; 14
      and 15 are distinct. Fifteen usable greys, monotonic. Nothing in
      `core/ui/view_model.h` uses level 1, and the simulator now renders 1 as
      0 (`EPD_SIM_DARKEST_MERGED_LEVEL`; since 2026-10-14 through the
      measured lightness table instead, ADR 0013). Not a photometric measurement --
      "identical" means to the eye, in room light. **Narrowed 2026-10-12, not closed.** The concern used to be
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
- [ ] **Residue that survives full refreshes, in greys only -- and how fast
      it fades.** *Since ADR 0035 the full refresh is the vendor's clear,
      which drives each row twice as long in all, and a draw from white: the
      owner saw no trace on the vendor's firmware. Whether this residue is
      gone is that item's ("The vendor's full refresh").* Found 2026-10-13, phase 2c
      (`docs/adr/0008-panel-timing.md`, "Residue that survives a full
      refresh"). Forty partial refreshes that flipped the CO2 headline
      between three and four digits left the "ppm" unit legible on mid-dark
      greys through ten full refreshes and six extra clears; extra clears
      made it fainter, not gone. A second identical run (30 whole-panel
      drives, ~20 minutes, in all) left it visible only from a few
      centimetres: **not permanent**. It is invisible at black and at white
      -- it shows only when a grey is drawn over it -- so `screen_home`'s
      black-on-white hides it, and grey inks drawn over the headline's rows
      would not. `REFRESH_FULL` was left as is. Open: whether time or drives
      erased it, and so whether one full refresh per ten partial ones keeps
      pace in steady use. Next, with the images that exist: run the
      `sdkconfig.calibration_ghost_shift` image to the end, power the board
      off for some hours, then flash the `sdkconfig.calibration_residue`
      image and judge its first, "baseline" ramp -- one full refresh after
      the idle period -- against the phase-2c photographs.
      **The unit no longer moves, 2026-10-16**
      (`docs/adr/0021-measurement-accuracy-and-iaq-levels.md`): "ppm" and the
      CO2 mark sit on the label's line, "CO2 ppm", where no reading puts ink,
      so a partial refresh of the reading never drives their pixels. The
      residue this item describes came from the unit moving with the
      reading's width. Whether residue still gathers elsewhere in the
      headline -- the digits drawn over each other -- is the board's to show.
      **Without partial refreshes too, 2026-10-13**
      (`docs/adr/0012-image-pipeline.md`, "Seen on the panel", "By eye"):
      in the slideshow test app, which drives nothing but `REFRESH_FULL`
      a minute apart, the 1-8 px checkerboards and lines of
      `03_fine_detail` still showed as faint stripes and small squares on
      the greys of the ramp and the dither bands three and four full
      refreshes later, while the 60 px squares shown immediately before the
      ramp left nothing visible. So residue is not only a partial-refresh
      matter, and it seems to follow edges rather than area -- which bears
      on text, and on dithered photographs.
      **Seen again 2026-10-14, the other way round**: the level-match chart,
      fine patterns everywhere, flashed straight after a run of dithered
      photographs, showed local lighter and darker patches across its
      mixes on its first refresh. Three reboots (three full refreshes of
      the same chart) improved it markedly; ten changed nothing further
      that the eye could see. Fine patterns show residue as readily as
      greys do.
- [ ] **How light each grey level really is.** **Table in place
      2026-10-14** (`docs/adr/0013-grey-level-lightness.md`): levels 1-10 as
      measured below, 11-14 estimated; the simulator and `image_gen.py
      prepare` both use it, and photographs dithered towards it at their
      own tones were judged best on the panel. Open: measuring 11-14, and
      the chart's own bias. **Measured under the old full refresh**: GC16
      takes a grey through black and lightens it; since ADR 0035 a full
      refresh darkens it from white (`MODE_EPDIY_WHITE_TO_GL16`), and the
      slideshow and photo frame show nothing else. `06_level_match` again
      under the new full refresh says whether the table still holds.
      Found 2026-10-14 with the
      photo frame (`docs/adr/0012-image-pipeline.md`, "The photo frame"):
      its photographs look overexposed on the panel, though their files are
      not bright -- mean level 6.7 of 15, about 4 % of pixels on levels 14
      and 15, most of them on 2-10 -- and the simulator shows them as
      intended. Both the simulator (`displayed_level()` in
      `platform/host/adp_display_sdl.c`) and `image_gen.py prepare`
      (`LEVEL_VALUE`) assume level L looks like 8-bit grey L x 17; the
      panel evidently shows the middle levels much lighter than that. If
      its levels were even in reflectance rather than in sRGB value, level
      7 would reflect about 47 % against the 18 % assumed, which is the
      size of effect seen. The dark end disagrees with any single curve:
      levels 0 and 1 look identical (the waveform item above), and the
      demo sphere's shadow merges into its black background. So a measured
      table is needed, not a gamma: the relative lightness of each of the
      sixteen levels, judged against dithered black-and-white mixes (which
      blend at viewing distance) or read from a RAW photograph of the ramp.
      It replaces `LEVEL_VALUE` and the simulator's `* 17` together.
      **Measured by eye 2026-10-14** with `06_level_match` (the slideshow's
      demo set, shown alone and static, in room light, from viewing
      distance). The column where each level's patch vanished, converted
      from the chart's labels to the white fraction the 8 x 8 Bayer pattern
      really has (k/64): level 1: 0 %; 2: 4.7 %; 3: 15.6 %; 4: 27.3 %
      ("25-30"); 5: 34.4 %; 6: 43.0 % ("40-45"); 7: 50.0 %; 8: 54.7 %; 9:
      59.4 %; 10: 65.6 %. Levels 4-10 lie within 3 points of L/15 -- the
      panel is close to **even in reflectance** over the middle, so level 7
      reflects about half way between its black and its white, where the
      L x 17 assumption put it at 18 %. That is the photo frame's
      overexposure, explained. The dark end is compressed (1 = 0, 2 = 4.7 %,
      against 6.7 and 13.3 for even steps). **Levels 11-13 could not be
      matched**: in the 70-95 % columns the mixes looked darker than they
      should, even darker towards the right, so no cell matched; level 14
      was matched only by the solid white of the 100 % column. The likely
      cause is the method, not the levels: on e-paper the boundary between
      a black and a white pixel renders grey, so a fine black-and-white mix
      is pulled towards mid-grey, and sparse black dots on white look darker
      than their share. The chart's mixes are trustworthy only where they
      are dense in both colours. To close the light end: a mix of level 10
      and 15 instead of 0 and 15 (lower-contrast boundaries), or a RAW
      photograph of the solid ramp, which has no fine pattern at all.
      **RAW attempt 2026-10-14: inconclusive.** Two pairs of Pixel DNGs
      (linear: identity linearisation curve) of a solid-level chart -- four
      rows of the sixteen levels, alternating direction, inside a black frame;
      not kept, for the reason in (1) -- each with a shot of the all-white
      panel for a flat field. Three things defeated it.
      (1) Light falls off by 40-50 % from the panel's centre to its edges in
      the raw data, and the chart's mirrored layout cannot cancel a
      symmetric fall-off: a level's four places share one horizontal
      distance from the centre, so the fall-off is indistinguishable from
      the level itself. (2) Dividing by the white shot, registered to the
      chart by the case around the panel, left the same level 10-17 points
      apart across its four places and the white bars +-7 %. Likely why: an
      all-white frame is the clear's white -- GC16 writes no row of it --
      while the chart's whites go through GC16's white-to-white flash, the
      "two slightly different whites" item below; and camera flare grows
      with the brighter shot. (3) Averaged anyway, it puts level 7 at 72 %,
      far above both the chart (50) and the photographs judged on the panel,
      which would look overexposed at 72. Not adopted. What would work: every
      grey patch measured against black and white patches beside it in the
      same shot (local normalisation, no flat field, no second shot), with
      the levels spread so each one sits at several distances from the
      centre.
- [ ] **Vertical stripes in white after a full refresh.** *No cause left
      since ADR 0035: the draw after the clear drives nothing that stays
      white. To confirm by eye ("The vendor's full refresh").* Found 2026-10-13,
      phase 2c. In `REFRESH_FULL`, GC16 after `epd_clear()` flashes
      white-to-white pixels (15 passes dark, 15 light -- GL16 does nothing
      for them), but only in columns that contain a changed pixel, because
      EPDiy's column mask is global. White areas therefore come out in two
      slightly different whites, split along content columns: on the grey
      ramp, stripes at the edges of the "15" label running the full height
      of its bar. Cosmetic, and nearly invisible on `screen_home`, whose
      rules span almost every column. Candidate fix: draw the post-clear
      image with `MODE_GL16`, after checking that its greys from white match
      GC16's on this unit -- it changes every full refresh, so it gets its
      own calibration run. **Caution, 2026-10-13
      (`docs/adr/0010-regional-full-refresh.md`):** the white-to-white flash
      this would remove is doing real cleaning. A full refresh of only part
      of the panel, where the empty columns missed it, left them grey and
      kept earlier digits' outlines; a GL16 redraw would take it from every
      white pixel. Any GL16 trial must re-run the ghosting probe, not just
      the grey ramp.
- [ ] **Re-measure the clear from the product's task.** *EPDiy's
      `epd_clear()` is no longer run (ADR 0035); the vendor's clear logs its own
      `took_ms` from wherever it runs, the event loop's task included.* The calibration ran
      on `app_main`'s task (core 0); the product refreshes from the event
      loop's task (core 1, priority 5), where the device run's log puts
      `epd_clear()` at about 2,322 ms against the calibrated 2,456 ms. EPDiy
      runs the clear's push loop in the calling task, so its context shows;
      partial refreshes, rendered on EPDiy's own tasks, do not differ. The
      simulator's full refresh is about 4 % slow for this reason alone
      (`docs/adr/0008-panel-timing.md`, "Task context"). Fix by running
      `epd_calibration_run()` on a task pinned like the event loop's, then
      update `EPD_SIM_CLEAR_REFRESH_MS`. **Data point 2026-10-13**: the
      slideshow test app, flushing from `app_main` (core 0) like the
      calibration, measured the clear at 2,456-2,458 ms on five different
      pictures (`docs/adr/0012-image-pipeline.md`, "Seen on the panel") --
      the calibrated figure exactly, so the context, not the content, is
      what moves it. Still open: the product's own task is not measured.
- [x] **A full refresh of part of the screen. Tried and rejected
      2026-10-13** (`docs/adr/0010-regional-full-refresh.md`, branch
      `experiment/regional-full-refresh`, not merged). It was fast -- 1.17 s
      for the CO2 headline's band against 3.3 s for the whole panel, a
      regional clear costing 502 ms + 3.607 ms per written row -- but the
      refreshed area left grey patches in its empty columns and showed as a
      whiter box against the rest of the screen. The notes below are what
      motivated the attempt. Asked 2026-10-13, not pursued in phase 2c. EPDiy supports it -- `epd_clear_area()` masks
      columns and skips rows outside the rectangle, and GC16 can follow over
      the same rectangle -- and costs by rows, so the CO2 headline's band
      would flash for well under a second instead of the whole panel for
      3.3 s. Two things stand in the way, neither of them the driver: (1)
      `update_policy` keeps one partial-refresh count for the whole panel,
      and a regional full refresh would reset it while the rest of the
      screen keeps accumulating, so the budget would need to become
      per-region -- a core change and an ADR; (2) the rectangle's edge may
      show, by the same mechanism as the white-on-white column stripes
      above. A GC16 pass alone, without the clear, does **not** do the job:
      in this waveform GC16's black-to-black transition is empty (only
      white-to-white flashes), so the undriven dark pixels that make the
      outline ghosting are left as they are. A calibration case timing and
      photographing a headline-band clear would settle (2) cheaply.
- [x] **Panel power: why the first-light build rendered at all.** **Resolved
      2026-10-15 by measurement** ("Power rails and connectors",
      "Measured"): the LT1945's feedback pins sit at its reference, -1.23 V
      and +1.21 V, from the moment the blue LED lights and through every
      idle gap. On this revision the high-voltage rails are not switched by
      `power_disable`/`pos_power_enable`/`neg_power_enable` at all -- the
      resistors that would carry them are not fitted -- but by `PWR_EN`,
      which EPDiy sets at init and never clears. So the first-light build
      rendered because the rails were already up, and on this revision
      `epd_poweron()`/`epd_poweroff()` do not switch the panel's supply,
      whatever else they set. What that means for panel health and idle current is a
      phase-5 question, opened as its own item below. The history follows.
      The build
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
      **Explained by the schematic 2026-10-13, not yet measured** ("Power
      rails and connectors", "The 74HCT4094"): on v2.4 the resistors that
      would carry `power_disable`/`pos_power_enable`/`neg_power_enable` to
      the supply are not fitted, and the LT1945 boost's shutdown pins are
      pulled up to the switched `3V3`, which `PWR_EN` -- EPDiy's
      `ep_scan_direction`, never cleared -- holds up. So the rails come up
      with the first register write and never go down while the firmware
      runs. The measurement that confirms it, between refreshes, on the
      LT1945's pins 1-5 only: pin 1 (`NFB1`) about -1.23 V and pin 5
      (`FB2`) about +1.23 V while the converters regulate (dividers
      560k/39k for -20 V and 2M/120k for +22 V), pins 2 and 4 (`SHDN1`/
      `SHDN2`) about 3.3 V; all near 0 V when off. Keep off pins 6-10: two
      are switch nodes, and `VIN` sits between the two `PGND` pins. Turning
      the rails off between refreshes would mean clearing `PWR_EN`, which
      also cuts the sensors -- a phase-5 decision, not a driver setting.
- [ ] **The panel's high-voltage rails stay up between refreshes (phase
      5).** Measured 2026-10-15 (item above): +22 V, -20 V and the ±15 V
      derived from them run as long as `PWR_EN` is set, which with EPDiy is
      from the first register write to the next reset. This is the vendor's
      design too -- EPDiy keeps the bit set "as in LILYGO's driver" -- so it
      is not something this firmware introduced. Two things are unknown:
      what it costs in idle current (the LT1945 and the two linear
      regulators run continuously; a current reading at the USB input with
      and without `PWR_EN` answers it), and whether holding the supply and
      VCOM up while the panel is not driven matters for its life. Switching
      it off between refreshes means clearing `PWR_EN`, which also cuts the
      sensors' `3V3`, so it is the same decision as the sensors' power in
      deep sleep ("The sensor wiring this implies").
- [ ] `update_policy`'s status trigger can fire on `charging` alone, which
      `screen_home` does not draw yet, making the empty-diff skip in
      `event_loop.c`'s `redraw()` (`apps/firmware_esp32/main/event_loop.c`)
      repeat every tick with no visible change -- wasteful, and **not quite
      harmless** (found 2026-10-21, ADR 0031): each skip counts as a redraw
      that made no progress, so the loop's backoff grows -- 1 s doubling to
      60 s -- and `redraw_not_before_ms` holds every redraw meanwhile, a
      tap's change of screen included. The diagnostics screen's first
      network version hit exactly this in the simulator, which retries every
      second without a backoff; on the board a tap would have waited up to a
      minute. Whatever a screen redraws for, it must draw; an empty diff
      that is not a failed flush might better be taken as the frame already
      on the glass than as a failure.
      Unreachable until the battery adapter lands in phase 5, which should
      draw it in the bar or take it out of the trigger. **`telemetry_connected`
      is gone since phase 4d** (`docs/adr/0031-the-network-on-the-screen.md`):
      the trigger fires on `air_reading_t.link`, the link the bar draws,
      held a minute, and no longer on the state message's `net_connected`,
      which nothing draws.
- [ ] **The network on the screen (phase 4d).** Built 2026-10-21, verified on
      the host only (`docs/adr/0031-the-network-on-the-screen.md`). Phase 4e
      brings the Wi-Fi adapter, not yet the reporter: what follows waits for
      the MQTT adapter too -- **there since phase 4g**, in an image with a
      broker in `sdkconfig.credentials` (open item "MQTT on the board"). **What
      the board shows without a broker**, with no reporter
      composed: no word where the bar
      said "offline", the battery text at the right margin instead; on the
      diagnostics screen a smaller title and two network lines, `Wi-Fi off`
      and `MQTT off` (the boot's full refresh draws the new layout, so the
      battery's move leaves nothing behind). **What to see once the Wi-Fi and
      MQTT adapters run**, and the lines that show it:
      - *The first word*: nothing in the bar's place at boot, then `online`
        about a minute after the session came -- `event_loop: link on the
        screen shown=unknown` at the first poll, `shown=online` some 60 s
        after `reporter: session up`, and a `decision ... reason=status` (or
        a redraw for something else) within a sensor cycle of it.
      - *An outage*: the access point switched off for two minutes -- `shown=
        no-wifi` 60 s after the Wi-Fi adapter's loss, `shown=online` 60 s after
        the next `session up`; for one of 30 s, no `link on the screen` line
        at all.
      - *A broker gone* (Home Assistant's mosquitto add-on stopped): `shown=
        no-broker` a minute after `session lost ... why=dropped`, while the
        Wi-Fi stays up.
      - *The hold's estimate* (`LINK_HOLD_DEFAULT_MS`, 60 s): from the card's
        `log/`, how long the board's own Wi-Fi drops and broker reconnections
        last, and how many `link on the screen` lines a day they make. Many
        drops just past a minute would argue for a longer hold; nothing but
        real outages, for the minute.
      - *The diagnostics screen*: the SSID as configured, the signal, the
        address the router gave (`Wi-Fi up <ssid> -NN dBm a.b.c.d`), `MQTT
        online sent N refused 0 last HH:MM`; the counts as of the tap,
        unchanged while the screen stays up.
      - *The test app*: the `port_net` contract against the Wi-Fi adapter,
        `info()`'s checks included -- the name terminated and the same up and
        down, an address only while up -- with the cut and restore checks "not
        exercised".
      **The Wi-Fi gone twice, 2026-10-23** (an image of `e44fbfc`, BSEC, from
      the card's `log/`, the owner's test of 11:07-11:16Z; the access point came
      back on another channel and gave new addresses, so it had restarted):
      every change of the bar's word came a minute after the link's, to the
      millisecond -- `no-wifi` 60.0 s after each loss (11:07:17, 11:12:31),
      `no-broker` 60.0 s after the Wi-Fi's return (11:10:17) while the broker
      refused every CONNECT, `online` 60.0 s after the session (11:14:59); six
      `link on the screen` lines in all, the boot's two included. On the
      diagnostics screen, where the owner kept tapping, the held link drew
      nothing, as ADR 0034 has it; the next home screen showed it.
- [ ] **Wi-Fi on the board (phase 4e).** Built 2026-10-21, verified on the
      host only (`docs/adr/0032-wifi-on-the-board.md`): the station behind
      `port_net` (`platform/esp32_t5s3/adp_net.c`), its decisions tested on
      the host, the firmware and the test app built. **Needs credentials**:
      copy `apps/firmware_esp32/sdkconfig.credentials.example` to
      `sdkconfig.credentials`, fill in both lines, delete the build's sdkconfig
      and reconfigure (the build refuses a stale one); `grep CONFIG_HAC_WIFI_SSID
      sdkconfig` names the network. Without them the image is the firmware of
      before -- `main: Wi-Fi off: no network in this image` -- and links no
      Wi-Fi at all. Nothing uses the link yet, and the screen shows nothing of
      it (no reporter composed: the bar stays empty, the diagnostics screen
      says `Wi-Fi off`); the console's `wifi` is the view. What each check is
      to show, and the lines that show it:
      - *A boot*: `main: device device_id=hac-<12 hex digits>`; `adp_net:
        station ready, the radio off until up() ssid=... password_len=...
        took_ms=... free_internal=...` before the first frame; `event_loop:
        first frame over now_ms=...` after it, then `adp_net: asked up:
        starting the radio`, `radio started esp_err=0 took_ms=...` (the RF
        calibration; at the first boot of this image also the PHY data's flash
        write), `associated bssid=... channel=...`, `up address=... gateway=...
        rssi=... took_ms=...`, and once `memory with the link up
        free_internal=... min_free_internal=...`. The router's client list
        names the device `hac-...`.
      - *Many boots on USB alone*: no `previous reset reason=BROWNOUT`, no
        `interrupt_watchdog`, through the radio's start in particular -- its
        RF calibration comes seconds after `PWR_EN`'s rise and between two
        panel drives by design. A reset there says the radio's current reaches
        `VDD3V3` as the rail's inrush did (open item "A reboot loop when the
        switched 3V3 comes up"): the levers then are the radio's transmit power
        and the supply, not the firmware's order. Also a few with the card in
        and the battery connected.
      - *Recovery*: the access point rebooted -- `link lost: sought again at
        once reason=...`, `attempt failed reason=no_ap_found ...` while it is
        gone, `up ... failed_attempts=N took_ms=...` within about 30 s of its
        return (`ADP_NET_BACKOFF_MAX_MS`, a CHOICE). The access point gone for
        an hour: the same, with the backoff at its ceiling and the failures
        logged once (and again only when their reason changes); `wifi` shows
        the count and the next attempt. A wrong password, if tried: `attempt
        refused: is the password right?` from the second, never a give-up.
      - *Memory*: `memory with the link up free_internal=` against phase 3g's
        176,143 after the card's mount (the build added 51,140 bytes of static
        internal RAM; the run-time share is an ESTIMATE of 40-60 KB more), the
        event loop's and the station task's `stack low-water mark` lines
        (`adp_net`, 6,144 bytes), and `idf.py size-components` of the image
        flashed (ADR 0032 has the build's).
      - *The panel*: EPDiy's per-update timing lines with Wi-Fi up against
        ADR 0008's table -- a full refresh 3.2-3.7 s, a partial by its rows --
        and the glass by eye, greys especially: Wi-Fi's interrupts on core 0
        can delay the two completion interrupts EPDiy's row loop on core 1
        waits for, which stretches a frame, not a row's drive.
      - *BSEC*: `BSEC called late` lines per day from the card's `log/`,
        against a day with the same image and no `sdkconfig.credentials`, the
        board otherwise alike.
      - *Heating*: the BME68x's temperature against the SCD41's, from a day's
        recordings (`data/<day>_bme68x.csv`, `_scd4x.csv`) with Wi-Fi and a
        day without -- the heating requirements 12 warns of, and
        `HAC_BSEC_DEVICE_HEAT_C`'s first measured value (open item "BSEC on the
        board", *Temperature*).
      - *GPIO14 with Wi-Fi on* (closes the open item on `BOARD_BATTERY_ADC_GPIO`):
        `vbat` at the console with Wi-Fi up and with an image without it --
        `VBAT ... N reads, N ok, 0 refused` and the battery's millivolts
        steady says ADC2 reads beside Wi-Fi; refusals or a wandering value say
        it does not. Battery connected, and with USB alone.
      - *The test app*: with the overlay in place, `idf.py -T hac_platform
        build` from a fresh `test_app/sdkconfig` adds `contract port_net
        adp_net+ESP-IDF station` lines, every one PASS -- five of them "not
        exercised", which need the access point taken away -- and a summary
        line with the connections and the stack;
        without it the case is IGNORED and says why.
      **First run 2026-10-21** (ELF `4734936f1...`, the BSEC image with the
      owner's network, a WPA3-SAE access point on channel 11; the owner's YAT
      logs of 20:19 and 20:43 local -- a boot after flashing, 24 minutes up,
      then `restart` and a second boot):
      - *Boots*: both ran through, no brownout, no interrupt watchdog --
        `previous reset reason=usb`, then `=software`. Two boots, not yet the
        many the item asks for.
      - *The radio's start*: `first frame over` at 4.27 and 4.32 s, `radio
        started` 40-60 ms later (62 ms at the first boot, which calibrated in
        full -- `phy_init: failed to load RF calibration data`, then `Saving
        new calibration data` -- and 43 ms at the second, from the data NVS
        now held: the partition opens, and is used). The next panel drive came
        5 s after, as designed.
      - *Joining*: `up` 6,867 and 6,699 ms after `asked up`, at the first
        attempt both times: the all-channel scan about 2.4 s, authentication
        and association 0.95 s, DHCP 3.3 s. RSSI -62 to -72 dBm. DHCP was
        told the hostname `hac-...`.
      - *Memory*: `memory with the link up free_internal=56703` and `56711`,
        `min_free_internal=42691` and `44371`. Against phase 3g's 176,143
        after the card's mount: 119,440 bytes less -- the build's 51,140 of
        static RAM and **68 KB at run time**, above ADR 0032's estimate of
        40-60: about 54 KB of it taken by the station's set-up before the
        radio starts (`card mounted free_internal=70939` came after it at the
        second boot), 13 KB by the start, the least free 13 KB below that.
        `free_internal_after_epd_init=143935` (201,375 in 3g). The event loop's
        stack low-water mark unchanged (3,368 bytes free); the station task's
        2,644 bytes free of 6,144 at its lowest, the card's 2,560. Before
        MQTT is added, ADR 0032's levers are worth weighing: the Wi-Fi IRAM
        optimisation off (~30 KB back), fewer static receive buffers.
      - *The panel*: EPDiy's draw times with the radio on are the old ones to
        the millisecond -- a 54-row partial 321-322 ms (322-323 before), a
        381-row one 564-566 (563-568), the 916 x 440 status redraw that ran
        during the association and DHCP 699 (699-701), the boot's full GC16
        draw at 25 C 911 (913) -- so Wi-Fi's interrupts do not measurably
        stretch the row output. The tap's 920 x 514 change of screen 1,136 ms,
        latency 1,231-1,244 ms. The glass by eye: not reported yet.
      - *BSEC*: late calls 437-439 ms at the boot's full refresh, 600 at a
        status redraw, 790 at a change of screen -- the pattern of before; the
        per-day count is still to compare.
      - *GPIO14*: `vbat` with Wi-Fi up (idle, modem sleep), the battery
        connected: 32 reads, 32 ok, none refused, raw 2,456-2,469, the pin
        2,106-2,117 mV, the battery 4,224 mV. ADC2 reads beside an idle link;
        under traffic, with Wi-Fi off, and on USB alone are still to see.
      - *Restart*: `restart` stops Wi-Fi in ESP-IDF's shutdown handlers, so
        the last lines before the reset read `esp_wifi_connect refused ...
        esp_err=12290` (`ESP_ERR_WIFI_NOT_STARTED`) and `link lost: sought
        again at once reason=assoc_leave` -- the adapter seeking the link the
        shutdown took. Harmless, and misleading in a log.
      - *The screen*: nothing of the link, as designed -- `Wi-Fi off` on the
        diagnostics screen while the station is up (ADR 0032).
      Still to see: many boots, the access point rebooted and gone, the glass
      by eye, a day's late BSEC calls and the heating against a day without
      Wi-Fi, `vbat` under traffic and without Wi-Fi, the test app.
      **The heating, a first comparison from the card** (2026-10-22): the
      BME680's BSEC temperature minus the SCD41's, minute by minute in the
      night's quiet hours up to 04:00Z (07:00 local), was 0.37 C (sd 0.12) from
      the boot at 22:04Z on 2026-10-21, with Wi-Fi, and 0.51 and 0.52 C (sd
      0.05) from 19:00Z on 2026-10-17 and 18, without: Wi-Fi does not warm the
      BME680 relative to the SCD41. Both sit by the same board, so what it does
      to both is the reference's to show.
      **Changed the same day** (ADR 0032): the Wi-Fi IRAM optimisations off,
      17,564 bytes of static internal RAM back by `idf.py size` -- the next
      image's `memory with the link up` should read about 74 KB free and 60 KB
      at the least, and the link come up as before (6-7 s; a slower driver
      would show there first) -- and a `restart` that no longer logs a lost
      link: the last line before the ROM's is `RESTART esp_restart() now`, or
      ESP-IDF's own Wi-Fi lines, but no `adp_net` one.
      **The test app, 2026-10-21** (ELF `93d6c0b1f...`, the BSEC variant with
      the overlay, the owner's YAT log of 21:32): `11 Tests 1 Failures` -- every
      other case PASS, and in the `port_net` contract every check but one.
      `up() returns without waiting` FAILED on the first `up()`: the test task
      (priority 1) shares core 0 with the station's task (4), which preempted
      it the moment it was woken and started the radio -- `radio started
      took_ms=39`, the RF calibration's CPU -- before `up()` returned; the second
      `up()`, whose start took 5 ms, passed. The runner's fault, not the port's:
      the firmware's caller is the event loop, above the station's task and on
      the other core. The runner now runs the contract at the event loop's
      priority (`tests/contract/esp32/test_port_net_contract.c`); the next run
      is to show all PASS. Also seen: the first attempt refused (`auth_fail`,
      202) and the second failed (`connection_fail`, 205) a minute after the
      firmware's session ended with esptool's reset and no deauthentication --
      the access point still holding the old association -- and the third
      joined, 14.8 s after `up()`: the backoff at work, and the case for never
      giving up on a refusal (ADR 0032). The stack: 2,664 bytes free of 6,144.
      **Run again with the runner's fix** (the image built for it, `0ef8e316c...`
      -- the log begins after the boot's hash line; the owner's YAT log of
      21:37): `11 Tests 0 Failures 0 Ignored`, `HAC_TEST_APP_RESULT ... OK`;
      every one of the 29 `contract port_net` lines PASS, five of them "not
      exercised" -- `up() returns without waiting` now printed before the
      station's `asked up`, the caller back before the task ran. Both joins at
      the first attempt, 6,580 and 6,521 ms after `up()`; the radio's first
      start 39 ms, the second 4; the station task's stack 3,016 bytes free at
      its lowest. The test app's checks for Wi-Fi are done.
      **The image with the IRAM optimisations off and the shutdown handler**
      (ELF `8baab9290...`, the owner's YAT log of 21:41: a boot after flashing,
      then two `restart`s): `memory with the link up free_internal=74419`,
      `74231`, `74231`, `min_free_internal=62035`, `60159`, `60159` -- the
      17.6 KB the build gave back, as predicted;
      `free_internal_after_epd_init=161599` (143,935 before). The link up at
      the first attempt in 6,786-6,866 ms, as with the optimisations on: the
      slower driver does not show. Each `restart`: `RESTART esp_restart()
      now`, ESP-IDF's own Wi-Fi lines, the ROM's -- no `adp_net` line. No
      brownout, no watchdog in the three boots (five with Wi-Fi so far). EPDiy's
      draw times as before (322-323, 699-700, 910 ms); `vbat` 32 of 32 again,
      4,222-4,224 mV.
      **The access point gone and back, twice, 2026-10-23** (an image of
      `e44fbfc`, BSEC, from the card's `log/`, the owner's test, 11:07-11:15Z):
      - *The loss*: `bcn_timeout`, ESP-IDF's probes for 2.5 s, then `link lost:
        sought again at once reason=beacon_timeout`; then `no_ap_found`, and the
        failures logged once while their reason held, as designed.
      - *The return*: the first attempt that met the access point again was
        refused with **reason 49, `WIFI_REASON_INVALID_PMKID`** in ESP-IDF's
        enum (`attempt failed reason=other code=49`): the station offered the
        WPA3-SAE PMK it had cached, and the restarted access point knew it no
        more. The next attempt, 30 s later at the backoff's ceiling, failed as
        `connection_fail` with no signal reported; the one after joined, with a
        full SAE handshake. The same three steps both times: the link back 65 s
        after the access point first answered (11:09:08 -> 11:10:13, 11:13:49
        -> 11:14:55), against ADR 0032's "within about 30 s of its return";
        `up ... failed_attempts=9 took_ms=179478`, then `failed_attempts=8
        took_ms=147748`. A candidate, not made: an attempt that met the access
        point -- reason 49 among them -- resets the backoff to its first step,
        and `adp_net_pure.c` names 49 so the log says what it is. Whether it
        gains the 30 s is not established: the `connection_fail` between may be
        the access point's own start. The owner's to decide.
      - *What else came back different*: the channel (11, then 6) and the
        address (`.140`, then `.109`, then `.113`) -- the restarted access
        point's DHCP had forgotten the lease. The broker's address is fixed in
        the image (ADR 0033); reserving addresses in the router for the board
        and the Pi would keep both.
      - *Measuring went on*: the sensors' minute samples on time throughout,
        taps answered, no reset.
- [ ] **Time from the network on the board (phase 4f).** Built 2026-10-21
      (`docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md`,
      "Amendment (phase 4f): time from the network"): SNTP from Wi-Fi's first
      address, `CONFIG_HAC_SNTP_SERVER` (`pool.ntp.org`), an answer an hour;
      each answer sets the system time, and the event loop then measures the
      RTC by its alarm's tick on INT -- IO9, stamped in an interrupt
      (`platform/esp32_t5s3/board_rtc_int.c`) -- logs its offset and drift,
      writes it to a second's start and measures where it landed
      (`apps/firmware_esp32/main/net_time.c`, `rtc_sync.c`). Needs an image with
      a network (`sdkconfig.credentials`); without one the boot has no
      `net_time` line and `time` says `TIME_SNTP off: no network in this
      image`. What each check is to show, and the lines that show it:
      - ~~*INT on IO9 at all*~~ -- **seen 2026-10-22**, below. The wiring is
        the schematic's, read, never measured: at boot `board_rtc_int: the RTC's INT interrupting on its
        falling edge gpio=9 level=high esp_err=0` (high: pulled up, no alarm
        due); within a minute of the first answer `rtc against sntp
        rtc_minus_sntp_ms=...`, not `rtc against sntp: nothing to compare
        why=no INT edge`. `time`'s first `TIME_SNTP` line counts the edges --
        two a sync. No edges at all says INT does not reach IO9: the RTC is
        still written, unmeasured, and the drift is not.
      - *A boot*: `net_time: waiting for the network's first address
        server=pool.ntp.org interval_s=3600 rtc_alarm_off=0` before the first
        frame; after `adp_net: up address=...`, `net_time: sntp started ...
        esp_err=0`; within lwIP's random start delay (5 s at most,
        `CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY`) and a DNS lookup, `synced
        utc=...Z system_minus_sntp_ms=... source_was=rtc syncs=1`; at the next
        minute boundary `rtc against sntp ... since_write_s=- drift_ppm=-`; a
        second or so later `rtc written utc=...Z first_tick_ms=508
        start_late_ms=0 stopped_ms=... rearms=0 start_window_ms=...`; at the
        minute boundary after, `rtc landed residual_ms=... uncertainty_ms=2
        first_tick_ms=... learned=true`. `time`: `source="sntp"`, the
        `TIME_SNTP` lines.
      - *The first landing, and the delay learned*: with the datasheet's 508
        ms the first write after a boot lands `residual_ms` off by the
        difference between this chip's delay and the datasheet's -- -481 on
        2026-10-21, below -- and `first_tick_ms` says this chip's. The second
        sync's `rtc written` carries that `first_tick_ms`, and its `rtc landed`
        a residual within ±12 ms -- this chip's delay varies 986-1,000 ms from
        one start to the next (seen 2026-10-22, below). No learning
        (`learned=false`), or a residual far outside that, is the thing to
        report.
      - *The RTC chip itself*: its marking, read off the board (the chip
        beside the backup cell). A PCF8563 first ticks 0.508 s after a start
        by its datasheet; this one takes a whole second (1,000 ms by its INT,
        2026-10-22) -- a look-alike (BM8563, HYM8563 and their kin) would
        explain it. If the second holds, it can be the default instead of the
        datasheet's, so that a boot's first write lands too (ADR 0023's
        amendment, "Not decided here").
      - *A cold boot with nothing in the RTC*: `rtc_stop` (`TIME_RTC_STOP
        ok`), then all power removed -- USB and battery -- and restored, with no
        `time_set` after. The boot: `main: wall clock source=none rtc=rtc:
        stopped`, the footer `updated --:--`; at the first answer `synced ...
        system_minus_sntp_ms=none source_was=none`, `rtc against sntp: nothing
        to compare why=rtc: stopped`, `rtc written ...`, `rtc landed ...`; the
        footer shows the right local time at the next poll, a `reason=status`
        redraw within a sensor cycle (ADR 0023, "One new trigger"). Then
        `restart`: `wall clock source=rtc rtc=rtc: valid`. The power-on boot
        itself is not in the terminal's log -- the terminal reopening the port
        resets the chip again (`reason=usb`), as in phase 3e -- and the logged
        boot after it shows the above only if that reset came before the first
        answer, some 15 to 25 s in; later, and the unlogged boot has already
        written the RTC, and the logged one says `source=rtc`. The card's
        `log/` has the unlogged boot's lines either way, with the card in.
      - *A cold boot with the RTC set*: all power off for a while and back,
        nothing typed. The boot takes the RTC's time (`source=rtc`), and the
        first answer's `rtc_minus_sntp_ms` says whether the RTC counted through
        the cut: its drift since the last write if it did, the outage's length
        behind if its oscillator stopped. The cell's capacity for hours of it
        is the item "The RTC on the board"'s.
      - *Every hour*: `synced` 3600 s apart, each followed a minute or so later
        by `rtc against sntp ... since_write_s=36xx drift_ppm=...
        drift_uncertainty_ppm=1.1`, then `rtc written` and `rtc landed`. From a
        day of the card's `log/` (`grep "rtc against sntp"`): the drift per
        hour, its mean and spread -- this unit's figure, replacing ADR 0023's
        ESTIMATE of ±20 ppm and closing the item "The RTC on the board"'s
        *Drift*. The spread beyond the ticks' 1 ppm is the network's: lwIP takes
        the server's time without a round-trip correction. `system_minus_sntp_ms`
        hour by hour is the ESP32's own clock's drift over an hour, a figure for
        phase 5. `rearms=0` and `start_late_ms` 0 or 1; a re-arm says the event
        loop was held -- a full refresh -- at the start's moment.
      - *An outage*: the access point off for over an hour. No `synced` lines
        meanwhile; once the link is back the next answer comes within lwIP's
        retry ceiling, 150 s, with `since_write_s` the whole outage and the
        drift over it.
      - *The cost*: `adp_net: memory with the link up free_internal=` against
        74,419 with the same network and no SNTP (ADR 0032) -- an ESTIMATE of
        about 2 KB less (lwIP's DNS client, `net_time`'s state); the event
        loop's `stack low-water mark` against 3,368 bytes free, now with the
        sync's steps and log lines on it.
      - *The bus and BSEC*: no new `i2c.master` lines and no `source failed`
        around a sync, and no `BSEC called late` at the write -- its spin is 30
        ms at most.
      - *The autumn change*, if the board runs through 2026-10-25 01:00 UTC:
        the footer's next redraw after it shows the hour step, 03:59 -> 03:00
        in Vilnius; the `synced` lines, in UTC, step nothing.
      - Not provoked: `time refused` -- a server's time before 2026 or from
        2100 on -- which only a broken or unsynchronised server sends.
      **The first version on the board, 2026-10-21** (ELF `9fb8fd2b5...`, the
      BSEC image, the owner's YAT log of 23:41 local, 14 minutes; the version
      with a task of its own that polled the RTC for its tick, replaced the
      same night -- ADR 0023's amendment, "The first version"):
      - *The boot*: one brownout first, after the USB reset that started the
        log, at the old place (`epdiy-i80: i80 row output ready`, then `BOD:
        Brownout detector was triggered`; open item "A reboot loop when the
        switched 3V3 comes up"); the next boot ran. Wi-Fi up at 20.2 s, at the
        third attempt (`failed_attempts=2 took_ms=15243`).
      - *The answer*: `sntp started` at 20.2 s, `synced
        utc=2026-10-21T20:41:53.885Z` 4.4 s later, `system_minus_sntp_ms=-13148
        source_was=rtc`: the boot's time, from the RTC, was 13.1 s behind.
      - *The RTC's drift since 2026-10-16*: `rtc_minus_sntp_ms=-12918
        uncertainty_ms=1` -- 12.9 s behind, the 0.23 s from the system time's
        figure the boot's truncation. Since `time_set 2026-10-16T17:35:40Z`
        (above, "The RTC on the board"), if nothing wrote it in between,
        443,175 s: **-29 ppm**, ±3 for the hand-typed second -- outside the ±20
        ppm ESTIMATE, some 105 ms an hour.
      - *The landing*: `rtc written utc=2026-10-21T20:41:56.000Z
        residual_ms=-481 uncertainty_ms=2 stopped_ms=45 start_late_ms=0
        rearms=0` -- started on time, the first tick came about 0.989 s after
        the start, not the datasheet's 0.508. One sample; what made the design
        measure the landing and learn the delay.
      - *The cost of the task*: `memory with the link up free_internal=66015
        min_free_internal=58711`, 8.4 KB under phase 4e's last image; its stack
        3,156 bytes used of 6,144. The task is gone.
      - *Unchanged*: EPDiy's draw times; BSEC late 530 ms at a status redraw and
        200 ms at a change of screen, as before.
      **The final version on the board, 2026-10-22** (ELF `59c002862...`, the
      BSEC image, the owner's YAT log of 00:38 local, 2.5 minutes -- one sync):
      - *The boot*: one brownout first, at the old place, as the evening
        before; the next boot ran. `board_rtc_int: the RTC's INT interrupting
        on its falling edge gpio=9 level=high esp_err=0` -- the pin pulled up,
        no alarm left armed (`rtc_alarm_off=0`).
      - *INT on IO9 works*: `rtc against sntp rtc_minus_sntp_ms=-554
        uncertainty_ms=2` at the minute boundary after the answer, and `rtc
        landed` at the one after -- both edges stamped, so the wiring is as the
        schematic says.
      - *The write*: `rtc written utc=2026-10-21T21:40:02.000Z first_tick_ms=508
        start_late_ms=0 stopped_ms=21 rearms=0 start_window_ms=0` -- on time,
        the clock stopped 21 ms.
      - *The landing*: `rtc landed residual_ms=-492 uncertainty_ms=2
        first_tick_ms=1000 learned=true` -- this chip's first tick comes a whole
        second after a start, to the 2 ms the edges are read to; the 989 ms of
        the evening before was the polling version's, its start's stamp taken
        before the transfer's wait. Learned: the next sync's write is planned
        with 1,000 ms. Not yet seen: that sync, an hour later.
      - *The drift over the hour between the two images*: the first version
        left the RTC 481 ms behind at 20:41:57.5Z; at 21:40:00.6Z it was 554
        behind -- 73 ms in 3,483 s, **-21 ppm** (±1.2), against the -29 ppm
        over the five days before. The temperature, or that estimate's
        assumptions (nothing writing the RTC, the hand-typed second); a day's
        hourly samples will tell.
      - *The answer*: `synced ... system_minus_sntp_ms=-837 source_was=rtc` --
        the boot's time, from the RTC, 0.84 s behind: the RTC's 0.55 s and the
        boot's truncation.
      - *Memory*: `memory with the link up free_internal=72503
        min_free_internal=63811`, 6.5 KB above the first version with its task,
        1.9 KB under phase 4e's last image -- as estimated.
      - *One error line at every boot*, from the touch adapter: `E gpio:
        gpio_install_isr_service(526): GPIO isr service already installed` --
        the RTC's INT had installed the shared service first. Harmless, and
        removed after this log: the service is installed once, on app_main
        before either asks (`platform/esp32_t5s3/board_gpio_isr.c`); not yet seen
        on the board.
      - *Unchanged*: EPDiy's draw times; BSEC late 449 ms at the boot's full
        refresh and 530 at a status redraw, none at the write.
      **A night of hourly syncs, 2026-10-22** (ELF `ec5d8a60f...`, the BSEC
      image with the GPIO ISR service installed once; the owner's YAT logs of
      01:03 local -- the boot and its first sync -- and 01:10-07:52, 6.7 hours
      of the same boot, syncs 2 to 7; syncs 8 to 10 from the card's `log/`,
      which the owner copied off the card the same afternoon -- the boot ran
      until the power went at 07:27Z):

      | Sync (UTC) | `drift_ppm` (±1.13) | before the write | landed | first tick |
      |---|---|---|---|---|
      | 22:05 (boot) | -- | -494 ms | -479 ms | 987 ms |
      | 23:05 | -21.19 | -554 | -8 | 994 |
      | 00:05 | -20.90 | -82 | -5 | 999 |
      | 01:05 | -18.08 | -69 | +8 | 991 |
      | 02:05 | -23.16 | -74 | +3 | 987 |
      | 03:05 | -20.90 | -71 | -9 | 995 |
      | 04:05 | -20.62 | -82 | -5 | 1000 |
      | 05:05 | -17.23 | -66 | +12 | 988 |
      | 06:05 | -22.60 | -68 | -7 | 994 |
      | 07:05 | -22.88 | -88 | +4 | 989 |

      - **This unit's RTC loses 20.8 ppm** -- the mean of nine hours, spread
        ±2.0 ppm hour to hour (±0.7 on the mean): 75 ms an hour, 1.8 s a day,
        about 54 s in a 30-day month. ADR 0023's ESTIMATE of ±20 ppm holds; the
        -29 ppm over the five days before was that estimate's own assumptions
        (a hand-typed `time_set`, nothing writing the RTC since). One night,
        indoors, the room at 23-24 C; a day and other temperatures are still to
        see.
      - **The learned delay lands every write within ±12 ms** of the second's
        start -- not the ±1-2 the design first claimed. The delay from a start
        to the first tick is not one figure: 987-1,000 ms over these ten
        writes (and 986-994 at the boots of the afternoon, below), so each
        write lands off by the difference between its delay and the one before.
        Every landing is measured, so the drift is unaffected.
      - *On time throughout*: `rearms=0`, `start_late_ms` 0 or 1, the clock
        stopped 21-30 ms.
      - *The ESP32's own clock*: `system_minus_sntp_ms` -2, -1, +8, -8, -1, -1
        ms an hour -- within ±2 ppm, and within SNTP's own noise.
      - *Nothing else*: no warning, no error, no `BSEC called late` in the 6.7
        hours of the terminal's log (five in the card's 9.4 hours, all after
        05Z, when the room woke and the redraws came: open item "BSEC on the
        board"); 11 redraws, all partial; the touch interrupt on (`adp_input:
        interrupt on gpio=47 ... esp_err=0`) and **no `E gpio: ... GPIO isr
        service already installed` line**: the fix holds.
      **Two boots of the afternoon, 2026-10-22** (ELF `6211314dd...`, phase 4g's
      image; the owner's YAT logs of 16:57 and 17:01 local, the open item "MQTT
      on the board" has their MQTT): each boot's first sync wrote with the
      datasheet's 508 ms and landed half a second behind (`residual_ms=-485
      first_tick_ms=993 learned=true` at 13:59Z) -- the learned delay lives in
      RAM, as designed; no `E gpio` line in either boot; the one `i2c.master`
      NACK after the `restart` is the SCD41's, still measuring. From that
      landing to the next boot's `rtc_minus_sntp_ms=-508` 661 s later: -23 ms,
      -35 ppm ±6 -- a span too short against SNTP's few milliseconds of noise
      to say more than "about the same".
      **A cold boot with the RTC set, from the card, 2026-10-22**: all power gone
      from about 07:27Z -- the last rows on the card, flushed every 10 s -- to
      13:56:11Z, 6 h 29 min (`previous reset reason=power_on`,
      `kept_across_reset=false`: the system time lost with it). The boot read
      `wall clock source=rtc rtc=rtc: valid` -- VL clear, the footer had a time
      from the first frame -- and its first answer found the RTC
      `rtc_minus_sntp_ms=-3127`, where the night's -20.8 ppm from the 07:05
      write's +4 ms predicts about -510: **2.6 s lost beyond the drift** during
      the outage, -112 ppm over it if spread evenly. Not established which: the
      oscillator running slower on the backup cell, or stopping for a moment as
      the supply changed over without the chip setting VL. One outage; a second,
      of a known length, and `time` against a reference right after the power
      returns would tell them apart. That boot's `rtc written` line is not on the
      card -- esptool reset the chip within the card's 10 s flush interval to
      flash phase 4g -- but the write happened: the next boot found the RTC
      -484 ms behind, where a write with the datasheet's 508 ms lands on this
      chip, not 3.1 s.
      **An evening of noisier answers, 2026-10-22** (ELF `6c4244ce7...`, the
      owner's YAT logs of 20:19 and 22:20 local): two hourly syncs read
      `drift_ppm=-9.887` and `-34.180`, where the night's nine spread +-2.0;
      their mean, -22, is the night's -20.8. The ESP32's own clock moved
      `system_minus_sntp_ms=38` and `-48` between the same answers, against +-8
      in the night: the server's answers, which lwIP takes without a round-trip
      correction, were tens of milliseconds noisier, not the RTC. One hour
      says little; a day's samples average it out.
      **A second night, 2026-10-22 to 23** (an image of `8c73fdd`, BSEC, from
      the card's `log/`, ten hourly syncs 22:37Z to 07:37Z): `drift_ppm` -17.2
      to -23.4, **mean -20.6** -- the first night's -20.8 again; every write on
      time (`rearms=0`), every landing within +-12 ms.
      Still to see: a day of hourly samples, and another temperature; the cold
      boot after `rtc_stop`; the chip's marking; the autumn change. Whether the
      measured delay, about 995 ms, should replace the datasheet's 508 as the
      default, so that a boot's first write lands too, is the owner's (ADR
      0023's amendment, "Not decided here").
- [ ] **MQTT on the board (phase 4g).** Built 2026-10-22, verified on the
      host only (`docs/adr/0033-mqtt-on-the-board.md`): esp-mqtt behind
      `port_telemetry` (`platform/esp32_t5s3/adp_mqtt.c`, a task of its own,
      the only caller of esp-mqtt), its decisions tested on the host, and the
      reporter composed; the firmware and the test app built with a dummy
      broker. **Needs a broker**: in `apps/firmware_esp32/sdkconfig.credentials`,
      beside the network, `CONFIG_HAC_MQTT_BROKER_URI="mqtt://<address>:1883"`
      and, for a broker with users, `CONFIG_HAC_MQTT_USER` and
      `CONFIG_HAC_MQTT_PASSWORD` (`sdkconfig.credentials.example`); delete the
      build's sdkconfig and reconfigure -- the build refuses a stale one -- and
      `grep CONFIG_HAC_MQTT_BROKER_URI sdkconfig`. Without one the image is phase
      4f's: `main: MQTT off: no broker in this image`. What each check is to
      show, and the lines that show it:
      - *A boot*: `adp_mqtt: client ready: plain MQTT, no TLS broker=... user=...
        password_len=... psram_bytes=...` and `main: MQTT: the reporter waits for
        the link topics=homeair/hac-<MAC> ...` before the first frame; after
        `adp_net: up`, `reporter: session up session=1`, `event_loop: published
        online=true discovery=9 session=1`, and once `adp_mqtt: memory with a
        session free_internal=... min_free_internal=... mqtt_stack_free_min=...`.
        About a minute later `event_loop: link on the screen shown=online`, and
        the bar says `online`.
      - *In Home Assistant*: a device "Home Air Monitor", model "LILYGO T5 4.7"
        e-Paper S3", hardware version the silkscreen's, software version the
        image's `git describe`, beside the simulator's "Home Air Monitor
        (simulator)"; nine entities with the BSEC image, seven without it --
        IAQ and its accuracy removed; the values those of the screen at the same minute,
        `rssi` that of `wifi`. `mosquitto_sub -v -t 'homeair/#' -t
        'homeassistant/sensor/hac-+/#'` shows the retained state and status.
      - *The console's `mqtt`*: `MQTT CONNECTED`, the broker and the user;
        published and acknowledged growing a message a minute at least,
        refused 0, queued 0 between messages, esp-mqtt's outbox 0 between
        messages; **the event loop's longest call in microseconds** -- tens,
        like the card's longest post -- and which call.
      - *A broker restart* (the add-on or the container restarted): `adp_mqtt:
        the session was lost`, `reporter: session lost why=dropped`, attempts
        until the broker is back -- `no connection to the broker why=...` once --
        then `session up session=2` and the configs again; the bar `no broker`
        only if the broker was gone more than a minute. The sensors' `sample`
        lines, the redraws and the card's rows go on through it.
      - *A Home Assistant restart*: `reporter: home assistant started:
        discovery again`, nine configs, the entities back without a restart of
        the board.
      - *Wi-Fi gone for an hour* (the access point off): `reporter: session lost
        why=network`, the will -- Home Assistant shows the device unavailable
        within 90 s -- the bar `no Wi-Fi` a minute after the loss; nothing
        queued meanwhile (`mqtt`: queued 0); after the access point's return,
        `adp_net: up`, `session up`, `online` and the state at once, the bar
        `online` a minute later. Measuring, the screen and the card unaffected
        throughout: the sensors' lines, `rec`'s rows and longest post, the
        `mqtt` longest call still microseconds.
      - *Resources*: `memory with a session` against phase 4f's 72,503 free
        with the link up (an ESTIMATE of 20-25 KB less); `esp-mqtt's task:
        stack low-water mark` of 6,144 (`ADP_MQTT_TASK_STACK_BYTES`, an
        ESTIMATE); `adp_mqtt: stack low-water mark` of the adapter's 6,144;
        the event loop's own, now with the reporter's step on it.
      - *A day unattended*, from the card's `log/`: `event_loop: published in
        the hour` -- 24 a day: state messages (ADR 0030's replay: 1,554-1,597 a
        day), heartbeats, discovery, sessions, lost, refused -- and `reporter:
        session lost` / `session up` for the reconnections; `BSEC called late`
        and `decision action=full` per day against earlier days' (open item "BSEC
        on the board"), to see whether MQTT's traffic moves either; and in Home
        Assistant, **long-term statistics for every measurement entity** --
        temperature, humidity, pressure, CO2, IAQ -- in Developer tools,
        Statistics, with no "unit changed" or "no state class" issues:
        requirements 16's acceptance for "Phase 3".
      - *The test app*: with a broker in the overlay, `idf.py -T hac_platform
        build` from a fresh `test_app/sdkconfig` adds `contract port_telemetry
        adp_mqtt+esp-mqtt` lines, every one PASS -- the full queue's two "not
        exercised", the drop and its will exercised by taking the station
        down -- and a summary line with the longest call and both stacks;
        without a broker the case is IGNORED and says why.
      **First run 2026-10-22** (ELF `6211314dd...`, the BSEC image, a mosquitto
      2.0.21 on a Raspberry Pi Zero W, plain MQTT with a user; the owner's YAT
      logs of 16:57 and 17:01 local, 12 minutes in all):
      - *A boot*: `adp_mqtt: client ready ... password_len=9`, the Wi-Fi up at
        20.2 s (two failed attempts, the access point still holding the old
        association), `reporter: session up session=1 attempts=1`, `published
        online=true discovery=9`, and 60 s later `link on the screen
        shown=online`. `memory with a session free_internal=51799` against 61,823
        with the link up before it: **MQTT costs about 10 KB of internal RAM**,
        under the 20-25 KB ESTIMATE; the stacks: esp-mqtt's 4,700 free of 6,144,
        the adapter's 3,256 of 6,144.
      - *The event loop's longest call into the port*: 138 us (`publish`) after
        3 minutes, 204 us after 7 -- microseconds, as the contract of "nothing
        waits" asks. esp-mqtt's outbox (4 KB) was full 7 and then 19 times at
        the sessions' starts: twelve messages in a burst, the rest waiting in the
        adapter's ring as designed; nothing refused (`refused 0`).
      - *A broker restart* (17:05:48): `transport_read(): EOF`, `session lost
        why=dropped`; the first attempt a second later met a broker still
        starting (`Connection reset by peer`, esp-mqtt knew within 0.5 s); the
        reporter, which cannot be told, waited out its 10 s connect timeout, then
        its 5 s backoff -- `session up session=2` at 17:06:03, **15 s after the
        loss, 10 of them the port's missing "the attempt is over"** (ADR 0030's
        amendment saw the same with the simulator; ADR 0033 weighed the port
        change and did not make it -- this is its first board figure). The
        console's `mqtt` said "session 3" for the adapter's third attempt where the
        reporter's log said session 2, the second that worked: since then it
        says `attempt 3 (2 sessions)`.
      - *A `restart`* (17:09:41): ESP-IDF's shutdown stopped Wi-Fi under the
        session (`tcp_read error, errno=Software caused connection abort`), the
        broker published the will -- `homeair/hac-d83bda706774/status` went
        `offline` -- and the new boot's `online` followed 12 s later.
      - *A write that did not finish* (14:10:58Z, from the card's `log/`, the
        session after that `restart`): 66 s into it, `transport_base: Poll
        timeout or error, errno=Connection already in progress ...
        timeout_ms=5000` and `mqtt_client: Writing didn't complete in specified
        timeout: errno=119` -- esp-mqtt's socket would not take a write for 5 s
        -- then `session lost why=dropped`; the client's stop took 5.5 s (its
        ping met the same), and `session up session=2` came 5.6 s after the
        loss, the configs again. The Wi-Fi stayed up (no `adp_net` line). Why the
        socket stalled is not in the log: the Pi Zero W's broker, or the air.
        The event loop never waited for it. To count in the day unattended.
      **Home Assistant and two hours, 2026-10-22** (ELF `6c4244ce7...`, branch
      `ui-screen-change-full-refresh`, its MQTT code phase 4g's; the owner's
      YAT logs of 20:19 and 22:20-22:26 local, uptime 2 h; the broker with its
      ACL -- the board may write only `homeair/<its id>/#` and
      `homeassistant/sensor/<its id>/#`, a simulator beside it under its own
      user: the broker's configuration, not in the board's log):
      - *A restart of Home Assistant* (22:25:52): `reporter: home assistant
        started: discovery again session=1`, then `published online=false
        discovery=9` 30 ms later -- the birth message answered with every
        config again, in the session that was up: no loss, no reconnection.
      - *Two hours*, taps in the first four minutes only: `published in the
        hour state=77 heartbeats=51 discovery=9 sessions=1 lost=0 refused=0`,
        then `state=66 heartbeats=54 discovery=0 sessions=0 lost=0 refused=0`:
        66-77 an hour, 1,584-1,848 a day, against the 1,554-1,597 ADR 0030's
        replay of the owner's days predicted. Why the first hour sent 11 more,
        the log does not say: the session's first state and one change of
        BSEC's accuracy (`unreliable` to `low`, at 180 s) are two of them.
      - *The ACL*: the state and status still arrive under `%c`, the client id
        being the device id. **The denial, seen 2026-10-22** on the broker:
        three publishes as `hac-board`, each `CONNACK (0)` and sent -- with
        client id `hac-board-probe` to `homeair/simulator/state`, with `probe1`
        to `homeair/probe1/x`, with `probe1` to `homeair/probe2/x` -- and a
        subscriber as `homeassistant` on `homeair/#` received only `homeair/probe1/x
        allowed-B`: the board's user can write under its own client id alone, and
        cannot touch the simulator's topics or another device's. Mosquitto drops
        a denied QoS 0 publish without telling the client, so the check is the
        receiver's.
      - *Not yet seen*: Wi-Fi gone for an hour (gone for three minutes, twice,
        2026-10-23, below); a full day's hourly lines and Home Assistant's
        long-term statistics.
      **The test app, 2026-10-22** (the non-BSEC variant with the overlay, the
      owner's YAT logs of 23:20 and 23:29 local):
      - *The first run, one failure*: `connected within
        PORT_TELEMETRY_CONTRACT_CONNECT_MS FAIL`. The runner's fault: the
        `port_net` case before it ended with a `down()` the station's task had
        not yet acted on, the port showed `NET_UP` until it had (ADR 0032, "what
        the port shows"), `test_station_up()` returned at once, and esp-mqtt's
        one attempt met a radio being stopped (`esp-tls: connect() error: Host
        is unreachable`, 60 ms before `radio stopped`); the port never
        reconnects, so the contract's 10 s ended in a failure. `test_station_up()`
        now takes the link down and waits for the station's own phase to be idle
        before it asks for it again. Nothing in the adapter: the firmware calls
        `up()` once.
      - *The run after the fix*: `11 Tests 0 Failures 0 Ignored`, `tests=11` (the
        BSEC variant has one case more); 45 `contract port_telemetry` lines
        PASS, the full queue's two "not exercised"; 47 s, 3 attempts, 3 sessions,
        one ended by the network -- the station taken down under the session and
        brought back -- and its will `gone` published, kept and cleared at the
        broker; 30 published, 37 forwarded (subscriptions included), 30
        acknowledged, 27 received; the longest call 136 us (`connect`); stacks 3,240
        free (the adapter's) and 3,980 (esp-mqtt's). The broker, watched with
        `mosquitto_sub -t 'hac-contract/#'`, saw the messages in the contract's
        order and nothing left over.
      **The Wi-Fi gone twice, 2026-10-23** (an image of `e44fbfc`, BSEC, from
      the card's `log/`; the item "Wi-Fi on the board" has the link's side):
      - *The loss*: the socket's read failed at the beacon timeout
        (`tcp_read error, errno=Software caused connection abort`), so the
        reporter saw the session drop a moment before `port_net` the link:
        `session lost ... why=dropped`, not the `why=network` this item
        expected. The same outcome -- no session, nothing queued -- only the
        word differs.
      - *The first return*: the link up at 11:10:17Z with a new address, and
        the broker at `192.168.1.141:1883` **reset every CONNECT for two
        minutes** (`no connection to the broker why=Connection reset by peer
        code=104`; attempts at 11:10:18, :34, :54, 11:11:24, 11:12:14, the
        reporter's backoff doubling) until the link went again. A reset is an
        answer: something at that address was up with nothing listening on
        1883 -- the Pi Zero W's mosquitto not yet back after its own
        reconnect, or another host on the address after the router's restart.
        The board's log cannot tell; the Pi's journal can.
      - *The second return*: `session up session=2 attempts=8` 0.2 s after the
        link, `online` and nine configs.
      - The will and Home Assistant's "unavailable" are not in the board's
        log: the broker's and Home Assistant's to show.
- [ ] **A change of screen as a full refresh (ADR 0034).** Built 2026-10-22,
      verified on the host only: every change of screen -- a tap, and the
      diagnostics screen's timeout -- is a full refresh, whatever the budget
      and the panel's minimum interval; the diagnostics screen's title has no
      link word. What to see:
      - *Readability*: the new screen with no outline of the old one, by eye,
        both ways -- what the owner saw wrong with partial changes.
      - *Latency*: `event_loop: screen changed ... latency_ms=... action=full
        partials=0`, the latency about 2,000 since ADR 0035 (the vendor's
        clear ~1.3 s and the draw from white ~0.6 s, both estimates, ~80 ms of
        rendering; 3,400 with EPDiy's clear and GC16), against 1,163-1,173 for
        a partial change; a tap during a full refresh handled after it.
      - *Taps in a row*: home, diagnostics and home again within a few
        seconds -- each change full, none deferred (`deferred` would say the
        panel's interval held it), the budget at 0 after each.
      - *BSEC*: a `BSEC called late` line at each change, as at every full
        refresh; whether IAQ's accuracy still reaches `high` on a day with a
        few visits to the diagnostics screen.
      - *The title*: `Diagnostics` and `tap to return`, nothing between them;
        the MQTT line says `online` or `offline` as of the tap.
      **Seen 2026-10-22**: with EPDiy's clear and GC16 (ELF `89905d8a5...`,
      the owner's YAT log of 19:49 local) six changes of screen, every one
      `action=full reason=request`, `latency_ms=3376` to `3411`, none deferred
      though five came within the panel's minute of the last full refresh,
      the budget at 0 after each; BSEC late 1.62-3.30 s at each. With the
      vendor's full refresh (ADR 0035, the item "The vendor's full refresh")
      1,842-1,901 ms. The title and the readability by eye: not reported yet.
- [ ] **The vendor's full refresh (ADR 0035).** Built 2026-10-22, verified on
      the host only: `REFRESH_FULL` and `REFRESH_CLEAR` start with the
      vendor's clear -- 4 cycles of 4 pushes towards black and 4 towards
      white, rows driven 50 us (`BOARD_EPD_CLEAR_*`) -- and a full refresh then
      draws with `MODE_EPDIY_WHITE_TO_GL16` instead of GC16. What to see:
      - *The flashes*: eight quick ones, then the picture darkening in with
        no black phase -- as on the vendor's firmware the owner compared.
      - *Timing*: `adp_display: panel cleared took_ms=` (ESTIMATE 1,280) and
        EPDiy's own timing line after it for the draw (ESTIMATE: the diff
        ~106 ms, then ~117 ms + 0.8 ms per non-white row, ~0.5 s for the home
        screen); the boot's first frame has both. Into
        `EPD_SIM_CLEAR_REFRESH_MS` and `EPD_SIM_FULL_DRAW_*`
        (`platform/host/epd_emulation.h`), or the calibration image's
        `clear`, `full_black`, `full_home`, `full_ramp`.
      - *No trace*: after a change of screen, and after the twenty-first
        redraw's full refresh, no outline of what was there -- close up, and
        on the greys (the item "Residue that survives full refreshes").
      - *No stripes in white*: the white columns beside text the same white
        as the rest (the item "Vertical stripes in white").
      - *Greys, full against partial*: the footer's "updated HH:MM" after a
        full refresh and after a partial one that changed some of its digits
        -- the same grey in both, or two? Likewise a status word in grey
        (`warm`, `stale`) drawn by each. Two greys mean `MODE_GL16` after the
        same clear instead (ADR 0035, "Greys come by another path").
      - *The slideshow*: `06_level_match` and the photographs, all full
        refreshes, against ADR 0013's judgement (the item "How light each grey
        level really is").
      - *BSEC*: a late call at each full refresh as before, shorter by about
        1.5 s.
      **Timed 2026-10-22** (ELF `6c4244ce7...`, the BSEC image, the owner's YAT
      logs of 20:19 and 22:20 local, two hours): 24 clears, `took_ms=`
      1,213-1,220 (1,214 the median, estimated 1,280 -- now the simulator's);
      EPDiy's draw after them `draw: 432-456ms` with `diff: 107-120ms` (the
      model's 448 for the diagnostics screen and 455 for the home screen); 22
      changes of screen, `latency_ms=1842` to `1901`; the twentieth partial's
      next redraw full as before (`forced_full=true`, 21:50). BSEC late at a
      change of screen 0.24-1.73 s. No error or warning line besides those.
      Still to report, by eye: the flashes, any trace, the stripes, the greys.
      **By eye, the owner, the same evening**: no trace after a full refresh;
      but after a partial one, lighter patches in static greys it had not
      redrawn -- the rules beside a changed digit, the footer's "updated". GL16
      re-drives an unchanged grey in every row and column it touches
      (L -> L: towards black and back), and `WHITE_TO_GL16`'s greys were darker
      than GL16's. Changed: GL16 after the vendor's clear (ADR 0035, amendment)
      -- the draw ~900 ms, a full refresh ~2.2 s. To see on the next image: the
      same patches gone; no stripes in white; a change of screen ~2,300 ms.
      **Better, not cured** (the owner): still lighter patches. Changed again
      (ADR 0035, second amendment): partial refreshes through white -- a
      changed pixel whitened and darkened as the full refresh darkens it, an
      unchanged one not driven at all -- and the full refresh the vendor's clear
      and `WHITE_TO_GL16` again. To see: `adp_display: partial refreshes
      through white, full ones darkened from white phases=30` at boot; no
      patches in static greys after any partial refresh; a changed digit's old
      strokes vanishing before the new ones appear; EPDiy's `draw:` for partial
      refreshes as long as with GL16; and over a budget of twenty partials,
      whether the greys they redrew drift, and whether close-up outlines
      (ADR 0008's ghosting) come sooner than the 11th.
      **Seen 2026-10-22 23:04 local** (ELF `216d55c04...`, five minutes;
      against ELF `7c58fe323...`, GL16 after the clear, 22:49): the owner likes
      it -- no serious artifact, only the expected trace around a black
      element that went away, a digit. `phases=30` at boot; partial draws to
      the millisecond GL16's (322 ms for 54 rows, 517 for 264, 567 for 381);
      the full refresh's draw 432-456 ms (864-911 with GL16), a change of
      screen 1,844-1,865 ms (2,277-2,309); the budget's full refresh after the
      twentieth partial as before; nothing in the log but BSEC's late calls.
      Still to see: a day of it -- greys a partial redraws often, the traces
      around vanished digits against the budget of twenty, residue in greys.
- [ ] **The card as a USB disk on the board (ADR 0036).** Built 2026-10-22,
      verified on the host only (`docs/adr/0036-the-card-as-a-usb-disk.md`):
      `usb_disk` at the console, or the button (IO21) held for two seconds,
      lends the card to the PC -- the PHY taken from USB-Serial-JTAG for
      TinyUSB's mass storage -- until the disk is ejected; then the PHY goes
      back and the card is mounted again, without a reset. Nothing of the
      switch has run on this board: that the PHY moves between the two
      controllers at all, and back, is read in ESP-IDF 5.5's source. What each
      check is to show, and the lines that show it (the session's own lines
      reach the card's `log/` after it, not the terminal):
      - *The switch*: `usb_disk` with a card in: `USB_DISK lent: in 300 ms
        this port goes`, COM17 gone, a removable disk "Home Air Monitor card"
        with the card's files in Explorer, and Windows' device manager naming
        it USB mass storage, VID 0x303A, PID 0x4002, serial the MAC. With no
        card: `USB_DISK refused`, nothing switched.
      - *The way back*: Explorer's "Eject": the disk goes, COM17 comes back
        (the same number, by the serial), and `sd` shows the card mounted and
        `USB_DISK last of 1: ended ejected`; the card's log has `lending the
        card to USB`, `usb disk started`, `the card is back from USB
        why=ejected ...`, `card mounted`; `rec` shows new files, the day's
        next `_N`.
      - *The other endings*: the button pressed (`why=button`); the cable
        pulled with the battery in (`why=host_gone`, about 3 s after);
        "Safely Remove Hardware" in the tray -- whether Windows sends the
        eject or only suspends the device (`why=ejected`, or `suspended` 10 s
        after); the PC put to sleep (`suspended`); a charger instead of a PC
        (`why=no_host` after 30 s). Each followed by the card mounted.
      - *Throughput*: a 100 MB file copied each way, timed on the PC, beside
        `read_kb=`, `written_kb=`, `slowest_read_us=`, `slowest_write_us=` in
        the `back from USB` line. Full speed's ceiling is about 1 MB/s; a lot
        less says the 4 KB buffer (`USB_DISK_MSC_BUFFER_BYTES`) or the SD
        clock is the limit.
      - *Nothing else held up*: through a session the sensors' `sample`
        lines, the redraws and MQTT go on; `rows_dropped` grows by the
        session's rows and no more; the card's task's `stack low-water mark`
        (of 6,144) with TinyUSB's callbacks and the SD driver on it; no
        `BSEC called late` beyond the usual.
      - *Twice in a boot*: a second `usb_disk` after the first came back --
        the second session takes the PHY from the USB-Serial-JTAG handle the
        first one made.
      - *A reboot while lent*: `restart` cannot be typed then, so a panic or
        the watchdog is the case; read, not seen: the next boot's early lines
        do not reach COM17, which returns once the console starts. RST always
        gives USB-Serial-JTAG back.
      - *The cost*: `free_internal=` in the `card mounted` line after a
        session against before (the build: +5,536 bytes of static internal
        RAM, TinyUSB's 4,947 of them).
      **First run 2026-10-23** (ELF `26000f158...`, the BSEC image, the
      owner's YAT log of 11:07 local, three sessions of 21-74 s): each
      `usb_disk` answered, the card was lent (`state=lent`), and the next
      thing on COM17 was a new boot, `rst:0x15 (USB_UART_CHIP_RESET)` and
      `previous reset reason=usb rom_code=21` -- a reset the USB host asked
      for through USB-Serial-JTAG's CDC line state, not a panic or a watchdog
      (none in the log). Each following boot mounted the card at once, so the
      host left the filesystem readable. **The terminal, not the mode**
      (confirmed the same day: no reboot with YAT closed, below): the
      session's end gives COM17 back, YAT reopens it by itself, and opening
      the port resets the chip -- as it does after a power cut (open item "The
      RTC on the board", phase 3e) and, by the same log, at YAT's first open:
      the file begins at the board's 3.9 s. The S3's USB-Serial-JTAG has no
      bit to refuse that reset (ESP-IDF 5.5's `usb_serial_jtag_reg.h` for the
      S3 has none; the C61's and later chips' `USB_UART_CHIP_RST_DIS` is
      absent), and refusing it would cost esptool's reset anyway. Two of the
      three resets were followed by a brownout at `PWR_EN`'s rise (open item
      "A reboot loop when the switched 3V3 comes up"). So: close YAT, or turn
      its automatic reopen off, before a session -- or accept a reboot after
      it. Also seen: two
      `sdspi_transaction: cmd=52 ... cmd=5, R1 response: command not
      supported` lines at every lend -- `sdmmc_card_init()` probing for SDIO,
      harmless.
      **The card's log of the same morning** (`log/2026-10-23.log`, copied off
      by the owner): the YAT sessions left nothing of themselves on the card --
      their lines were in the ring, which a reset loses, and the `lending` line
      comes after the files close -- and the boots after them read `previous
      reset reason=BROWNOUT` three times (YAT's first open at 08:07:40Z, and
      after the first two sessions) and `usb` once: a host reset, then a
      brownout at `PWR_EN`'s rise in the boot it started, as before. One boot,
      after a `usb` reset with no brownout, found the GT911 at `0x14` -- the
      latch again (open item "The touch panel on the board"), taken as
      designed. **Then a session with YAT closed, entered and ended by the
      button** (08:16:09Z-08:16:17Z): `lending the card to USB ... by=button`,
      `usb disk started ... status=0` 180 ms later, the PC configured the disk
      once and read 2,832 KB -- Explorer opening it; no write -- `the card is
      back from USB why=button lasted_ms=7232 io_errors=0 refused=0
      slowest_read_us=94637`, `card mounted` 100 ms later, the day's sixth
      files, **no reboot**; the log runs on as before. So both of the button's
      uses work, the PHY goes to USB-OTG and back without a reset, and the
      card mounts after the host. In passing: `free_internal=43715` at that
      mount against 43,987 with the MQTT session up before it (one session,
      no leak to speak of); the card's task's low-water mark 2,464 bytes free
      of 6,144 with TinyUSB on it; temperature redraws for half a minute after
      it, the BME680 at up to 28 C -- the hand at the button, beside the
      sensor. Still to see: the eject (`why=ejected`), "Safely Remove
      Hardware", the PC asleep, a charger (`no_host`), a cable pulled on the
      battery (`host_gone`), writes and throughput, a second session in a
      boot, `sd`'s `USB_DISK` line.
- [ ] **Firmware from the card on the board (ADR 0037).** Built 2026-10-23,
      verified on the host only (`docs/adr/0037-firmware-from-the-card.md`):
      `home_air_monitor.bin` in the card's root is installed into the other OTA
      slot at the next mount, deleted, and booted; the new image confirms itself
      at its first frame, or the bootloader -- new, with rollback -- returns to
      the old one. **Needs the new bootloader once**: the usual esptool command,
      bootloader at 0x0 and `ota_data_initial.bin` at 0xf000 included. What each
      check is to show, and the lines that show it:
      - *The bootloader*: after the esptool flash, `fw` says `FW running ota_0
        ...: home_air_monitor <version> ..., flashed` and the boot log
        `fw_update: running partition=ota_0 state=flashed`. (**Corrected
        2026-10-23**: `valid`, below.)
      - *An update through the USB disk*: copy a build's `home_air_monitor.bin`
        to the disk's root, eject. Then, in the card's `log/`: `firmware on the
        card: installing version=... from_version=... bytes=... slot=ota_1`,
        `firmware from the card installed: restarting into it ... took_ms=`
        (an ESTIMATE of 5-10 s); the panel's boot; `fw_update: running
        partition=ota_1 state=pending_verify`, about four seconds later `the
        update confirmed: it reached its first frame`; `fw` says `valid`; the
        file is gone from the card. The version in Home Assistant's device
        page follows.
      - *Back again*: the older image copied the same way installs into
        `ota_0` -- no version check, by design.
      - *Refusals*: the slideshow's image (another project), a bootloader or any
        other file under that name, an empty file: `firmware on the card
        refused: renamed .rejected verdict=project` (`not_an_image`, `empty`),
        the file now `home_air_monitor.bin.rejected`, nothing installed. A copy
        cut short (the cable pulled mid-copy): `verdict=invalid step=end`.
      - *A rollback*: hard to provoke on purpose -- an image that panics before
        its first frame, or RST held during the first boot of an update. The
        boot after: `the other slot holds an update that did not start: rolled
        back`, and `fw` says so.
      - *The update's cost to the rest*: `took_ms=`, a `BSEC called late` at
        it, whether a panel refresh under way showed a glitch, the card task's
        `stack low-water mark`.
      - *esptool after an update*: the usual command, `ota_data_initial.bin`
        included, boots what it wrote (`fw`: `running ota_0 ... flashed`);
        without it, the board would stay on `ota_1`.
      **On the board, 2026-10-23** (an image of `e44fbfc`, BSEC, flashed with
      the new bootloader; from the card's `log/`, 10:39-11:16Z):
      - *The bootloader*: the first boot read `running partition=ota_0
        state=valid`, not `flashed`. With rollback on and no factory slot,
        ESP-IDF's bootloader writes an `otadata` entry for `ota_0` itself when
        it finds `otadata` empty (`bootloader_utility.c`, "No factory image,
        trying OTA 0"), so an image from esptool reads `valid`; `flashed` is
        left for an `otadata` that cannot be read. The comments say so now.
      - *An update through the USB disk*: the disk lent from the console at
        11:01:25Z, 1,443 KB written to it, back at 11:02:26 (`why=suspended`,
        Windows' eject), mounted, and in the same second `firmware on the card:
        installing version=e44fbfc from_version=e44fbfc bytes=1450048
        slot=ota_1` -- the same image again, installed as designed. `installed:
        restarting into it ... took_ms=19364`: **19.4 s**, against the ESTIMATE
        of 5-10 s; ESP-IDF's two verifications of the slot (`esp_ota_end()`,
        then `esp_ota_set_boot_partition()`, each an `esp_image` segment list)
        took 0.4 s of it, and the log does not say how the rest divides between
        the card's read, the erase and the write. The restart a software reset
        (`reason=software`), no brownout; `running partition=ota_1
        state=pending_verify`, and at 3.7 s of uptime `the update confirmed: it
        reached its first frame partition=ota_1`. The file was gone: the next
        mounts found nothing to install. Wi-Fi joined at its first attempt
        after it, where every esptool or USB reset that morning cost two refused
        attempts and 15 s -- the software reset's shutdown leaves the access
        point nothing stale.
      - *The update's cost to the rest*: two taps during it changed the screen,
        each full refresh with **2.7-2.8 s of clear** (1.21 s otherwise) and a
        diff of 270-320 ms (107) -- the flash's erases and writes stalling the
        caches the clear's loop and EPDiy run from -- 3.7-3.9 s to the glass,
        and BSEC late by 3.4-3.8 s. The glass by eye: not reported.
      - *A refusal*: the slideshow's image (`hac_slideshow`, `c5c41db-dirty`,
        1,574,880 bytes) under the name: `firmware on the card refused: renamed
        .rejected ... verdict=project step=judge took_ms=29`; the next mount
        found nothing to try.
      - **The card's task's stack at 1,304 bytes free** of 6,144 right after
        that refusal, its lowest yet (2,048 when a card was pulled,
        2026-10-22): the open, the header's read, the rename through FatFs with
        long names, and a log line of fourteen fields through the tee's
        `vsnprintf`. The install's own low-water mark is not in the log -- the
        restart comes before the next report. Enough today, with little margin:
        the stack to 8 KB, or the update's log lines split, before anything
        else lands on that task.
      Still to see: a rollback; an older image back into the other slot; a copy
      cut short (`verdict=invalid`); `fw` after an update; the version in Home
      Assistant after one; the install's stack.
