# Phase 5: what to do at the bench before the plan is rewritten

Started 2026-11-07. The steps to take on the board, in order, before phase 5's plan
is written down for good: what to measure, what to solder, and which decision each
result settles. Fill the results in here; the plan is rewritten from this file.

Everything marked *read* comes from the v2.4 schematic (`board_notes.md`, "The
battery path") and has not been measured. Nothing on this board has had its current
measured yet.

## Rules for the whole bench

- **USB changes what is being measured.** With `VBUS` present `Q5` is off and the
  board runs from USB, so every figure "on battery" is taken with USB unplugged, and
  so with no console: read the panel and the card's `log/`.
- **The battery tap**: a short JST lead with one wire brought out to two test leads,
  so that a meter sits in series with the cell and nothing is cut on the board.
- **A meter's µA range drops volts**: put a jumper across it for boot and wake, and
  open the jumper only once the board is asleep. On the mA range the drop is smaller
  but not nothing: note the range used.
- **No protection on the board**: below about 3.3 V nothing disconnects a cell. Stop
  by hand unless the cell has a protection circuit of its own.
- **Do not leave USB and a cell together unattended until step B is done.**

## A. Before a cell goes on (minutes, a voltmeter)

| # | Do | Expect | Result |
|---|---|---|---|
| A1 | Read the markings: `U13` (ME6217 or RT9080), `U8` (PCF8563 or a look-alike); see whether `Q3` is fitted, and `R23`, `R26` | One of each named | |
| A2 | Flash the gauge's image. USB alone, no cell: `vbat`, the bar, a meter on `PY2` | *read:* about 4.6 V, the bar `Battery n/a`, the log `state=not-a-cell` | |
| A3 | The same, RST held down (`PWR_EN` low): a meter on `PY2` | What the charger's output does with no cell. A big difference from A2 means `Q3` conducts | |
| A4 | The cell: its capacity, a protection board (under the tape at the leads), its polarity by meter against the silkscreen's `BAT+`, whether the connector fits (`PY2`: 1.27 mm on the schematic, PH 2.0 on LilyGO's pinmap) | 500 mAh or more: the charger pushes 0.5 A (`R19`, *read*) | |

A2 and A3 settle `BOARD_BATTERY_HIGHEST_CELL_MV` too.

## B. A cell and USB: does `Q3` bypass the charger? (attended)

The battery tap, a meter on a range that takes 1 A, USB in.

| # | Do | Expect | Result |
|---|---|---|---|
| B1 | RST held: current into the cell, voltage at `PY2`, the LEDs | The charger alone: up to 0.5 A, red; falling, then green at 4.22 V | |
| B2 | Firmware running: the same | The same if `Q3` does nothing. More current, or a cell above 4.25 V, or current into a cell the green LED calls full: the charger is bypassed. **Take the cell off at once if so** | |

Settles decision 1. On a charged cell, A4 is enough for C.

## C. The discharge run: how long does a cell last? (hours; attended at the end)

The image as usual, Wi-Fi, MQTT, sensors, the card in. Full cell, USB out, note the
time. `battery: sample` lines go to `log/<day>.log` once a minute.

| # | Do | Expect | Result |
|---|---|---|---|
| C1 | Take a meter reading at `PY2` beside the log's `burst_mv` at the start, the middle and the end | Within some tens of mV; the difference is the ADC and divider's error | |
| C2 | Run until the bar says 0 % (about 3.27 V). Take the cell off, unless it has its own protection and the point is to see it act | | |
| C3 | Copy the card. From the log: hours, voltage against time, `refused=`, `took_us=`, any `previous reset reason=BROWNOUT` | Average current = capacity / hours, since the current is steady | |

Gives this cell's table in place of `board_battery_curve[]` (charge left is time
left), the step and the limits their first figures, and today's firmware its autonomy.

## D. Where the current goes, awake (tap, mA range, USB out)

Each against D1, a minute or two each, min and max if the meter keeps them.

| # | State | Answers | mA |
|---|---|---|---|
| D1 | The usual image, a quiet moment | The baseline | |
| D2 | The sensors' whole cable off (power and bus) | The sensors | |
| D3 | The card out | The card, idle and mounted | |
| D4 | An image without `sdkconfig.credentials` | Wi-Fi and MQTT, idle | |
| D5 | Wi-Fi joining (the 7 s after boot), a full refresh, a partial one | The peaks; a scope across a shunt if there is one | |

## E. The floor, asleep (tap, µA range, jumper, USB out)

