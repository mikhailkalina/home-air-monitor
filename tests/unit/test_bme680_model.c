// SPDX-License-Identifier: Apache-2.0
//
// The BME680 model (platform/host/i2c_models/bme680_model.c) against the
// datasheet it claims to follow (Bosch BME680, revision 1.9) and against the
// one outside source of numbers for its calibration -- checked here, through
// the fake bus, before any driver is run against it. Register traffic is
// written out byte by byte from the datasheet's tables, not through Bosch's
// code, so this file and the model are two readings of the same pages.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "hac_test.h"
#include "i2c_models/bme680_model.h"

#define ADDR 0x77u
#define TO_MS 20u

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    bme680_model_t sensor;
    port_i2c_t *port;
} rig_t;

static rig_t g_rig;

static rig_t *rig_make(void)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof *r);
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_i2c_fake_init(&r->bus, &r->clock);
    bme680_model_init(&r->sensor, 0u);
    (void)adp_i2c_fake_attach(&r->bus, ADDR, bme680_model_port(&r->sensor));
    r->port = adp_i2c_fake_port(&r->bus);
    adp_clock_virtual_advance(&r->clock, BME680_MODEL_START_UP_MS);
    return r;
}

static void wait_ms(rig_t *r, uint64_t ms)
{
    adp_clock_virtual_advance(&r->clock, ms);
}

// One (address, data) pair: datasheet 6.2.1.
static hal_status_t set_reg(rig_t *r, uint8_t reg, uint8_t value)
{
    const uint8_t d[2] = {reg, value};
    return r->port->write(r->port, ADDR, d, sizeof d, TO_MS);
}

// The register address, a repeated START, and auto-incremented bytes: 6.2.2.
static hal_status_t get_regs(rig_t *r, uint8_t reg, uint8_t *out, size_t n)
{
    return r->port->write_read(r->port, ADDR, &reg, 1u, out, n, TO_MS);
}

static uint8_t get_reg(rig_t *r, uint8_t reg)
{
    uint8_t v = 0xEEu;
    (void)get_regs(r, reg, &v, 1u);
    return v;
}

static uint16_t le16(const uint8_t *nvm, uint8_t lsb_addr, uint8_t msb_addr, uint8_t base)
{
    return (uint16_t)(((unsigned)nvm[msb_addr - base] << 8) | nvm[lsb_addr - base]);
}

// --- identity and reset state ----------------------------------------------------

static void it_is_a_bme680_at_its_reset_state(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(get_reg(r, 0xD0u), 0x61);  // chip_id, Table 20
    HAC_CHECK_EQ_INT(get_reg(r, 0xF0u), 0x00);  // read as a BME680 by Bosch's API

    // Table 20's reset states: 0x80 in each MSB of the data registers.
    uint8_t data[8];
    HAC_CHECK_EQ_INT(get_regs(r, 0x1Fu, data, sizeof data), HAL_OK);
    const uint8_t expected[8] = {0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00};
    HAC_CHECK(memcmp(data, expected, sizeof data) == 0);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du), 0x00);
    for (uint8_t reg = 0x70u; reg <= 0x75u; reg++) {
        HAC_CHECK_EQ_INT(get_reg(r, reg), 0x00);
    }
}

static void it_is_silent_for_its_start_up_time(void)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, &clock);
    static bme680_model_t sensor;
    bme680_model_init(&sensor, 0u);
    (void)adp_i2c_fake_attach(&bus, ADDR, bme680_model_port(&sensor));
    port_i2c_t *port = adp_i2c_fake_port(&bus);

    adp_clock_virtual_advance(&clock, BME680_MODEL_START_UP_MS - 1u);
    HAC_CHECK_EQ_INT(port->write(port, ADDR, NULL, 0u, TO_MS), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT((int)sensor.stats.nacked_while_busy, 1);
    adp_clock_virtual_advance(&clock, 1u);
    HAC_CHECK_EQ_INT(port->write(port, ADDR, NULL, 0u, TO_MS), HAL_OK);
}

// --- the calibration NVM -----------------------------------------------------------

