// SPDX-License-Identifier: Apache-2.0
//
// Every number the BME68x driver (bme68x_source.c, bme68x_hal.c) works with
// that is not a register address, in one place, each marked with where it
// comes from:
//
//   DATASHEET  Bosch BME680 datasheet, revision 1.9 (BST-BME680-DS001-09,
//              February 2024), with the table or section it is in
//   ESTIMATE   no source gives it for this sensor on this board; a reasoned
//              guess, naming what replaces it
//   CHOICE     not a property of the sensor at all, but this driver's own
//              decision
//   PHYSICS    a constant of nature, with its reference
//
// The three ESTIMATEs that shape what the screen shows -- the heater
// profile, the gas burn-in and the self-heating offset -- are phase 3c's to
// replace: with BSEC's own heater profile and corrections if BSEC is linked,
// and from this unit's readings on the board either way.
//
// The prefix is HAC_BME68X_, not BME68X_: Bosch's headers own that one
// (BME68X_MIN_TEMPERATURE, BME68X_PERIOD_POLL, ...), and a name that collides
// with a future vendor version would be silently redefined.

#ifndef HAC_DRIVERS_BME68X_BME68X_LIMITS_H
#define HAC_DRIVERS_BME68X_BME68X_LIMITS_H

#include "bme68x_defs.h"

// DATASHEET, Table 1: t_startup, the time to first communication after the
// supply rises. On this board the supply is the switched 3V3 that EPDiy's
// init raises (docs/hardware/board_notes.md, "Power rails and connectors"),
// and a driver cannot know how recently that happened, so init() waits this
// out first. Bosch's soft reset, which init() also issues, waits its own
// 10 ms (BME68X_PERIOD_RESET) after it.
#define HAC_BME68X_POWER_UP_MS 2u

// DATASHEET, "Key features" and Tables 6-8: the operating range, -40 to
// 85 C, 0 to 100 %RH, 300 to 1100 hPa. A reading outside it is VAL_ERROR.
// Judged on what the die reports, before the self-heating offset below.
// Bosch's compensation already clamps humidity to 0..100; the check stays,
// so a changed compensation cannot slip past it.
#define HAC_BME68X_TEMPERATURE_MIN_C (-40.0f)
#define HAC_BME68X_TEMPERATURE_MAX_C 85.0f
#define HAC_BME68X_HUMIDITY_MIN_RH 0.0f
#define HAC_BME68X_HUMIDITY_MAX_RH 100.0f
#define HAC_BME68X_PRESSURE_MIN_HPA 300.0f
#define HAC_BME68X_PRESSURE_MAX_HPA 1100.0f

// CHOICE: oversampling, as the datasheet's own quick-start example sets it
// (3.2.1): temperature x2, pressure x16, humidity x1. Bosch's codes, from
// bme68x_defs.h. With them one forced measurement takes 42.59 ms of
// conversions by bme68x_get_meas_dur(), plus the heater's time below.
#define HAC_BME68X_OS_TEMPERATURE BME68X_OS_2X
#define HAC_BME68X_OS_PRESSURE BME68X_OS_16X
#define HAC_BME68X_OS_HUMIDITY BME68X_OS_1X

// CHOICE: the IIR filter off. It smooths temperature and pressure over
// several measurements, and how often this source measures is its caller's
// business; smoothing, where it is wanted, belongs above the driver.
#define HAC_BME68X_FILTER BME68X_FILTER_OFF

// ESTIMATE: the heater profile -- 300 C for 100 ms, the datasheet's own
// quick-start example (3.2.1), whose gas_wait_0 of 0x59 encodes 100 ms
// exactly. The datasheet says only that "approximately 20-30 ms" bring the
// plate to temperature (3.3.5) and that typical targets are 200-400 C; it
// recommends no profile. Replaced in phase 3c, by BSEC's own profile if
// BSEC is linked, or from this unit's gas readings if it is not.
#define HAC_BME68X_HEATER_TEMPERATURE_C 300u
#define HAC_BME68X_HEATER_DURATION_MS 100u

