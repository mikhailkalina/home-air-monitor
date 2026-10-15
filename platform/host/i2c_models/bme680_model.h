// SPDX-License-Identifier: Apache-2.0
//
// A BME680 on the fake I2C bus, modelled at the level of its registers
// (Bosch BME680 datasheet, revision 1.9, BST-BME680-DS001-09, February
// 2024): its identity, the calibration NVM, the control registers, and the
// forced-mode measurement with its status bits, whose raw ADC values come
// from a reading given in physical units.
//
// What it is a test of. Raw values are made by INVERTING the compensation
// -- the datasheet's floating-point formulas (3.3.1-3.3.3, 3.4.1), which
// Bosch's vendor code (drivers/bme68x/vendor/bme68x.c, BME68X_USE_FPU)
// implements step for step -- by bisection, since each is monotonic over
// its ADC's range (test_bme680_model.c sweeps them). So a driver that reads
// this model and compensates with Bosch's code gets its target back by
// construction. Host tests against it prove the PLUMBING: register
// addresses, the I2C framing, byte order and bit packing of the NVM and the
// data registers, the status bits, the timing and the error paths. They do
// NOT prove Bosch's arithmetic, which is checked only where an outside
// source gives numbers: the compensation against Pimoroni's published test
// vectors for the same calibration (below), and the heater code against the
// datasheet's formula.
//
// Where the datasheet is silent the model chooses, and says so with ASSUMED
// at the code that does it. The ones a driver could notice:
//
//   * the address is refused for the start-up time after power-up and after
//     a soft reset (Table 1 gives the time, not what the bus sees);
//   * a soft reset takes effect even while a measurement runs;
//   * the measurement takes the time Bosch's bme68x_get_meas_dur() says it
//     does: 1963 us per oversampling sample, 1908 us of switching, 2385 us
//     for the gas conversion slot and 1 ms to wake, plus the heater's time.
//     Neither the BME680 nor the BME688 datasheet gives a conversion time,
//     and no other source for this part does. So the model checks that a
//     driver waits what the vendor estimates -- not that the estimate is
//     right. A newer vendor version that changes it must change this.
//   * the heater reaches its target if it has 30 ms (3.3.5: "approximately
//     20-30 ms are necessary");
//   * new_data_0 is cleared when the next measurement starts, not on read;
//   * the gas ADC range is the one whose middle is nearest the reading;
//   * the IIR filter is not modelled: with it on, temperature and pressure
//     come at its 20-bit resolution but unfiltered;
//   * register writes while a measurement runs are dropped. The datasheet
//     (3.1) says a mode change is deferred to the end of the measurement and
//     other control writes are ignored until then; this model drops the mode
//     change too. A driver that relies on either is wrong for this model,
//     and the counter shows it.
//
// The calibration is a published one, not this board's: phase 3c replaces
// it with the unit's own, read off the board once a composition runs the
// driver there.
//
// Only the BME680's gas registers are modelled. A test may set variant_id to
// the BME688's 0x01 to see what a driver reports for it, but Bosch's driver
// then reads the BME688's gas registers (0x2C/0x2D) and the high-variant run
// bit, which this model does not have: identity only.

#ifndef HAC_PLATFORM_HOST_I2C_MODELS_BME680_MODEL_H
#define HAC_PLATFORM_HOST_I2C_MODELS_BME680_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "i2c_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Identity (datasheet 5.3.1.6; BME688 datasheet 5.3.2.1 for the variant).
#define BME680_MODEL_CHIP_ID 0x61u
#define BME680_MODEL_VARIANT_BME680 0x00u  // no such register on a BME680; Bosch reads 0 as one
#define BME680_MODEL_VARIANT_BME688 0x01u

// Timing (see the header comment for which are ASSUMED).
#define BME680_MODEL_START_UP_MS 2u  // Table 1, t_startup
#define BME680_MODEL_WAKE_UP_US 1000u
#define BME680_MODEL_SAMPLE_US 1963u
#define BME680_MODEL_SWITCHING_US 1908u
#define BME680_MODEL_GAS_SLOT_US 2385u
#define BME680_MODEL_HEAT_UP_MS 30u

