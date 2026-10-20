// SPDX-License-Identifier: Apache-2.0
//
// core/reporting/ha_discovery.c: Home Assistant's config messages, byte for
// byte where it matters; which entities a BSEC build and a plain one provide
// and the empty message that removes the others; that nothing but the
// SCD41's CO2 is called CO2 (requirements 13.3); that a message which does
// not fit is refused rather than sent empty; and that the display precision
// HA is given is the home screen's. tests/unit/mqtt_messages_check.py parses
// every message with a real JSON parser.

#include "reporting/ha_discovery.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "domain/civil_time.h"
#include "hac_test.h"
#include "reporting/mqtt_payload.h"
#include "ui/view_model.h"

#define TS 1000u
#define HEARTBEAT_S 60u

#define SCD4X_CAPS ((sensor_caps_t)(SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY))
#define BME68X_CAPS                                                                 \
    ((sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | \
                     SENS_CAP_GAS_RES))
#define BSEC_CAPS ((sensor_caps_t)(BME68X_CAPS | SENS_CAP_IAQ))

static mqtt_topics_t g_topics;

static ha_discovery_t discovery(sensor_caps_t caps)
{
    HAC_CHECK_EQ_INT(mqtt_topics_init(&g_topics, "homeair", "hac-a1b2c3d4e5f6", "homeassistant"),
                     HAL_OK);
    return (ha_discovery_t){
        .topics = &g_topics,
        .device = {.name = "Home Air Monitor",
                   .model = "T5 4.7\" e-Paper S3",
                   .hw_version = "Screen-4.7-S3 v2.4",
                   .sw_version = "4a-test"},
        .origin = {.name = "home-air-monitor",
                   .sw_version = "4a-test",
                   .support_url = "https://example.org/hac"},
        .caps = caps,
        .has_rssi = true,
        .heartbeat_s = HEARTBEAT_S,
    };
}

static bool contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static bool contains_folded(const char *haystack, const char *needle)
{
    char folded[HA_DISCOVERY_CONFIG_MAX];
    size_t i = 0u;
    for (; haystack[i] != '\0' && i + 1u < sizeof(folded); ++i) {
        folded[i] = (char)tolower((unsigned char)haystack[i]);
    }
    folded[i] = '\0';
    return contains(folded, needle);
}

static void the_temperature_config_byte_for_byte(void)
{
    const ha_discovery_t d = discovery((sensor_caps_t)(SCD4X_CAPS | BSEC_CAPS));
    char buf[HA_DISCOVERY_CONFIG_MAX];
    size_t len = 99u;
    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_TEMPERATURE, &len));
    const char *expected =
        "{\"~\":\"homeair/hac-a1b2c3d4e5f6\",\"name\":\"Temperature\","
        "\"unique_id\":\"hac-a1b2c3d4e5f6_temperature\",\"state_topic\":\"~/state\","
        "\"value_template\":\"{{ value_json.temperature }}\","
        "\"json_attributes_topic\":\"~/state\","
        "\"json_attributes_template\":\"{{ {'status': value_json.temperature_status} | tojson }}\","
        "\"availability_topic\":\"~/status\",\"expire_after\":180,"
        "\"device_class\":\"temperature\",\"unit_of_measurement\":\"\xC2\xB0"
        "C\",\"state_class\":\"measurement\",\"suggested_display_precision\":1,"
        "\"device\":{\"identifiers\":[\"hac-a1b2c3d4e5f6\"],\"name\":\"Home Air Monitor\","
        "\"model\":\"T5 4.7\\\" e-Paper S3\",\"hw_version\":\"Screen-4.7-S3 v2.4\","
        "\"sw_version\":\"4a-test\"},"
        "\"origin\":{\"name\":\"home-air-monitor\",\"sw_version\":\"4a-test\","
        "\"support_url\":\"https://example.org/hac\"}}";
    HAC_CHECK_EQ_U64(len, strlen(expected));
    HAC_CHECK(strcmp(buf, expected) == 0);
}

