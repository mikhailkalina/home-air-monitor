// SPDX-License-Identifier: Apache-2.0
//
// The SCD41 model (platform/host/i2c_models/scd41_model.c) against the
// datasheet it claims to follow (Sensirion SCD4x, version 1.7) -- checked
// here, through the fake bus, before any driver is run against it. The
// datasheet's own examples are the fixtures wherever it gives one.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "hac_test.h"
#include "i2c_models/scd41_model.h"

#define ADDR 0x62u
#define TO_MS 20u

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    scd41_model_t sensor;
    port_i2c_t *port;
} rig_t;

static void rig_init(rig_t *r)
{
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_i2c_fake_init(&r->bus, &r->clock);
    scd41_model_init(&r->sensor, 0u);
    (void)adp_i2c_fake_attach(&r->bus, ADDR, scd41_model_port(&r->sensor));
    r->port = adp_i2c_fake_port(&r->bus);
    adp_clock_virtual_advance(&r->clock, SCD41_MODEL_POWER_UP_MS);
}

static void wait_ms(rig_t *r, uint64_t ms)
{
    adp_clock_virtual_advance(&r->clock, ms);
}

static hal_status_t command(rig_t *r, uint16_t code)
{
    const uint8_t d[2] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xFFu)};
    return r->port->write(r->port, ADDR, d, sizeof d, TO_MS);
}

static hal_status_t command_with_arg(rig_t *r, uint16_t code, uint16_t arg)
{
    uint8_t d[5] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xFFu), (uint8_t)(arg >> 8),
                    (uint8_t)(arg & 0xFFu), 0u};
    d[4] = scd41_model_crc8(&d[2], 2u);
    return r->port->write(r->port, ADDR, d, sizeof d, TO_MS);
}

static hal_status_t read_bytes(rig_t *r, uint8_t *d, size_t n)
{
    return r->port->read(r->port, ADDR, d, n, TO_MS);
}

static uint16_t word_at(const uint8_t *d)
{
    return (uint16_t)(((unsigned)d[0] << 8) | d[1]);
}

static bool crcs_valid(const uint8_t *d, size_t words)
{
    for (size_t i = 0; i < words; i++) {
        if (scd41_model_crc8(&d[3u * i], 2u) != d[3u * i + 2u]) {
            return false;
        }
    }
    return true;
}

// A command, its execution time, and its one-word response.
static bool fetch_word(rig_t *r, uint16_t code, uint16_t *out)
{
    uint8_t d[3];
    if (command(r, code) != HAL_OK) {
        return false;
    }
    wait_ms(r, 1u);
    if (read_bytes(r, d, sizeof d) != HAL_OK || !crcs_valid(d, 1u)) {
        return false;
    }
    *out = word_at(d);
    return true;
}

static void the_crc_matches_every_example_in_the_datasheet(void)
{
    static const struct {
        uint16_t word;
        uint8_t crc;
    } EXAMPLES[] = {
        {0xBEEFu, 0x92u},  // Table 40, the checksum's own example
        {0x01F4u, 0x33u},  // Table 11, read_measurement: 500 ppm
        {0x6667u, 0xA2u},  // Table 11: 25 C
        {0x5EB9u, 0x3Cu},  // Table 11: 37 %RH
        {0x8000u, 0xA2u},  // Table 25, get_data_ready_status: not ready
        {0xF896u, 0x31u},  // Table 27, get_serial_number
        {0x9F07u, 0xC2u},  // Table 27
        {0x3BBEu, 0x89u},  // Table 27
        {0x1440u, 0x51u},  // Table 31, get_sensor_variant: SCD41
        {0x0440u, 0x3Fu},  // Table 31: SCD40
        {0x03DBu, 0x42u},  // Table 17, set_ambient_pressure: 98,700 Pa
    };
    for (size_t i = 0; i < sizeof EXAMPLES / sizeof EXAMPLES[0]; i++) {
        const uint8_t d[2] = {(uint8_t)(EXAMPLES[i].word >> 8),
                              (uint8_t)(EXAMPLES[i].word & 0xFFu)};
        HAC_CHECK_EQ_INT(scd41_model_crc8(d, 2u), EXAMPLES[i].crc);
    }
}

