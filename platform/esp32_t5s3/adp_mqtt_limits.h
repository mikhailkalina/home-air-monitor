// SPDX-License-Identifier: Apache-2.0
//
// Every number the board's MQTT client runs on (phase 4g,
// docs/adr/0033-mqtt-on-the-board.md): the queues' in adp_mqtt_pure.h, the
// rest in adp_mqtt.c. Each says whether it is a choice or an estimate, and
// which board check gives an estimate its measured value
// (docs/hardware/board_notes.md, open item "MQTT on the board").

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_LIMITS_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_LIMITS_H

// The broker's port when the URI names none: MQTT's own (IANA).
#define ADP_MQTT_DEFAULT_PORT 1883u

// CHOICE: a broker's name or address, without the terminator. A LAN broker is
// an address or a short name; a longer one is refused at boot.
#define ADP_MQTT_HOST_MAX 64u

// CHOICE: messages and subscriptions accepted and not yet handed to esp-mqtt.
// A session's start is twelve -- the subscription, "online", nine discovery
// configs and the state, all accepted in one reporter step -- so sixteen
// hold it whole. Each slot is a topic and a payload at their longest, about
// 1.2 KB, in PSRAM. publish() says HAL_ERR_NO_MEM past these.
#define ADP_MQTT_OUT_SLOTS 16u

// CHOICE: received messages kept until polled: the port's minimum
// (PORT_TELEMETRY_MIN_INBOX). What the device subscribes to is Home
// Assistant's status, a word a restart.
#define ADP_MQTT_INBOX_SLOTS 8u

// CHOICE: distinct filters in one session, twice the port's minimum
// (PORT_TELEMETRY_MIN_SUBSCRIPTIONS). The reporter needs one.
#define ADP_MQTT_MAX_SUBSCRIPTIONS 8u

// CHOICE: what esp-mqtt's own outbox may hold, in bytes (its outbox.limit):
// QoS 1 messages stay there until the broker's PUBACK, in internal RAM. Four
// discovery configs; the rest of a session's start waits in the slots above,
// in PSRAM, and goes as acknowledgements free room -- milliseconds on a LAN.
#define ADP_MQTT_OUTBOX_LIMIT_BYTES 4096u

// CHOICE: after esp-mqtt's outbox refused a message, the adapter's task
// tries again at the next acknowledgement, or after this.
#define ADP_MQTT_FORWARD_RETRY_MS 200u

// CHOICES: esp-mqtt's buffers. Out holds the longest message whole -- a
// fixed header of 5 bytes, a topic of up to 127 and its length, a packet id,
// a payload of up to 1023 -- so nothing is sent in fragments; in holds what
// the device receives, Home Assistant's status, and a longer message arrives
// in pieces the adapter puts together.
#define ADP_MQTT_OUT_BUFFER_BYTES 1280u
#define ADP_MQTT_IN_BUFFER_BYTES 512u

// CHOICE: how long esp-mqtt waits for the network in one step -- the TCP
// handshake, the broker's CONNACK, a write. A LAN broker answers in
// milliseconds; half the reporter's 10 s for an attempt
// (REPORT_CONNECT_TIMEOUT_MS), so that an attempt esp-mqtt gives up is over
// before the reporter gives up on it. The adapter's task, never the event
// loop, is what waits.
#define ADP_MQTT_NETWORK_TIMEOUT_MS 5000u

// CHOICE: esp-mqtt's reconnection is off (the port never reconnects), and a
// client whose session dropped waits for a reconnection request in steps of
// half this. Stopping it waits for the step to end: a second bounds that to
// half a second.
#define ADP_MQTT_IDLE_POLL_MS 1000u

// ESP-IDF's defaults for esp-mqtt's own task, kept: ESTIMATE until the
// board's first session -- its stack's low-water mark, which the adapter
// measures from inside the task's event handler (`mqtt` at the console, and
// the "memory with a session" line).
#define ADP_MQTT_TASK_STACK_BYTES 6144u
#define ADP_MQTT_TASK_PRIORITY 5

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_MQTT_LIMITS_H
