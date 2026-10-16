// SPDX-License-Identifier: Apache-2.0

#include "bme680_model.h"

#include <math.h>
#include <string.h>

// The calibration of a real BME680, read out of one and published in
// pimoroni/bme680-python: tests/conftest.py, the "calibration" fixture, at
// commit 30fcb34efc2d19f60a63ece656b9059adb373150 (the dump is credited there
// to the project's issue #11). The same file's neighbour,
// tests/test_compensation.py, computes test vectors from it with that
// project's integer compensation, which test_bme680_model.c holds this
// model's floating-point compensation to. Kept for those vectors: the model's
// own default is this board's set, below.
const bme680_model_calibration_t BME680_MODEL_PUBLISHED_CALIBRATION = {
    .par_t1 = 26041,
    .par_t2 = 26469,
    .par_t3 = 3,
    .par_p1 = 36673,
    .par_p2 = -10515,
    .par_p3 = 88,
    .par_p4 = 7310,
    .par_p5 = -129,
    .par_p6 = 30,
    .par_p7 = 46,
    .par_p8 = -3177,
    .par_p9 = -2379,
    .par_p10 = 30,
    .par_h1 = 676,
    .par_h2 = 1029,
    .par_h3 = 0,
    .par_h4 = 45,
    .par_h5 = 20,
    .par_h6 = 120,
    .par_h7 = -100,
    .par_g1 = -30,
    .par_g2 = -24754,
    .par_g3 = 18,
    .res_heat_range = 1,
    .res_heat_val = 48,
    .range_switching_error = 0,
};

// The calibration of this board's BME680 (docs/hardware/board_notes.md,
// "Sensors on the bus"), read out of its NVM by Bosch's bme68x_init() and
// logged by bme68x_source as "calibration (1/5)" .. "(5/5)" in the on-target
// test app, 2026-10-16, ELF SHA256 6ab6ea91b... -- the model's default since
// phase 3c, so the simulator's fake-i2c mode and the host tests run against
// this unit's numbers, not someone else's.
const bme680_model_calibration_t BME680_MODEL_BOARD_CALIBRATION = {
    .par_t1 = 26195,
    .par_t2 = 26383,
    .par_t3 = 3,
    .par_p1 = 36513,
    .par_p2 = -10358,
    .par_p3 = 88,
    .par_p4 = 8598,
    .par_p5 = -181,
    .par_p6 = 30,
    .par_p7 = 42,
    .par_p8 = -2657,
    .par_p9 = -2592,
    .par_p10 = 30,
    .par_h1 = 917,
    .par_h2 = 982,
    .par_h3 = 0,
    .par_h4 = 45,
    .par_h5 = 20,
    .par_h6 = 120,
    .par_h7 = -100,
    .par_g1 = -60,
    .par_g2 = -10528,
    .par_g3 = 18,
    .res_heat_range = 1,
    .res_heat_val = 36,
    .range_switching_error = 1,
};

// A room, for a model nobody gave a reading.
#define DEFAULT_TEMPERATURE_C 23.0f
#define DEFAULT_HUMIDITY_RH 45.0f
#define DEFAULT_PRESSURE_HPA 1012.8f
#define DEFAULT_GAS_RESISTANCE_OHM 150000.0f

#define PA_PER_HPA 100.0
#define US_PER_MS 1000u

// Register bits, Table 20 and 5.3.
#define CTRL_MEAS_MODE_MASK 0x03u
#define OSRS_T_SHIFT 5u
#define OSRS_P_SHIFT 2u
#define OSRS_MASK 0x07u
#define FILTER_SHIFT 2u
#define FILTER_MASK 0x07u
#define HEAT_OFF 0x08u
#define RUN_GAS 0x10u
#define NB_CONV_MASK 0x0Fu
#define NB_CONV_MAX 9u
#define GAS_INDEX_MASK 0x0Fu

// The first and last of the data registers a read must not reach into while
// a measurement runs: meas_status_0 through gas_r_lsb.
#define DATA_FIRST BME680_MODEL_REG_MEAS_STATUS_0
#define DATA_LAST BME680_MODEL_REG_GAS_R_LSB