static void the_address_is_refused_until_the_power_up_time_has_passed(void)
{
    rig_t r;
    adp_clock_virtual_init(&r.clock, 1.0);
    adp_i2c_fake_init(&r.bus, &r.clock);
    scd41_model_init(&r.sensor, 0u);
    (void)adp_i2c_fake_attach(&r.bus, ADDR, scd41_model_port(&r.sensor));
    r.port = adp_i2c_fake_port(&r.bus);

    wait_ms(&r, SCD41_MODEL_POWER_UP_MS - 1u);
    HAC_CHECK_EQ_INT(r.port->write(r.port, ADDR, NULL, 0u, TO_MS), HAL_ERR_NOT_FOUND);
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(r.port->write(r.port, ADDR, NULL, 0u, TO_MS), HAL_OK);
}

static void identity_answers_with_the_datasheet_examples(void)
{
    rig_t r;
    rig_init(&r);

    uint8_t d[9];
    HAC_CHECK_EQ_INT(command(&r, 0x3682u), HAL_OK);  // get_serial_number
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(read_bytes(&r, d, sizeof d), HAL_OK);
    static const uint8_t SERIAL[9] = {0xF8, 0x96, 0x31, 0x9F, 0x07, 0xC2, 0x3B, 0xBE, 0x89};
    HAC_CHECK(memcmp(d, SERIAL, sizeof SERIAL) == 0);

    uint16_t variant = 0u;
    HAC_CHECK(fetch_word(&r, 0x202Fu, &variant));
    HAC_CHECK_EQ_INT(variant, SCD41_MODEL_VARIANT_SCD41);
}

static void a_response_is_not_readable_during_the_execution_time(void)
{
    rig_t r;
    rig_init(&r);

    uint8_t d[3];
    HAC_CHECK_EQ_INT(command(&r, 0x202Fu), HAL_OK);
    HAC_CHECK_EQ_INT(read_bytes(&r, d, sizeof d), HAL_ERR_NOT_FOUND);  // 1 ms not yet passed
    HAC_CHECK_EQ_INT((int)r.sensor.stats.nacked_while_busy, 1);
}