// Decoded here from Tables 11-15 -- independently of the model's encoder --
// and compared with the published values written out again as literals.
static void the_nvm_holds_the_published_calibration_where_the_datasheet_puts_it(void)
{
    rig_t *r = rig_make();
    uint8_t c1[23];  // 0x8A..0xA0
    uint8_t c2[14];  // 0xE1..0xEE
    uint8_t c3[5];   // 0x00..0x04
    HAC_CHECK_EQ_INT(get_regs(r, 0x8Au, c1, sizeof c1), HAL_OK);
    HAC_CHECK_EQ_INT(get_regs(r, 0xE1u, c2, sizeof c2), HAL_OK);
    HAC_CHECK_EQ_INT(get_regs(r, 0x00u, c3, sizeof c3), HAL_OK);

    HAC_CHECK_EQ_INT(le16(c2, 0xE9u, 0xEAu, 0xE1u), 26041);           // par_t1
    HAC_CHECK_EQ_INT((int16_t)le16(c1, 0x8Au, 0x8Bu, 0x8Au), 26469);  // par_t2
    HAC_CHECK_EQ_INT((int8_t)c1[0x8C - 0x8A], 3);                     // par_t3

    HAC_CHECK_EQ_INT(le16(c1, 0x8Eu, 0x8Fu, 0x8Au), 36673);            // par_p1
    HAC_CHECK_EQ_INT((int16_t)le16(c1, 0x90u, 0x91u, 0x8Au), -10515);  // par_p2
    HAC_CHECK_EQ_INT((int8_t)c1[0x92 - 0x8A], 88);                     // par_p3
    HAC_CHECK_EQ_INT((int16_t)le16(c1, 0x94u, 0x95u, 0x8Au), 7310);    // par_p4
    HAC_CHECK_EQ_INT((int16_t)le16(c1, 0x96u, 0x97u, 0x8Au), -129);    // par_p5
    HAC_CHECK_EQ_INT((int8_t)c1[0x99 - 0x8A], 30);                     // par_p6
    HAC_CHECK_EQ_INT((int8_t)c1[0x98 - 0x8A], 46);                     // par_p7
    HAC_CHECK_EQ_INT((int16_t)le16(c1, 0x9Cu, 0x9Du, 0x8Au), -3177);   // par_p8
    HAC_CHECK_EQ_INT((int16_t)le16(c1, 0x9Eu, 0x9Fu, 0x8Au), -2379);   // par_p9
    HAC_CHECK_EQ_INT(c1[0xA0 - 0x8A], 30);                             // par_p10

    // par_h1 is 0xE2<3:0> / 0xE3, par_h2 0xE2<7:4> / 0xE1 (Table 13).
    HAC_CHECK_EQ_INT(((unsigned)c2[0xE3 - 0xE1] << 4) | (c2[0xE2 - 0xE1] & 0x0Fu), 676);
    HAC_CHECK_EQ_INT(((unsigned)c2[0xE1 - 0xE1] << 4) | (c2[0xE2 - 0xE1] >> 4), 1029);
    HAC_CHECK_EQ_INT((int8_t)c2[0xE4 - 0xE1], 0);     // par_h3
    HAC_CHECK_EQ_INT((int8_t)c2[0xE5 - 0xE1], 45);    // par_h4
    HAC_CHECK_EQ_INT((int8_t)c2[0xE6 - 0xE1], 20);    // par_h5
    HAC_CHECK_EQ_INT(c2[0xE7 - 0xE1], 120);           // par_h6
    HAC_CHECK_EQ_INT((int8_t)c2[0xE8 - 0xE1], -100);  // par_h7

    HAC_CHECK_EQ_INT((int8_t)c2[0xED - 0xE1], -30);                    // par_g1
    HAC_CHECK_EQ_INT((int16_t)le16(c2, 0xEBu, 0xECu, 0xE1u), -24754);  // par_g2
    HAC_CHECK_EQ_INT((int8_t)c2[0xEE - 0xE1], 18);                     // par_g3

    HAC_CHECK_EQ_INT((int8_t)c3[0x00], 48);        // res_heat_val
    HAC_CHECK_EQ_INT((c3[0x02] >> 4) & 0x03u, 1);  // res_heat_range, 0x02<5:4>
    HAC_CHECK_EQ_INT((int8_t)c3[0x04] >> 4, 0);    // range_switching_error, 0x04<7:4>
}