// What a skipped measurement leaves in its registers (5.3.2.1-5.3.2.4):
// 0x80000 for temperature and pressure, 0x8000 for humidity -- and, as reset
// states, the same (Table 20).
#define SKIPPED_MSB 0x80u

// The ADCs (3.3.1-3.3.3, 5.3.4).
#define TP_ADC_BITS 20u
#define TP_ADC_MIN_BITS 16u  // with osrs x1 and the filter off
#define HUM_ADC_COUNT 65536u
#define GAS_ADC_COUNT 1024u
#define GAS_ADC_MIDDLE 512u
#define GAS_RANGES 16u

// osrs codes and the samples each takes (5.3.2): 101 and above are x16.
#define OSRS_CODE_MAX 5u
static const unsigned SAMPLES[OSRS_CODE_MAX + 1u] = {0u, 1u, 2u, 4u, 8u, 16u};

// gas_wait_x: 6 bits of 1 ms steps times a factor in bits 7:6 (5.3.3.3).
#define GAS_WAIT_STEPS_MASK 0x3Fu
#define GAS_WAIT_FACTOR_SHIFT 6u
static const uint32_t GAS_WAIT_FACTOR[4] = {1u, 4u, 16u, 64u};

// Table 16, floating-point columns.
static const double GAS_CONST1[GAS_RANGES] = {
    1.0, 1.0, 1.0, 1.0, 1.0, 0.99, 1.0, 0.992, 1.0, 1.0, 0.998, 0.995, 1.0, 0.99, 1.0, 1.0,
};
static const double GAS_CONST2[GAS_RANGES] = {
    8000000.0,   4000000.0, 2000000.0, 1000000.0, 499500.4995, 248262.1648, 125000.0,  63004.03226,
    31281.28128, 15625.0,   7812.5,    3906.25,   1953.125,    976.5625,    488.28125, 244.140625,
};

// --- the datasheet's compensation, floating point ------------------------------

double bme680_model_temperature(const bme680_model_calibration_t *c, uint32_t temp_adc,
                                double *t_fine)
{
    const double adc = (double)temp_adc;
    const double var1 = ((adc / 16384.0) - ((double)c->par_t1 / 1024.0)) * (double)c->par_t2;
    const double d = (adc / 131072.0) - ((double)c->par_t1 / 8192.0);
    const double var2 = d * d * ((double)c->par_t3 * 16.0);
    const double fine = var1 + var2;
    if (t_fine != NULL) {
        *t_fine = fine;
    }
    return fine / 5120.0;
}

double bme680_model_pressure(const bme680_model_calibration_t *c, uint32_t press_adc, double t_fine)
{
    double var1 = (t_fine / 2.0) - 64000.0;
    double var2 = var1 * var1 * ((double)c->par_p6 / 131072.0);
    var2 = var2 + (var1 * (double)c->par_p5 * 2.0);
    var2 = (var2 / 4.0) + ((double)c->par_p4 * 65536.0);
    var1 = ((((double)c->par_p3 * var1 * var1) / 16384.0) + ((double)c->par_p2 * var1)) / 524288.0;
    var1 = (1.0 + (var1 / 32768.0)) * (double)c->par_p1;
    double press = 1048576.0 - (double)press_adc;
    press = ((press - (var2 / 4096.0)) * 6250.0) / var1;
    var1 = ((double)c->par_p9 * press * press) / 2147483648.0;
    var2 = press * ((double)c->par_p8 / 32768.0);
    const double var3 =
        (press / 256.0) * (press / 256.0) * (press / 256.0) * ((double)c->par_p10 / 131072.0);
    return press + (var1 + var2 + var3 + ((double)c->par_p7 * 128.0)) / 16.0;
}

