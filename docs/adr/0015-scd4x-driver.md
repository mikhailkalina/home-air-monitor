# 15. The SCD4x driver: Sensirion's code over `port_i2c`, and a command-level model to run it against

Date: 2026-10-14

## Status

Accepted. Verified on the host only: the sensor is not yet wired to the
board, so nothing below has met a real SCD41. The items marked ASSUMED are
the questions it will answer.

## Context

Phase 3a brings the first sensor, the SCD41 (true CO2, plus temperature and
humidity; `docs/requirements.md` §4.2, §6.3, §13.1). Its protocol is
Sensirion's: 16-bit commands, a CRC-8 after every data word, an execution
time after each command during which the sensor must not be addressed, a
periodic mode with a reading every 5 s, and only five commands accepted while
it runs. Phase 2d's fake bus (`docs/adr/0011-i2c-bus-adapter.md`) held a
register file at each address -- enough for the `port_i2c` contract and the
bus scan, and unable to express any of that.

Two facts about this board shape the start-up (`docs/hardware/board_notes.md`,
"Power rails and connectors"): the sensors take power from the switched
`3V3` that EPDiy raises at init, so a driver cannot know how recently its
supply came up; and a hardware reset drops that rail, while what a software
reset does is not yet observed.

The reference used throughout is the Sensirion SCD4x datasheet, version 1.7
(April 2025).

## Decision

### Sensirion's driver, vendored, behind a HAL of our own

`Sensirion/embedded-i2c-scd4x` tag `1.2.0` (commit `d953712`, BSD-3-Clause)
is vendored unmodified into `drivers/scd4x/vendor/`, with its provenance and
file hashes in that directory's `README.md` and its version in
`THIRD_PARTY_NOTICES.md`. It is the manufacturer's code for the manufacturer's
protocol; rewriting it would add a second, untested reading of the same
datasheet to the one that ships.

Its HAL template (`sensirion_i2c_hal.c`, all TODOs) is replaced by
`drivers/scd4x/scd4x_hal.c` over `port_i2c` and `port_clock`. Sleeps become
`delay_ms()`, rounded up; transfers get a 20 ms deadline
(`SCD4X_TRANSFER_TIMEOUT_MS`). Sensirion passes the HAL's return value
through as an `int16_t`, so the HAL returns Sensirion's own codes and records
the port's real `hal_status_t`, and whether the transfer was a read, for the
wrapper to consult.

**One SCD4x per image.** The HAL takes no context, and `scd4x_i2c.c` keeps
its address and command buffer in globals; the bus and clock are therefore
globals too. A claim (`scd4x_hal_claim()`) makes a second user an error
instead of a silent takeover. A second SCD4x could not share this bus anyway
-- 0x62 is its only address.

