// SPDX-License-Identifier: Apache-2.0

#include "scd41_model.h"

#include <string.h>

// Command codes, execution times and the "during measurement" column are
// Table 9 of the datasheet; argument and response word counts are the
// command descriptions in 3.6-3.11.
#define CMD_START_PERIODIC 0x21b1u
#define CMD_READ_MEASUREMENT 0xec05u
#define CMD_STOP_PERIODIC 0x3f86u
#define CMD_SET_TEMPERATURE_OFFSET 0x241du
#define CMD_GET_TEMPERATURE_OFFSET 0x2318u
#define CMD_SET_SENSOR_ALTITUDE 0x2427u
#define CMD_GET_SENSOR_ALTITUDE 0x2322u
#define CMD_AMBIENT_PRESSURE 0xe000u  // set and get differ only in the argument
#define CMD_FORCED_RECALIBRATION 0x362fu
#define CMD_SET_ASC_ENABLED 0x2416u
#define CMD_GET_ASC_ENABLED 0x2313u
#define CMD_SET_ASC_TARGET 0x243au
#define CMD_GET_ASC_TARGET 0x233fu
#define CMD_START_LOW_POWER_PERIODIC 0x21acu
#define CMD_GET_DATA_READY_STATUS 0xe4b8u
#define CMD_PERSIST_SETTINGS 0x3615u
#define CMD_GET_SERIAL_NUMBER 0x3682u
#define CMD_PERFORM_SELF_TEST 0x3639u
#define CMD_PERFORM_FACTORY_RESET 0x3632u
#define CMD_REINIT 0x3646u
#define CMD_GET_SENSOR_VARIANT 0x202fu
#define CMD_MEASURE_SINGLE_SHOT 0x219du
#define CMD_MEASURE_SINGLE_SHOT_RHT_ONLY 0x2196u
#define CMD_POWER_DOWN 0x36e0u
#define CMD_WAKE_UP 0x36f6u
#define CMD_SET_ASC_INITIAL_PERIOD 0x2445u
#define CMD_GET_ASC_INITIAL_PERIOD 0x2340u
#define CMD_SET_ASC_STANDARD_PERIOD 0x244eu
#define CMD_GET_ASC_STANDARD_PERIOD 0x234bu

#define WORD_BYTES 3u     // two data bytes and their CRC
#define COMMAND_BYTES 2u  // a command word carries no CRC (3.12)

// Bits 15..12 of the variant word name the part (Table 31).
#define VARIANT_MASK 0xF000u

// get_data_ready_status: "if the least significant 11 bits are 0, data not
// ready" (3.9.2). 0x8000 is the datasheet's own not-ready example; which of
// the low bits a ready sensor sets is not said, so the model sets one.
#define DATA_READY_NOT_READY 0x8000u
#define DATA_READY_READY 0x8001u

// perform_forced_recalibration answers 0xffff when the sensor was not
// operated before it (3.8.1), and the correction plus 0x8000 otherwise; the
// model always corrects by 0.
#define FRC_FAILED 0xFFFFu
#define FRC_NO_CORRECTION 0x8000u

// set_ambient_pressure's default, 101300 Pa (3.7.5), in its hPa word.
#define DEFAULT_AMBIENT_PRESSURE_HPA 1013u

// The sensor's factory settings (3.7-3.11). The temperature offset is 4 C,
// in ticks of 175/65535 C.
#define FACTORY_TEMPERATURE_OFFSET 1498u
#define FACTORY_SENSOR_ALTITUDE_M 0u
#define FACTORY_ASC_ENABLED 1u
#define FACTORY_ASC_TARGET_PPM 400u
#define FACTORY_ASC_INITIAL_PERIOD_H 44u
#define FACTORY_ASC_STANDARD_PERIOD_H 156u

// The datasheet's example reading (Table 11): 500 ppm, 25 C, 37 %RH.
#define DEFAULT_CO2_PPM 500.0f
#define DEFAULT_TEMPERATURE_C 25.0f
#define DEFAULT_HUMIDITY_RH 37.0f

