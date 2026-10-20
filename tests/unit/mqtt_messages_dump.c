// SPDX-License-Identifier: Apache-2.0
//
// Prints every kind of message core/reporting/ builds, one per line, for
// tests/unit/mqtt_messages_check.py to parse with a real JSON parser:
//
//   <kind>\t<label>\t<topic>\t<payload>
//
// kind is "state" or "config"; label names a build or a case, and groups a
// state message with the configs that describe it. A payload holds no raw
// tab or newline: the writer emits no whitespace and escapes every control
// character inside a string. An empty payload is a config that removes its
// entity.

#include <math.h>
#include <stdio.h>

#include "port_sensor.h"
#include "reporting/ha_discovery.h"
#include "reporting/mqtt_payload.h"
#include "reporting/topics.h"

#define TS 1000u
#define HEARTBEAT_S 60u

// The checker expects exactly this name back from the "escaping" case.
#define ESCAPING_NAME "Air \"monitor\" \\ tab\t nl\n bell\x07 \xC3\xA9"

#define SCD4X_CAPS (SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY)
#define BME68X_CAPS \
    (SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_GAS_RES)

static int g_failures;

static void print_message(const char *kind, const char *label, const char *topic,
                          const char *payload)
{
    printf("%s\t%s\t%s\t%s\n", kind, label, topic, payload);
}

static air_reading_t full_reading(void)
{
    air_reading_t r;
    air_reading_init(&r);
    r.temperature_c = measurement_ok(-3.456f, TS, 2u);
    r.humidity_rh = measurement_ok(48.2f, TS, 2u);
    r.pressure_hpa = measurement_ok(1006.2144f, TS, 2u);
    r.co2_ppm = measurement_ok(812.0f, TS, 1u);
    r.iaq = measurement_ok(51.37f, TS, 2u);
    r.iaq.accuracy = ACC_HIGH;
    r.gas_resistance_ohm = measurement_ok(153204.0f, TS, 2u);
    r.net_connected = true;
    r.wifi_rssi_dbm = -61;
    r.uptime_s = 3600u;
    return r;
}

static void state(const char *label, const mqtt_topics_t *t, const air_reading_t *r)
{
    char payload[MQTT_PAYLOAD_STATE_MAX];
    if (mqtt_payload_state(payload, sizeof(payload), r) == 0u) {
        fprintf(stderr, "state %s did not fit\n", label);
        g_failures++;
        return;
    }
    print_message("state", label, t->state, payload);
}

static void configs(const char *label, const ha_discovery_t *d)
{
    for (unsigned e = 0u; e < (unsigned)HA_ENTITY_COUNT; ++e) {
        char topic[MQTT_TOPIC_MAX];
        char payload[HA_DISCOVERY_CONFIG_MAX];
        size_t len = 0u;
        if (ha_discovery_topic(topic, sizeof(topic), d, (ha_entity_t)e) == 0u ||
            !ha_discovery_config(payload, sizeof(payload), d, (ha_entity_t)e, &len)) {
            fprintf(stderr, "config %s entity %u failed\n", label, e);
            g_failures++;
            continue;
        }
        print_message("config", label, topic, payload);
    }
}

static ha_discovery_t discovery_for(const mqtt_topics_t *t, sensor_caps_t caps)
{
    return (ha_discovery_t){
        .topics = t,
        .device = {.name = "Home Air Monitor",
                   .manufacturer = "home-air-monitor",
                   .model = "LILYGO T5 4.7\" e-Paper S3",
                   .hw_version = "Screen-4.7-S3 v2.4",
                   .sw_version = "phase-4a"},
        .origin = {.name = "home-air-monitor",
                   .sw_version = "phase-4a",
                   .support_url = "https://github.com/mikhailkalina/home-air-monitor"},
        .caps = caps,
        .has_rssi = true,
        .heartbeat_s = HEARTBEAT_S,
    };
}

static void dump_bsec_build(void)
{
    mqtt_topics_t t;
    (void)mqtt_topics_init(&t, "homeair", "hac-a1b2c3d4e5f6", "homeassistant");
    const air_reading_t r = full_reading();
    state("bsec", &t, &r);
    const ha_discovery_t d =
        discovery_for(&t, (sensor_caps_t)(SCD4X_CAPS | BME68X_CAPS | SENS_CAP_IAQ));
    configs("bsec", &d);
}