// Register map, Table 20 (I2C addresses).
#define BME680_MODEL_REG_MEAS_STATUS_0 0x1Du
#define BME680_MODEL_REG_PRESS_MSB 0x1Fu
#define BME680_MODEL_REG_TEMP_MSB 0x22u
#define BME680_MODEL_REG_HUM_MSB 0x25u
#define BME680_MODEL_REG_GAS_R_MSB 0x2Au
#define BME680_MODEL_REG_GAS_R_LSB 0x2Bu
#define BME680_MODEL_REG_IDAC_HEAT_0 0x50u
#define BME680_MODEL_REG_RES_HEAT_0 0x5Au
#define BME680_MODEL_REG_GAS_WAIT_0 0x64u
#define BME680_MODEL_REG_CTRL_GAS_0 0x70u
#define BME680_MODEL_REG_CTRL_GAS_1 0x71u
#define BME680_MODEL_REG_CTRL_HUM 0x72u
#define BME680_MODEL_REG_STATUS 0x73u
#define BME680_MODEL_REG_CTRL_MEAS 0x74u
#define BME680_MODEL_REG_CONFIG 0x75u
#define BME680_MODEL_REG_CHIP_ID 0xD0u
#define BME680_MODEL_REG_RESET 0xE0u
#define BME680_MODEL_REG_VARIANT_ID 0xF0u

// meas_status_0 bits (5.3.5) and gas_r_lsb bits (5.3.4.5, 5.3.5.5-6).
#define BME680_MODEL_NEW_DATA 0x80u
#define BME680_MODEL_GAS_MEASURING 0x40u
#define BME680_MODEL_MEASURING 0x20u
#define BME680_MODEL_GAS_VALID 0x20u
#define BME680_MODEL_HEAT_STAB 0x10u

#define BME680_MODEL_SOFT_RESET_CMD 0xB6u  // 5.3.1.5
#define BME680_MODEL_MODE_FORCED 0x01u     // 5.3.1.3

// The calibration parameters of 3.3.1-3.4.1 and Tables 11-15, as the
// datasheet names them.
typedef struct {
    uint16_t par_t1;
    int16_t par_t2;
    int8_t par_t3;
    uint16_t par_p1;
    int16_t par_p2;
    int8_t par_p3;
    int16_t par_p4;
    int16_t par_p5;
    int8_t par_p6;
    int8_t par_p7;
    int16_t par_p8;
    int16_t par_p9;
    uint8_t par_p10;
    uint16_t par_h1;  // 12 bits
    uint16_t par_h2;  // 12 bits
    int8_t par_h3;
    int8_t par_h4;
    int8_t par_h5;
    uint8_t par_h6;
    int8_t par_h7;
    int8_t par_g1;
    int16_t par_g2;
    int8_t par_g3;
    uint8_t res_heat_range;  // 2 bits
    int8_t res_heat_val;
    int8_t range_switching_error;  // signed 4 bits
} bme680_model_calibration_t;

// A real BME680's calibration, as published in pimoroni/bme680-python; see
// the definition for where exactly.
extern const bme680_model_calibration_t BME680_MODEL_PUBLISHED_CALIBRATION;

// One measurement, in physical units, as the sensor's die sees it -- its
// self-heating included. The model turns it into raw ADC values.
typedef struct {
    float temperature_c;
    float humidity_rh;
    float pressure_hpa;
    float gas_resistance_ohm;
} bme680_model_reading_t;

// Called once per completed measurement, at the virtual time it completes.
typedef void (*bme680_model_reading_fn)(void *ctx, uint64_t at_ms, bme680_model_reading_t *out);

// overrun_ms that means "never finishes".
#define BME680_MODEL_NEVER UINT32_MAX

// Faults to inject. Each counter is spent one event at a time.
typedef struct {
    unsigned nack_address;  // the next N transfers, either direction, have their address refused
    uint32_t nack_call;     // the address refused on this call, counted as stats.calls is; 0: none
    unsigned nack_data;     // the next N writes carrying data have their last byte refused
    bool heater_unstable;   // heat_stab_r stays 0: the plate never reaches its target
    bool gas_invalid;       // gas_valid_r stays 0: a dummy gas conversion
    uint32_t overrun_ms;    // each measurement runs this much longer; BME680_MODEL_NEVER: forever
} bme680_model_faults_t;