static void the_iaq_and_its_accuracy(void)
{
    const ha_discovery_t d = discovery(BSEC_CAPS);
    char buf[HA_DISCOVERY_CONFIG_MAX];
    size_t len = 0u;
    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_IAQ, &len) && len > 0u);
    HAC_CHECK(contains(buf, "\"value_template\":\"{{ value_json.iaq }}\","));
    HAC_CHECK(contains(buf, "\"json_attributes_template\":\"{{ {'status': value_json.iaq_status, "
                            "'accuracy': value_json.iaq_accuracy} | tojson }}\","));
    HAC_CHECK(contains(buf, "\"device_class\":\"aqi\",\"state_class\":\"measurement\","
                            "\"suggested_display_precision\":0,\"device\""));
    HAC_CHECK(!contains(buf, "unit_of_measurement"));
    HAC_CHECK(!contains(buf, "entity_category"));

    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_IAQ_ACCURACY, &len) && len > 0u);
    HAC_CHECK(contains(buf, "\"value_template\":\"{{ value_json.iaq_accuracy }}\","));
    // An enum: no unit, no state class, no attributes.
    HAC_CHECK(contains(buf, "\"expire_after\":180,\"device_class\":\"enum\","
                            "\"options\":[\"unreliable\",\"low\",\"medium\",\"high\"],"
                            "\"entity_category\":\"diagnostic\",\"device\""));
    HAC_CHECK(!contains(buf, "json_attributes"));
}

static void the_diagnostics(void)
{
    const ha_discovery_t d = discovery(BSEC_CAPS);
    char buf[HA_DISCOVERY_CONFIG_MAX];
    size_t len = 0u;
    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_GAS_RESISTANCE, &len));
    HAC_CHECK(contains(buf, "\"name\":\"Gas resistance\","));
    HAC_CHECK(contains(buf, "\"unit_of_measurement\":\"\xCE\xA9\",\"state_class\":\"measurement\","
                            "\"suggested_display_precision\":0,"
                            "\"entity_category\":\"diagnostic\""));
    HAC_CHECK(!contains(buf, "device_class"));

    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_RSSI, &len));
    HAC_CHECK(contains(buf, "\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\","
                            "\"state_class\":\"measurement\""));
    HAC_CHECK(contains(buf, "\"entity_category\":\"diagnostic\""));
    HAC_CHECK(!contains(buf, "json_attributes"));

    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_UPTIME, &len));
    HAC_CHECK(contains(buf, "\"device_class\":\"duration\",\"unit_of_measurement\":\"s\","
                            "\"suggested_display_precision\":0,"
                            "\"entity_category\":\"diagnostic\""));
    HAC_CHECK(!contains(buf, "state_class"));
}

// Which entities each build provides; the rest get an empty message, which
// removes them -- and success, so that the caller publishes it.
static void a_build_provides_what_its_sources_measure(void)
{
    static const struct {
        sensor_caps_t caps;
        bool has_rssi;
        bool provided[HA_ENTITY_COUNT];
    } BUILDS[] = {
        // SCD41 and BSEC, on Wi-Fi: everything.
        {(sensor_caps_t)(SCD4X_CAPS | BSEC_CAPS), true, {1, 1, 1, 1, 1, 1, 1, 1, 1}},
        // SCD41 and the plain BME68x source: no IAQ, no IAQ accuracy.
        {(sensor_caps_t)(SCD4X_CAPS | BME68X_CAPS), true, {1, 1, 1, 1, 0, 1, 0, 1, 1}},
        // BSEC without the SCD41: no CO2 -- BSEC's own estimate is never one.
        {BSEC_CAPS, true, {1, 1, 1, 0, 1, 1, 1, 1, 1}},
        // The SCD41 alone, no network signal: CO2, temperature, humidity, uptime.
        {SCD4X_CAPS, false, {1, 1, 0, 1, 0, 0, 0, 0, 1}},
        // Nothing at all: uptime still.
        {(sensor_caps_t)0, false, {0, 0, 0, 0, 0, 0, 0, 0, 1}},
    };
    for (size_t b = 0u; b < sizeof(BUILDS) / sizeof(BUILDS[0]); ++b) {
        ha_discovery_t d = discovery(BUILDS[b].caps);
        d.has_rssi = BUILDS[b].has_rssi;
        for (unsigned e = 0u; e < (unsigned)HA_ENTITY_COUNT; ++e) {
            char buf[HA_DISCOVERY_CONFIG_MAX];
            size_t len = 99u;
            HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, (ha_entity_t)e, &len));
            HAC_CHECK_EQ_INT(ha_discovery_provides(&d, (ha_entity_t)e), BUILDS[b].provided[e]);
            if (BUILDS[b].provided[e]) {
                HAC_CHECK(len > 0u && buf[0] == '{');
            } else {
                HAC_CHECK_EQ_U64(len, 0u);
                HAC_CHECK(buf[0] == '\0');
            }
        }
    }
}