double bme680_model_humidity(const bme680_model_calibration_t *c, uint16_t hum_adc, double temp_c)
{
    const double var1 =
        (double)hum_adc - (((double)c->par_h1 * 16.0) + (((double)c->par_h3 / 2.0) * temp_c));
    const double var2 = var1 * (((double)c->par_h2 / 262144.0) *
                                (1.0 + (((double)c->par_h4 / 16384.0) * temp_c) +
                                 (((double)c->par_h5 / 1048576.0) * temp_c * temp_c)));
    const double var3 = (double)c->par_h6 / 16384.0;
    const double var4 = (double)c->par_h7 / 2097152.0;
    return var2 + ((var3 + (var4 * temp_c)) * var2 * var2);
}

double bme680_model_gas_resistance(const bme680_model_calibration_t *c, uint16_t gas_adc,
                                   uint8_t gas_range)
{
    const unsigned r = gas_range & (GAS_RANGES - 1u);
    const double var1 = (1340.0 + 5.0 * (double)c->range_switching_error) * GAS_CONST1[r];
    return var1 * GAS_CONST2[r] / ((double)gas_adc - 512.0 + var1);
}

uint8_t bme680_model_res_heat(const bme680_model_calibration_t *c, double target_c,
                              double ambient_c)
{
    const double var1 = ((double)c->par_g1 / 16.0) + 49.0;
    const double var2 = (((double)c->par_g2 / 32768.0) * 0.0005) + 0.00235;
    const double var3 = (double)c->par_g3 / 1024.0;
    const double var4 = var1 * (1.0 + (var2 * target_c));
    const double var5 = var4 + (var3 * ambient_c);
    return (uint8_t)(3.4 * ((var5 * (4.0 / (4.0 + (double)c->res_heat_range)) *
                             (1.0 / (1.0 + ((double)c->res_heat_val * 0.002)))) -
                            25.0));
}

// --- inverting it -----------------------------------------------------------------

typedef struct {
    const bme680_model_calibration_t *c;
    unsigned shift;  // the ADC's unused low bits at this resolution
    double t_fine;
    double temp_c;
    uint8_t gas_range;
} curve_t;

typedef double (*curve_fn)(const curve_t *curve, uint32_t k);

static double temperature_at(const curve_t *curve, uint32_t k)
{
    return bme680_model_temperature(curve->c, k << curve->shift, NULL);
}

static double pressure_at(const curve_t *curve, uint32_t k)
{
    return bme680_model_pressure(curve->c, k << curve->shift, curve->t_fine);
}

static double humidity_at(const curve_t *curve, uint32_t k)
{
    return bme680_model_humidity(curve->c, (uint16_t)k, curve->temp_c);
}

static double gas_at(const curve_t *curve, uint32_t k)
{
    return bme680_model_gas_resistance(curve->c, (uint16_t)k, curve->gas_range);
}

// The code k in [0, count) whose value is nearest `target`, for a curve
// monotonic over that range, rising or falling. A target beyond either end
// gets that end: the ADC cannot say more.
static uint32_t invert(curve_fn f, const curve_t *curve, uint32_t count, double target)
{
    uint32_t lo = 0u;
    uint32_t hi = count - 1u;
    const bool rising = f(curve, hi) >= f(curve, lo);
    while (hi - lo > 1u) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if ((f(curve, mid) < target) == rising) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return fabs(f(curve, lo) - target) <= fabs(f(curve, hi) - target) ? lo : hi;
}

// --- registers ---------------------------------------------------------------------

static uint8_t lsb(uint16_t v)
{
    return (uint8_t)(v & 0xFFu);
}

static uint8_t msb(uint16_t v)
{
    return (uint8_t)(v >> 8);
}