static void a_negative_range_switching_error_keeps_its_sign(void)
{
    rig_t *r = rig_make();
    r->sensor.calibration.range_switching_error = -3;
    HAC_CHECK_EQ_INT((int8_t)get_reg(r, 0x04u) / 16, -3);  // signed 4 bits in <7:4>
}

// --- the compensation, against an outside source ----------------------------------

// pimoroni/bme680-python, tests/test_compensation.py, for the same
// calibration -- computed there with the datasheet's INTEGER formulas, so
// the floating-point ones here agree to within their rounding, not exactly.
static void the_compensation_reproduces_the_published_test_vectors(void)
{
    const bme680_model_calibration_t *c = &BME680_MODEL_PUBLISHED_CALIBRATION;
    double t_fine = 0.0;
    const double t = bme680_model_temperature(c, 501240u, &t_fine);
    HAC_CHECK_EQ_F32((float)t, 26.69f, 0.01f);
    HAC_CHECK_EQ_F32((float)t_fine, 136667.0f, 5.0f);
    // Integer and floating point differ by 5 Pa here.
    HAC_CHECK_EQ_F32((float)bme680_model_pressure(c, 353485u, t_fine), 98712.0f, 10.0f);
    HAC_CHECK_EQ_F32((float)bme680_model_humidity(c, 19019u, t), 42.402f, 0.02f);
    HAC_CHECK_EQ_F32((float)bme680_model_gas_resistance(c, 0u, 0u), 12946860.0f, 1.0f);
}

// What makes bisection a valid inverse: every curve is monotonic over its
// whole ADC range, for this calibration, at the ends of the operating range.
static void every_compensation_is_monotonic_over_its_adc(void)
{
    const bme680_model_calibration_t *c = &BME680_MODEL_PUBLISHED_CALIBRATION;
    bool rising = true;
    double prev = bme680_model_temperature(c, 0u, NULL);
    for (uint32_t adc = 1u; adc < (1u << 20); adc++) {
        const double v = bme680_model_temperature(c, adc, NULL);
        rising = rising && v > prev;
        prev = v;
    }
    HAC_CHECK(rising);

    const double temps[] = {-40.0, 25.0, 85.0};
    for (size_t i = 0u; i < sizeof temps / sizeof temps[0]; i++) {
        const double t_fine = temps[i] * 5120.0;
        bool falling = true;
        prev = bme680_model_pressure(c, 0u, t_fine);
        for (uint32_t adc = 1u; adc < (1u << 20); adc++) {
            const double v = bme680_model_pressure(c, adc, t_fine);
            falling = falling && v < prev;
            prev = v;
        }
        HAC_CHECK(falling);

        rising = true;
        prev = bme680_model_humidity(c, 0u, temps[i]);
        for (uint32_t adc = 1u; adc <= UINT16_MAX; adc++) {
            const double v = bme680_model_humidity(c, (uint16_t)adc, temps[i]);
            rising = rising && v > prev;
            prev = v;
        }
        HAC_CHECK(rising);
    }

    for (uint8_t range = 0u; range < 16u; range++) {
        bool falling = true;
        prev = bme680_model_gas_resistance(c, 0u, range);
        for (uint16_t adc = 1u; adc < 1024u; adc++) {
            const double v = bme680_model_gas_resistance(c, adc, range);
            falling = falling && v < prev;
            prev = v;
        }
        HAC_CHECK(falling);
    }
}

// --- the I2C framing ---------------------------------------------------------------