// The conversions of 3.6.2: T = -45 + 175 * word / 65535, RH = 100 * word /
// 65535.
#define TICKS_FULL_SCALE 65535.0
#define TEMPERATURE_OFFSET_C 45.0
#define TEMPERATURE_SPAN_C 175.0
#define HUMIDITY_SPAN_RH 100.0

#define CRC8_POLYNOMIAL 0x31u
#define CRC8_INIT 0xFFu

typedef struct {
    uint16_t code;
    uint16_t exec_ms;
    bool during_measurement;
    uint8_t words_in;
    uint8_t words_out;
} command_t;

static const command_t COMMANDS[] = {
    {CMD_START_PERIODIC, 0u, false, 0u, 0u},
    {CMD_READ_MEASUREMENT, 1u, true, 0u, 3u},
    {CMD_STOP_PERIODIC, SCD41_MODEL_STOP_MS, true, 0u, 0u},
    {CMD_SET_TEMPERATURE_OFFSET, 1u, false, 1u, 0u},
    {CMD_GET_TEMPERATURE_OFFSET, 1u, false, 0u, 1u},
    {CMD_SET_SENSOR_ALTITUDE, 1u, false, 1u, 0u},
    {CMD_GET_SENSOR_ALTITUDE, 1u, false, 0u, 1u},
    {CMD_AMBIENT_PRESSURE, 1u, true, 1u, 0u},
    {CMD_AMBIENT_PRESSURE, 1u, true, 0u, 1u},
    {CMD_FORCED_RECALIBRATION, 400u, false, 1u, 1u},
    {CMD_SET_ASC_ENABLED, 1u, false, 1u, 0u},
    {CMD_GET_ASC_ENABLED, 1u, false, 0u, 1u},
    {CMD_SET_ASC_TARGET, 1u, false, 1u, 0u},
    {CMD_GET_ASC_TARGET, 1u, false, 0u, 1u},
    {CMD_START_LOW_POWER_PERIODIC, 0u, false, 0u, 0u},
    {CMD_GET_DATA_READY_STATUS, 1u, true, 0u, 1u},
    {CMD_PERSIST_SETTINGS, 800u, false, 0u, 0u},
    {CMD_GET_SERIAL_NUMBER, 1u, false, 0u, 3u},
    {CMD_PERFORM_SELF_TEST, 10000u, false, 0u, 1u},
    {CMD_PERFORM_FACTORY_RESET, 1200u, false, 0u, 0u},
    {CMD_REINIT, 30u, false, 0u, 0u},
    {CMD_GET_SENSOR_VARIANT, 1u, false, 0u, 1u},
    {CMD_MEASURE_SINGLE_SHOT, SCD41_MODEL_SINGLE_SHOT_MS, false, 0u, 0u},
    {CMD_MEASURE_SINGLE_SHOT_RHT_ONLY, 50u, false, 0u, 0u},
    {CMD_POWER_DOWN, 1u, false, 0u, 0u},
    {CMD_WAKE_UP, 30u, false, 0u, 0u},
    {CMD_SET_ASC_INITIAL_PERIOD, 1u, false, 1u, 0u},
    {CMD_GET_ASC_INITIAL_PERIOD, 1u, false, 0u, 1u},
    {CMD_SET_ASC_STANDARD_PERIOD, 1u, false, 1u, 0u},
    {CMD_GET_ASC_STANDARD_PERIOD, 1u, false, 0u, 1u},
};
#define COMMAND_COUNT (sizeof COMMANDS / sizeof COMMANDS[0])

uint8_t scd41_model_crc8(const uint8_t *d, size_t n)
{
    // Unsigned throughout: a uint8_t promotes to int in every shift, which
    // GCC's sanitizer builds then flag as a sign change.
    unsigned crc = CRC8_INIT;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (unsigned bit = 0; bit < 8u; bit++) {
            crc = ((crc & 0x80u) != 0u) ? ((crc << 1) ^ CRC8_POLYNOMIAL) : (crc << 1);
            crc &= 0xFFu;
        }
    }
    return (uint8_t)crc;
}