**Needs a probe image, not written yet: ask for it at this point.** On a battery there
is no console, so its button (IO21) steps through the states and the panel says which
state it is in. Nothing else about it is decided by this file.

| # | State | Answers | Result |
|---|---|---|---|
| E1 | Awake, radio off, `PWR_EN` high | | |
| E2 | Awake, radio off, `PWR_EN` low, bus released | E1 - E2: the HV rails, the 78L/79L regulators, LM358 (*read:* some mA) | |
| E3 | Light sleep, `PWR_EN` low | Against E5: is deep sleep worth a reboot? | |
| E4 | Deep sleep, `PWR_EN` high (IO12, IO13 held) | | |
| E5 | Deep sleep, `PWR_EN` low, touch asleep | The floor. *Read:* 63 uA of resistors, the chip, the LDO | |
| E6 | E5 with the sensors' bus unplugged | *Read:* about 0.27 mA less: the pull-ups into unpowered sensors | |
| E7 | E5 with the card out | The card, idle, on a rail nothing switches | |
| E8 | E5 with the touch awake | The GT911 | |

## F. Brownouts at `PWR_EN`'s rise (no meter)

Ten cold boots each, the terminal closed, the card's log read after: lines `previous
reset reason=BROWNOUT` per ten boots.

| # | Supply | Result |
|---|---|---|
| F1 | USB alone | |
| F2 | USB and a cell | |
| F3 | A cell alone, full | |
| F4 | A cell alone, near empty | |

Every wake with a redraw raises that rail in phase 5, hundreds of times a day. A scope
on `VDD3V3` at the rise, if there is one, is worth more than the counts.

## G. What to solder, once the numbers say so

| # | Change | When | Check after |
|---|---|---|---|
| G1 | **USB sense**: `P8` pin 2 (`VBUS`) - 100 kOhm - `P8` pin 19 (IO10) - 150 kOhm - `P8` pin 6 (GND), 100 nF across the 150 kOhm if wanted | For automatic modes; decision 2 | Pin 19: 2.85-3.15 V with USB, 0 without. Never on pin 23: IO45 straps the flash voltage |
| G2 | **Remove `Q3`** | If B says bypass. `Q5` still feeds the board on a cell | B, F1, a full refresh on a cell |
| G3 | **The sensors' V to `VDD3V3`**, from `P5` pin 1 to a pad found by continuity to the module's 3V3 (`C27`/`C28` are near it; `P2` is the touch flex's) | If E says `PWR_EN` up is costly and down is costly too; decision 3 | The boot scan; E5 again, with the modules' own LEDs and regulators in sight: remove a power LED if a module has one |
| G4 | **`CHRG` and `STDBY` to GPIOs**: a Schottky, cathode at the LED's cathode, anode at the GPIO with a pull-up. IO39 (pin 24) for one; the other needs IO48 or IO45 of `P4` | Only if the screen is to say "charging" and "full"; decision 4 | Levels through a charge |
| G5 | **Bulk capacitance on `VDD3V3`**, or a slower rise of `Q4` | If F is bad | F again |

Firmware follows each: G1 and G3 are not used until it does.

## Decisions these settle

1. `Q3`: leave it, or remove it; whether USB and a cell may stay together.
2. How the board knows it is on a battery: G1, or a configuration.
3. Where the sensors are powered from, and with that whether a wake must raise `PWR_EN`
   at all.
4. Whether "charging" and "full" are worth G4, or a plug mark from G1 is enough.
5. Deep or light sleep between wakes (E3, E5).
6. The card on a battery: keep it, batch to it, or leave it out (E7).
7. The Wi-Fi window: how often, against what D5 says a window costs and what Home
   Assistant's history is to resolve.
8. The autonomy to aim for, in days, on this cell.
9. A firmware cut-off below 0 %, against what A4 found.

## The plan these feed

Written down so far only in conversation, and to be rewritten from the results:
`app_core` first (ADR 0030 left it to this phase); `port_power` with a simulator
behind it, and its contract; `persisted_state_t` and the simulator's cold boot, with
`now_ms` continuous across a sleep, an operation on `port_display` for what is on the
glass, and BSEC's state as an opaque blob through `port_sensor`; `power_policy` and an
energy model holding D and E's figures; the board's `adp_power`; and an acceptance
run on a cell, against the model's prediction. Modes: USB as today; a battery with a
wake every 300 s (BSEC's ULP period, and the interval the SCD41's ASC assumes in
single shot); low and critical below that.