static void every_entity_has_its_topic_and_a_distinct_unique_id(void)
{
    const ha_discovery_t d = discovery((sensor_caps_t)(SCD4X_CAPS | BSEC_CAPS));
    static const char *const OBJECT_IDS[HA_ENTITY_COUNT] = {
        "temperature",    "humidity",     "pressure", "co2",    "iaq",
        "gas_resistance", "iaq_accuracy", "rssi",     "uptime",
    };
    for (unsigned e = 0u; e < (unsigned)HA_ENTITY_COUNT; ++e) {
        HAC_CHECK(strcmp(ha_entity_object_id((ha_entity_t)e), OBJECT_IDS[e]) == 0);
        char topic[MQTT_TOPIC_MAX];
        char expected[MQTT_TOPIC_MAX];
        strcpy(expected, "homeassistant/sensor/hac-a1b2c3d4e5f6/");
        strcat(expected, OBJECT_IDS[e]);
        strcat(expected, "/config");
        HAC_CHECK_EQ_U64(ha_discovery_topic(topic, sizeof(topic), &d, (ha_entity_t)e),
                         strlen(expected));
        HAC_CHECK(strcmp(topic, expected) == 0);

        char buf[HA_DISCOVERY_CONFIG_MAX];
        size_t len = 0u;
        HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, (ha_entity_t)e, &len));
        char unique_id[96];
        strcpy(unique_id, "\"unique_id\":\"hac-a1b2c3d4e5f6_");
        strcat(unique_id, OBJECT_IDS[e]);
        strcat(unique_id, "\"");
        HAC_CHECK(contains(buf, unique_id));
    }
    // The object ids of the measurements are the state message's keys.
    HAC_CHECK(strcmp(ha_entity_object_id(HA_ENTITY_GAS_RESISTANCE),
                     mqtt_payload_key(MQTT_VALUE_GAS_RESISTANCE)) == 0);
    HAC_CHECK(ha_entity_object_id(HA_ENTITY_COUNT) == NULL);
    char topic[MQTT_TOPIC_MAX];
    HAC_CHECK_EQ_U64(ha_discovery_topic(topic, sizeof(topic), &d, HA_ENTITY_COUNT), 0u);
}

// Requirements 13.3: VOC and IAQ are never labelled CO2. Only the co2 entity
// says carbon dioxide, and it exists only where SENS_CAP_CO2 does.
static void nothing_but_the_scd41_is_called_co2(void)
{
    const ha_discovery_t d = discovery((sensor_caps_t)(SCD4X_CAPS | BSEC_CAPS));
    for (unsigned e = 0u; e < (unsigned)HA_ENTITY_COUNT; ++e) {
        char buf[HA_DISCOVERY_CONFIG_MAX];
        size_t len = 0u;
        HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, (ha_entity_t)e, &len));
        if ((ha_entity_t)e == HA_ENTITY_CO2) {
            HAC_CHECK(contains(buf, "\"device_class\":\"carbon_dioxide\",\"unit_of_measurement\":"
                                    "\"ppm\""));
            HAC_CHECK(contains(buf, "\"name\":\"CO2\""));
        } else {
            HAC_CHECK(!contains_folded(buf, "co2"));
            HAC_CHECK(!contains_folded(buf, "carbon"));
            HAC_CHECK(!contains(buf, "ppm"));
        }
    }
    const ha_discovery_t bsec_only = discovery(BSEC_CAPS);
    HAC_CHECK(!ha_discovery_provides(&bsec_only, HA_ENTITY_CO2));
}

static void expiry_is_three_heartbeats(void)
{
    ha_discovery_t d = discovery(SCD4X_CAPS);
    char buf[HA_DISCOVERY_CONFIG_MAX];
    size_t len = 0u;
    d.heartbeat_s = 1u;
    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));
    HAC_CHECK(contains(buf, "\"expire_after\":3,"));
    d.heartbeat_s = HA_DISCOVERY_HEARTBEAT_MAX_S;
    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));
    HAC_CHECK(contains(buf, "\"expire_after\":259200,"));
}