static uint16_t to_ticks(double x)
{
    if (!(x > 0.0)) {
        return 0u;  // also catches NaN
    }
    if (x >= TICKS_FULL_SCALE) {
        return UINT16_MAX;
    }
    return (uint16_t)(x + 0.5);
}

static uint16_t be16(const uint8_t *d)
{
    return (uint16_t)(((unsigned)d[0] << 8) | d[1]);
}

static const command_t *find_command(uint16_t code, size_t n)
{
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (COMMANDS[i].code == code && n == COMMAND_BYTES + WORD_BYTES * COMMANDS[i].words_in) {
            return &COMMANDS[i];
        }
    }
    return NULL;
}

static bool is_measuring(const scd41_model_t *m)
{
    return m->state == SCD41_MODEL_PERIODIC || m->state == SCD41_MODEL_LOW_POWER_PERIODIC;
}

static void produce(scd41_model_t *m, uint64_t at_ms, bool rht_only)
{
    m->measured_since_power_up = true;
    if (m->faults.hold_data) {
        return;
    }
    scd41_model_reading_t r = m->default_reading;
    if (m->reading != NULL) {
        m->reading(m->reading_ctx, at_ms, &r);
    }
    // measure_single_shot_rht_only returns CO2 as 0 ppm (3.11.2).
    m->data[0] = rht_only ? 0u : to_ticks((double)r.co2_ppm);
    m->data[1] = to_ticks(((double)r.temperature_c + TEMPERATURE_OFFSET_C) * TICKS_FULL_SCALE /
                          TEMPERATURE_SPAN_C);
    m->data[2] = to_ticks((double)r.humidity_rh * TICKS_FULL_SCALE / HUMIDITY_SPAN_RH);
    m->data_ready = true;
    m->stats.measurements++;
}

// Brings the sensor's own time up to `now`: periodic measurements that have
// completed, and a single shot whose execution time has passed. Only the
// newest periodic reading survives, as in the sensor's one-deep buffer.
static void advance(scd41_model_t *m, uint64_t now)
{
    if (is_measuring(m) && now >= m->next_measurement_ms) {
        const uint64_t interval = (m->state == SCD41_MODEL_PERIODIC)
                                      ? SCD41_MODEL_INTERVAL_MS
                                      : SCD41_MODEL_LOW_POWER_INTERVAL_MS;
        const uint64_t at =
            m->next_measurement_ms + ((now - m->next_measurement_ms) / interval) * interval;
        produce(m, at, false);
        m->next_measurement_ms = at + interval;
    }
    if (m->single_shot_pending && now >= m->busy_until_ms) {
        produce(m, m->busy_until_ms, m->single_shot_rht_only);
        m->single_shot_pending = false;
    }
}

static bool address_refused(scd41_model_t *m, uint64_t now)
{
    bool refused = false;
    if (m->faults.nack_address > 0u) {
        m->faults.nack_address--;
        refused = true;
    } else if (now < m->busy_until_ms) {
        // ASSUMED: refused while a command executes (see the header).
        m->stats.nacked_while_busy++;
        refused = true;
    } else if (m->state == SCD41_MODEL_SLEEP) {
        refused = true;
    }
    if (refused) {
        m->stats.address_nacks++;
    }
    return refused;
}

static hal_status_t reject(scd41_model_t *m)
{
    // ASSUMED: refused with a data NACK (see the header).
    m->stats.rejected++;
    return HAL_ERR_IO;
}

static void respond(scd41_model_t *m, const uint16_t *words, size_t count)
{
    m->response_len = 0u;
    for (size_t i = 0; i < count; i++) {
        uint8_t *w = &m->response[m->response_len];
        w[0] = (uint8_t)(words[i] >> 8);
        w[1] = (uint8_t)(words[i] & 0xFFu);
        w[2] = scd41_model_crc8(w, 2u);
        m->response_len += WORD_BYTES;
    }
}

