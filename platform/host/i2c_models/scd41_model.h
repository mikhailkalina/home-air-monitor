// SPDX-License-Identifier: Apache-2.0
//
// An SCD41 on the fake I2C bus, modelled at the level of its command protocol
// (Sensirion SCD4x datasheet, version 1.7, April 2025, section 3): 16-bit
// commands; a CRC-8 after every data word in both directions; an execution
// time after each command during which the sensor must not be addressed; a
// periodic measurement that makes a new reading available every 5 s; and the
// five commands it accepts while one runs. The register-file model cannot
// express any of that, which is why phase 3 starts with this sensor
// (docs/adr/0015-scd4x-driver.md).
//
// It is a second reading of the datasheet, independent of the first: the
// driver under test is Sensirion's own code (drivers/scd4x/vendor/), and
// nothing here is taken from it -- not the CRC, not the execution times, not
// the unit conversions. Where the two disagree a test fails, and the
// datasheet decides which one is wrong.
//
// Where the datasheet is silent, the model has to choose, and says so with
// ASSUMED at the code that does it. Each is a question the real sensor
// answers on the board:
//
//   * while a command executes, the sensor refuses its address. The
//     datasheet says only that no command may be sent then (3.4);
//     ports/port_i2c.h records that several Sensirion parts refuse their
//     address while busy.
//   * a command the current state does not allow, an unknown command, a
//     wrongly sized one and an argument word with a bad CRC each have a data
//     byte refused (HAL_ERR_IO). The datasheet says such commands "may not be
//     issued" (3.6), not how they are refused.
//   * a buffered reading is discarded by stop_periodic_measurement.
//
// The sensor's own behaviour that IS specified, and how it reaches the bus:
// read_measurement with nothing in the buffer refuses the read header
// (HAL_ERR_NOT_FOUND on the read, datasheet 3.6.2); wake_up is never
// acknowledged (3.11.4).

#ifndef HAC_PLATFORM_HOST_I2C_MODELS_SCD41_MODEL_H
#define HAC_PLATFORM_HOST_I2C_MODELS_SCD41_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "i2c_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// The datasheet's figures this model keeps time by. Public so tests can wait
// them out by name.
#define SCD41_MODEL_POWER_UP_MS 30u               // Table 7, maximum
#define SCD41_MODEL_INTERVAL_MS 5000u             // 3.6.1, periodic signal update interval
#define SCD41_MODEL_LOW_POWER_INTERVAL_MS 30000u  // 3.9.1, "approximately 30 seconds"
#define SCD41_MODEL_SINGLE_SHOT_MS 5000u          // Table 9, measure_single_shot
#define SCD41_MODEL_STOP_MS 500u                  // Table 9, stop_periodic_measurement

// What the model's identity commands answer by default: the serial number of
// the datasheet's own example (Table 27) and its SCD41 variant word (Table 31).
#define SCD41_MODEL_DEFAULT_SERIAL 0xF8969F073BBEull
#define SCD41_MODEL_VARIANT_SCD40 0x0440u
#define SCD41_MODEL_VARIANT_SCD41 0x1440u

// How many accepted commands the model remembers, newest last.
#define SCD41_MODEL_HISTORY 16u

typedef enum {
    SCD41_MODEL_IDLE = 0,
    SCD41_MODEL_PERIODIC,
    SCD41_MODEL_LOW_POWER_PERIODIC,
    SCD41_MODEL_SLEEP,  // after power_down; only wake_up gets through
} scd41_model_state_t;

// One measurement, in physical units. The model converts it to the sensor's
// words with the datasheet's formulas (3.6.2), rounding to the nearest tick
// and clamping to 0..65535 -- so a test can ask for a temperature the sensor
// could report but would not be specified for.
typedef struct {
    float co2_ppm;
    float temperature_c;
    float humidity_rh;
} scd41_model_reading_t;

// Called once per completed measurement, at the virtual time it completes.
typedef void (*scd41_model_reading_fn)(void *ctx, uint64_t at_ms, scd41_model_reading_t *out);

// Faults to inject. Each counter is spent one event at a time.
typedef struct {
    unsigned nack_address;  // the next N transfers, either direction, have their address refused
    unsigned nack_data;     // the next N writes carrying data have their last byte refused
    unsigned corrupt_crc;   // the next N reads that return data have their first CRC inverted
    bool hold_data;         // measurements complete but leave nothing to read
    // A single shot executes this much longer than the datasheet's 5000 ms,
    // as a sensor whose clock runs slow against the reader's would.
    uint32_t single_shot_overrun_ms;
} scd41_model_faults_t;

typedef struct {
    uint32_t accepted;            // commands executed
    uint32_t rejected;            // commands refused with a data NACK, for any reason
    uint32_t address_nacks;       // every refusal of the address, for any reason
    uint32_t nacked_while_busy;   // of those, refused because a command was executing
    uint32_t reads_without_data;  // read headers refused because nothing was buffered
    uint32_t measurements;        // readings produced
    uint16_t history[SCD41_MODEL_HISTORY];
    size_t history_len;
} scd41_model_stats_t;

// A setting the sensor keeps in RAM and, after persist_settings, in EEPROM.
typedef struct {
    uint16_t set_code;
    uint16_t get_code;
    uint16_t value;
    uint16_t persisted;
    uint16_t factory;
} scd41_model_setting_t;

#define SCD41_MODEL_SETTINGS 6u

typedef struct {
    i2c_model_t model;

    // Identity, settable before use.
    uint64_t serial;  // 48 bits
    uint16_t variant_word;

    scd41_model_reading_fn reading;  // NULL: the constant default_reading
    void *reading_ctx;
    scd41_model_reading_t default_reading;

    scd41_model_faults_t faults;
    scd41_model_stats_t stats;

    // Internal state, readable by tests.
    scd41_model_state_t state;
    uint64_t busy_until_ms;  // the address is refused before this
    uint64_t next_measurement_ms;
    bool single_shot_pending;
    bool single_shot_rht_only;
    bool measured_since_power_up;
    bool data_ready;
    uint16_t data[3];  // CO2, temperature, humidity words
    uint8_t response[9];
    size_t response_len;
    uint16_t ambient_pressure_hpa;
    scd41_model_setting_t settings[SCD41_MODEL_SETTINGS];
} scd41_model_t;

// A sensor that was powered up at `now_ms`: idle, nothing buffered, factory
// settings, the default serial and an SCD41 variant word, and refusing its
// address for the power-up time.
void scd41_model_init(scd41_model_t *m, uint64_t now_ms);

i2c_model_t *scd41_model_port(scd41_model_t *m);

void scd41_model_set_reading(scd41_model_t *m, scd41_model_reading_fn fn, void *ctx);

// Supply removed and restored at `now_ms`, as a hardware reset of this board
// does (docs/hardware/board_notes.md, "Power rails and connectors"): idle,
// RAM settings back to what EEPROM holds, the buffer empty.
void scd41_model_power_cycle(scd41_model_t *m, uint64_t now_ms);

// Periodic measurement already running, started at `started_ms` -- what an
// ESP32 reset that does not cut the sensor's supply leaves behind.
void scd41_model_set_measuring(scd41_model_t *m, uint64_t started_ms);

// The datasheet's checksum (3.12): CRC-8, polynomial 0x31, init 0xFF.
uint8_t scd41_model_crc8(const uint8_t *d, size_t n);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_I2C_MODELS_SCD41_MODEL_H