// A configuration the core cannot describe is refused -- never answered with
// the empty message, which would remove the entity from Home Assistant.
static void an_unusable_configuration_is_refused_not_emptied(void)
{
    char buf[HA_DISCOVERY_CONFIG_MAX];
    size_t len = 99u;
    ha_discovery_t d = discovery(SCD4X_CAPS);

    d.heartbeat_s = 0u;
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));
    HAC_CHECK_EQ_U64(len, 0u);
    // ... even for an entity this build does not provide.
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_IAQ, &len));
    d.heartbeat_s = HA_DISCOVERY_HEARTBEAT_MAX_S + 1u;
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));

    d = discovery(SCD4X_CAPS);
    d.device.name = NULL;
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));
    d.device.name = "";
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));

    d = discovery(SCD4X_CAPS);
    d.topics = NULL;
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));
    mqtt_topics_t refused;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&refused, "homeair", "bad/id", "homeassistant"),
                     HAL_ERR_INVALID_ARG);
    d.topics = &refused;
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));

    d = discovery(SCD4X_CAPS);
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_COUNT, &len));
    HAC_CHECK(!ha_discovery_config(buf, sizeof(buf), NULL, HA_ENTITY_CO2, &len));
    HAC_CHECK(!ha_discovery_config(buf, 0u, &d, HA_ENTITY_IAQ, &len));
    HAC_CHECK_EQ_U64(len, 0u);
}

static void a_buffer_exactly_large_enough_and_one_byte_short(void)
{
    const ha_discovery_t d = discovery(BSEC_CAPS);
    char reference[HA_DISCOVERY_CONFIG_MAX];
    size_t n = 0u;
    HAC_CHECK(ha_discovery_config(reference, sizeof(reference), &d, HA_ENTITY_IAQ, &n));

    char buf[HA_DISCOVERY_CONFIG_MAX];
    size_t len = 0u;
    memset(buf, 'X', sizeof(buf));
    HAC_CHECK(ha_discovery_config(buf, n + 1u, &d, HA_ENTITY_IAQ, &len));
    HAC_CHECK_EQ_U64(len, n);
    HAC_CHECK(strcmp(buf, reference) == 0);

    memset(buf, 'X', sizeof(buf));
    len = 99u;
    HAC_CHECK(!ha_discovery_config(buf, n, &d, HA_ENTITY_IAQ, &len));
    HAC_CHECK_EQ_U64(len, 0u);
    HAC_CHECK(buf[0] == '\0');
    HAC_CHECK(buf[n] == 'X');

    // The empty message needs a byte too, for its terminator.
    HAC_CHECK(ha_discovery_config(buf, 1u, &d, HA_ENTITY_CO2, &len));
    HAC_CHECK_EQ_U64(len, 0u);
}

static void optional_device_fields_and_origin_are_left_out(void)
{
    ha_discovery_t d = discovery(SCD4X_CAPS);
    d.device = (ha_device_t){.name = "Air \"1\""};
    d.origin = (ha_origin_t){0};
    char buf[HA_DISCOVERY_CONFIG_MAX];
    size_t len = 0u;
    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));
    HAC_CHECK(contains(buf, "\"device\":{\"identifiers\":[\"hac-a1b2c3d4e5f6\"],"
                            "\"name\":\"Air \\\"1\\\"\"}}"));
    HAC_CHECK(!contains(buf, "origin") && !contains(buf, "manufacturer") &&
              !contains(buf, "sw_version"));

    d.device.manufacturer = "M";
    d.origin = (ha_origin_t){.name = "o"};
    HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, HA_ENTITY_CO2, &len));
    HAC_CHECK(contains(buf, "\"manufacturer\":\"M\"}"));
    HAC_CHECK(contains(buf, "\"origin\":{\"name\":\"o\"}}"));
}