static void a_write_is_pairs_of_address_and_data_not_a_register_file(void)
{
    rig_t *r = rig_make();
    // ctrl_hum and ctrl_meas in one write; status (0x73) between them is
    // not touched.
    const uint8_t pairs[4] = {0x72u, 0x01u, 0x74u, 0x40u};
    HAC_CHECK_EQ_INT(r->port->write(r->port, ADDR, pairs, sizeof pairs, TO_MS), HAL_OK);
    HAC_CHECK_EQ_INT(get_reg(r, 0x72u), 0x01);
    HAC_CHECK_EQ_INT(get_reg(r, 0x73u), 0x00);
    HAC_CHECK_EQ_INT(get_reg(r, 0x74u), 0x40);

    // An address and two data bytes, as a register file would take them: the
    // second byte is an address with no data, and sets the read pointer.
    const uint8_t burst[3] = {0x72u, 0x02u, 0x75u};
    HAC_CHECK_EQ_INT(r->port->write(r->port, ADDR, burst, sizeof burst, TO_MS), HAL_OK);
    HAC_CHECK_EQ_INT(get_reg(r, 0x72u), 0x02);
    HAC_CHECK_EQ_INT(get_reg(r, 0x73u), 0x00);
    uint8_t v = 0xEEu;
    HAC_CHECK_EQ_INT(r->port->write(r->port, ADDR, burst, sizeof burst, TO_MS), HAL_OK);
    HAC_CHECK_EQ_INT(r->port->read(r->port, ADDR, &v, 1u, TO_MS), HAL_OK);
    HAC_CHECK_EQ_INT(v, 0x00);  // config, 0x75, at its reset state
}

static void a_read_auto_increments_and_wraps(void)
{
    rig_t *r = rig_make();
    uint8_t d[3];
    HAC_CHECK_EQ_INT(get_regs(r, 0xFFu, d, sizeof d), HAL_OK);
    HAC_CHECK_EQ_INT(d[0], 0x00);  // 0xFF, reserved
    HAC_CHECK_EQ_INT(d[1], 48);    // 0x00, res_heat_val
    HAC_CHECK_EQ_INT(d[2], 0x00);  // 0x01, reserved
}

static void a_write_keeps_only_the_bits_the_datasheet_defines(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(set_reg(r, 0x70u, 0xFFu), HAL_OK);
    HAC_CHECK_EQ_INT(get_reg(r, 0x70u), 0x08);  // heat_off
    HAC_CHECK_EQ_INT(set_reg(r, 0x71u, 0xFFu), HAL_OK);
    HAC_CHECK_EQ_INT(get_reg(r, 0x71u), 0x1F);  // run_gas, nb_conv
    HAC_CHECK_EQ_INT(set_reg(r, 0x72u, 0xFFu), HAL_OK);
    HAC_CHECK_EQ_INT(get_reg(r, 0x72u), 0x47);  // spi_3w_int_en, osrs_h
    HAC_CHECK_EQ_INT(set_reg(r, 0x75u, 0xFFu), HAL_OK);
    HAC_CHECK_EQ_INT(get_reg(r, 0x75u), 0x1D);  // filter, spi_3w_en

    HAC_CHECK_EQ_INT(set_reg(r, 0xD0u, 0x12u), HAL_OK);  // chip_id is read only
    HAC_CHECK_EQ_INT(get_reg(r, 0xD0u), 0x61);
    HAC_CHECK_EQ_INT(set_reg(r, 0x8Au, 0x12u), HAL_OK);  // and so is the NVM
    HAC_CHECK_EQ_INT((int)r->sensor.stats.read_only_writes, 2);
}

// --- a forced measurement ----------------------------------------------------------

static bme680_model_reading_t g_air;
static uint64_t g_reading_at_ms;

static void reading_from_air(void *ctx, uint64_t at_ms, bme680_model_reading_t *out)
{
    (void)ctx;
    g_reading_at_ms = at_ms;
    *out = g_air;
}

// The datasheet's quick-start configuration (3.2.1): humidity x1,
// temperature x2, pressure x16, 100 ms of heating (gas_wait_0 = 0x59) at the
// code for 300 C, profile 0, gas on.
static void configure_quick_start(rig_t *r)
{
    const uint8_t res_heat = bme680_model_res_heat(&r->sensor.calibration, 300.0, 25.0);
    const uint8_t pairs[] = {
        0x72u, 0x01u,                  // ctrl_hum: osrs_h x1
        0x74u, (2u << 5) | (5u << 2),  // ctrl_meas: osrs_t x2, osrs_p x16, sleep
        0x64u, 0x59u,                  // gas_wait_0
        0x5Au, res_heat,               // res_heat_0
        0x71u, 0x10u,                  // ctrl_gas_1: run_gas, nb_conv 0
    };
    HAC_CHECK_EQ_INT(r->port->write(r->port, ADDR, pairs, sizeof pairs, TO_MS), HAL_OK);
}