typedef struct {
    uint32_t calls;              // every write and read the bus made of the model
    uint32_t address_nacks;      // every refusal of the address, for any reason
    uint32_t nacked_while_busy;  // of those, inside the start-up time
    uint32_t soft_resets;
    uint32_t measurements;      // forced measurements started
    uint32_t completed;         // ... and finished
    uint32_t early_reads;       // reads into the data registers (0x1D-0x2B) while measuring
    uint32_t ignored_writes;    // register writes dropped because a measurement was running
    uint32_t read_only_writes;  // writes to a register the datasheet does not make writable
} bme680_model_stats_t;

#define BME680_MODEL_REGISTERS 256u

typedef struct {
    i2c_model_t model;

    // Identity and calibration, settable at any time; the NVM bytes are
    // encoded from `calibration` whenever they are read.
    uint8_t chip_id;
    uint8_t variant_id;
    bme680_model_calibration_t calibration;

    bme680_model_reading_fn reading;  // NULL: the constant default_reading
    void *reading_ctx;
    bme680_model_reading_t default_reading;

    bme680_model_faults_t faults;
    bme680_model_stats_t stats;

    // Internal state, readable by tests.
    uint8_t regs[BME680_MODEL_REGISTERS];  // control, data and status registers
    uint8_t pointer;                       // where the next read starts
    uint64_t busy_until_ms;                // the address is refused before this
    bool measuring;
    uint64_t measurement_started_ms;
    uint64_t gas_phase_ms;         // when the heater starts, if the gas runs
    uint64_t measurement_ends_ms;  // UINT64_MAX: never
    bme680_model_reading_t last;   // what the last completed measurement was of
} bme680_model_t;

// A sensor that was powered up at `now_ms`: the published calibration, a
// BME680's identity, every register at its reset state (Table 20), the
// default reading, and the address refused for the start-up time.
void bme680_model_init(bme680_model_t *m, uint64_t now_ms);

i2c_model_t *bme680_model_port(bme680_model_t *m);

void bme680_model_set_reading(bme680_model_t *m, bme680_model_reading_fn fn, void *ctx);

// Supply removed and restored at `now_ms`, as a hardware reset of this board
// does (docs/hardware/board_notes.md, "Power rails and connectors"): every
// register at its reset state, no measurement, and the start-up time again.
void bme680_model_power_cycle(bme680_model_t *m, uint64_t now_ms);

// The register a read at `addr` returns, as of the model's last transfer.
// No side effects; tests use it to look inside without a bus transfer.
uint8_t bme680_model_peek(const bme680_model_t *m, uint8_t addr);

// --- the datasheet's compensation, floating point ----------------------------
//
// Exposed so the tests can hold them to outside numbers, and to decode what
// the model wrote.

// 3.3.1: temperature in C from the 20-bit temp_adc; `t_fine` gets the
// intermediate the other two need.
double bme680_model_temperature(const bme680_model_calibration_t *c, uint32_t temp_adc,
                                double *t_fine);

// 3.3.2: pressure in Pa from the 20-bit press_adc.
double bme680_model_pressure(const bme680_model_calibration_t *c, uint32_t press_adc,
                             double t_fine);

// 3.3.3: relative humidity in % from hum_adc, at `temp_c`. Not clamped: the
// datasheet's formula is not.
double bme680_model_humidity(const bme680_model_calibration_t *c, uint16_t hum_adc, double temp_c);

// 3.4.1 and Table 16: gas resistance in ohms from the 10-bit gas_adc and
// gas_range.
double bme680_model_gas_resistance(const bme680_model_calibration_t *c, uint16_t gas_adc,
                                   uint8_t gas_range);

// 3.3.5: the res_heat_x code for a heater target, at an ambient temperature.
uint8_t bme680_model_res_heat(const bme680_model_calibration_t *c, double target_c,
                              double ambient_c);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_I2C_MODELS_BME680_MODEL_H
