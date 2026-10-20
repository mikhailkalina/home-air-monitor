// SPDX-License-Identifier: Apache-2.0
//
// air_reading_t as the state message, one JSON object, every key always
// present and always in this order
// (docs/adr/0029-mqtt-topics-payload-and-discovery.md):
//
//   {"temperature":23.41,"temperature_status":"ok",
//    "humidity":48.20,"humidity_status":"ok",
//    "pressure":1006.214,"pressure_status":"ok",
//    "co2":812,"co2_status":"ok",
//    "iaq":null,"iaq_status":"warmup","iaq_accuracy":"unreliable",
//    "gas_resistance":153204,"gas_resistance_status":"ok",
//    "rssi":-61,"uptime":3600}
//
// (one line on the wire, no whitespace). For each measurement:
//
//   <key>          the number, for VAL_OK; null for anything else, VAL_STALE
//                  included -- Home Assistant stamps a state with the time it
//                  arrives, so an old number would enter its history as new.
//                  Null makes HA show "unknown", a gap in its history rather
//                  than a flat line.
//   <key>_status   "ok", "warmup", "stale", "error", "unavailable": the
//                  recordings' words (record_format.h). A VAL_OK value that
//                  is not a finite number, or too large to write, is
//                  published as null with "error" -- what measurement_ok()
//                  would have made of it.
//
// The numbers carry the recordings' decimals (record_decimals()): HA's
// history is a second archive of the same readings. "iaq_accuracy" is the
// source's verdict on IAQ (docs/adr/0021): "unreliable", "low", "medium",
// "high", or null when the source gives none. "rssi" is the Wi-Fi signal in
// dBm while the network is up, null otherwise; "uptime" whole seconds.
//
// A quantity this build does not measure is still in the message, null and
// "unavailable": the schema is the same for every build, and Home Assistant
// is told which entities exist by discovery (ha_discovery.h), not by the
// state. The VOC index, which no source provides, and the battery (phase 5)
// are not in it yet; adding a key later breaks no reader.

#ifndef HAC_CORE_REPORTING_MQTT_PAYLOAD_H
#define HAC_CORE_REPORTING_MQTT_PAYLOAD_H

#include <stddef.h>

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// Room for any state message mqtt_payload_state() writes, terminator
// included: the longest, every number at the largest text_append_fixed()
// writes, is under 400 bytes (tests/unit/test_mqtt_payload.c holds it).
#define MQTT_PAYLOAD_STATE_MAX 512u

// The measurements the state message carries, each with its "_status".
typedef enum {
    MQTT_VALUE_TEMPERATURE = 0,
    MQTT_VALUE_HUMIDITY,
    MQTT_VALUE_PRESSURE,
    MQTT_VALUE_CO2,
    MQTT_VALUE_IAQ,
    MQTT_VALUE_GAS_RESISTANCE,
    MQTT_VALUE_COUNT,
} mqtt_value_t;

// The other keys.
#define MQTT_PAYLOAD_IAQ_ACCURACY_KEY "iaq_accuracy"
#define MQTT_PAYLOAD_RSSI_KEY "rssi"
#define MQTT_PAYLOAD_UPTIME_KEY "uptime"

// "temperature", "humidity", "pressure", "co2", "iaq", "gas_resistance";
// NULL out of range.
const char *mqtt_payload_key(mqtt_value_t value);

// The same with "_status"; NULL out of range.
const char *mqtt_payload_status_key(mqtt_value_t value);

// The decimals a value is published with: its recording column's.
unsigned mqtt_payload_decimals(mqtt_value_t value);

// The state message for `reading` into `dst`, terminated; its length, or 0
// when it does not fit `cap` (nothing usable is left in `dst` then).
size_t mqtt_payload_state(char *dst, size_t cap, const air_reading_t *reading);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_REPORTING_MQTT_PAYLOAD_H