static void trigger_forced(rig_t *r)
{
    HAC_CHECK_EQ_INT(set_reg(r, 0x74u, (uint8_t)((get_reg(r, 0x74u) & 0xFCu) | 0x01u)), HAL_OK);
}

typedef struct {
    double temperature_c;
    double pressure_hpa;
    double humidity_rh;
    double gas_ohm;
    uint8_t gas_lsb;
} decoded_t;

// The data registers decoded by 5.3.4 and compensated by the model's own
// formulas: what any driver would get.
static decoded_t decode(rig_t *r)
{
    uint8_t d[15];  // 0x1D..0x2B
    (void)get_regs(r, 0x1Du, d, sizeof d);
    const uint32_t press = ((uint32_t)d[2] << 12) | ((uint32_t)d[3] << 4) | (d[4] >> 4);
    const uint32_t temp = ((uint32_t)d[5] << 12) | ((uint32_t)d[6] << 4) | (d[7] >> 4);
    const uint16_t hum = (uint16_t)(((unsigned)d[8] << 8) | d[9]);
    const uint16_t gas = (uint16_t)(((unsigned)d[13] << 2) | (d[14] >> 6));
    const bme680_model_calibration_t *c = &r->sensor.calibration;
    decoded_t out;
    double t_fine = 0.0;
    out.temperature_c = bme680_model_temperature(c, temp, &t_fine);
    out.pressure_hpa = bme680_model_pressure(c, press, t_fine) / 100.0;
    out.humidity_rh = bme680_model_humidity(c, hum, out.temperature_c);
    out.gas_ohm = bme680_model_gas_resistance(c, gas, (uint8_t)(d[14] & 0x0Fu));
    out.gas_lsb = d[14];
    return out;
}

static void a_forced_measurement_runs_its_course(void)
{
    rig_t *r = rig_make();
    g_air = (bme680_model_reading_t){.temperature_c = 24.3f,
                                     .humidity_rh = 51.7f,
                                     .pressure_hpa = 998.4f,
                                     .gas_resistance_ohm = 87000.0f};
    bme680_model_set_reading(&r->sensor, reading_from_air, NULL);
    configure_quick_start(r);
    const uint64_t t0 = r->clock.now_ms;
    trigger_forced(r);

    // 1 ms wake, 19 samples, switching: 40.205 ms of T, P and H, then the
    // heater's 100 ms and the gas slot -- 142.59 ms, so done at 143.
    HAC_CHECK_EQ_INT(get_reg(r, 0x74u) & 0x03u, 0x01);  // still forced
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du), 0x20);          // measuring
    wait_ms(r, 41u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du), 0x60);  // and now the gas phase
    wait_ms(r, 142u - 41u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du) & 0x80u, 0x00);  // not yet at 142 ms
    wait_ms(r, 1u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du), 0x80);          // new_data, gas index 0
    HAC_CHECK_EQ_INT(get_reg(r, 0x74u) & 0x03u, 0x00);  // back to sleep by itself
    HAC_CHECK_EQ_U64(g_reading_at_ms, t0 + 143u);

    const decoded_t d = decode(r);
    HAC_CHECK_EQ_F32((float)d.temperature_c, 24.3f, 0.005f);
    HAC_CHECK_EQ_F32((float)d.pressure_hpa, 998.4f, 0.005f);
    HAC_CHECK_EQ_F32((float)d.humidity_rh, 51.7f, 0.01f);
    HAC_CHECK_EQ_F32((float)d.gas_ohm, 87000.0f, 87000.0f * 0.001f);
    HAC_CHECK_EQ_INT(d.gas_lsb & 0x30u, 0x30);  // gas_valid_r, heat_stab_r
    HAC_CHECK_EQ_INT((int)r->sensor.stats.completed, 1);
}

