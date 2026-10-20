// SPDX-License-Identifier: Apache-2.0

#include "reporting/mqtt_payload.h"

#include <stdint.h>

#include "app/sensor_manager.h"
#include "recording/record_format.h"
#include "reporting/json_writer.h"
#include "text/text_append.h"

typedef struct {
    const char *key;
    const char *status_key;
    sensor_field_t field;  // its recording column, for the decimals
} value_info_t;

static const value_info_t VALUES[MQTT_VALUE_COUNT] = {
    [MQTT_VALUE_TEMPERATURE] = {"temperature", "temperature_status", SENSOR_FIELD_TEMPERATURE},
    [MQTT_VALUE_HUMIDITY] = {"humidity", "humidity_status", SENSOR_FIELD_HUMIDITY},
    [MQTT_VALUE_PRESSURE] = {"pressure", "pressure_status", SENSOR_FIELD_PRESSURE},
    [MQTT_VALUE_CO2] = {"co2", "co2_status", SENSOR_FIELD_CO2},
    [MQTT_VALUE_IAQ] = {"iaq", "iaq_status", SENSOR_FIELD_IAQ},
    [MQTT_VALUE_GAS_RESISTANCE] = {"gas_resistance", "gas_resistance_status",
                                   SENSOR_FIELD_GAS_RESISTANCE},
};

static bool in_range(mqtt_value_t value)
{
    return (unsigned)value < (unsigned)MQTT_VALUE_COUNT;
}

const char *mqtt_payload_key(mqtt_value_t value)
{
    return in_range(value) ? VALUES[value].key : NULL;
}

const char *mqtt_payload_status_key(mqtt_value_t value)
{
    return in_range(value) ? VALUES[value].status_key : NULL;
}

unsigned mqtt_payload_decimals(mqtt_value_t value)
{
    return in_range(value) ? record_decimals(VALUES[value].field) : 0u;
}

static const measurement_f32_t *measurement_of(const air_reading_t *r, mqtt_value_t value)
{
    switch (value) {
        case MQTT_VALUE_TEMPERATURE:
            return &r->temperature_c;
        case MQTT_VALUE_HUMIDITY:
            return &r->humidity_rh;
        case MQTT_VALUE_PRESSURE:
            return &r->pressure_hpa;
        case MQTT_VALUE_CO2:
            return &r->co2_ppm;
        case MQTT_VALUE_IAQ:
            return &r->iaq;
        case MQTT_VALUE_GAS_RESISTANCE:
        case MQTT_VALUE_COUNT:
        default:
            return &r->gas_resistance_ohm;
    }
}

// A value and its status. Only VAL_OK publishes a number; one that cannot be
// written as a JSON number is an error, as measurement_ok() would have
// called it.
static void put_value(json_writer_t *w, const measurement_f32_t *m, mqtt_value_t value)
{
    const unsigned decimals = mqtt_payload_decimals(value);
    value_status_t status = m->status;
    if (status == VAL_OK && !text_fixed_representable((double)m->value, decimals)) {
        status = VAL_ERROR;
    }
    if (status == VAL_OK) {
        json_member_fixed(w, VALUES[value].key, (double)m->value, decimals);
    } else {
        json_member_null(w, VALUES[value].key);
    }
    json_member_string(w, VALUES[value].status_key, record_status_word(status));
}

size_t mqtt_payload_state(char *dst, size_t cap, const air_reading_t *reading)
{
    json_writer_t w;
    json_begin(&w, dst, cap);
    json_object_open(&w);
    for (unsigned v = 0u; v < (unsigned)MQTT_VALUE_COUNT; ++v) {
        put_value(&w, measurement_of(reading, (mqtt_value_t)v), (mqtt_value_t)v);
        if ((mqtt_value_t)v == MQTT_VALUE_IAQ) {
            // "" is ACC_NOT_REPORTED, or a value outside the scale: no verdict.
            const char *accuracy = record_accuracy_word(reading->iaq.accuracy);
            if (accuracy[0] == '\0') {
                json_member_null(&w, MQTT_PAYLOAD_IAQ_ACCURACY_KEY);
            } else {
                json_member_string(&w, MQTT_PAYLOAD_IAQ_ACCURACY_KEY, accuracy);
            }
        }
    }
    if (reading->net_connected) {
        json_member_i64(&w, MQTT_PAYLOAD_RSSI_KEY, reading->wifi_rssi_dbm);
    } else {
        json_member_null(&w, MQTT_PAYLOAD_RSSI_KEY);
    }
    json_member_u64(&w, MQTT_PAYLOAD_UPTIME_KEY, reading->uptime_s);
    json_object_close(&w);

    const size_t len = json_end(&w);
    if (len == 0u && cap > 0u) {
        dst[0] = '\0';
    }
    return len;
}