static void dump_plain_build(void)
{
    mqtt_topics_t t;
    (void)mqtt_topics_init(&t, "home/air", "plain_1", "homeassistant");
    air_reading_t r = full_reading();
    r.iaq = measurement_unavailable();
    r.net_connected = false;
    state("plain", &t, &r);
    ha_discovery_t d = discovery_for(&t, (sensor_caps_t)(SCD4X_CAPS | BME68X_CAPS));
    d.has_rssi = false;
    d.origin = (ha_origin_t){0};
    configs("plain", &d);
}

// Every status of every value, every accuracy, and numbers JSON cannot hold,
// as states of one device.
static void dump_statuses(void)
{
    mqtt_topics_t t;
    (void)mqtt_topics_init(&t, "homeair", "statuses", "homeassistant");
    const value_status_t statuses[] = {VAL_UNAVAILABLE, VAL_WARMUP, VAL_OK, VAL_STALE, VAL_ERROR};
    for (size_t s = 0u; s < sizeof(statuses) / sizeof(statuses[0]); ++s) {
        air_reading_t r = full_reading();
        r.temperature_c.status = statuses[s];
        r.humidity_rh.status = statuses[s];
        r.pressure_hpa.status = statuses[s];
        r.co2_ppm.status = statuses[s];
        r.iaq.status = statuses[s];
        r.gas_resistance_ohm.status = statuses[s];
        r.iaq.accuracy = (value_accuracy_t)s;  // ACC_NOT_REPORTED .. ACC_HIGH
        state("statuses", &t, &r);
    }
    const float bad[] = {NAN, INFINITY, -INFINITY, 1e20f, -0.0f};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        air_reading_t r = full_reading();
        r.temperature_c.value = bad[i];
        r.co2_ppm.value = bad[i];
        r.gas_resistance_ohm.value = bad[i];
        state("statuses", &t, &r);
    }
    air_reading_t nothing;
    air_reading_init(&nothing);
    state("statuses", &t, &nothing);
    const ha_discovery_t d =
        discovery_for(&t, (sensor_caps_t)(SCD4X_CAPS | BME68X_CAPS | SENS_CAP_IAQ));
    configs("statuses", &d);
}

static void dump_escaping(void)
{
    mqtt_topics_t t;
    (void)mqtt_topics_init(&t, "homeair", "escaping", "homeassistant");
    const air_reading_t r = full_reading();
    state("escaping", &t, &r);
    ha_discovery_t d = discovery_for(&t, (sensor_caps_t)(SCD4X_CAPS | BME68X_CAPS));
    d.device.name = ESCAPING_NAME;
    configs("escaping", &d);
}

static void dump_longest_topics(void)
{
    mqtt_topics_t t;
    if (mqtt_topics_init(&t, "abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcd",
                         "0123456789abcdef0123456789ABCDEF",
                         "homeassistant/abcdefghij/abcdefg") != HAL_OK) {
        fprintf(stderr, "the longest topics were refused\n");
        g_failures++;
        return;
    }
    const air_reading_t r = full_reading();
    state("longest", &t, &r);
    ha_discovery_t d = discovery_for(&t, (sensor_caps_t)(SCD4X_CAPS | BME68X_CAPS | SENS_CAP_IAQ));
    // test_ha_discovery.c's longest plausible strings.
    d.device.model = "LILYGO T5 4.7\" e-Paper S3, SCD41, BME680";
    d.device.hw_version = "Screen-4.7-S3 v2.4, 2024-12-03";
    d.device.sw_version = "phase-4a-mqtt-payload-1234567-dirty";
    d.origin.sw_version = "phase-4a-mqtt-payload-1234567-dirty";
    d.heartbeat_s = HA_DISCOVERY_HEARTBEAT_MAX_S;
    configs("longest", &d);
}

int main(void)
{
    dump_bsec_build();
    dump_plain_build();
    dump_statuses();
    dump_escaping();
    dump_longest_topics();
    return (g_failures == 0) ? 0 : 1;
}