static void the_round_trip_holds_across_the_operating_range(void)
{
    rig_t *r = rig_make();
    bme680_model_set_reading(&r->sensor, reading_from_air, NULL);
    configure_quick_start(r);
    const float temps[] = {-40.0f, 0.0f, 21.37f, 85.0f};
    const float pressures[] = {300.0f, 750.0f, 1013.25f, 1100.0f};
    const float hums[] = {0.0f, 33.3f, 80.0f, 100.0f};
    const float gases[] = {500.0f, 9000.0f, 150000.0f, 2.0e6f};
    for (size_t i = 0u; i < 4u; i++) {
        g_air = (bme680_model_reading_t){.temperature_c = temps[i],
                                         .humidity_rh = hums[i],
                                         .pressure_hpa = pressures[i],
                                         .gas_resistance_ohm = gases[i]};
        trigger_forced(r);
        wait_ms(r, 200u);
        const decoded_t d = decode(r);
        HAC_CHECK_EQ_F32((float)d.temperature_c, temps[i], 0.005f);
        HAC_CHECK_EQ_F32((float)d.pressure_hpa, pressures[i], 0.005f);
        HAC_CHECK_EQ_F32((float)d.humidity_rh, hums[i], 0.01f);
        HAC_CHECK_EQ_F32((float)d.gas_ohm, gases[i], gases[i] * 0.001f);
    }
}

// Every gas resistance from the lowest range's bottom to the highest's top,
// in 5 % steps -- across every boundary between two ADC ranges.
static void every_gas_resistance_finds_a_range_that_holds_it(void)
{
    rig_t *r = rig_make();
    bme680_model_set_reading(&r->sensor, reading_from_air, NULL);
    configure_quick_start(r);
    g_air = (bme680_model_reading_t){
        .temperature_c = 22.0f, .humidity_rh = 40.0f, .pressure_hpa = 1000.0f};
    unsigned off = 0u;
    unsigned steps = 0u;
    for (double ohm = 180.0; ohm < 12.9e6; ohm *= 1.05) {
        g_air.gas_resistance_ohm = (float)ohm;
        trigger_forced(r);
        wait_ms(r, 200u);
        const decoded_t d = decode(r);
        off += fabs(d.gas_ohm / ohm - 1.0) > 0.001 ? 1u : 0u;
        steps++;
    }
    HAC_CHECK(steps > 200u);
    HAC_CHECK_EQ_INT((int)off, 0);
}

static void skipped_measurements_leave_their_reset_values(void)
{
    rig_t *r = rig_make();
    trigger_forced(r);  // every osrs 0, gas off
    // 1 ms wake, no samples, switching and the gas slot: 5.293 ms.
    wait_ms(r, 5u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du) & 0x80u, 0x00);
    wait_ms(r, 1u);
    uint8_t d[15];
    HAC_CHECK_EQ_INT(get_regs(r, 0x1Du, d, sizeof d), HAL_OK);
    HAC_CHECK_EQ_INT(d[0], 0x80);  // new data, no gas index
    const uint8_t skipped[8] = {0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00};
    HAC_CHECK(memcmp(&d[2], skipped, sizeof skipped) == 0);
    HAC_CHECK_EQ_INT(d[14] & 0x30u, 0x00);  // no gas conversion: neither valid nor stable
}

static void the_resolution_follows_the_oversampling(void)
{
    rig_t *r = rig_make();
    g_air.temperature_c = 22.2222f;
    bme680_model_set_reading(&r->sensor, reading_from_air, NULL);
    // osrs_t x1: 16 bits, the low four of 20 -- temp_xlsb -- zero (3.3.1).
    HAC_CHECK_EQ_INT(set_reg(r, 0x74u, 1u << 5), HAL_OK);
    trigger_forced(r);
    wait_ms(r, 50u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x24u), 0x00);
    // osrs_t x2: 17 bits, the low three zero.
    HAC_CHECK_EQ_INT(set_reg(r, 0x74u, 2u << 5), HAL_OK);
    trigger_forced(r);
    wait_ms(r, 50u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x24u) & 0x70u, 0x00);
}

