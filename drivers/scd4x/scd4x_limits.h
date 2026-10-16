// SPDX-License-Identifier: Apache-2.0
//
// Every number the SCD4x driver (scd4x_source.c, scd4x_hal.c) works with
// that is not a command code, in one place, each marked with where it comes
// from:
//
//   DATASHEET  Sensirion SCD4x datasheet, version 1.7, April 2025, with the
//              table or section it is in
//   ESTIMATE   no source gives it; a reasoned guess, naming what replaces it
//   CHOICE     not a property of the sensor at all, but this driver's own
//              decision
//
// Command execution times are not here. Sensirion's code
// (vendor/scd4x_i2c.c) waits them itself, and the fake bus's SCD41 model
// (platform/host/i2c_models/scd41_model.c) holds the datasheet's figures
// independently -- so a vendor wait shorter than the datasheet makes the
// model refuse the next transfer and a test fail, rather than passing
// silently because both sides read the same constant.

#ifndef HAC_DRIVERS_SCD4X_SCD4X_LIMITS_H
#define HAC_DRIVERS_SCD4X_SCD4X_LIMITS_H

// DATASHEET, Table 7: the sensor reaches idle at most 30 ms after its supply
// rises. On this board the supply is the switched 3V3 that EPDiy's init
// raises (docs/hardware/board_notes.md, "Power rails and connectors"), and a
// driver cannot know how recently that happened, so init() waits this out
// first.
#define SCD4X_POWER_UP_MS 30u

// DATASHEET, 3.6.1: the periodic measurement's signal update interval.
#define SCD4X_PERIODIC_INTERVAL_MS 5000u

// DATASHEET, Table 9: measure_single_shot's maximum duration.
#define SCD4X_SINGLE_SHOT_MS 5000u

// DATASHEET, Table 1: the CO2 output range is 0 - 40,000 ppm.
#define SCD4X_CO2_MAX_PPM 40000u

// DATASHEET, 3.11.2: a reading with CO2 = 0 ppm is how the sensor reports
// "no CO2 value" (measure_single_shot_rht_only returns exactly that). Real
// air is never near 0 ppm, so 0 is an error, not a reading, and the lowest
// value accepted is 1.
#define SCD4X_CO2_MIN_PPM 1u

// DATASHEET, Table 3: the temperature measurement range, -10 to 60 C.
#define SCD4X_TEMPERATURE_MIN_C (-10.0f)
#define SCD4X_TEMPERATURE_MAX_C 60.0f

// DATASHEET, Table 2: the humidity measurement range, 0 to 100 %RH. The
// conversion (100 * word / 65535) cannot leave it; the check stays, so a
// changed conversion cannot slip past it.
#define SCD4X_HUMIDITY_MIN_RH 0.0f
#define SCD4X_HUMIDITY_MAX_RH 100.0f

// DATASHEET, 3.7.5: set_ambient_pressure accepts 70,000 - 120,000 Pa.
#define SCD4X_AMBIENT_PRESSURE_MIN_HPA 700.0f
#define SCD4X_AMBIENT_PRESSURE_MAX_HPA 1200.0f

// ESTIMATE: how long after the first start() following init() readings are
// reported as VAL_WARMUP rather than VAL_OK. The datasheet gives no warm-up
// for periodic measurement, and version 1.6 removed its earlier advice to
// discard the first single shot after a power cycle. 60 s is one CO2
// response time (tau63, Table 1); temperature and humidity settle more
// slowly still (90 s and 120 s), as the sensor's own 15 mA warms its
// surroundings. On this board every hardware reset power-cycles the sensor
// (docs/hardware/board_notes.md), so the warm-up recurs at every such boot.
// Replaced from the first cold-start readings on the board by where the
// readings stop drifting: phase 3c composes the driver into the firmware,
// whose once-a-minute sample lines carry them (docs/hardware/board_notes.md,
// "Checks still to do on the board").
#define SCD4X_WARMUP_MS 60000u

// ESTIMATE: how long past a single shot's 5000 ms a refused address still
// means "measuring", not "gone". The datasheet's 5000 ms is a maximum, so a
// sensor still busy after it is out of specification -- but it is the
// sensor's clock against this board's, and nothing measures how far apart
// they run. Replaced, like the warm-up, from the board: single shot is phase
// 5's mode, so by that phase at the latest. Two shots seen so far, both
// ready at the first read after 5000 ms on this board's clock, so the grace
// was never needed (the test app, 2026-10-16) -- too few to drop it.
#define SCD4X_BUSY_GRACE_MS 1000u

// CHOICE: how soon to ask again after the sensor said "not yet" -- a
// get_data_ready_status that was not ready, or a refused read. Sensirion's
// example polls every 100 ms; a quarter of a second costs one 1 ms command
// per poll and is still a twentieth of the 5 s interval.
#define SCD4X_NOT_READY_RETRY_MS 250u

// CHOICE: the deadline handed to port_i2c for each transfer. The longest
// transfer this driver makes is nine bytes plus an address, about 1 ms at
// 100 kHz; 20 ms leaves two 10 ms scheduler ticks of margin, as
// BOARD_I2C_PROBE_TIMEOUT_MS does, and bounds the damage of a stalled bus.
#define SCD4X_TRANSFER_TIMEOUT_MS 20u

#endif  // HAC_DRIVERS_SCD4X_SCD4X_LIMITS_H