// The calibration NVM, encoded at the addresses of Tables 11-15 from the
// parameters. False for an address that holds none of them.
static bool nvm_byte(const bme680_model_calibration_t *c, uint8_t addr, uint8_t *out)
{
    switch (addr) {
        case 0x00u:
            *out = (uint8_t)c->res_heat_val;
            return true;
        case 0x02u:
            *out = (uint8_t)((c->res_heat_range & 0x03u) << 4);  // <5:4>
            return true;
        case 0x04u:
            *out = (uint8_t)(((uint8_t)c->range_switching_error & 0x0Fu) << 4);  // <7:4>
            return true;
        case 0x8Au:
            *out = lsb((uint16_t)c->par_t2);
            return true;
        case 0x8Bu:
            *out = msb((uint16_t)c->par_t2);
            return true;
        case 0x8Cu:
            *out = (uint8_t)c->par_t3;
            return true;
        case 0x8Eu:
            *out = lsb(c->par_p1);
            return true;
        case 0x8Fu:
            *out = msb(c->par_p1);
            return true;
        case 0x90u:
            *out = lsb((uint16_t)c->par_p2);
            return true;
        case 0x91u:
            *out = msb((uint16_t)c->par_p2);
            return true;
        case 0x92u:
            *out = (uint8_t)c->par_p3;
            return true;
        case 0x94u:
            *out = lsb((uint16_t)c->par_p4);
            return true;
        case 0x95u:
            *out = msb((uint16_t)c->par_p4);
            return true;
        case 0x96u:
            *out = lsb((uint16_t)c->par_p5);
            return true;
        case 0x97u:
            *out = msb((uint16_t)c->par_p5);
            return true;
        case 0x98u:
            *out = (uint8_t)c->par_p7;  // p7 before p6, as Table 12 has them
            return true;
        case 0x99u:
            *out = (uint8_t)c->par_p6;
            return true;
        case 0x9Cu:
            *out = lsb((uint16_t)c->par_p8);
            return true;
        case 0x9Du:
            *out = msb((uint16_t)c->par_p8);
            return true;
        case 0x9Eu:
            *out = lsb((uint16_t)c->par_p9);
            return true;
        case 0x9Fu:
            *out = msb((uint16_t)c->par_p9);
            return true;
        case 0xA0u:
            *out = c->par_p10;
            return true;
        case 0xE1u:
            *out = (uint8_t)((c->par_h2 >> 4) & 0xFFu);  // par_h2 <11:4>
            return true;
        case 0xE2u:  // par_h2 <3:0> in the high nibble, par_h1 <3:0> in the low
            *out = (uint8_t)(((c->par_h2 & 0x0Fu) << 4) | (c->par_h1 & 0x0Fu));
            return true;
        case 0xE3u:
            *out = (uint8_t)((c->par_h1 >> 4) & 0xFFu);  // par_h1 <11:4>
            return true;
        case 0xE4u:
            *out = (uint8_t)c->par_h3;
            return true;
        case 0xE5u:
            *out = (uint8_t)c->par_h4;
            return true;
        case 0xE6u:
            *out = (uint8_t)c->par_h5;
            return true;
        case 0xE7u:
            *out = c->par_h6;
            return true;
        case 0xE8u:
            *out = (uint8_t)c->par_h7;
            return true;
        case 0xE9u:
            *out = lsb(c->par_t1);
            return true;
        case 0xEAu:
            *out = msb(c->par_t1);
            return true;
        case 0xEBu:
            *out = lsb((uint16_t)c->par_g2);
            return true;
        case 0xECu:
            *out = msb((uint16_t)c->par_g2);
            return true;
        case 0xEDu:
            *out = (uint8_t)c->par_g1;
            return true;
        case 0xEEu:
            *out = (uint8_t)c->par_g3;
            return true;
        default:
            return false;
    }
}

uint8_t bme680_model_peek(const bme680_model_t *m, uint8_t addr)
{
    uint8_t value = 0u;
    if (addr == BME680_MODEL_REG_CHIP_ID) {
        return m->chip_id;
    }
    if (addr == BME680_MODEL_REG_VARIANT_ID) {
        return m->variant_id;
    }
    if (nvm_byte(&m->calibration, addr, &value)) {
        return value;
    }
    return m->regs[addr];
}