static void respond1(scd41_model_t *m, uint16_t word)
{
    respond(m, &word, 1u);
}

static void remember(scd41_model_t *m, uint16_t code)
{
    if (m->stats.history_len == SCD41_MODEL_HISTORY) {
        memmove(&m->stats.history[0], &m->stats.history[1],
                (SCD41_MODEL_HISTORY - 1u) * sizeof m->stats.history[0]);
        m->stats.history_len--;
    }
    m->stats.history[m->stats.history_len++] = code;
}

static bool supports_single_shot(const scd41_model_t *m)
{
    // SCD41 and SCD43 only (3.11); the SCD40 does not have the mode.
    return (m->variant_word & VARIANT_MASK) != (SCD41_MODEL_VARIANT_SCD40 & VARIANT_MASK);
}

static bool apply_setting(scd41_model_t *m, uint16_t code, const uint16_t *arg)
{
    for (size_t i = 0; i < SCD41_MODEL_SETTINGS; i++) {
        scd41_model_setting_t *s = &m->settings[i];
        if (arg != NULL && code == s->set_code) {
            s->value = *arg;
            return true;
        }
        if (arg == NULL && code == s->get_code) {
            respond1(m, s->value);
            return true;
        }
    }
    return false;
}

static void execute(scd41_model_t *m, const command_t *c, const uint16_t *arg, uint64_t now)
{
    switch (c->code) {
        case CMD_START_PERIODIC:
            m->state = SCD41_MODEL_PERIODIC;
            m->next_measurement_ms = now + SCD41_MODEL_INTERVAL_MS;
            m->data_ready = false;
            break;
        case CMD_START_LOW_POWER_PERIODIC:
            m->state = SCD41_MODEL_LOW_POWER_PERIODIC;
            m->next_measurement_ms = now + SCD41_MODEL_LOW_POWER_INTERVAL_MS;
            m->data_ready = false;
            break;
        case CMD_STOP_PERIODIC:
            // Accepted in idle too: Sensirion's example sends it first,
            // whatever the state. ASSUMED: a buffered reading is discarded.
            m->state = SCD41_MODEL_IDLE;
            m->data_ready = false;
            break;
        case CMD_READ_MEASUREMENT:
            if (m->data_ready) {
                respond(m, m->data, 3u);  // the buffer is emptied on read-out (3.6.2)
                m->data_ready = false;
            }
            break;
        case CMD_GET_DATA_READY_STATUS:
            respond1(m, m->data_ready ? DATA_READY_READY : DATA_READY_NOT_READY);
            break;
        case CMD_AMBIENT_PRESSURE:
            if (arg != NULL) {
                m->ambient_pressure_hpa = *arg;
            } else {
                respond1(m, m->ambient_pressure_hpa);
            }
            break;
        case CMD_FORCED_RECALIBRATION:
            respond1(m, m->measured_since_power_up ? FRC_NO_CORRECTION : FRC_FAILED);
            break;
        case CMD_GET_SERIAL_NUMBER: {
            const uint16_t words[3] = {
                (uint16_t)((m->serial >> 32) & 0xFFFFu),
                (uint16_t)((m->serial >> 16) & 0xFFFFu),
                (uint16_t)(m->serial & 0xFFFFu),
            };
            respond(m, words, 3u);
            break;
        }
        case CMD_GET_SENSOR_VARIANT:
            respond1(m, m->variant_word);
            break;
        case CMD_PERFORM_SELF_TEST:
            respond1(m, 0u);  // no malfunction (3.10.3)
            break;
        case CMD_PERSIST_SETTINGS:
            for (size_t i = 0; i < SCD41_MODEL_SETTINGS; i++) {
                m->settings[i].persisted = m->settings[i].value;
            }
            break;
        case CMD_PERFORM_FACTORY_RESET:
            for (size_t i = 0; i < SCD41_MODEL_SETTINGS; i++) {
                m->settings[i].persisted = m->settings[i].factory;
                m->settings[i].value = m->settings[i].factory;
            }
            break;
        case CMD_REINIT:
            // Reloads user settings from EEPROM (3.10.5). ASSUMED: the ambient
            // pressure, never persisted, returns to its default too.
            for (size_t i = 0; i < SCD41_MODEL_SETTINGS; i++) {
                m->settings[i].value = m->settings[i].persisted;
            }
            m->ambient_pressure_hpa = DEFAULT_AMBIENT_PRESSURE_HPA;
            break;
        case CMD_MEASURE_SINGLE_SHOT:
        case CMD_MEASURE_SINGLE_SHOT_RHT_ONLY:
            m->single_shot_pending = true;
            m->single_shot_rht_only = (c->code == CMD_MEASURE_SINGLE_SHOT_RHT_ONLY);
            m->data_ready = false;
            break;
        case CMD_POWER_DOWN:
            m->state = SCD41_MODEL_SLEEP;
            break;
        case CMD_WAKE_UP:
            break;  // ASSUMED: an awake sensor takes it as a no-op
        default:
            (void)apply_setting(m, c->code, arg);
            break;
    }
}