// ESTIMATE: the ambient temperature the heater's target resistance is
// computed for (datasheet 3.3.5, amb_temp). It is set once, at init(),
// before any reading exists. The error it leaves is small: by the
// datasheet's formula and the model's published calibration
// (platform/host/i2c_models/bme680_model.h), 5 C of ambient error moves the
// heater's target by about 1 C of its 300. Replaced in phase 3c, which may
// feed the measured temperature back before each measurement.
#define HAC_BME68X_HEATER_AMBIENT_C 25

// ESTIMATE: how long after the first start() following init() gas readings
// are VAL_WARMUP rather than VAL_OK. A metal-oxide layer drifts for minutes
// after its heater starts cycling, and the datasheet gives no figure: BSEC
// reports its own "run in status" instead (Table 18). 300 s is what
// Pimoroni's bme680-python example burns in for before trusting the gas
// reading (examples/indoor-air-quality.py, burn_in_time). On this board a
// hardware reset power-cycles the sensor, so the burn-in recurs at every
// such boot; a software reset does not, but the driver cannot tell the two
// apart and counts it anyway. Replaced in phase 3c from this unit's gas
// readings after a cold start, by where they stop drifting.
#define HAC_BME68X_GAS_BURN_IN_MS 300000u

// ESTIMATE: how much warmer the die is than the air around it, subtracted
// from every temperature reading (relative humidity is moved to match, by
// the Magnus formula below). The datasheet says the internal temperature
// "is typically above ambient" (Table 8, note 15) and gives no figure.
// Back-of-envelope, not measured: the heater draws about 12 mA at 320 C
// (Table 2), some 40 mW from the module's 3.3 V, for 100 ms of each
// measurement; at one measurement every 5 s (the SCD4x's pace) that is
// 0.8 mW on average, and a few hundred K/W from a small breakout to still
// air makes it a few tenths of a degree. Heat from the board and the
// enclosure is not in it. Replaced in phase 3c by the difference against a
// reference thermometer and the SCD41 once both run on the board
// (docs/requirements.md 13.2), or by BSEC's heat-compensated output.
#define HAC_BME68X_SELF_HEATING_C 0.3f

// PHYSICS: the Magnus formula for the saturation vapour pressure over
// water, e_s(t) = 6.112 hPa * exp(17.62 t / (243.12 + t)), with the
// coefficients the WMO Guide to Instruments and Methods of Observation
// (WMO-No. 8) gives. Only their ratio at two temperatures is used, so the
// 6.112 hPa cancels and is not here.
#define HAC_BME68X_MAGNUS_B 17.62f
#define HAC_BME68X_MAGNUS_C_C 243.12f

// CHOICE: how far past the expected end of a forced measurement the sensor
// may still report no new data before that is a failure rather than "not
// yet". The datasheets give no conversion time at all; the expected end is
// Bosch's estimate (bme68x_get_meas_dur()). A second is seven times the
// whole measurement.
#define HAC_BME68X_MEASUREMENT_GRACE_MS 1000u

// CHOICE: how soon to ask again after the sensor said "not yet". Bosch's
// bme68x_get_data() has already polled five times, 10 ms apart, inside the
// call that said so.
#define HAC_BME68X_NOT_READY_RETRY_MS 100u

// CHOICE: the deadline handed to port_i2c for each transfer. The longest
// transfer the driver makes is Bosch's 23-byte calibration read, about
// 2.5 ms at 100 kHz; 20 ms leaves two 10 ms scheduler ticks of margin, as
// SCD4X_TRANSFER_TIMEOUT_MS does.
#define HAC_BME68X_TRANSFER_TIMEOUT_MS 20u

#endif  // HAC_DRIVERS_BME68X_BME68X_LIMITS_H
