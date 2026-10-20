// SPDX-License-Identifier: Apache-2.0

#include "reporting/ha_discovery.h"

#include "recording/record_format.h"
#include "reporting/json_writer.h"
#include "reporting/mqtt_payload.h"
#include "text/text_append.h"

// The units, in UTF-8: Home Assistant compares them byte for byte.
// "\xC2\xB0\x43" is "°C" -- the 'C' escaped too, so that no adjacent
// literal is needed to end the hex escape.
#define UNIT_CELSIUS "\xC2\xB0\x43"
#define UNIT_OHM "\xCE\xA9"

// Room for the longest template and unique id built below, terminator
// included: the IAQ's attributes template is under 100 characters, a
// unique id 32 + 1 + 32.
#define TEMPLATE_MAX 128u
#define UNIQUE_ID_MAX (MQTT_DEVICE_ID_MAX + 1u + MQTT_OBJECT_ID_MAX + 1u)

typedef enum {
    ATTRIBUTES_NONE = 0,
    ATTRIBUTES_STATUS,               // {'status': ...}
    ATTRIBUTES_STATUS_AND_ACCURACY,  // {'status': ..., 'accuracy': ...}
} attributes_t;

typedef struct {
    mqtt_value_t value;  // the measurement it shows, or MQTT_VALUE_COUNT
    const char *key;     // the state message's key, when it is not a measurement
    const char *name;
    const char *device_class;  // NULL: none
    const char *unit;          // NULL: none
    const char *state_class;   // NULL: none -- no long-term statistics
    bool has_precision;
    // CHOICE: the home screen's decimals (core/ui/view_model.c), so HA's
    // cards show what the glass shows; the state message itself carries the
    // recordings' finer ones, for HA's history.
    unsigned precision;
    bool diagnostic;
    bool accuracy_options;  // an enum of the accuracy words
    attributes_t attributes;
    sensor_caps_t needs;  // the sensor capability that provides it; 0: none
    bool needs_rssi;
} entity_info_t;

static const entity_info_t ENTITIES[HA_ENTITY_COUNT] = {
    [HA_ENTITY_TEMPERATURE] = {.value = MQTT_VALUE_TEMPERATURE,
                               .name = "Temperature",
                               .device_class = "temperature",
                               .unit = UNIT_CELSIUS,
                               .state_class = "measurement",
                               .has_precision = true,
                               .precision = 1u,
                               .attributes = ATTRIBUTES_STATUS,
                               .needs = SENS_CAP_TEMPERATURE},
    [HA_ENTITY_HUMIDITY] = {.value = MQTT_VALUE_HUMIDITY,
                            .name = "Humidity",
                            .device_class = "humidity",
                            .unit = "%",
                            .state_class = "measurement",
                            .has_precision = true,
                            .precision = 0u,
                            .attributes = ATTRIBUTES_STATUS,
                            .needs = SENS_CAP_HUMIDITY},
    [HA_ENTITY_PRESSURE] = {.value = MQTT_VALUE_PRESSURE,
                            .name = "Pressure",
                            .device_class = "atmospheric_pressure",
                            .unit = "hPa",
                            .state_class = "measurement",
                            .has_precision = true,
                            .precision = 0u,
                            .attributes = ATTRIBUTES_STATUS,
                            .needs = SENS_CAP_PRESSURE},
    // The SCD41's: SENS_CAP_CO2, which no BME68x source claims.
    [HA_ENTITY_CO2] = {.value = MQTT_VALUE_CO2,
                       .name = "CO2",
                       .device_class = "carbon_dioxide",
                       .unit = "ppm",
                       .state_class = "measurement",
                       .has_precision = true,
                       .precision = 0u,
                       .attributes = ATTRIBUTES_STATUS,
                       .needs = SENS_CAP_CO2},
    // BSEC's index, 0-500: HA's Air Quality Index class, unitless.
    [HA_ENTITY_IAQ] = {.value = MQTT_VALUE_IAQ,
                       .name = "IAQ",
                       .device_class = "aqi",
                       .state_class = "measurement",
                       .has_precision = true,
                       .precision = 0u,
                       .attributes = ATTRIBUTES_STATUS_AND_ACCURACY,
                       .needs = SENS_CAP_IAQ},
    // No HA class for a resistance; whole ohms, as published.
    [HA_ENTITY_GAS_RESISTANCE] = {.value = MQTT_VALUE_GAS_RESISTANCE,
                                  .name = "Gas resistance",
                                  .unit = UNIT_OHM,
                                  .state_class = "measurement",
                                  .has_precision = true,
                                  .precision = 0u,
                                  .diagnostic = true,
                                  .attributes = ATTRIBUTES_STATUS,
                                  .needs = SENS_CAP_GAS_RES},
    [HA_ENTITY_IAQ_ACCURACY] = {.value = MQTT_VALUE_COUNT,
                                .key = MQTT_PAYLOAD_IAQ_ACCURACY_KEY,
                                .name = "IAQ accuracy",
                                .device_class = "enum",
                                .diagnostic = true,
                                .accuracy_options = true,
                                .needs = SENS_CAP_IAQ},
    [HA_ENTITY_RSSI] = {.value = MQTT_VALUE_COUNT,
                        .key = MQTT_PAYLOAD_RSSI_KEY,
                        .name = "Wi-Fi signal",
                        .device_class = "signal_strength",
                        .unit = "dBm",
                        .state_class = "measurement",
                        .has_precision = true,
                        .precision = 0u,
                        .diagnostic = true,
                        .needs_rssi = true},
    // No state class: a counter of seconds has no statistics worth keeping,
    // and its history shows the reboots.
    [HA_ENTITY_UPTIME] = {.value = MQTT_VALUE_COUNT,
                          .key = MQTT_PAYLOAD_UPTIME_KEY,
                          .name = "Uptime",
                          .device_class = "duration",
                          .unit = "s",
                          .has_precision = true,
                          .precision = 0u,
                          .diagnostic = true},
};