// The longest topics and long but plausible strings: every message stays
// under the bound (969 bytes at most, measured, ha_discovery.h).
static void the_longest_messages_fit(void)
{
    mqtt_topics_t topics;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&topics, "abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcd",
                                      "0123456789abcdef0123456789ABCDEF",
                                      "homeassistant/abcdefghij/abcdefg"),
                     HAL_OK);
    const ha_discovery_t d = {
        .topics = &topics,
        .device = {.name = "Home Air Monitor",
                   .manufacturer = "home-air-monitor",
                   .model = "LILYGO T5 4.7\" e-Paper S3, SCD41, BME680",
                   .hw_version = "Screen-4.7-S3 v2.4, 2024-12-03",
                   .sw_version = "phase-4a-mqtt-payload-1234567-dirty"},
        .origin = {.name = "home-air-monitor",
                   .sw_version = "phase-4a-mqtt-payload-1234567-dirty",
                   .support_url = "https://github.com/mikhailkalina/home-air-monitor"},
        .caps = (sensor_caps_t)(SCD4X_CAPS | BSEC_CAPS),
        .has_rssi = true,
        .heartbeat_s = HA_DISCOVERY_HEARTBEAT_MAX_S,
    };
    size_t longest = 0u;
    for (unsigned e = 0u; e < (unsigned)HA_ENTITY_COUNT; ++e) {
        char buf[HA_DISCOVERY_CONFIG_MAX];
        size_t len = 0u;
        HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, (ha_entity_t)e, &len));
        if (len > longest) {
            longest = len;
        }
    }
    HAC_CHECK(longest > 0u && longest < HA_DISCOVERY_CONFIG_MAX);
}

// The number of digits after the point in `text`.
static unsigned decimals_in(const char *text)
{
    const char *point = strchr(text, '.');
    return (point == NULL) ? 0u : (unsigned)strlen(point + 1);
}

static unsigned precision_in(const char *config)
{
    const char *key = strstr(config, "\"suggested_display_precision\":");
    HAC_CHECK(key != NULL);
    return (key == NULL) ? 99u : (unsigned)atoi(key + strlen("\"suggested_display_precision\":"));
}

// HA is told the home screen's decimals (core/ui/view_model.c): its cards
// show what the glass shows.
static void the_display_precision_is_the_screens(void)
{
    air_reading_t r;
    air_reading_init(&r);
    r.temperature_c = measurement_ok(21.2345f, TS, 2u);
    r.humidity_rh = measurement_ok(48.2345f, TS, 2u);
    r.pressure_hpa = measurement_ok(1006.2345f, TS, 2u);
    r.co2_ppm = measurement_ok(812.2345f, TS, 1u);
    r.iaq = measurement_ok(51.2345f, TS, 2u);
    view_model_t vm;
    const time_zone_t utc = time_zone_fixed(0);
    view_model_build(&vm, &r, TS, &utc);

    static const struct {
        ha_entity_t entity;
        vm_field_t field;
    } PAIRS[] = {
        {HA_ENTITY_TEMPERATURE, VM_FIELD_TEMPERATURE},
        {HA_ENTITY_HUMIDITY, VM_FIELD_HUMIDITY},
        {HA_ENTITY_PRESSURE, VM_FIELD_PRESSURE},
        {HA_ENTITY_CO2, VM_FIELD_CO2},
        {HA_ENTITY_IAQ, VM_FIELD_IAQ},
    };
    const ha_discovery_t d = discovery((sensor_caps_t)(SCD4X_CAPS | BSEC_CAPS));
    for (size_t i = 0u; i < sizeof(PAIRS) / sizeof(PAIRS[0]); ++i) {
        HAC_CHECK(vm.fields[PAIRS[i].field].has_value);
        char buf[HA_DISCOVERY_CONFIG_MAX];
        size_t len = 0u;
        HAC_CHECK(ha_discovery_config(buf, sizeof(buf), &d, PAIRS[i].entity, &len));
        HAC_CHECK_EQ_U64(precision_in(buf), decimals_in(vm.fields[PAIRS[i].field].text));
    }
}

int main(void)
{
    HAC_RUN(the_temperature_config_byte_for_byte);
    HAC_RUN(the_iaq_and_its_accuracy);
    HAC_RUN(the_diagnostics);
    HAC_RUN(a_build_provides_what_its_sources_measure);
    HAC_RUN(every_entity_has_its_topic_and_a_distinct_unique_id);
    HAC_RUN(nothing_but_the_scd41_is_called_co2);
    HAC_RUN(expiry_is_three_heartbeats);
    HAC_RUN(an_unusable_configuration_is_refused_not_emptied);
    HAC_RUN(a_buffer_exactly_large_enough_and_one_byte_short);
    HAC_RUN(optional_device_fields_and_origin_are_left_out);
    HAC_RUN(the_longest_messages_fit);
    HAC_RUN(the_display_precision_is_the_screens);
    return hac_test_summary();
}
