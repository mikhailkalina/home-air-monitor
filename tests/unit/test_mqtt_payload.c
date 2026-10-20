// SPDX-License-Identifier: Apache-2.0
//
// core/reporting/mqtt_payload.c: the state message, byte for byte, for every
// status and every accuracy, for numbers JSON cannot hold, negative and
// negative-zero temperatures, and at the edge of the buffer.
// tests/unit/mqtt_messages_check.py parses the same messages with a real
// JSON parser.

#include "reporting/mqtt_payload.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "app/sensor_manager.h"
#include "hac_test.h"
#include "recording/record_format.h"

#define TS 1000u
#define SCD4X 1u
#define BME68X 2u

static air_reading_t full_reading(void)
{
    air_reading_t r;
    air_reading_init(&r);
    r.temperature_c = measurement_ok(23.41f, TS, BME68X);
    r.humidity_rh = measurement_ok(48.2f, TS, BME68X);
    r.pressure_hpa = measurement_ok(1006.2144f, TS, BME68X);
    r.co2_ppm = measurement_ok(812.0f, TS, SCD4X);
    r.iaq = measurement_ok(51.37f, TS, BME68X);
    r.iaq.accuracy = ACC_LOW;
    r.gas_resistance_ohm = measurement_ok(153204.0f, TS, BME68X);
    r.net_connected = true;
    r.wifi_rssi_dbm = -61;
    r.uptime_s = 3600u;
    return r;
}

static size_t state_of(const air_reading_t *r, char *buf, size_t cap)
{
    return mqtt_payload_state(buf, cap, r);
}

static bool contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static void a_full_reading_is_every_key_in_order(void)
{
    const air_reading_t r = full_reading();
    char buf[MQTT_PAYLOAD_STATE_MAX];
    const char *expected = "{\"temperature\":23.41,\"temperature_status\":\"ok\","
                           "\"humidity\":48.20,\"humidity_status\":\"ok\","
                           "\"pressure\":1006.214,\"pressure_status\":\"ok\","
                           "\"co2\":812,\"co2_status\":\"ok\","
                           "\"iaq\":51.37,\"iaq_status\":\"ok\",\"iaq_accuracy\":\"low\","
                           "\"gas_resistance\":153204,\"gas_resistance_status\":\"ok\","
                           "\"rssi\":-61,\"uptime\":3600}";
    HAC_CHECK_EQ_U64(state_of(&r, buf, sizeof(buf)), strlen(expected));
    HAC_CHECK(strcmp(buf, expected) == 0);
}

static void nothing_known_yet_is_every_key_null_and_unavailable(void)
{
    air_reading_t r;
    air_reading_init(&r);
    char buf[MQTT_PAYLOAD_STATE_MAX];
    const char *expected = "{\"temperature\":null,\"temperature_status\":\"unavailable\","
                           "\"humidity\":null,\"humidity_status\":\"unavailable\","
                           "\"pressure\":null,\"pressure_status\":\"unavailable\","
                           "\"co2\":null,\"co2_status\":\"unavailable\","
                           "\"iaq\":null,\"iaq_status\":\"unavailable\",\"iaq_accuracy\":null,"
                           "\"gas_resistance\":null,\"gas_resistance_status\":\"unavailable\","
                           "\"rssi\":null,\"uptime\":0}";
    HAC_CHECK_EQ_U64(state_of(&r, buf, sizeof(buf)), strlen(expected));
    HAC_CHECK(strcmp(buf, expected) == 0);
}

// Every status of every value: a number for VAL_OK only, the status word
// beside it. A stale value keeps its number on the screen and in the
// recordings, but not here: HA would stamp it with the time it arrived.
static void every_status_of_every_value(void)
{
    static const char *const KEYS[MQTT_VALUE_COUNT] = {
        "temperature", "humidity", "pressure", "co2", "iaq", "gas_resistance",
    };
    static const struct {
        value_status_t status;
        const char *word;
    } STATUSES[] = {
        {VAL_WARMUP, "warmup"},
        {VAL_STALE, "stale"},
        {VAL_ERROR, "error"},
        {VAL_UNAVAILABLE, "unavailable"},
    };
    for (unsigned v = 0u; v < (unsigned)MQTT_VALUE_COUNT; ++v) {
        for (size_t s = 0u; s < sizeof(STATUSES) / sizeof(STATUSES[0]); ++s) {
            air_reading_t r = full_reading();
            measurement_f32_t *m = (v == MQTT_VALUE_TEMPERATURE) ? &r.temperature_c
                                   : (v == MQTT_VALUE_HUMIDITY)  ? &r.humidity_rh
                                   : (v == MQTT_VALUE_PRESSURE)  ? &r.pressure_hpa
                                   : (v == MQTT_VALUE_CO2)       ? &r.co2_ppm
                                   : (v == MQTT_VALUE_IAQ)       ? &r.iaq
                                                                 : &r.gas_resistance_ohm;
            // The number stays in the measurement, as ageing leaves it.
            m->status = STATUSES[s].status;

            char buf[MQTT_PAYLOAD_STATE_MAX];
            HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
            char fragment[96];
            fragment[0] = '\0';
            strcat(fragment, "\"");
            strcat(fragment, KEYS[v]);
            strcat(fragment, "\":null,\"");
            strcat(fragment, KEYS[v]);
            strcat(fragment, "_status\":\"");
            strcat(fragment, STATUSES[s].word);
            strcat(fragment, "\"");
            HAC_CHECK(contains(buf, fragment));
            // The other values are untouched.
            HAC_CHECK(contains(buf, (v == MQTT_VALUE_CO2) ? "\"temperature\":23.41"
                                                          : "\"co2\":812,\"co2_status\":\"ok\""));
        }
    }
}

