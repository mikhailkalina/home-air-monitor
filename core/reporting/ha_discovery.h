// SPDX-License-Identifier: Apache-2.0
//
// Home Assistant's MQTT discovery: one retained config message per entity,
// on <discovery prefix>/sensor/<device id>/<entity>/config (topics.h), each
// describing one value of the state message (mqtt_payload.h)
// (docs/adr/0029-mqtt-topics-payload-and-discovery.md).
//
//   measurements  temperature, humidity, pressure, co2, and iaq when a source
//                 provides it (a BSEC build)
//   diagnostics   gas_resistance, iaq_accuracy, rssi, uptime
//
// An entity this build does not provide gets an empty config, which removes
// it from Home Assistant: a device moved between a BSEC build and a plain
// one leaves no dead entity behind. Nothing measured by the BME68x is ever
// called CO2 (requirements 13.3): only the co2 entity, fed by SENS_CAP_CO2,
// the SCD41's, carries the carbon_dioxide class.
//
// Per entity rather than HA's device-based discovery (one message for all):
// it works with every HA version still in use, and keeps each message under
// HA_DISCOVERY_CONFIG_MAX. No object_id and no default_entity_id: the first
// is deprecated since HA 2025.10, the second needs it; without either, HA
// names each entity from the device's name and its own.
//
// Every name and version is the composition's -- the core holds none.

#ifndef HAC_CORE_REPORTING_HA_DISCOVERY_H
#define HAC_CORE_REPORTING_HA_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "port_sensor.h"
#include "reporting/topics.h"

#ifdef __cplusplus
extern "C" {
#endif

// Room for any config message plausible strings make; a message that does
// not fit is refused, never cut. Measured: about 800 bytes with this
// device's topics, 969 with every topic at its longest and long device
// strings (gas_resistance, the longest; tests/unit/test_ha_discovery.c
// holds it under this bound).
#define HA_DISCOVERY_CONFIG_MAX 1024u

// An entity expires -- HA shows it unavailable -- after this many heartbeats
// without a state message. CHOICE: three, so that one lost or late message
// does not blank the dashboard, while a device that went silent without its
// last will (a half-open TCP connection) is caught within minutes.
#define HA_DISCOVERY_EXPIRE_HEARTBEATS 3u

// The longest heartbeat accepted: a day.
#define HA_DISCOVERY_HEARTBEAT_MAX_S 86400u

typedef enum {
    HA_ENTITY_TEMPERATURE = 0,
    HA_ENTITY_HUMIDITY,
    HA_ENTITY_PRESSURE,
    HA_ENTITY_CO2,
    HA_ENTITY_IAQ,
    HA_ENTITY_GAS_RESISTANCE,
    HA_ENTITY_IAQ_ACCURACY,
    HA_ENTITY_RSSI,
    HA_ENTITY_UPTIME,
    HA_ENTITY_COUNT,
} ha_entity_t;

// The device block. `name` is required; any other may be NULL, and is then
// left out.
typedef struct {
    const char *name;          // "Home Air Monitor"
    const char *manufacturer;  //
    const char *model;         //
    const char *hw_version;    // the board's silkscreen revision
    const char *sw_version;    // the image's version
} ha_device_t;

// The origin block HA recommends: the software that sent the message.
// `name` NULL leaves the block out; the others may be NULL.
typedef struct {
    const char *name;
    const char *sw_version;
    const char *support_url;
} ha_origin_t;

typedef struct {
    const mqtt_topics_t *topics;  // the device id is its identifier
    ha_device_t device;
    ha_origin_t origin;
    sensor_caps_t caps;    // OR of every sensor source's caps in this build
    bool has_rssi;         // the network reports a signal strength
    uint32_t heartbeat_s;  // the longest the device goes between state messages
} ha_discovery_t;

// The entity's object id: the last level but one of its topic, the suffix
// of its unique id, and the state message's key it shows. NULL out of range.
const char *ha_entity_object_id(ha_entity_t entity);

// True when this build provides `entity`.
bool ha_discovery_provides(const ha_discovery_t *d, ha_entity_t entity);

// The entity's config topic (topics.h), its length; 0 on failure.
size_t ha_discovery_topic(char *dst, size_t cap, const ha_discovery_t *d, ha_entity_t entity);

// The entity's config message into `dst`, terminated, its length in
// `*out_len`: 0 -- an empty message, which removes the entity -- when this
// build does not provide it. False when it does not fit `cap`, or `d` is
// not usable (no topics, no device name, a heartbeat outside
// 1..HA_DISCOVERY_HEARTBEAT_MAX_S): then nothing may be published, since
// an empty message would remove the entity.
bool ha_discovery_config(char *dst, size_t cap, const ha_discovery_t *d, ha_entity_t entity,
                         size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_REPORTING_HA_DISCOVERY_H