static bool in_range(ha_entity_t entity)
{
    return (unsigned)entity < (unsigned)HA_ENTITY_COUNT;
}

const char *ha_entity_object_id(ha_entity_t entity)
{
    if (!in_range(entity)) {
        return NULL;
    }
    const entity_info_t *e = &ENTITIES[entity];
    return (e->value != MQTT_VALUE_COUNT) ? mqtt_payload_key(e->value) : e->key;
}

static bool usable(const ha_discovery_t *d)
{
    return d != NULL && d->topics != NULL && d->topics->device_id[0] != '\0' &&
           d->device.name != NULL && d->device.name[0] != '\0' && d->heartbeat_s > 0u &&
           d->heartbeat_s <= HA_DISCOVERY_HEARTBEAT_MAX_S;
}

bool ha_discovery_provides(const ha_discovery_t *d, ha_entity_t entity)
{
    if (d == NULL || !in_range(entity)) {
        return false;
    }
    const entity_info_t *e = &ENTITIES[entity];
    if (e->needs != 0u) {
        return (d->caps & e->needs) != 0u;
    }
    if (e->needs_rssi) {
        return d->has_rssi;
    }
    return true;
}

size_t ha_discovery_topic(char *dst, size_t cap, const ha_discovery_t *d, ha_entity_t entity)
{
    if (d == NULL || d->topics == NULL || !in_range(entity)) {
        if (cap > 0u) {
            dst[0] = '\0';
        }
        return 0u;
    }
    return mqtt_topics_discovery(d->topics, MQTT_DISCOVERY_COMPONENT_SENSOR,
                                 ha_entity_object_id(entity), dst, cap);
}

// "~/<last level>": a topic of the device's, relative to the base topic.
static void member_topic(json_writer_t *w, const char *key, const char *last_level)
{
    char topic[MQTT_TOPIC_MAX];
    size_t len = text_append_str(topic, sizeof(topic), 0u, "~/");
    len = text_append_str(topic, sizeof(topic), len, last_level);
    if (text_overflowed(len, sizeof(topic))) {
        json_fail(w);  // cannot happen with the topics' bounds
        return;
    }
    json_member_string(w, key, topic);
}

static size_t append_value_json(char *dst, size_t cap, size_t len, const char *key)
{
    len = text_append_str(dst, cap, len, "value_json.");
    return text_append_str(dst, cap, len, key);
}

static void member_value_template(json_writer_t *w, const char *key)
{
    char tpl[TEMPLATE_MAX];
    size_t len = text_append_str(tpl, sizeof(tpl), 0u, "{{ ");
    len = append_value_json(tpl, sizeof(tpl), len, key);
    len = text_append_str(tpl, sizeof(tpl), len, " }}");
    if (text_overflowed(len, sizeof(tpl))) {
        json_fail(w);  // cannot happen: the keys are ours
        return;
    }
    json_member_string(w, "value_template", tpl);
}

