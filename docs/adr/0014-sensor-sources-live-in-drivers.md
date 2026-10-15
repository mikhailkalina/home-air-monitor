# 14. Sensor sources live in `drivers/`, so the host runs them over the fake bus

Date: 2026-10-14

## Status

Accepted. Implemented in phase 3a with the first of them,
`drivers/scd4x/scd4x_source.c`.

## Context

`docs/architecture.md` has two substitution points for sensor data (§5.3,
§5.4): `sensor_source_t`, which the core consumes, and `port_i2c_t`, which
drivers talk to. Its layout (§3) put the code between them -- the wrapper that
turns a driver into a `sensor_source_t` -- in
`platform/esp32_t5s3/adp_sensors.c`, and its layering rule 4 (§2.1) let
`drivers/` depend on `port_i2c.h` and `port_clock.h` only.

Phase 3 brings the first driver, and with it the question of where the
wrapper goes. The wrapper is not glue. For the SCD4x it holds:

- the start-up sequence (stop a measurement a reset left running, reinit,
  read back serial and variant -- `docs/hardware/board_notes.md` on what a
  reset does to the sensor's supply);
- when to read: the 5 s interval, the not-ready answer, the single-shot window
  inside which the sensor must not be addressed;
- what a refused address means at each point -- busy inside a window the
  wrapper opened, absent outside one;
- the datasheet's range limits, and the warm-up, which decide `VAL_ERROR` and
  `VAL_WARMUP`.

That is exactly the behaviour phase 3's brief wants exercised -- bad CRC,
NACK mid-transfer, busy NACK, not ready, out of range, a sensor still
measuring after a reset. In `platform/esp32_t5s3/` it builds only under
`idf.py`; nothing there has a host test, and the board notes already record
that gap for the one pure function that once lived there. The host's fake bus
would test Sensirion's code, but not ours.

## Decision

**A driver's `sensor_source_t` wrapper lives beside the driver, in
`drivers/<part>/`**, and builds on the host exactly as on the board, from the
same `cmake/sources_drivers.cmake`. On the host it runs over
`platform/host/adp_i2c_fake.c` with a command-level model of the part
(`platform/host/i2c_models/`); on the board over `adp_i2c`. The composition
roots (`apps/*/main.c`) construct it, with the bus, the clock, the log and the
address from `board_config.h`.

So `drivers/` now depends on `ports/port_sensor.h` too -- and through it on
`core/domain/measurement.h`, the one core header the port includes, for
`measurement_f32_t` and its constructors (`measurement_ok()`,
`measurement_status()`), which are in `hac_core`. And on `ports/port_log.h`,
since a driver reports the serial number and variant it found. Layering rule
4 now reads: **`drivers/` depend only on `ports/*.h` and the domain types
`port_sensor.h` brings with it**; no ESP-IDF, FreeRTOS, `driver/`, SDL or
stdio, which `scripts/check_sources.sh` (section 4) enforces for every
first-party file under `drivers/`, with a fixture test
(`scripts/tests/test_driver_include_check.sh`) asserting the exact violation
set. Vendored code under `drivers/*/vendor/` is exempt, as it is from every
check.

The dependency stays one-way. `core/` still knows nothing of `drivers/`; the
domain type is the vocabulary both use, which is what
`docs/requirements.md` §7.3 asked of `air_reading_t` from the start.

`platform/esp32_t5s3/adp_sensors.c` is not created. What would have lived
there is in `drivers/`; what is truly ESP32-specific about sensors -- the
bus -- is already `adp_i2c.c`.

## Consequences

- **Every failure path of a sensor source is a host test** --
  `tests/unit/test_scd4x_source.c` against the SCD41 model -- and the
  `port_sensor` contract runs against a real driver, not only against CSV
  replay (`tests/contract/test_port_sensor_contract.c`).
- **`drivers/` now depends on `hac_core`** (for `measurement.c`), in both
  builds: `hac_drivers` links it on the host, and the ESP-IDF component
  `REQUIRES hac_core`. A driver cannot be lifted out without the domain
  types; it was never meant to be.
- **A driver's behaviour is decided once.** The simulator's `--sensors
  fake-i2c` mode (architecture §5.4), when it arrives, will compose the same
  `sensor_source_t` the firmware does, over the fake bus, instead of a
  host-only copy of the wrapper.
- **Amended together:** `docs/architecture.md` §2.1 rule 4 and §3, and
  `CLAUDE.md`'s layering rules.
