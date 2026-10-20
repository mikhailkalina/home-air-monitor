// SPDX-License-Identifier: Apache-2.0
//
// The MQTT topics the device publishes to, from a prefix and a device id the
// composition passes in (docs/requirements.md 15.4: topics are configurable),
// and the QoS and retain flag each is published with
// (docs/adr/0029-mqtt-topics-payload-and-discovery.md):
//
//   <prefix>/<device id>/state     the readings (mqtt_payload.h)   QoS 1, retained
//   <prefix>/<device id>/status    "online" / "offline", and the   QoS 1, retained
//                                  last will's "offline"
//   <discovery prefix>/sensor/<device id>/<entity>/config
//                                  Home Assistant's discovery      QoS 1, retained
//                                  (ha_discovery.h)
//   <discovery prefix>/status      Home Assistant's own birth and last will,
//                                  which phase 4b subscribes to
//
// Every level is checked once, here, at init: one or more characters of
// [A-Za-z0-9_-] -- Home Assistant's rule for the node id and object id of a
// discovery topic, which the device id becomes -- so no '+' or '#', no
// leading '$' (the broker's own topics), no space, no empty level. The
// prefixes may have several levels, separated by single '/', none empty.
// MQTT allows more; nothing this device names needs it.

#ifndef HAC_CORE_REPORTING_TOPICS_H
#define HAC_CORE_REPORTING_TOPICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Lengths without the terminator. CHOICES: long enough for anything a
// person would type, short enough that every topic below fits
// MQTT_TOPIC_MAX and every discovery message HA_DISCOVERY_CONFIG_MAX.
#define MQTT_PREFIX_MAX 48u
#define MQTT_DEVICE_ID_MAX 32u
#define MQTT_DISCOVERY_PREFIX_MAX 32u
#define MQTT_OBJECT_ID_MAX 32u

// Room for any topic built here, terminator included: the longest is a
// discovery topic, 32 + "/sensor/" + 32 + "/" + 32 + "/config" = 112.
#define MQTT_TOPIC_MAX 128u

// The last levels of the device's two topics.
#define MQTT_TOPIC_STATE "state"
#define MQTT_TOPIC_STATUS "status"

// Home Assistant's default discovery prefix, and its component for sensors.
#define MQTT_DISCOVERY_PREFIX_DEFAULT "homeassistant"
#define MQTT_DISCOVERY_COMPONENT_SENSOR "sensor"

// The status topic's payloads: Home Assistant's defaults for
// payload_available and payload_not_available, so discovery leaves both out.
#define MQTT_STATUS_ONLINE "online"
#define MQTT_STATUS_OFFLINE "offline"

// QoS and retain per topic; the reasons are in ADR 0029. QoS 1 throughout:
// the device learns that the broker has each message, which a sensor that
// publishes and then sleeps needs (requirements 11.2), and every message is
// idempotent, so a duplicate costs nothing. QoS 2 buys nothing more.
#define MQTT_STATE_QOS 1u
#define MQTT_STATE_RETAIN true
#define MQTT_STATUS_QOS 1u
#define MQTT_STATUS_RETAIN true
#define MQTT_DISCOVERY_QOS 1u
#define MQTT_DISCOVERY_RETAIN true

typedef struct {
    char device_id[MQTT_DEVICE_ID_MAX + 1u];
    char discovery_prefix[MQTT_DISCOVERY_PREFIX_MAX + 1u];
    char base[MQTT_TOPIC_MAX];  // <prefix>/<device id>: discovery's "~"
    char state[MQTT_TOPIC_MAX];
    char status[MQTT_TOPIC_MAX];
    char ha_status[MQTT_TOPIC_MAX];
} mqtt_topics_t;

// One level: 1..`max` characters of [A-Za-z0-9_-]. False for NULL.
bool mqtt_topic_level_valid(const char *level, size_t max);

// Levels as above, joined by single '/', at most `max` characters in all.
bool mqtt_topic_prefix_valid(const char *prefix, size_t max);

// Check the three parts and build every fixed topic into `out`.
// HAL_ERR_INVALID_ARG, with `out` zeroed, when any part is not valid --
// mqtt_topic_*_valid() say which.
hal_status_t mqtt_topics_init(mqtt_topics_t *out, const char *prefix, const char *device_id,
                              const char *discovery_prefix);

// <discovery prefix>/<component>/<device id>/<object id>/config, its length
// (text_append.h's conventions); 0 when the component or the object id is
// not a valid level, or the topic does not fit.
size_t mqtt_topics_discovery(const mqtt_topics_t *t, const char *component, const char *object_id,
                             char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_REPORTING_TOPICS_H
