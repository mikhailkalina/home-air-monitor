# 18. Ambient compensation through `port_sensor`: an optional `set_ambient()`

Date: 2026-10-16

## Status

Accepted. Verified on the host against the SCD41 model. On the board the
command's acceptance is checked: the `port_sensor` contract in the test app
passed both `set_ambient()` checks in both of the SCD41's modes (2026-10-16,
ELF `6ab6ea91b...`). What it does to the CO2 reading is not measured.

## Context

The SCD4x's CO2 reading depends on the air's pressure: the sensor assumes
sea level unless it is told an altitude or, better, the current pressure,
which `set_ambient_pressure` takes even while it measures (its datasheet,
3.7.5, which recommends updating it where the pressure changes). On this
board the pressure is measured -- by the BME68x, since phase 3b -- and
`scd4x_source_set_ambient_pressure()` existed with nothing calling it.

Connecting the two was this phase's to decide (requirements §13.1 asks for
the SCD41's calibration to be handled, and an NDIR reading scales roughly
with the pressure: about 1 % per 10 hPa away from the 1013 hPa it assumes).
Three ways were open:

1. **The composition calls the driver directly**, with the merged pressure,
   from a hook in the event loop. It works, but it routes a sensor's input
   around the port the core drives every sensor through: the simulator's
   replay mode, the contract tests and any future source would know nothing
   of it, and the composition would have to track re-inits -- the SCD4x's
   `init()` reinitialises the sensor, which drops the pressure back to its
   default -- that only the sensor manager sees.
2. **An open item**, with the CO2 left uncompensated.
3. **Widen `port_sensor`** with an optional entry point, and let the sensor
   manager hand over what it merged.

## Decision

Option 3. `sensor_source_t` gains

```c
typedef struct {
    measurement_f32_t pressure_hpa;
} sensor_ambient_t;

hal_status_t (*set_ambient)(sensor_source_t *self, const sensor_ambient_t *ambient);
```

**Optional**: `NULL` for a source that corrects for nothing -- the BME68x,
the replay sources. A field that is not `VAL_OK` is ignored, so a source never
acts on a number nobody measured. `HAL_OK` when taken or when nothing concerns
it; `HAL_ERR_INVALID_ARG` for a value outside what the source accepts (the
SCD4x: 700-1200 hPa); `HAL_ERR_BUSY` when it cannot take one now (the SCD4x
measuring a single shot); `HAL_ERR_NOT_READY` before `init()`. What it was
handed is lost by `init()`.

**The sensor manager hands it over** (`core/app/sensor_manager.c`), with the
merged pressure, to every initialised source that has the entry point: at the
first poll after each `init()`, and again whenever the pressure has moved by
at least `pressure_delta_hpa` (1 hPa -- the SCD4x takes whole hectopascals)
since that source last took it. `HAL_ERR_BUSY` and bus failures are asked
again at the next poll; an `HAL_ERR_INVALID_ARG` value is not offered again
until the pressure moves.

Pressure only, for now. The struct is the place for the next quantity a
source compensates for, when one appears: not the BME68x's heater ambient
temperature, which is the BME68x's own reading and stays inside its driver.

## Consequences

- One more entry point in the port's vtable, and two checks in its contract
  where a source has it: an unmeasured pressure is ignored, a sea-level one
  taken. Each has a mutant that fails exactly it. The host checks the value
  reaching the SCD41 model in whole hectopascals, and a refused one leaving
  the sensor's setting alone; `test_scenario_world.c` checks that the
  BME68x's pressure reaches the SCD41 model through the manager over the real
  drivers.
- `fault_inject` offers `set_ambient()` exactly when the source it wraps
  does.
- Each change of a hectopascal costs one 1 ms command on the bus. In this
  room that is a few a day.
- **Not measured**: whether compensation moves the board's CO2 reading the
  way the datasheet says. The `ambient handed over` log line shows what was
  sent; reading it back (`get_ambient_pressure`) and comparing CO2 at two
  pressures are left to the board.