static void every_accuracy_of_iaq(void)
{
    static const struct {
        value_accuracy_t accuracy;
        const char *text;
    } ACCURACIES[] = {
        {ACC_NOT_REPORTED, "\"iaq_accuracy\":null,"},
        {ACC_UNRELIABLE, "\"iaq_accuracy\":\"unreliable\","},
        {ACC_LOW, "\"iaq_accuracy\":\"low\","},
        {ACC_MEDIUM, "\"iaq_accuracy\":\"medium\","},
        {ACC_HIGH, "\"iaq_accuracy\":\"high\","},
        // Outside the scale: no verdict, not an invented one.
        {(value_accuracy_t)99, "\"iaq_accuracy\":null,"},
    };
    for (size_t a = 0u; a < sizeof(ACCURACIES) / sizeof(ACCURACIES[0]); ++a) {
        air_reading_t r = full_reading();
        r.iaq.accuracy = ACCURACIES[a].accuracy;
        char buf[MQTT_PAYLOAD_STATE_MAX];
        HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
        HAC_CHECK(contains(buf, "\"iaq\":51.37,\"iaq_status\":\"ok\","));
        HAC_CHECK(contains(buf, ACCURACIES[a].text));
    }

    // BSEC's accuracy 0: warming up, rated unreliable -- the accuracy still
    // published beside the missing number.
    air_reading_t r = full_reading();
    r.iaq = measurement_status(VAL_WARMUP, TS, BME68X);
    r.iaq.accuracy = ACC_UNRELIABLE;
    char buf[MQTT_PAYLOAD_STATE_MAX];
    HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
    HAC_CHECK(
        contains(buf, "\"iaq\":null,\"iaq_status\":\"warmup\",\"iaq_accuracy\":\"unreliable\","));
}

// A VAL_OK that is not a number JSON can hold is published as what
// measurement_ok() would have made of it: an error.
static void nan_infinity_and_overflow_are_errors_not_numbers(void)
{
    const float bad[] = {NAN, INFINITY, -INFINITY, 1e20f};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        air_reading_t r = full_reading();
        r.temperature_c.value = bad[i];  // VAL_OK, as a buggy source might leave it
        r.gas_resistance_ohm.value = bad[i];
        char buf[MQTT_PAYLOAD_STATE_MAX];
        HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
        HAC_CHECK(contains(buf, "\"temperature\":null,\"temperature_status\":\"error\","));
        HAC_CHECK(contains(buf, "\"gas_resistance\":null,\"gas_resistance_status\":\"error\","));
        HAC_CHECK(!contains(buf, "nan") && !contains(buf, "inf"));
    }
}

static void negative_temperatures_and_negative_zero(void)
{
    static const struct {
        float value;
        const char *text;
    } CASES[] = {
        {-12.25f, "\"temperature\":-12.25,"},
        {-0.5f, "\"temperature\":-0.50,"},
        {-0.0f, "\"temperature\":0.00,"},
        {-0.004f, "\"temperature\":0.00,"},  // rounds to zero: never "-0.00"
        {-0.006f, "\"temperature\":-0.01,"},
        {0.004f, "\"temperature\":0.00,"},
        {-40.0f, "\"temperature\":-40.00,"},
    };
    for (size_t i = 0u; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        air_reading_t r = full_reading();
        r.temperature_c = measurement_ok(CASES[i].value, TS, BME68X);
        char buf[MQTT_PAYLOAD_STATE_MAX];
        HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
        HAC_CHECK(contains(buf, CASES[i].text));
        HAC_CHECK(!contains(buf, "-0.00"));
    }
}

static void rssi_is_published_only_while_the_network_is_up(void)
{
    air_reading_t r = full_reading();
    char buf[MQTT_PAYLOAD_STATE_MAX];
    r.wifi_rssi_dbm = -128;
    HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
    HAC_CHECK(contains(buf, "\"rssi\":-128,"));
    r.wifi_rssi_dbm = 0;
    HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
    HAC_CHECK(contains(buf, "\"rssi\":0,"));
    r.net_connected = false;
    r.wifi_rssi_dbm = -61;
    HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
    HAC_CHECK(contains(buf, "\"rssi\":null,"));
    r.uptime_s = UINT32_MAX;
    HAC_CHECK(state_of(&r, buf, sizeof(buf)) != 0u);
    HAC_CHECK(contains(buf, "\"uptime\":4294967295}"));
}