Vendored code builds with the project's flags plus `-Wno-conversion
-Wno-strict-prototypes -Wno-missing-prototypes` (the three Sensirion 1.2.0
trips: `f()` declarations, implicit narrowing) and SYSTEM includes, from
`cmake/sources_drivers.cmake`; anything new a vendor update brings still
fails the build.

### The fake bus holds models

`adp_i2c_fake` now routes each transfer to an `i2c_model_t` -- write, read,
and the virtual clock's time (`platform/host/i2c_models/i2c_model.h`). The
register file is one model (`regfile_model.c`), still what
`adp_i2c_fake_add_device()` attaches; the `port_i2c` contract and
`test_i2c_scan` pass unchanged.

`scd41_model.c` models the SCD41 at command level: every command of Table 9
with its execution time, CRC checks on what it receives and CRCs on what it
sends, the address refused while a command executes, the periodic state
machine with data every 5 s (30 s in low-power mode), single shot with a 5 s
window, the five commands accepted while measuring, sleep and wake-up,
settings with RAM and EEPROM copies. Readings come from a callback in
physical units. Fault knobs: refuse the address, refuse a data byte, corrupt
a CRC, hold data back, overrun a single shot.

**It is an independent reading of the datasheet**: nothing is taken from
Sensirion's code -- not the CRC, the times or the conversions. A vendor wait
shorter than the datasheet's makes the model refuse the next transfer and a
test fail. Where the datasheet is silent the model says ASSUMED:

- the address is refused while a command executes (the datasheet only says
  no command may be sent; `ports/port_i2c.h` records the behaviour for
  several Sensirion parts);
- a command not allowed in the current state, an unknown or wrongly sized
  command, and an argument with a bad CRC each have a data byte refused;
- `stop_periodic_measurement` discards a buffered reading; `reinit` returns
  the ambient pressure to its default.

### `scd4x_source`: the sensor as a `sensor_source_t`

In `drivers/scd4x/scd4x_source.c` (ADR 0014 for why there).

- **Two modes, one source**: periodic for USB power
  (`docs/requirements.md` §11.2), single shot for battery (phase 5). Single
  shot does not call Sensirion's `scd4x_measure_single_shot()`, which sleeps
  the whole 5000 ms inside the call; it sends the same command through the
  same framing and leaves the wait to `ready_in_ms()`.
- **Waits are split by length.** Command execution times (1 ms; 500 ms for a
  stop) are waited inside Sensirion's calls, through `delay_ms()`, and only in
  `init()`, `suspend()` and `set_ambient_pressure()`. Measurements are never
  waited: `ready_in_ms()` says when, `read()` before then is
  `HAL_ERR_NOT_READY`, and in single shot it does not touch the bus at all.
- **Busy, not absent.** A refused address inside a window the source opened
  -- a single shot, plus `SCD4X_BUSY_GRACE_MS` -- is "still measuring"; a
  refused read header after an acknowledged `read_measurement` is "nothing
  buffered" (datasheet 3.6.2). Both are `HAL_ERR_NOT_READY`. A refused address
  anywhere else is `HAL_ERR_NOT_FOUND`.
- **`init()`**: wait the 30 ms power-up time (Table 7) -- the supply may have
  just risen; ask for the serial number once, only to learn whether the sensor
  was measuring (a measuring sensor refuses it); stop periodic measurement;
  reinit; read the serial number and variant, and log both with
  `found_measuring`. The stop runs whatever the reset was, as the owner asked:
  a hardware reset leaves the sensor idle on this board, and a software reset
  leaves it powered and measuring (seen 2026-10-16, below). About 0.56 s. An SCD40 is refused single-shot mode
  (`HAL_ERR_UNSUPPORTED`); a firmware too old for `get_sensor_variant` leaves
  the variant "unknown" instead of failing.
- **Readings**: converted from the raw words with the datasheet's formulas
  in double -- Sensirion's fixed-point `scd4x_read_measurement()` puts the
  word for exactly 60 C at 59.998, and the range checks sit on the
  datasheet's limits. Outside them -- CO2 0 or above 40,000 ppm, temperature
  outside -10..60 C, humidity outside 0..100 %RH -- the field is
  `VAL_ERROR`; 0 ppm is how the sensor says "no CO2 value" (3.11.2). Each
  field is judged alone. Otherwise `VAL_WARMUP` for `SCD4X_WARMUP_MS` after
  the first start, else `VAL_OK`.
- **Failures**: `HAL_ERR_IO` (a refused byte, a bad CRC), `HAL_ERR_NOT_FOUND`
  or `HAL_ERR_TIMEOUT`, with every capability `VAL_ERROR`, logged once per
  streak, and retried no sooner than the sensor's own interval. A sensor that
  merely has nothing new keeps answering `HAL_ERR_NOT_READY`; its last
  readings age into `VAL_STALE` in the core.
- **Ambient pressure** (`scd4x_source_set_ambient_pressure()`, 700-1200 hPa,
  allowed while measuring) is supported and tested, and fed by nothing: the
  BME68x is phase 3b's.

Every limit is in `drivers/scd4x/scd4x_limits.h`, each marked DATASHEET,
ESTIMATE or CHOICE. The two estimates: the warm-up (60 s, one CO2 response
time; the datasheet gives none) and the single-shot grace (1 s); both name
what replaces them.

### Not yet in the firmware

`hac_drivers` builds into the firmware and the test app, but no composition
constructs the source yet: wiring it into the event loop needs the sensor
manager (architecture §6.3) and is the next step, not this one. What reaches
the board now is the bus scan's three new rows -- the SCD4x at 0x62, the
BME68x at 0x77 or 0x76 -- and a console `restart` command, which answers by
the blue LED whether a software reset drops the sensors' supply.

## Consequences

- **Verified on the host** (2026-10-14): `test_scd41_model` (94 checks, the
  model against the datasheet's own examples: every CRC example, the serial
  number, the 500 ppm / 25 C / 37 %RH reading), `test_scd4x_source` (204
  checks: every failure path above, the range boundaries, the reset that
  leaves the sensor measuring, the HAL glue), and the `port_sensor` contract
  against `scd4x_source` in both modes (61 checks with replay_source and two
  deliberately broken wrappers it is shown to catch). Sensirion's driver
  produces readings on a PC through the real command protocol, and the model
  refused none of its transfers on the happy paths. The `port_i2c` contract
  and `test_i2c_scan` pass unchanged on the new bus.
- **Verified for the ESP32-S3**: the firmware and the test app build with the
  project's warning set; the vendored files get exactly the three relaxations.
- **Verified on the board, 2026-10-16: the addresses.** With the sensors
  wired, the boot scan found the SCD4x at 0x62 and the BME68x at 0x77, and
  nothing unexpected (`docs/hardware/board_notes.md`, "Sensors on the bus").
  It took a change of boot order first: the sensors run from the switched
  `3V3` that EPDiy's init raises, and while it is down they hold the bus
  low. With the bus brought up and scanned before the display, every boot
  ended in a reset as the rail came up; brought up after it, the board boots
  on USB alone. Why touching the bus early caused the reset is not
  established -- the board notes record what is known and what it binds
  phase 5 to.
- **Verified on the board, 2026-10-16: a software reset keeps the sensors'
  supply.** `restart` at the console (`esp_restart()`) left the blue LED on
  the switched `3V3` lit, as ESP-IDF's source predicted: it resets the CPUs,
  not the GPIOs that hold `PWR_EN`. So the stop at init is needed, not
  merely defensive.
- **Not verified on the board**: anything the driver itself does with the
  real SCD41 -- its variant, the three ASSUMED behaviours, the warm-up, the
  timing on the real bus. Each surfaces in the first logs once a
  composition runs the source on the board.
- **The `i2c.master` driver logs every NACK** (ADR 0011). A single shot read
  inside the grace, or a sensor that is unplugged, will put one such line in
  the log per attempt. Whether to quieten that tag is decided with the
  composition that first polls the sensor.