static void the_heater_must_have_its_time_and_be_on(void)
{
    rig_t *r = rig_make();
    configure_quick_start(r);
    HAC_CHECK_EQ_INT(set_reg(r, 0x64u, 20u), HAL_OK);  // 20 ms: too short
    trigger_forced(r);
    wait_ms(r, 100u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x2Bu) & 0x30u, 0x20);  // valid, not stable

    HAC_CHECK_EQ_INT(set_reg(r, 0x64u, 30u), HAL_OK);  // just enough
    trigger_forced(r);
    wait_ms(r, 100u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x2Bu) & 0x30u, 0x30);

    HAC_CHECK_EQ_INT(set_reg(r, 0x70u, 0x08u), HAL_OK);  // heat_off
    trigger_forced(r);
    wait_ms(r, 100u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x2Bu) & 0x30u, 0x20);

    HAC_CHECK_EQ_INT(set_reg(r, 0x70u, 0x00u), HAL_OK);
    r->sensor.faults.heater_unstable = true;
    trigger_forced(r);
    wait_ms(r, 100u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x2Bu) & 0x30u, 0x20);

    r->sensor.faults.heater_unstable = false;
    r->sensor.faults.gas_invalid = true;
    trigger_forced(r);
    wait_ms(r, 100u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x2Bu) & 0x30u, 0x10);
}

static void gas_wait_decodes_its_multiplication_factor(void)
{
    rig_t *r = rig_make();
    // 0x59 is 25 x 4 = 100 ms (the datasheet's own example); 0xC1 is 1 x 64.
    HAC_CHECK_EQ_INT(set_reg(r, 0x71u, 0x10u), HAL_OK);
    HAC_CHECK_EQ_INT(set_reg(r, 0x64u, 0xC1u), HAL_OK);
    trigger_forced(r);
    // 1 ms wake + switching = 2.908 ms, + 64 ms, + the gas slot: 69.293 ms.
    wait_ms(r, 69u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du) & 0x80u, 0x00);
    wait_ms(r, 1u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du) & 0x80u, 0x80);
}

// --- what a driver must not do -----------------------------------------------------

static void writes_during_a_measurement_are_dropped_and_counted(void)
{
    rig_t *r = rig_make();
    configure_quick_start(r);
    trigger_forced(r);
    HAC_CHECK_EQ_INT(set_reg(r, 0x72u, 0x05u), HAL_OK);
    HAC_CHECK_EQ_INT(get_reg(r, 0x72u), 0x01);
    HAC_CHECK_EQ_INT(set_reg(r, 0x74u, 0x00u), HAL_OK);  // a mode change too
    HAC_CHECK_EQ_INT((int)r->sensor.stats.ignored_writes, 2);
    wait_ms(r, 143u);
    // The model keeps its time at each transfer, so the measurement ends at
    // the next look -- still the forced one, untouched by the writes.
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du), 0x80);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.completed, 1);
}

static void reading_the_data_while_measuring_is_counted(void)
{
    rig_t *r = rig_make();
    configure_quick_start(r);
    trigger_forced(r);
    (void)get_reg(r, 0x74u);  // the mode: fine to poll
    HAC_CHECK_EQ_INT((int)r->sensor.stats.early_reads, 0);
    uint8_t d[17];
    (void)get_regs(r, 0x1Du, d, sizeof d);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.early_reads, 1);
    wait_ms(r, 143u);
    (void)get_regs(r, 0x1Du, d, sizeof d);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.early_reads, 1);
}

static void a_soft_reset_restores_power_on_state_even_mid_measurement(void)
{
    rig_t *r = rig_make();
    configure_quick_start(r);
    trigger_forced(r);
    HAC_CHECK_EQ_INT(set_reg(r, 0xE0u, 0x00u), HAL_OK);  // not the command: nothing
    HAC_CHECK(r->sensor.measuring);
    HAC_CHECK_EQ_INT(set_reg(r, 0xE0u, 0xB6u), HAL_OK);
    HAC_CHECK(!r->sensor.measuring);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.soft_resets, 1);
    HAC_CHECK_EQ_INT(r->port->write(r->port, ADDR, NULL, 0u, TO_MS), HAL_ERR_NOT_FOUND);
    wait_ms(r, BME680_MODEL_START_UP_MS);
    HAC_CHECK_EQ_INT(get_reg(r, 0x72u), 0x00);
    HAC_CHECK_EQ_INT(get_reg(r, 0x74u), 0x00);
    HAC_CHECK_EQ_INT(get_reg(r, 0x71u), 0x00);
    HAC_CHECK_EQ_INT(get_reg(r, 0x22u), 0x80);
    HAC_CHECK_EQ_INT(get_reg(r, 0xEAu), 26041 >> 8);  // the NVM survives
}