static hal_status_t model_write(i2c_model_t *self, const uint8_t *d, size_t n, uint64_t now)
{
    scd41_model_t *m = (scd41_model_t *)self->impl;
    advance(m, now);

    // A sleeping sensor hears wake_up but never acknowledges it (3.11.4).
    if (m->state == SCD41_MODEL_SLEEP && now >= m->busy_until_ms && m->faults.nack_address == 0u &&
        n >= COMMAND_BYTES && be16(d) == CMD_WAKE_UP) {
        m->state = SCD41_MODEL_IDLE;
        m->busy_until_ms = now + find_command(CMD_WAKE_UP, COMMAND_BYTES)->exec_ms;
        remember(m, CMD_WAKE_UP);
        m->stats.address_nacks++;
        return HAL_ERR_NOT_FOUND;
    }
    if (address_refused(m, now)) {
        return HAL_ERR_NOT_FOUND;
    }
    if (n == 0u) {
        return HAL_OK;  // an address-only probe
    }

    // ASSUMED: a new write abandons a response nobody read.
    m->response_len = 0u;

    if (m->faults.nack_data > 0u) {
        m->faults.nack_data--;
        m->stats.rejected++;
        return HAL_ERR_IO;
    }
    if (n < COMMAND_BYTES) {
        return reject(m);  // half a command word
    }
    const command_t *c = find_command(be16(d), n);
    if (c == NULL) {
        return reject(m);  // unknown, or the wrong number of argument bytes
    }

    uint16_t arg = 0u;
    if (c->words_in > 0u) {
        const uint8_t *w = &d[COMMAND_BYTES];
        if (scd41_model_crc8(w, 2u) != w[2]) {
            return reject(m);
        }
        arg = be16(w);
    }
    if (is_measuring(m) && !c->during_measurement) {
        return reject(m);
    }
    if ((c->code == CMD_MEASURE_SINGLE_SHOT || c->code == CMD_MEASURE_SINGLE_SHOT_RHT_ONLY) &&
        !supports_single_shot(m)) {
        return reject(m);
    }

    execute(m, c, c->words_in > 0u ? &arg : NULL, now);
    m->busy_until_ms = now + c->exec_ms;
    if (c->code == CMD_MEASURE_SINGLE_SHOT) {
        m->busy_until_ms += m->faults.single_shot_overrun_ms;
    }
    remember(m, c->code);
    m->stats.accepted++;
    return HAL_OK;
}

static hal_status_t model_read(i2c_model_t *self, uint8_t *d, size_t n, uint64_t now)
{
    scd41_model_t *m = (scd41_model_t *)self->impl;
    advance(m, now);

    if (address_refused(m, now)) {
        return HAL_ERR_NOT_FOUND;
    }
    if (m->response_len == 0u) {
        // Nothing to say -- read_measurement with an empty buffer (3.6.2).
        m->stats.reads_without_data++;
        m->stats.address_nacks++;
        return HAL_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < n; i++) {
        d[i] = (i < m->response_len) ? m->response[i] : 0xFFu;  // SDA released past the end
    }
    if (m->faults.corrupt_crc > 0u && n >= WORD_BYTES) {
        m->faults.corrupt_crc--;
        d[2] = (uint8_t)~d[2];
    }
    m->response_len = 0u;
    return HAL_OK;
}

