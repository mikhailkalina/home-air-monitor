# 17. The sensor manager: per-source cycles, a per-field merge, and re-init with backoff

Date: 2026-10-16

## Status

Accepted. Verified on the host: unit tests over replay sources and fault
injection, and an integration test of both real drivers over the fake bus.
On the board, 2026-10-16: the extended `port_sensor` contract -- init again
without deinit included -- passes against both real sensors in the test app
(ELF `6ab6ea91b...`), and the firmware ran both under the manager for half
an hour (ELF `ea261721f...`): an unplug of both sensors showed `ERR` on the
panel at once and recovered 25 s after the replug, at the fifth init, with no
reboot (`docs/hardware/board_notes.md`, "Checks still to do on the board",
item 6).

## Context

Phase 3c puts the two sensor drivers of phases 3a and 3b on the board and
behind the core. Until now the merge lived in the simulator's `main.c`,
marked as the placeholder for `core/app/sensor_manager.c` -- one source per
quantity, last write wins, every source read every 5 s -- and the firmware
drew a synthetic sawtooth from `bringup_reading.c`. What the core has to
provide (`docs/architecture.md` §6.3, `docs/requirements.md` §4.3, §9.2,
§13.1, §15.1):

- **per-field sources with a fallback**: temperature and humidity from the
  BME68x, the SCD41's when the BME68x is down; CO2 from the SCD41 alone;
- **a sensor failure on the screen**, not a plausible old number, and not a
  hang: "the device must not hang when a single sensor fails";
- **recovery without a reboot**, when a sensor that failed -- or was
  unplugged -- answers again;
- **each source's own pace**: the SCD4x measures every 5 s by itself and is
  read when it says; the BME68x measures once per `start()`, ready about
  143 ms later.

## Decision

### The cycle per source