static void periodic_measurement_makes_a_reading_every_five_seconds(void)
{
    rig_t r;
    rig_init(&r);

    HAC_CHECK_EQ_INT(command(&r, 0x21B1u), HAL_OK);  // start_periodic_measurement

    uint16_t ready = 0u;
    wait_ms(&r, SCD41_MODEL_INTERVAL_MS - 2u);
    HAC_CHECK(fetch_word(&r, 0xE4B8u, &ready));
    HAC_CHECK_EQ_INT(ready & 0x07FFu, 0);  // 4998 ms after the start: not yet
    HAC_CHECK_EQ_INT(ready, 0x8000);       // the datasheet's own not-ready word

    wait_ms(&r, 1u);
    HAC_CHECK(fetch_word(&r, 0xE4B8u, &ready));
    HAC_CHECK((ready & 0x07FFu) != 0);

    uint8_t d[9];
    HAC_CHECK_EQ_INT(command(&r, 0xEC05u), HAL_OK);  // read_measurement
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(read_bytes(&r, d, sizeof d), HAL_OK);
    HAC_CHECK(crcs_valid(d, 3u));
    // The default reading is the datasheet's example, 500 ppm, 25 C, 37 %RH.
    HAC_CHECK_EQ_INT(word_at(&d[0]), 500);
    HAC_CHECK_EQ_F32(-45.0f + 175.0f * (float)word_at(&d[3]) / 65535.0f, 25.0f, 0.01f);
    HAC_CHECK_EQ_F32(100.0f * (float)word_at(&d[6]) / 65535.0f, 37.0f, 0.01f);

    // Read out once per interval: the buffer is emptied (3.6.2), and the
    // sensor refuses the read header until the next reading.
    HAC_CHECK_EQ_INT(command(&r, 0xEC05u), HAL_OK);
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(read_bytes(&r, d, sizeof d), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT((int)r.sensor.stats.reads_without_data, 1);

    wait_ms(&r, SCD41_MODEL_INTERVAL_MS);
    HAC_CHECK(fetch_word(&r, 0xE4B8u, &ready));
    HAC_CHECK((ready & 0x07FFu) != 0);
    HAC_CHECK_EQ_INT((int)r.sensor.stats.measurements, 2);
}

static void while_measuring_only_five_commands_are_accepted(void)
{
    rig_t r;
    rig_init(&r);
    HAC_CHECK_EQ_INT(command(&r, 0x21B1u), HAL_OK);

    // Refused: everything but the five of 3.6.
    HAC_CHECK_EQ_INT(command(&r, 0x3682u), HAL_ERR_IO);  // get_serial_number
    HAC_CHECK_EQ_INT(command(&r, 0x3646u), HAL_ERR_IO);  // reinit
    HAC_CHECK_EQ_INT(command(&r, 0x21B1u), HAL_ERR_IO);  // start_periodic_measurement again
    HAC_CHECK_EQ_INT(command(&r, 0x219Du), HAL_ERR_IO);  // measure_single_shot
    HAC_CHECK_EQ_INT((int)r.sensor.stats.rejected, 4);

    // Accepted: set/get_ambient_pressure, get_data_ready_status,
    // read_measurement, stop_periodic_measurement.
    HAC_CHECK_EQ_INT(command_with_arg(&r, 0xE000u, 987u), HAL_OK);
    wait_ms(&r, 1u);
    uint16_t hpa = 0u;
    HAC_CHECK(fetch_word(&r, 0xE000u, &hpa));
    HAC_CHECK_EQ_INT(hpa, 987);
    uint16_t ready = 0u;
    HAC_CHECK(fetch_word(&r, 0xE4B8u, &ready));
    HAC_CHECK_EQ_INT(command(&r, 0xEC05u), HAL_OK);
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(command(&r, 0x3F86u), HAL_OK);
    HAC_CHECK(r.sensor.state == SCD41_MODEL_IDLE);
}

static void stop_takes_half_a_second_during_which_the_address_is_refused(void)
{
    rig_t r;
    rig_init(&r);
    HAC_CHECK_EQ_INT(command(&r, 0x21B1u), HAL_OK);
    HAC_CHECK_EQ_INT(command(&r, 0x3F86u), HAL_OK);

    wait_ms(&r, SCD41_MODEL_STOP_MS - 1u);
    HAC_CHECK_EQ_INT(command(&r, 0x3682u), HAL_ERR_NOT_FOUND);
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(command(&r, 0x3682u), HAL_OK);  // idle now: get_serial_number works
}

static void malformed_commands_are_refused(void)
{
    rig_t r;
    rig_init(&r);

    // An argument word whose CRC is wrong.
    const uint8_t bad_crc[5] = {0xE0, 0x00, 0x03, 0xDB, 0x43};  // Table 17 has 0x42
    HAC_CHECK_EQ_INT(r.port->write(r.port, ADDR, bad_crc, sizeof bad_crc, TO_MS), HAL_ERR_IO);
    const uint8_t good_crc[5] = {0xE0, 0x00, 0x03, 0xDB, 0x42};
    HAC_CHECK_EQ_INT(r.port->write(r.port, ADDR, good_crc, sizeof good_crc, TO_MS), HAL_OK);
    HAC_CHECK_EQ_INT(r.sensor.ambient_pressure_hpa, 987);

    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(command(&r, 0x1234u), HAL_ERR_IO);  // no such command
    const uint8_t half[1] = {0x36};
    HAC_CHECK_EQ_INT(r.port->write(r.port, ADDR, half, sizeof half, TO_MS), HAL_ERR_IO);
    const uint8_t short_arg[4] = {0xE0, 0x00, 0x03, 0xDB};  // the CRC byte missing
    HAC_CHECK_EQ_INT(r.port->write(r.port, ADDR, short_arg, sizeof short_arg, TO_MS), HAL_ERR_IO);
}

static void single_shot_is_an_scd41_feature(void)
{
    rig_t r;
    rig_init(&r);
    HAC_CHECK_EQ_INT(command(&r, 0x219Du), HAL_OK);
    HAC_CHECK_EQ_INT(r.port->write(r.port, ADDR, NULL, 0u, TO_MS), HAL_ERR_NOT_FOUND);  // measuring

    wait_ms(&r, SCD41_MODEL_SINGLE_SHOT_MS);
    uint8_t d[9];
    HAC_CHECK_EQ_INT(command(&r, 0xEC05u), HAL_OK);
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(read_bytes(&r, d, sizeof d), HAL_OK);
    HAC_CHECK_EQ_INT(word_at(&d[0]), 500);

    // RH/T only: CO2 comes back as 0 ppm (3.11.2).
    HAC_CHECK_EQ_INT(command(&r, 0x2196u), HAL_OK);
    wait_ms(&r, 50u);
    HAC_CHECK_EQ_INT(command(&r, 0xEC05u), HAL_OK);
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(read_bytes(&r, d, sizeof d), HAL_OK);
    HAC_CHECK_EQ_INT(word_at(&d[0]), 0);

    rig_t scd40;
    rig_init(&scd40);
    scd40.sensor.variant_word = SCD41_MODEL_VARIANT_SCD40;
    HAC_CHECK_EQ_INT(command(&scd40, 0x219Du), HAL_ERR_IO);
}

static void a_sleeping_sensor_wakes_without_acknowledging(void)
{
    rig_t r;
    rig_init(&r);
    HAC_CHECK_EQ_INT(command(&r, 0x36E0u), HAL_OK);  // power_down
    wait_ms(&r, 1u);
    HAC_CHECK_EQ_INT(command(&r, 0x3682u), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT(command(&r, 0x36F6u), HAL_ERR_NOT_FOUND);  // wake_up: never acknowledged
    wait_ms(&r, 30u);
    HAC_CHECK_EQ_INT(command(&r, 0x3682u), HAL_OK);
}

static void reinit_reloads_settings_and_a_power_cycle_stops_everything(void)
{
    rig_t r;
    rig_init(&r);
    HAC_CHECK_EQ_INT(command_with_arg(&r, 0x2427u, 1950u), HAL_OK);  // set_sensor_altitude
    wait_ms(&r, 1u);
    uint16_t altitude = 0u;
    HAC_CHECK(fetch_word(&r, 0x2322u, &altitude));
    HAC_CHECK_EQ_INT(altitude, 1950);
    HAC_CHECK_EQ_INT(command(&r, 0x3646u), HAL_OK);  // reinit: back to EEPROM's 0
    wait_ms(&r, 30u);
    HAC_CHECK(fetch_word(&r, 0x2322u, &altitude));
    HAC_CHECK_EQ_INT(altitude, 0);

    HAC_CHECK_EQ_INT(command(&r, 0x21B1u), HAL_OK);
    scd41_model_power_cycle(&r.sensor, r.clock.now_ms);
    HAC_CHECK(r.sensor.state == SCD41_MODEL_IDLE);
    HAC_CHECK_EQ_INT(command(&r, 0x3682u), HAL_ERR_NOT_FOUND);  // powering up
}

static void the_fault_knobs_do_what_they_say(void)
{
    rig_t r;
    rig_init(&r);

    r.sensor.faults.nack_address = 1u;
    HAC_CHECK_EQ_INT(command(&r, 0x202Fu), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT(command(&r, 0x202Fu), HAL_OK);
    wait_ms(&r, 1u);

    r.sensor.faults.nack_data = 1u;
    HAC_CHECK_EQ_INT(command(&r, 0x202Fu), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(command(&r, 0x202Fu), HAL_OK);
    wait_ms(&r, 1u);

    uint8_t d[3];
    r.sensor.faults.corrupt_crc = 1u;
    HAC_CHECK_EQ_INT(read_bytes(&r, d, sizeof d), HAL_OK);
    HAC_CHECK(!crcs_valid(d, 1u));

    r.sensor.faults.hold_data = true;
    HAC_CHECK_EQ_INT(command(&r, 0x21B1u), HAL_OK);
    wait_ms(&r, 3u * SCD41_MODEL_INTERVAL_MS);
    uint16_t ready = 1u;
    HAC_CHECK(fetch_word(&r, 0xE4B8u, &ready));
    HAC_CHECK_EQ_INT(ready & 0x07FFu, 0);
}

int main(void)
{
    HAC_RUN(the_crc_matches_every_example_in_the_datasheet);
    HAC_RUN(the_address_is_refused_until_the_power_up_time_has_passed);
    HAC_RUN(identity_answers_with_the_datasheet_examples);
    HAC_RUN(a_response_is_not_readable_during_the_execution_time);
    HAC_RUN(periodic_measurement_makes_a_reading_every_five_seconds);
    HAC_RUN(while_measuring_only_five_commands_are_accepted);
    HAC_RUN(stop_takes_half_a_second_during_which_the_address_is_refused);
    HAC_RUN(malformed_commands_are_refused);
    HAC_RUN(single_shot_is_an_scd41_feature);
    HAC_RUN(a_sleeping_sensor_wakes_without_acknowledging);
    HAC_RUN(reinit_reloads_settings_and_a_power_cycle_stops_everything);
    HAC_RUN(the_fault_knobs_do_what_they_say);
    return hac_test_summary();
}