static void init_settings(scd41_model_t *m)
{
    static const struct {
        uint16_t set_code;
        uint16_t get_code;
        uint16_t factory;
    } TABLE[SCD41_MODEL_SETTINGS] = {
        {CMD_SET_TEMPERATURE_OFFSET, CMD_GET_TEMPERATURE_OFFSET, FACTORY_TEMPERATURE_OFFSET},
        {CMD_SET_SENSOR_ALTITUDE, CMD_GET_SENSOR_ALTITUDE, FACTORY_SENSOR_ALTITUDE_M},
        {CMD_SET_ASC_ENABLED, CMD_GET_ASC_ENABLED, FACTORY_ASC_ENABLED},
        {CMD_SET_ASC_TARGET, CMD_GET_ASC_TARGET, FACTORY_ASC_TARGET_PPM},
        {CMD_SET_ASC_INITIAL_PERIOD, CMD_GET_ASC_INITIAL_PERIOD, FACTORY_ASC_INITIAL_PERIOD_H},
        {CMD_SET_ASC_STANDARD_PERIOD, CMD_GET_ASC_STANDARD_PERIOD, FACTORY_ASC_STANDARD_PERIOD_H},
    };
    for (size_t i = 0; i < SCD41_MODEL_SETTINGS; i++) {
        m->settings[i].set_code = TABLE[i].set_code;
        m->settings[i].get_code = TABLE[i].get_code;
        m->settings[i].factory = TABLE[i].factory;
        m->settings[i].persisted = TABLE[i].factory;
        m->settings[i].value = TABLE[i].factory;
    }
}

void scd41_model_init(scd41_model_t *m, uint64_t now_ms)
{
    memset(m, 0, sizeof *m);
    m->model.write = model_write;
    m->model.read = model_read;
    m->model.impl = m;
    m->serial = SCD41_MODEL_DEFAULT_SERIAL;
    m->variant_word = SCD41_MODEL_VARIANT_SCD41;
    m->default_reading.co2_ppm = DEFAULT_CO2_PPM;
    m->default_reading.temperature_c = DEFAULT_TEMPERATURE_C;
    m->default_reading.humidity_rh = DEFAULT_HUMIDITY_RH;
    init_settings(m);
    scd41_model_power_cycle(m, now_ms);
}

i2c_model_t *scd41_model_port(scd41_model_t *m)
{
    return &m->model;
}

void scd41_model_set_reading(scd41_model_t *m, scd41_model_reading_fn fn, void *ctx)
{
    m->reading = fn;
    m->reading_ctx = ctx;
}

void scd41_model_power_cycle(scd41_model_t *m, uint64_t now_ms)
{
    m->state = SCD41_MODEL_IDLE;
    m->busy_until_ms = now_ms + SCD41_MODEL_POWER_UP_MS;
    m->next_measurement_ms = 0u;
    m->single_shot_pending = false;
    m->measured_since_power_up = false;
    m->data_ready = false;
    m->response_len = 0u;
    m->ambient_pressure_hpa = DEFAULT_AMBIENT_PRESSURE_HPA;
    for (size_t i = 0; i < SCD41_MODEL_SETTINGS; i++) {
        m->settings[i].value = m->settings[i].persisted;
    }
}

void scd41_model_set_measuring(scd41_model_t *m, uint64_t started_ms)
{
    m->state = SCD41_MODEL_PERIODIC;
    m->busy_until_ms = started_ms;
    m->next_measurement_ms = started_ms + SCD41_MODEL_INTERVAL_MS;
    m->measured_since_power_up = true;
}