`sensor_manager_poll()` keeps one slot per source and drives it through the
port's life cycle: `init()` when it is first due, then per interval
`start()`, `ready_in_ms()`, `read()`. The slot keeps the source's latest
sample, restricted to the capabilities it declares -- a source that does not
claim CO2 cannot put a number in the CO2 field. The poll returns the earliest
point any slot needs attention; both event loops (the firmware's FreeRTOS
pump and the simulator's loop) sleep until the earlier of that and
`update_policy`'s deadline.

Nothing in the manager waits. `init()` is the only call that may take a
while -- the SCD4x's stop and reinit, about 0.56 s -- and only what the
drivers' own command times cost.

### Which source, per field, is the composition's

`sensor_manager_set_priority()` takes source ids per field. The core holds no
ids and no sensor names: `apps/firmware_esp32/main/main.c` and
`apps/simulator/main.c` both say "temperature and humidity: 2, then 1; CO2:
1; pressure and gas: 2", with 1 the SCD4x and 2 the BME68x. A field with no
priority takes the sources in the order they were added.

The merge, per field, takes the best status among the named sources --
`VAL_OK`, then `VAL_STALE` (aged with the manager's `max_age_ms`), then
`VAL_WARMUP`, then `VAL_ERROR`, then `VAL_UNAVAILABLE` -- and of equals the
one named first. So a working fallback beats a failed, stale or warming
primary; a stale old number beats a fallback that is only warming up; and
with both failed, the primary's error is what shows.

### Failure, and starting over

- A `read()` or `start()` that fails marks every field the source claims
  `VAL_ERROR` at once, whatever the source left in its output (a
  fault-injection wrapper leaves nothing). The next attempt is an interval
  later: a failing source is not asked more often than a working one.
- **A stall is a failure too.** A source that keeps answering "not yet" for
  `stall_timeout_ms` (30 s, the same as the max age) after a cycle began has
  failed, though it never said so -- otherwise a sensor that stopped
  producing data would never be started over. Its fields go `VAL_OK` ->
  `VAL_STALE` -> `VAL_ERROR`.
- **Three failures in a row** start the source over: `init()` again after a
  backoff of 5 s, doubling to a ceiling of 120 s for as long as no read
  succeeds. A successful read resets the count and the backoff.
- An `init()` that fails -- the sensor absent at boot, or unplugged -- goes
  straight to the backoff. Its fields are `VAL_ERROR` from the first attempt:
  the sensor was looked for and not found, which is not `n/a`.

**`init()` again, without `deinit()`, is now part of the port**
(`ports/port_sensor.h`): it must work at any time and forget the measurement
in progress, as a reset would. Both drivers already reset their state in
`init()`; the contract (`tests/contract/port_sensor_contract.c`) now checks it
on every implementation -- a second `init()`, a read that finds nothing, a
second measurement -- on the host and on the board, with a mutant for each
new check.

### The backoff ceiling, and the I2C driver's error lines

Every attempt at an absent sensor costs a few failed transfers, and for each
ESP-IDF's `i2c.master` driver logs three `E` lines -- `I2C transaction
unexpected nack detected`, then `I2C transaction failed` twice, from
`s_i2c_synchronous_transaction()` and from the public call (counted on the
board 2026-10-16, the test app's log) -- the cost ADR 0011 left to this
phase. The ceiling trades that
against how long a sensor plugged back in waits to be found: at 120 s an
absent sensor is tried 30 times an hour, a few hundred lines an hour, and a
replugged one is back within two minutes.

**The `i2c.master` tag is not quietened.** Three reasons:

1. ESP-IDF's levels cannot keep the bus faults and drop the NACKs: every line
   that driver writes on a failed transfer is `ESP_LOGE`, so the only level
   that silences the NACK lines is `NONE`, which also silences `I2C software
   timeout`, `I2C hardware timeout detected` and `clear bus failed` -- the
   lines a stuck bus is diagnosed from. `adp_i2c` logs its own classified
   failures at `DEBUG` only.
2. Those lines are evidence the board still owes: the open item "What the
   SCD41 does where its datasheet is silent" is settled by what the sensor
   does on the wire, and they are where that shows.
3. The backoff already bounds them. The sensor manager logs each change of
   state once -- `source failed` with the step and status, `source will be
   initialised again` with the wait, `source reading again` -- so the
   driver's lines are never the only record.

Revisit when a stored log (phase 5) makes volume cost something, or if the
board shows a source that fails often without being absent.

### Time after the poll

A poll takes time on the clock it is measured with: the drivers wait out
their sensors' command times through `port_clock`'s `delay_ms()`. The
simulator's fake-i2c mode showed the consequence at once: its loop aged the
reading against the time it had read *before* the poll, so every value
stamped during the poll was "from the future", of unknown age
(`measurement_age_ms()`), and stale -- the CO2 field flipped between `VAL_OK`
and `VAL_STALE` every 5 s and spent the panel's partial-refresh budget on it.
The board would have done the same. The manager merges with the time after
its calls, both loops now read the clock again after the poll, and
`test_sensor_manager.c` has a source whose `read()` waits, to keep it so.

### Replay sources: one scenario clock

A replay source used to restart its scenario at every `start()`, which the
old simulator loop called once. The manager calls `start()` every interval,
as the BME68x's forced mode needs, so the scenario clock now starts at the
first `start()` only, and `init()` forgets only the measurement in progress,
not the scenario: the room goes on whatever the sensor does.

### The simulator's fake-i2c mode

`--sensors replay|fake-i2c` (architecture §5.4). In fake-i2c mode the board's
own drivers run over `adp_i2c_fake`, at the addresses `board_config.h`
gives, and the SCD41 and BME680 models measure `platform/host/scenario_world.c`:
the same CSV files, the SCD41 seeing the CO2 file and the BME680 the
environment file, through a die that runs warmer than the room by exactly the
`HAC_BME68X_SELF_HEATING_C` its driver takes off, with its humidity moved by
the Magnus formula. A row whose status is not "ok" becomes a refused address
on the model's next transfer -- a failed read. `test_scenario_world.c` runs
both modes under the manager on one scenario and holds them to the same
readings once the drivers' warm-ups are over: whole ppm, a few hundredths of
a degree, a tenth of a hectopascal, 2 % of gas resistance.

## Consequences

- `apps/firmware_esp32/main/bringup_reading.c` and the simulator's merge are
  gone; both loops run the same core code over different sources.
- **The init order is the hardware's**, unchanged from phase 3a and now
  carrying the drivers: display adapter (EPDiy raises `PWR_EN`), or in a
  `CONFIG_HAC_DISPLAY_NULL` build `board_power_switched_3v3_on()`; then
  `BOARD_SWITCHED_3V3_SETTLE_MS`; then the bus and its scan; then the event
  loop, whose first poll initialises the sensors, each driver waiting its own
  power-up time first (30 ms for the SCD4x, its datasheet's Table 7; 2 ms
  for the BME68x, Table 1).
- **The test app raises the rail itself**, for the same reason: it never runs
  EPDiy. Without that the sensors, now wired, would hold the bus low under
  the `port_i2c` contract too.
- The event loop's redraw backoff became a point before which no redraw is
  tried, since the loop now also wakes for the sensors every few seconds.
- The firmware logs each source's latest sample once a minute (`scd4x:
  sample ...`, `bme68x: sample ...`), so the SCD41's temperature is in the
  log beside the BME68x's even while the BME68x's is the one shown -- for
  requirements §13.2's comparison.
- The event loop's task stack went from 6144 to 8192 bytes for the drivers
  and the float formatting; it logs its free-stack low-water mark at each new
  low, and in **bytes**: the old comment said `uxTaskGetStackHighWaterMark()`
  counts 32-bit words, but it counts `StackType_t`, which ESP-IDF's Xtensa
  port defines as `uint8_t`.
- Not done here: the manager does not suspend sources before deep sleep, and
  persists nothing. Both are phase 5's, with `persisted_state_t`.