// The bits of each control register the datasheet defines (Table 20); a
// write stores those and drops the rest. 0 for a register it does not make
// writable.
static uint8_t writable_bits(uint8_t addr)
{
    if (addr >= BME680_MODEL_REG_IDAC_HEAT_0 && addr <= BME680_MODEL_REG_GAS_WAIT_0 + 9u) {
        return 0xFFu;  // idac_heat_x, res_heat_x, gas_wait_x
    }
    switch (addr) {
        case BME680_MODEL_REG_CTRL_GAS_0:
            return HEAT_OFF;
        case BME680_MODEL_REG_CTRL_GAS_1:
            return RUN_GAS | NB_CONV_MASK;
        case BME680_MODEL_REG_CTRL_HUM:
            return 0x40u | OSRS_MASK;  // spi_3w_int_en, osrs_h
        case BME680_MODEL_REG_STATUS:
            return 0x10u;  // spi_mem_page
        case BME680_MODEL_REG_CTRL_MEAS:
            return 0xFFu;
        case BME680_MODEL_REG_CONFIG:
            return (uint8_t)((FILTER_MASK << FILTER_SHIFT) | 0x01u);  // filter, spi_3w_en
        default:
            return 0u;
    }
}

static void reset_registers(bme680_model_t *m)
{
    memset(m->regs, 0, sizeof m->regs);
    m->regs[BME680_MODEL_REG_PRESS_MSB] = SKIPPED_MSB;
    m->regs[BME680_MODEL_REG_TEMP_MSB] = SKIPPED_MSB;
    m->regs[BME680_MODEL_REG_HUM_MSB] = SKIPPED_MSB;
    m->pointer = 0u;  // ASSUMED: the datasheet gives no reset state for it
    m->measuring = false;
}

static unsigned osrs_code(unsigned code)
{
    return code > OSRS_CODE_MAX ? OSRS_CODE_MAX : code;
}

static unsigned nb_conv(const bme680_model_t *m)
{
    return m->regs[BME680_MODEL_REG_CTRL_GAS_1] & NB_CONV_MASK;
}

static bool gas_runs(const bme680_model_t *m)
{
    return (m->regs[BME680_MODEL_REG_CTRL_GAS_1] & RUN_GAS) != 0u && nb_conv(m) <= NB_CONV_MAX;
}

static uint32_t gas_wait_ms(const bme680_model_t *m)
{
    const uint8_t w = m->regs[BME680_MODEL_REG_GAS_WAIT_0 + nb_conv(m)];
    return (uint32_t)(w & GAS_WAIT_STEPS_MASK) * GAS_WAIT_FACTOR[w >> GAS_WAIT_FACTOR_SHIFT];
}

static uint64_t ceil_ms(uint32_t us)
{
    return (uint64_t)(us / US_PER_MS + ((us % US_PER_MS) != 0u ? 1u : 0u));
}

// Writing 01 to mode<1:0> (5.3.1.3).
static void start_measurement(bme680_model_t *m, uint64_t now)
{
    const uint8_t meas = m->regs[BME680_MODEL_REG_CTRL_MEAS];
    const unsigned samples = SAMPLES[osrs_code((meas >> OSRS_T_SHIFT) & OSRS_MASK)] +
                             SAMPLES[osrs_code((meas >> OSRS_P_SHIFT) & OSRS_MASK)] +
                             SAMPLES[osrs_code(m->regs[BME680_MODEL_REG_CTRL_HUM] & OSRS_MASK)];
    // ASSUMED, Bosch's bme68x_get_meas_dur() (see the header).
    const uint32_t tph_us =
        BME680_MODEL_WAKE_UP_US + BME680_MODEL_SAMPLE_US * samples + BME680_MODEL_SWITCHING_US;
    // The heater runs only for a real gas conversion (Table 10), after the
    // T, P and H conversions (3.3.5); the gas slot is there either way, a
    // dummy one when the gas does not run (5.3.5.5).
    const uint32_t heat_us = gas_runs(m) ? gas_wait_ms(m) * US_PER_MS : 0u;
    const uint32_t total_us = tph_us + heat_us + BME680_MODEL_GAS_SLOT_US;

    m->measuring = true;
    m->measurement_started_ms = now;
    m->gas_phase_ms = now + ceil_ms(tph_us);
    m->measurement_ends_ms = m->faults.overrun_ms == BME680_MODEL_NEVER
                                 ? UINT64_MAX
                                 : now + ceil_ms(total_us) + m->faults.overrun_ms;
    // ASSUMED: new_data_0 clears when the next measurement starts.
    m->regs[BME680_MODEL_REG_MEAS_STATUS_0] = BME680_MODEL_MEASURING;
    m->stats.measurements++;
}

