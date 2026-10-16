# 16. The BME68x driver: Bosch's Sensor API over `port_i2c`, and a BME680 model whose raw values invert the compensation

Date: 2026-10-15

## Status

Accepted. Verified on the host only: no composition runs the driver on the
board yet. The board's boot scan has seen the sensor answer at `0x77`
(2026-10-16, `docs/hardware/board_notes.md`, "Sensors on the bus"), nothing
more.

**Update 2026-10-16, phase 3c:** the driver met the board's sensor in the
test app's `port_sensor` contract (ELF `6ab6ea91b...`) and passed every
check. It is a BME680 (`variant_id=0x00`), its calibration is now the host
model's default, and its first two measurements after init reported the gas
valid but the heater not stable -- the first evidence against the 100 ms
heater ESTIMATE (`docs/hardware/board_notes.md`, "Checks still to do on the
board", item 5).

## Context

Phase 3b brings the second sensor, the BME68x: temperature, humidity,
pressure and a metal-oxide gas sensor (`docs/requirements.md` §4.1, §6.3,
§13.2, §13.3). Which part sits on this module -- a BME680 or a BME688 -- is
not established (board notes, open items).

Three facts shape the driver:

- **IAQ needs BSEC**, Bosch's proprietary library, which is not linked
  (`THIRD_PARTY_NOTICES.md`). Without it the sensor still measures its gas
  plate's resistance, and requirements §17.2 asks for that in the MVP: "show
  raw gas resistance or a simple VOC indicator". Until now nothing produced
  it, and the screen's IAQ cell said "n/a" for good.
- **The sensor is a register file with a forced mode**: configure, write
  `mode = 01`, wait for the conversions and the heater, read 17 bytes. Its
  raw values only mean something through a compensation that uses 26
  calibration parameters held in the sensor's NVM.
- **The board's supply and resets** (board notes, "Power rails and
  connectors"): the sensor runs from the switched `3V3` that EPDiy's init
  raises, a hardware reset power-cycles it, and a software reset leaves it
  powered and configured as the last firmware left it.

References: the BME680 datasheet, revision 1.9 (BST-BME680-DS001-09,
February 2024); the BME688 datasheet, revision 1.3, for the variant id.

## Decision

### Bosch's Sensor API, vendored, behind callbacks of our own

`boschsensortec/BME68x_SensorAPI` tag `v4.4.8` (commit `80ea120`,
BSD-3-Clause) is vendored unmodified into `drivers/bme68x/vendor/`, with its
provenance and file hashes in that directory's `README.md`. It is the
manufacturer's code for the manufacturer's compensation, and it handles the
BME680 and the BME688 alike, which matters while the variant is unknown.

Unlike Sensirion's, Bosch's HAL takes a context: every bus and delay
callback receives the device's `intf_ptr`. So `drivers/bme68x/bme68x_hal.c`
has no globals and no claim; two BME68x on one bus (`0x76` and `0x77`)
would be two contexts, and a test runs exactly that. A read is one
`write_read()`; a write is one `write()` whose data Bosch has already
interleaved into (address, data) pairs, which is what the sensor expects
(datasheet 6.2.1). The callbacks return the port's `hal_status_t` cast to
Bosch's `int8_t`, and Bosch keeps that value in `intf_rslt` -- so the real
status survives without a side channel.

**Delays round up to whole milliseconds.** Bosch asks for microseconds and
`port_clock` offers milliseconds; every wait Bosch asks for is a minimum, so
1 us waits 1 ms and 1001 us waits 2 ms. Bosch's own waits are 10 ms
multiples, so in practice only the waits this driver computes are
lengthened, by under a millisecond.

Built with Bosch's floating-point output (`BME68X_USE_FPU`, its default),
which the ESP32-S3 computes in hardware; `bme68x_source.c` refuses to build
without it. The vendored code needs one relaxation, `-Wno-conversion`, which
the vendor list already had for Sensirion.

### `bme68x_source`: forced mode, no IAQ

In `drivers/bme68x/bme68x_source.c` (ADR 0014 for why there).

- **Capabilities**: temperature, humidity, pressure, gas resistance. Not
  `SENS_CAP_IAQ` or `SENS_CAP_VOC`: without BSEC there is no such number.
- **Forced mode, one measurement per `start()`**, which returns at once;
  `ready_in_ms()` gives Bosch's estimate of the conversions plus the
  heater's time, rounded up -- 143 ms with this configuration; `read()`
  before then returns `HAL_ERR_NOT_READY` without touching the bus. How often
  to measure is the caller's choice: forced mode has no pace of its own.
- **`init()`**: wait the 2 ms start-up time (the supply may have just
  risen), then `bme68x_init()` -- a soft reset first, so a sensor a software
  reset left configured or measuring starts from its power-on state as one a
  hardware reset left does -- chip id, variant id, calibration. Log the chip
  id, the variant (`BME680`, `BME688` or `unknown`, from register `0xF0`) and
  the measurement time. A chip id other than `0x61` -- a BMP280 can also sit
  at `0x77` -- is `HAL_ERR_UNSUPPORTED`. Each failure names its step.
- **Bosch's unbounded wait is never entered blind.** `bme68x_set_op_mode()`,
  which every configuration call goes through, loops until the sensor
  reports sleep mode, with no bound. A sensor that never finishes a
  measurement would hang it. So the source reads the mode itself first: in
  `init()` after the reset (still measuring means the reset did not take,
  `HAL_ERR_IO`) and in every `start()` (`HAL_ERR_BUSY`).
- **A gas value is a reading only if the sensor marked it both valid and
  heater-stable** (datasheet 3.4). Otherwise it is `VAL_ERROR`, logged once
  per streak with the two bits, which tell a short heater profile from a gas
  conversion that did not run.
- **Ranges**: the datasheet's operating range, -40..85 C, 0..100 %RH,
  300..1100 hPa, on what the die reports. A temperature outside it takes
  humidity and pressure with it, since both are compensated with it; the gas
  is judged alone.
- **Late measurements**: Bosch's `bme68x_get_data()` itself polls five
  times, 10 ms apart, before it says there is nothing new, so a sensor a
  little slower than the estimate is read on time. Later than that is "not
  yet" for `HAC_BME68X_MEASUREMENT_GRACE_MS` past the estimate, then
  `HAL_ERR_TIMEOUT`, and the measurement is abandoned.
- **Failures**: `HAL_ERR_NOT_FOUND`, `HAL_ERR_IO`, `HAL_ERR_TIMEOUT` with all
  four quantities `VAL_ERROR`, logged once per streak, as the SCD4x does.

### Three estimates, in one place, for phase 3c

`drivers/bme68x/bme68x_limits.h` marks every number DATASHEET, ESTIMATE,
CHOICE or PHYSICS. Three estimates shape what the screen shows, and phase
3c replaces them -- with BSEC's own profile and corrections if BSEC is
linked, and from this unit's readings on the board either way:

- **The heater profile**, 300 C for 100 ms: the datasheet's own quick-start
  example (3.2.1); it recommends none.
- **The gas burn-in**, 300 s of `VAL_WARMUP` after the first `start()`:
  what Pimoroni's `bme680-python` example burns in for. The datasheet has no
  figure; BSEC reports a "run in status" of its own.
- **The self-heating offset**, 0.3 C off every temperature, with relative
  humidity moved to the corrected temperature by the Magnus formula so the
  two stay consistent. The datasheet says only that the internal
  temperature "is typically above ambient"; 0.3 C is a back-of-envelope from
  the heater's power and duty cycle, written out at its definition.

### A BME680 model whose raw values come from inverting the compensation

`platform/host/i2c_models/bme680_model.c`, on 3a's device-model interface:
the chip and variant ids, the calibration NVM encoded at the datasheet's
addresses from a parameter set, the control registers with only the bits
the datasheet defines, writes as (address, data) pairs and reads
auto-incremented, the forced-mode state machine with `measuring`,
`gas_measuring` and `new_data`, the mode returning to sleep by itself,
skipped measurements reading `0x80000`, the resolution following the
oversampling, `gas_valid` and `heat_stab`, the soft reset, and counters for
everything a driver must not do (read the data while measuring, write while
measuring, write a read-only register).

**Raw values are made by inverting the datasheet's floating-point
compensation by bisection**, each curve monotonic over its ADC's range
(the model's test sweeps every code to show it). Bosch's code implements the
same formulas step for step, so a driver reading the model gets its target
back by construction. **Host tests against it prove the plumbing -- register
addresses, framing, byte order and bit packing, status bits, timing, error
paths -- not Bosch's arithmetic.** The model says so in its header. Two
outside checks reach the arithmetic: the compensation reproduces the test
vectors Pimoroni publishes for the same calibration (computed there with the
datasheet's integer formulas, so to within 5 Pa and 0.01 %RH, not exactly),
and the heater code Bosch writes equals the datasheet's own formula.

**The calibration is a published one**: a real BME680's, from
`pimoroni/bme680-python` (`tests/conftest.py`, commit `30fcb34`), credited
there to that project's issue #11. Phase 3c replaces it with this unit's.

Where the datasheets are silent, the model says ASSUMED. The one that
matters most: **neither the BME680 nor the BME688 datasheet gives a
conversion time**, so the model takes Bosch's (`bme68x_get_meas_dur()`: 1963
us per oversampling sample, and so on). The timing tests therefore show that
the driver waits what the vendor estimates plus the heater's time, rounded
the right way -- not that the estimate is right. The others: the address is
refused for the start-up time; a soft reset works mid-measurement;
`new_data` clears when the next measurement starts; the heater is stable
after 30 ms (3.3.5: "approximately 20-30 ms"); the gas ADC range is the one
whose middle is nearest the reading among those that can hold it; the IIR
filter is not modelled; writes during a measurement, mode changes included,
are dropped.

Only the BME680's gas registers are modelled. A test may set the BME688's
variant id to see the driver name it; the BME688's own gas registers are
not there.

### The screen says something about air quality without BSEC

`core/ui/view_model.c`: when IAQ is `VAL_UNAVAILABLE` and gas resistance is
not, the IAQ cell shows gas resistance in kOhm, labelled "Gas resistance"
and never as IAQ (requirements §13.3), with the gas's own status treatment --
"warm" through the burn-in, "ERR" when not valid. An IAQ in any other status
keeps the cell. The "updated" line and the fault counts follow what the cell
shows. The three existing golden frames are unchanged; a fourth,
`screen_home_no_bsec`, differs from `screen_home_normal` in that cell only.
The simulator, whose scenarios carry `gas_res_ohm` and no IAQ, now shows it
too. Since 2026-10-16 the label is "Gas": at the home screen's larger type
(ADR 0019's branch, `core/ui/screen_home.h`) "Gas resistance" no longer fits
a quarter of the panel, and the kOhm under the number says the rest.

## Consequences

- **Verified on the host** (2026-10-15): `test_bme680_model` (the model
  against the datasheet's tables, Pimoroni's vectors, and a round trip over
  the operating range and every gas range boundary), `test_bme68x_source`
  (every failure path above, every call of `init()` refused in turn, the
  range limits, the gas bits and burn-in boundary, the self-heating
  arithmetic, two sensors on one bus, the HAL glue), the `port_sensor`
  contract against `bme68x_source`, and the view model and golden frames.
  Bosch's driver reads its calibration back from the model's NVM exactly,
  writes the datasheet's own heater code and `gas_wait_0 = 0x59`, and never
  reads early, writes while measuring or touches a read-only register.
  Seen to fail when they should: dropping the heater's time from the wait
  fails the unit tests and the contract (the model counts the early reads);
  rounding the HAL's delays down fails the HAL's own test -- only there,
  since Bosch's own waits are whole multiples of 10 ms.
- **Verified for the ESP32-S3**: the firmware and the test app build with
  the project's warning set.
- **Not verified on the board**: everything the driver does with the real
  sensor -- its variant, the conversion time, whether the heater is stable in
  100 ms, how far the die runs above the air, how long the gas takes to
  settle. Each surfaces in the first logs and minutes of readings once a
  composition runs the source; the estimates are then replaced (phase 3c).
- **Bosch's soft-reset result is ignored** by `bme68x_init()`. A sensor whose
  reset is refused but which was not measuring is reconfigured completely by
  `init()` anyway; one still measuring is caught by the mode check.
- **A BME68x that lost power on its own between two measurements** would come
  back unconfigured, and nothing here notices. On this board it cannot: its
  rail drops only with a hardware reset, which restarts the firmware too.
- **The heater's ambient temperature is fixed at 25 C** at init. By the
  datasheet's formula 5 C of error moves the heater's target by about 1 C;
  phase 3c may feed the measured temperature back.
- **The SCD4x's ambient-pressure input** (`scd4x_source_set_ambient_pressure()`)
  now has a source on the board; wiring the one to the other belongs to the
  composition.