// The status -- and IAQ's accuracy -- as the entity's attributes.
static void member_attributes(json_writer_t *w, const entity_info_t *e)
{
    char tpl[TEMPLATE_MAX];
    size_t len = text_append_str(tpl, sizeof(tpl), 0u, "{{ {'status': ");
    len = append_value_json(tpl, sizeof(tpl), len, mqtt_payload_status_key(e->value));
    if (e->attributes == ATTRIBUTES_STATUS_AND_ACCURACY) {
        len = text_append_str(tpl, sizeof(tpl), len, ", 'accuracy': ");
        len = append_value_json(tpl, sizeof(tpl), len, MQTT_PAYLOAD_IAQ_ACCURACY_KEY);
    }
    len = text_append_str(tpl, sizeof(tpl), len, "} | tojson }}");
    if (text_overflowed(len, sizeof(tpl))) {
        json_fail(w);  // cannot happen: the keys are ours
        return;
    }
    member_topic(w, "json_attributes_topic", MQTT_TOPIC_STATE);
    json_member_string(w, "json_attributes_template", tpl);
}

static void member_optional(json_writer_t *w, const char *key, const char *value)
{
    if (value != NULL) {
        json_member_string(w, key, value);
    }
}

static void put_device(json_writer_t *w, const ha_discovery_t *d)
{
    json_key(w, "device");
    json_object_open(w);
    json_key(w, "identifiers");
    json_array_open(w);
    json_string(w, d->topics->device_id);
    json_array_close(w);
    json_member_string(w, "name", d->device.name);
    member_optional(w, "manufacturer", d->device.manufacturer);
    member_optional(w, "model", d->device.model);
    member_optional(w, "hw_version", d->device.hw_version);
    member_optional(w, "sw_version", d->device.sw_version);
    json_object_close(w);

    if (d->origin.name != NULL) {
        json_key(w, "origin");
        json_object_open(w);
        json_member_string(w, "name", d->origin.name);
        member_optional(w, "sw_version", d->origin.sw_version);
        member_optional(w, "support_url", d->origin.support_url);
        json_object_close(w);
    }
}

bool ha_discovery_config(char *dst, size_t cap, const ha_discovery_t *d, ha_entity_t entity,
                         size_t *out_len)
{
    *out_len = 0u;
    if (cap == 0u) {
        return false;
    }
    dst[0] = '\0';
    if (!usable(d) || !in_range(entity)) {
        return false;
    }
    if (!ha_discovery_provides(d, entity)) {
        return true;  // the empty message: Home Assistant removes the entity
    }
    const entity_info_t *e = &ENTITIES[entity];
    const char *object_id = ha_entity_object_id(entity);

    char unique_id[UNIQUE_ID_MAX];
    size_t uid_len = text_append_str(unique_id, sizeof(unique_id), 0u, d->topics->device_id);
    uid_len = text_append_char(unique_id, sizeof(unique_id), uid_len, '_');
    uid_len = text_append_str(unique_id, sizeof(unique_id), uid_len, object_id);
    if (text_overflowed(uid_len, sizeof(unique_id))) {
        return false;
    }

    json_writer_t w;
    json_begin(&w, dst, cap);
    json_object_open(&w);
    json_member_string(&w, "~", d->topics->base);
    json_member_string(&w, "name", e->name);
    json_member_string(&w, "unique_id", unique_id);
    member_topic(&w, "state_topic", MQTT_TOPIC_STATE);
    member_value_template(&w, object_id);
    if (e->attributes != ATTRIBUTES_NONE) {
        member_attributes(&w, e);
    }
    member_topic(&w, "availability_topic", MQTT_TOPIC_STATUS);
    json_member_u64(&w, "expire_after", (uint64_t)d->heartbeat_s * HA_DISCOVERY_EXPIRE_HEARTBEATS);
    member_optional(&w, "device_class", e->device_class);
    member_optional(&w, "unit_of_measurement", e->unit);
    member_optional(&w, "state_class", e->state_class);
    if (e->has_precision) {
        json_member_u64(&w, "suggested_display_precision", e->precision);
    }
    if (e->accuracy_options) {
        json_key(&w, "options");
        json_array_open(&w);
        for (unsigned a = (unsigned)ACC_UNRELIABLE; a <= (unsigned)ACC_HIGH; ++a) {
            json_string(&w, record_accuracy_word((value_accuracy_t)a));
        }
        json_array_close(&w);
    }
    if (e->diagnostic) {
        json_member_string(&w, "entity_category", "diagnostic");
    }
    put_device(&w, d);
    json_object_close(&w);

    const size_t len = json_end(&w);
    if (len == 0u) {
        dst[0] = '\0';
        return false;
    }
    *out_len = len;
    return true;
}