static unsigned tp_shift(const bme680_model_t *m, unsigned code)
{
    // 3.3.1-3.3.2: 20 bits with the IIR filter on, else 16 + (osrs - 1).
    const unsigned filter = (m->regs[BME680_MODEL_REG_CONFIG] >> FILTER_SHIFT) & FILTER_MASK;
    if (filter != 0u) {
        return 0u;
    }
    return TP_ADC_BITS - (TP_ADC_MIN_BITS + osrs_code(code) - 1u);
}

static void put20(bme680_model_t *m, uint8_t reg, uint32_t adc)
{
    m->regs[reg] = (uint8_t)((adc >> 12) & 0xFFu);
    m->regs[reg + 1u] = (uint8_t)((adc >> 4) & 0xFFu);
    m->regs[reg + 2u] = (uint8_t)((adc & 0x0Fu) << 4);
}

// ASSUMED: the gas ADC picks, of the ranges that can represent the reading,
// the one whose middle code is nearest it; the datasheet says only that
// gas_range_r reports the range used. Each range spans about 0.72 to 1.62
// times its middle and the middles are a factor of 2 apart, so "nearest
// middle" alone would sometimes pick a range whose lowest value is still
// above the reading. Beyond every range, the nearest end.
static uint8_t gas_range_for(const bme680_model_calibration_t *c, double ohm)
{
    if (ohm >= bme680_model_gas_resistance(c, 0u, 0u)) {
        return 0u;
    }
    uint8_t best = GAS_RANGES - 1u;
    double best_distance = INFINITY;
    for (uint8_t r = 0u; r < GAS_RANGES; r++) {
        const bool representable = ohm <= bme680_model_gas_resistance(c, 0u, r) &&
                                   ohm >= bme680_model_gas_resistance(c, GAS_ADC_COUNT - 1u, r);
        const double distance = fabs(log(ohm / bme680_model_gas_resistance(c, GAS_ADC_MIDDLE, r)));
        if (representable && distance < best_distance) {
            best = r;
            best_distance = distance;
        }
    }
    return best;
}