static void a_buffer_exactly_large_enough_and_one_byte_short(void)
{
    const air_reading_t r = full_reading();
    char reference[MQTT_PAYLOAD_STATE_MAX];
    const size_t n = state_of(&r, reference, sizeof(reference));
    HAC_CHECK(n > 0u);

    char buf[MQTT_PAYLOAD_STATE_MAX];
    memset(buf, 'X', sizeof(buf));
    HAC_CHECK_EQ_U64(state_of(&r, buf, n + 1u), n);
    HAC_CHECK(strcmp(buf, reference) == 0);

    memset(buf, 'X', sizeof(buf));
    HAC_CHECK_EQ_U64(state_of(&r, buf, n), 0u);
    HAC_CHECK(buf[0] == '\0');  // nothing that could be sent by mistake
    HAC_CHECK(buf[n] == 'X');   // and nothing written past the buffer

    HAC_CHECK_EQ_U64(state_of(&r, buf, 0u), 0u);
}

// The longest message there can be still fits MQTT_PAYLOAD_STATE_MAX, with
// the margin the header claims.
static void the_longest_message_fits(void)
{
    air_reading_t r = full_reading();
    // Each at the largest magnitude text_append_fixed() writes for its decimals.
    r.temperature_c.value = -9.9e12f;
    r.humidity_rh.value = -9.9e12f;
    r.pressure_hpa.value = -9.9e11f;
    r.co2_ppm.value = -9.9e14f;
    r.iaq.value = -9.9e12f;
    r.iaq.accuracy = ACC_UNRELIABLE;
    r.gas_resistance_ohm.value = -9.9e14f;
    r.wifi_rssi_dbm = INT8_MIN;
    r.uptime_s = UINT32_MAX;
    char buf[MQTT_PAYLOAD_STATE_MAX];
    const size_t numbers = state_of(&r, buf, sizeof(buf));
    HAC_CHECK(numbers > 0u && numbers < 400u);
    HAC_CHECK(!contains(buf, "null,\"temperature_status\""));  // all written as numbers

    air_reading_t worded = r;
    worded.temperature_c.status = VAL_UNAVAILABLE;
    worded.humidity_rh.status = VAL_UNAVAILABLE;
    worded.pressure_hpa.status = VAL_UNAVAILABLE;
    worded.co2_ppm.status = VAL_UNAVAILABLE;
    worded.iaq.status = VAL_UNAVAILABLE;
    worded.gas_resistance_ohm.status = VAL_UNAVAILABLE;
    const size_t words = state_of(&worded, buf, sizeof(buf));
    HAC_CHECK(words > 0u && words < 400u);
}

// The published decimals are the recordings' (ADR 0029).
static void the_decimals_are_the_recordings(void)
{
    static const sensor_field_t FIELDS[MQTT_VALUE_COUNT] = {
        [MQTT_VALUE_TEMPERATURE] = SENSOR_FIELD_TEMPERATURE,
        [MQTT_VALUE_HUMIDITY] = SENSOR_FIELD_HUMIDITY,
        [MQTT_VALUE_PRESSURE] = SENSOR_FIELD_PRESSURE,
        [MQTT_VALUE_CO2] = SENSOR_FIELD_CO2,
        [MQTT_VALUE_IAQ] = SENSOR_FIELD_IAQ,
        [MQTT_VALUE_GAS_RESISTANCE] = SENSOR_FIELD_GAS_RESISTANCE,
    };
    for (unsigned v = 0u; v < (unsigned)MQTT_VALUE_COUNT; ++v) {
        HAC_CHECK_EQ_U64(mqtt_payload_decimals((mqtt_value_t)v), record_decimals(FIELDS[v]));
    }
    HAC_CHECK(mqtt_payload_key(MQTT_VALUE_COUNT) == NULL);
    HAC_CHECK(mqtt_payload_status_key(MQTT_VALUE_COUNT) == NULL);
    HAC_CHECK(strcmp(mqtt_payload_status_key(MQTT_VALUE_GAS_RESISTANCE), "gas_resistance_status") ==
              0);
}

int main(void)
{
    HAC_RUN(a_full_reading_is_every_key_in_order);
    HAC_RUN(nothing_known_yet_is_every_key_null_and_unavailable);
    HAC_RUN(every_status_of_every_value);
    HAC_RUN(every_accuracy_of_iaq);
    HAC_RUN(nan_infinity_and_overflow_are_errors_not_numbers);
    HAC_RUN(negative_temperatures_and_negative_zero);
    HAC_RUN(rssi_is_published_only_while_the_network_is_up);
    HAC_RUN(a_buffer_exactly_large_enough_and_one_byte_short);
    HAC_RUN(the_longest_message_fits);
    HAC_RUN(the_decimals_are_the_recordings);
    return hac_test_summary();
}