// --- faults --------------------------------------------------------------------------

static void injected_faults_reach_the_bus(void)
{
    rig_t *r = rig_make();
    r->sensor.faults.nack_address = 1u;
    HAC_CHECK_EQ_INT(set_reg(r, 0x72u, 0x01u), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT(get_reg(r, 0x72u), 0x00);

    // A refused last byte: everything before it takes effect, it does not.
    r->sensor.faults.nack_data = 1u;
    const uint8_t pairs[4] = {0x72u, 0x01u, 0x75u, 0x04u};
    HAC_CHECK_EQ_INT(r->port->write(r->port, ADDR, pairs, sizeof pairs, TO_MS), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(get_reg(r, 0x72u), 0x01);
    HAC_CHECK_EQ_INT(get_reg(r, 0x75u), 0x00);

    r->sensor.faults.overrun_ms = 50u;
    configure_quick_start(r);
    trigger_forced(r);
    wait_ms(r, 143u + 49u);
    HAC_CHECK(r->sensor.measuring);
    wait_ms(r, 1u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du) & 0x80u, 0x80);

    r->sensor.faults.overrun_ms = BME680_MODEL_NEVER;
    trigger_forced(r);
    wait_ms(r, 3600000u);
    HAC_CHECK_EQ_INT(get_reg(r, 0x1Du), 0x60);  // still measuring, an hour on
    HAC_CHECK_EQ_INT(get_reg(r, 0x74u) & 0x03u, 0x01);
}

static void a_power_cycle_loses_the_configuration(void)
{
    rig_t *r = rig_make();
    configure_quick_start(r);
    bme680_model_power_cycle(&r->sensor, r->clock.now_ms);
    HAC_CHECK_EQ_INT(r->port->write(r->port, ADDR, NULL, 0u, TO_MS), HAL_ERR_NOT_FOUND);
    wait_ms(r, BME680_MODEL_START_UP_MS);
    HAC_CHECK_EQ_INT(get_reg(r, 0x71u), 0x00);
    HAC_CHECK_EQ_INT(get_reg(r, 0x64u), 0x00);
}

int main(void)
{
    HAC_RUN(it_is_a_bme680_at_its_reset_state);
    HAC_RUN(it_is_silent_for_its_start_up_time);
    HAC_RUN(the_nvm_holds_the_published_calibration_where_the_datasheet_puts_it);
    HAC_RUN(a_negative_range_switching_error_keeps_its_sign);
    HAC_RUN(the_compensation_reproduces_the_published_test_vectors);
    HAC_RUN(every_compensation_is_monotonic_over_its_adc);
    HAC_RUN(a_write_is_pairs_of_address_and_data_not_a_register_file);
    HAC_RUN(a_read_auto_increments_and_wraps);
    HAC_RUN(a_write_keeps_only_the_bits_the_datasheet_defines);
    HAC_RUN(a_forced_measurement_runs_its_course);
    HAC_RUN(the_round_trip_holds_across_the_operating_range);
    HAC_RUN(every_gas_resistance_finds_a_range_that_holds_it);
    HAC_RUN(skipped_measurements_leave_their_reset_values);
    HAC_RUN(the_resolution_follows_the_oversampling);
    HAC_RUN(the_heater_must_have_its_time_and_be_on);
    HAC_RUN(gas_wait_decodes_its_multiplication_factor);
    HAC_RUN(writes_during_a_measurement_are_dropped_and_counted);
    HAC_RUN(reading_the_data_while_measuring_is_counted);
    HAC_RUN(a_soft_reset_restores_power_on_state_even_mid_measurement);
    HAC_RUN(injected_faults_reach_the_bus);
    HAC_RUN(a_power_cycle_loses_the_configuration);
    return hac_test_summary();
}