// The end of the measurement: raw values for the reading, written where
// Table 20 puts them, and the status bits of 5.3.5.
static void complete_measurement(bme680_model_t *m)
{
    const uint64_t at = m->measurement_ends_ms;
    bme680_model_reading_t r = m->default_reading;
    if (m->reading != NULL) {
        m->reading(m->reading_ctx, at, &r);
    }

    const bme680_model_calibration_t *c = &m->calibration;
    const uint8_t meas = m->regs[BME680_MODEL_REG_CTRL_MEAS];
    const unsigned osrs_t = (meas >> OSRS_T_SHIFT) & OSRS_MASK;
    const unsigned osrs_p = (meas >> OSRS_P_SHIFT) & OSRS_MASK;
    const unsigned osrs_h = m->regs[BME680_MODEL_REG_CTRL_HUM] & OSRS_MASK;

    // Temperature first: pressure and humidity are compensated with the
    // temperature a driver will compute from these very registers.
    curve_t curve = {.c = c, .shift = tp_shift(m, osrs_t)};
    const uint32_t t_code =
        invert(temperature_at, &curve, 1u << (TP_ADC_BITS - curve.shift), r.temperature_c);
    const uint32_t temp_adc = t_code << curve.shift;
    curve.temp_c = bme680_model_temperature(c, temp_adc, &curve.t_fine);
    if (osrs_t == 0u) {
        put20(m, BME680_MODEL_REG_TEMP_MSB, (uint32_t)SKIPPED_MSB << 12);
    } else {
        put20(m, BME680_MODEL_REG_TEMP_MSB, temp_adc);
    }

    curve.shift = tp_shift(m, osrs_p);
    if (osrs_p == 0u) {
        put20(m, BME680_MODEL_REG_PRESS_MSB, (uint32_t)SKIPPED_MSB << 12);
    } else {
        const uint32_t p_code = invert(pressure_at, &curve, 1u << (TP_ADC_BITS - curve.shift),
                                       (double)r.pressure_hpa * PA_PER_HPA);
        put20(m, BME680_MODEL_REG_PRESS_MSB, p_code << curve.shift);
    }

    if (osrs_h == 0u) {
        m->regs[BME680_MODEL_REG_HUM_MSB] = SKIPPED_MSB;
        m->regs[BME680_MODEL_REG_HUM_MSB + 1u] = 0u;
    } else {
        const uint32_t hum_adc = invert(humidity_at, &curve, HUM_ADC_COUNT, r.humidity_rh);
        m->regs[BME680_MODEL_REG_HUM_MSB] = (uint8_t)(hum_adc >> 8);
        m->regs[BME680_MODEL_REG_HUM_MSB + 1u] = (uint8_t)(hum_adc & 0xFFu);
    }

    uint8_t status = BME680_MODEL_NEW_DATA;
    if (gas_runs(m)) {
        curve.gas_range = gas_range_for(c, (double)r.gas_resistance_ohm);
        const uint32_t gas_adc =
            invert(gas_at, &curve, GAS_ADC_COUNT, (double)r.gas_resistance_ohm);
        const bool heater_on = (m->regs[BME680_MODEL_REG_CTRL_GAS_0] & HEAT_OFF) == 0u;
        // ASSUMED: the plate is at its target after BME680_MODEL_HEAT_UP_MS.
        const bool stable =
            heater_on && gas_wait_ms(m) >= BME680_MODEL_HEAT_UP_MS && !m->faults.heater_unstable;
        m->regs[BME680_MODEL_REG_GAS_R_MSB] = (uint8_t)(gas_adc >> 2);
        m->regs[BME680_MODEL_REG_GAS_R_LSB] =
            (uint8_t)(((gas_adc & 0x03u) << 6) |
                      (m->faults.gas_invalid ? 0u : BME680_MODEL_GAS_VALID) |
                      (stable ? BME680_MODEL_HEAT_STAB : 0u) | curve.gas_range);
        status |= (uint8_t)(nb_conv(m) & GAS_INDEX_MASK);
    } else {
        m->regs[BME680_MODEL_REG_GAS_R_MSB] = 0u;  // a dummy conversion: nothing valid
        m->regs[BME680_MODEL_REG_GAS_R_LSB] = 0u;
    }

    m->regs[BME680_MODEL_REG_MEAS_STATUS_0] = status;
    // "Sensor automatically returns to sleep mode afterwards" (Table 10).
    m->regs[BME680_MODEL_REG_CTRL_MEAS] = (uint8_t)(meas & ~CTRL_MEAS_MODE_MASK);
    m->measuring = false;
    m->last = r;
    m->stats.completed++;
}

// Brings the sensor's own time up to `now`.
static void advance(bme680_model_t *m, uint64_t now)
{
    if (!m->measuring) {
        return;
    }
    if (now >= m->measurement_ends_ms) {
        complete_measurement(m);
        return;
    }
    uint8_t status = BME680_MODEL_MEASURING;
    if (gas_runs(m) && now >= m->gas_phase_ms) {
        status |= BME680_MODEL_GAS_MEASURING;  // only during the gas phase (5.3.5.2)
    }
    m->regs[BME680_MODEL_REG_MEAS_STATUS_0] = status;
}

static void soft_reset(bme680_model_t *m, uint64_t now)
{
    // "The same effect like power-on reset" (5.3.1.5). ASSUMED: even while a
    // measurement runs.
    reset_registers(m);
    m->busy_until_ms = now + BME680_MODEL_START_UP_MS;
    m->stats.soft_resets++;
}

static void write_register(bme680_model_t *m, uint8_t addr, uint8_t value, uint64_t now)
{
    if (addr == BME680_MODEL_REG_RESET) {
        if (value == BME680_MODEL_SOFT_RESET_CMD) {
            soft_reset(m, now);
        }
        return;
    }
    const uint8_t bits = writable_bits(addr);
    if (bits == 0u) {
        m->stats.read_only_writes++;
        return;
    }
    if (m->measuring) {
        m->stats.ignored_writes++;  // see the header: dropped, mode changes too
        return;
    }
    m->regs[addr] = (uint8_t)(value & bits);
    if (addr == BME680_MODEL_REG_CTRL_MEAS &&
        (value & CTRL_MEAS_MODE_MASK) == BME680_MODEL_MODE_FORCED) {
        start_measurement(m, now);
    }
}

static bool address_refused(bme680_model_t *m, uint64_t now)
{
    bool refused = false;
    m->stats.calls++;
    if (m->faults.nack_call != 0u && m->stats.calls == m->faults.nack_call) {
        refused = true;
    } else if (m->faults.nack_address > 0u) {
        m->faults.nack_address--;
        refused = true;
    } else if (now < m->busy_until_ms) {
        // ASSUMED: silent until the start-up time has passed (see the header).
        m->stats.nacked_while_busy++;
        refused = true;
    }
    if (refused) {
        m->stats.address_nacks++;
    }
    return refused;
}

// 6.2.1: "the master sends pairs of register addresses and register data" --
// not an address and then auto-incremented data, as a register file takes
// them. A lone address, the whole of a write or after the pairs, sets where
// the next read starts (6.2.2).
static hal_status_t model_write(i2c_model_t *self, const uint8_t *d, size_t n, uint64_t now)
{
    bme680_model_t *m = (bme680_model_t *)self->impl;
    advance(m, now);
    if (address_refused(m, now)) {
        return HAL_ERR_NOT_FOUND;
    }
    size_t usable = n;
    hal_status_t result = HAL_OK;
    if (n > 0u && m->faults.nack_data > 0u) {
        m->faults.nack_data--;
        usable = n - 1u;  // the refused byte, and nothing after it, takes effect
        result = HAL_ERR_IO;
    }
    size_t i = 0u;
    for (; i + 1u < usable; i += 2u) {
        write_register(m, d[i], d[i + 1u], now);
        m->pointer = d[i];  // ASSUMED: left at the last address written
    }
    if (i < usable) {
        m->pointer = d[i];
    }
    return result;
}

// 6.2.2: bytes from the pointer, auto-incremented.
static hal_status_t model_read(i2c_model_t *self, uint8_t *d, size_t n, uint64_t now)
{
    bme680_model_t *m = (bme680_model_t *)self->impl;
    advance(m, now);
    if (address_refused(m, now)) {
        return HAL_ERR_NOT_FOUND;
    }
    if (m->measuring && m->pointer >= DATA_FIRST && m->pointer <= DATA_LAST) {
        m->stats.early_reads++;
    }
    for (size_t i = 0u; i < n; i++) {
        d[i] = bme680_model_peek(m, m->pointer);
        m->pointer = (uint8_t)(m->pointer + 1u);  // ASSUMED: wraps from 0xFF to 0x00
    }
    return HAL_OK;
}

void bme680_model_init(bme680_model_t *m, uint64_t now_ms)
{
    memset(m, 0, sizeof *m);
    m->model.write = model_write;
    m->model.read = model_read;
    m->model.impl = m;
    m->chip_id = BME680_MODEL_CHIP_ID;
    m->variant_id = BME680_MODEL_VARIANT_BME680;
    m->calibration = BME680_MODEL_BOARD_CALIBRATION;
    m->default_reading.temperature_c = DEFAULT_TEMPERATURE_C;
    m->default_reading.humidity_rh = DEFAULT_HUMIDITY_RH;
    m->default_reading.pressure_hpa = DEFAULT_PRESSURE_HPA;
    m->default_reading.gas_resistance_ohm = DEFAULT_GAS_RESISTANCE_OHM;
    bme680_model_power_cycle(m, now_ms);
}

i2c_model_t *bme680_model_port(bme680_model_t *m)
{
    return &m->model;
}

void bme680_model_set_reading(bme680_model_t *m, bme680_model_reading_fn fn, void *ctx)
{
    m->reading = fn;
    m->reading_ctx = ctx;
}

void bme680_model_power_cycle(bme680_model_t *m, uint64_t now_ms)
{
    reset_registers(m);
    m->busy_until_ms = now_ms + BME680_MODEL_START_UP_MS;
}
