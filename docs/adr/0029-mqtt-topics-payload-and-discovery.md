# 29. MQTT topics, payload and Home Assistant discovery, as pure functions

Date: 2026-10-20

## Status

Accepted. Phase 4a, branch `phase-4a-mqtt-payload`. Pure functions,
verified on the host only: unit tests with exact expected text, and every
message parsed by Python's own `json` module with cross-checks of its own
(`mqtt_messages_parse`), which three deliberate mutations made fail.
Nothing sends yet (phase 4b), so nothing here has been seen in a Home
Assistant.

## Context

Phase 4 (`docs/architecture.md` section 10) puts the device into Home
Assistant over MQTT with discovery (requirements 10.1, 14.3). Its first
step is what goes over the wire, as pure functions in the core, before
anything sends. The wire contract is worth settling first because it is
expensive to change afterwards: the broker keeps retained messages until
something overwrites them, and Home Assistant keeps each entity's history
under its `unique_id`, so a renamed key or id is a migration, not an edit.

What the requirements ask for: configurable topics (15.4); a sensor error
visible in MQTT, not only on the screen (15.1); a retained state topic, an
availability topic, unique identifiers (14.3); nothing from the BME68x
labelled CO2 (13.3).

## Decision

### Topics

    <prefix>/<device id>/state                               QoS 1, retained
    <prefix>/<device id>/status   online / offline, LWT      QoS 1, retained
    <discovery prefix>/sensor/<device id>/<entity>/config    QoS 1, retained
    <discovery prefix>/status     HA's birth and last will; phase 4b subscribes

`core/reporting/topics.c` builds them from three parts the composition
passes in, checked once at `mqtt_topics_init()`. Every level is one or more
characters of `[A-Za-z0-9_-]`: Home Assistant's rule for the node id and
object id of a discovery topic, which the device id becomes. That rules out
`+` and `#`, a leading `$` (the broker's own topics), spaces and empty
levels; the two prefixes may have several levels, separated by single `/`.
MQTT allows more, and nothing the device names needs it. Bounds, CHOICES:
prefix 48 characters, device id 32, discovery prefix 32 (default
`homeassistant`), so every topic fits `MQTT_TOPIC_MAX` (128; the longest is
112) and every discovery message `HA_DISCOVERY_CONFIG_MAX`.

On the board the device id is to be derived from the Wi-Fi MAC (phase 4b).
Every `unique_id` hangs on it: a different id is a different device to
Home Assistant.

### QoS and retain

**QoS 1 for everything.** A battery-powered device publishes in a short
window and then sleeps (requirements 11.2); only a `PUBACK` tells it the
broker has the message before it turns the radio off -- at QoS 0 a message
still in the TCP send buffer is lost with the connection, and the device
never knows. On USB the cost is one acknowledgement per message, minutes
apart. Every message is idempotent -- a state, an availability, a config --
so QoS 1's possible duplicate costs nothing, and QoS 2's extra handshake
buys nothing.

**Everything retained.**

- *Status*: the last will, `offline`, is retained, and so is `online`.
  Without it a Home Assistant that starts after the device went away would
  not know.
- *Discovery*: Home Assistant re-reads retained configs whenever it
  restarts. Phase 4b also republishes them when its birth message arrives.
- *State*: requirements 14.3 asks for it, and any subscriber -- MQTT
  Explorer, a script, a second Home Assistant -- gets the last reading at
  once. Home Assistant's own documentation advises against it for a sensor
  with `expire_after`: on its restart the broker replays the retained value,
  and HA takes it for a fresh one. Here the retained `offline` already makes
  every entity unavailable whenever the device dropped off cleanly or its
  keepalive ran out. What is left is a device that died while the broker
  still thinks it is connected: then an old reading can be shown after a HA
  restart, for at most `expire_after`. Accepted.

### The state message

One JSON object, every key always present, always in this order, no
whitespace (`core/reporting/mqtt_payload.h` has the example):
`temperature`, `humidity`, `pressure`, `co2`, `iaq` and `gas_resistance`,
each followed by `<key>_status`; `iaq_accuracy` after IAQ's status; then
`rssi` and `uptime`.

- **A value that is not `VAL_OK` is `null`** -- `VAL_STALE` included. Home
  Assistant stamps a state with the time it arrives, so an old number would
  enter its history as a new one. `null` makes HA show "unknown", and its
  history has a gap instead of a flat line. The screen and the recordings
  keep showing a stale number; they say how old it is, HA could not.
- **`<key>_status` says why**: `ok`, `warmup`, `stale`, `error`,
  `unavailable` -- the recordings' words (`record_format.h`), so a card's CSV
  and an MQTT message use one vocabulary. Exposed to Home Assistant as the
  entity's attributes. So a fault is visible in MQTT (15.1), and a warm-up
  is told apart from a fault.
- **A `VAL_OK` value JSON cannot hold** -- NaN, an infinity, a number too
  large to write exactly -- is published as `null` with `error`: what
  `measurement_ok()` would have made of it. JSON has no NaN, and a payload
  that held one would be refused by a strict parser.
- **IAQ publishes its number with `iaq_accuracy` beside it**: `unreliable`,
  `low`, `medium`, `high`, or `null` for no verdict -- the screen's question
  mark (ADR 0021), as data. The accuracy is published whatever the status:
  BSEC's accuracy 0 is `"iaq":null,"iaq_status":"warmup",
  "iaq_accuracy":"unreliable"`.
- **`rssi`** is the Wi-Fi signal in dBm while the network is up, `null`
  otherwise; **`uptime`** whole seconds.
- **Every build sends every key.** A plain build's IAQ is `null` and
  `unavailable`: the schema does not depend on the build, and which entities
  exist is discovery's business. Not in it yet: the VOC index, which no
  source provides, and the battery (phase 5). A key added later breaks no
  reader.

**Decimals: the recordings'** (`record_decimals()`, used directly):
temperature and humidity to 0.01, pressure to 0.001 hPa, CO2 in whole ppm,
IAQ to 0.01, gas resistance in whole ohms. Not the screen's, for two
reasons. Home Assistant's history and long-term statistics are a second
archive of the same readings: rounding them to the screen's whole hPa and
whole percent would quantise their means, and the 0.07 hPa a closing window
made (`board_notes.md`, phase 3c's measurements) would vanish. And the
screen's resolution is a display decision HA can make itself: discovery
passes the screen's decimals as `suggested_display_precision`, so HA's
cards show what the glass shows (a test holds the two to the view model).
The cost: a few bytes a message, and HA's graphs show the sensors' noise.

### Discovery

One retained config per entity on
`<discovery prefix>/sensor/<device id>/<entity>/config`:

| Entity | Class | Unit | State class | Shown | Category | Provided when |
|---|---|---|---|---|---|---|
| `temperature` | temperature | °C | measurement | 1 | | `SENS_CAP_TEMPERATURE` |
| `humidity` | humidity | % | measurement | 0 | | `SENS_CAP_HUMIDITY` |
| `pressure` | atmospheric_pressure | hPa | measurement | 0 | | `SENS_CAP_PRESSURE` |
| `co2` | carbon_dioxide | ppm | measurement | 0 | | `SENS_CAP_CO2` |
| `iaq` | aqi | | measurement | 0 | | `SENS_CAP_IAQ` |
| `gas_resistance` | | Ω | measurement | 0 | diagnostic | `SENS_CAP_GAS_RES` |
| `iaq_accuracy` | enum: unreliable, low, medium, high | | | | diagnostic | `SENS_CAP_IAQ` |
| `rssi` | signal_strength | dBm | measurement | 0 | diagnostic | the network reports one |
| `uptime` | duration | s | | 0 | diagnostic | always |

The capabilities are the OR of every sensor source's caps in the build,
passed in by the composition. The measured entities carry their
`<key>_status` as attributes (`json_attributes_template`), IAQ its accuracy
too. Uptime has no state class: a counter of seconds has no statistics
worth keeping, and its history shows the reboots.

- **An entity this build does not provide gets an empty retained config**,
  which removes it from Home Assistant. A device moved between a BSEC build
  and a plain one leaves no dead entity behind. `ha_discovery_config()`
  answers success with length 0 for it, and failure -- never the empty
  message -- when it cannot build one, so that a buffer too short can never
  remove an entity.
- **Nothing from the BME68x is called CO2** (13.3): only the `co2` entity
  has the carbon_dioxide class, and it exists only where `SENS_CAP_CO2` does
  -- the SCD41's. BSEC's CO2 equivalent was never subscribed (ADR 0020). A
  test reads every other config for "co2", "carbon" and "ppm".
- **`expire_after` is three heartbeats** (`HA_DISCOVERY_EXPIRE_HEARTBEATS`,
  a CHOICE): one lost or late message does not blank the dashboard, while a
  device that stops publishing without its last will is caught within
  minutes -- a half-open TCP connection the broker has not noticed, or an
  event loop that hangs while the MQTT client's own task still answers the
  keepalive, which no last will ever reports. The
  heartbeat, the longest the device goes between state messages, is the
  composition's, decided with the publish cadence in phase 4b (1 s to a
  day).
- **Per entity, not device-based discovery.** Device-based discovery is one
  message for all entities, roughly 4 KB here, and needs Home Assistant
  2024.11 or later (the phase's brief; the version not checked here). Per entity works with every version still in use, and each
  message stays under 1 KB: measured, about 800 bytes with this device's
  topics, 969 at most with every topic at its longest and long device
  strings.
- **Full key names, with `~`.** The base topic `<prefix>/<device id>` is
  given once as `~`, and the three topics are `~/state`, `~/status`. Home
  Assistant's abbreviations (`stat_t`, `uniq_id`...) would save another 150
  bytes; the full names stay readable in MQTT Explorer, and the messages fit
  without them.
- **No `object_id`, no `default_entity_id`.** `object_id` is gone from Home
  Assistant's MQTT documentation; projects that migrated report it
  deprecated in 2025.10, with HA's log announcing its removal for 2026.4.
  `default_entity_id`, its replacement, needs 2025.10 or later.
  Without either, HA builds the entity id from the device's name and the
  entity's ("sensor.home_air_monitor_temperature"); the `unique_id`,
  `<device id>_<entity>`, is what keeps an entity and its history across
  renames.
- **Names are short** -- "Temperature", "CO2", "IAQ accuracy" -- since HA
  prefixes every entity's name with its device's.
- **The device block**: `identifiers` is the device id; `name` is required;
  `manufacturer`, `model`, `hw_version` (the board's silkscreen revision)
  and `sw_version` (the image's version) are optional. **The origin block**
  Home Assistant recommends -- the software that sent the message: `name`,
  `sw_version`, `support_url` -- is optional. All of them are the
  composition's; the core holds no names.
- **`null` reaches Home Assistant as "unknown"**: the template renders a
  JSON `null` as `None`, and HA's MQTT sensor sets a `None` state to unknown
  -- for the enum sensor too, since it checks for `None` before it checks
  the options (`homeassistant/components/mqtt/sensor.py`, read 2026-10-20).
- **The status topic's payloads** are Home Assistant's defaults, `online`
  and `offline`, so `payload_available` and `payload_not_available` are
  left out.

The keys were checked against Home Assistant's documentation on 2026-10-20
(the MQTT integration and MQTT sensor pages) and its MQTT sensor's source.
Not checked key by key: which release introduced each of them. All are in
the current documentation, and none is deprecated.

### Where the code lives

- `core/text/text_append.h` -- text without stdio, numbers and fixed point.
  It was `core/recording/record_text.h`; the payload needs the same numbers,
  so it moved here, renamed, with `text_append_i64()` for the RSSI and
  `text_fixed_representable()`, which lets the payload tell "no number" from
  "no room".
- `core/reporting/json_writer.h` -- JSON into the caller's buffer: commas
  and colons placed, strings escaped as RFC 8259 asks, no allocation. A
  message that does not fit, or is put together wrongly, is refused whole.
- `core/reporting/topics.h`, `mqtt_payload.h`, `ha_discovery.h` -- the
  three above. Pure: the caller owns every buffer.

## Consequences

**Verified on the host.** Unit tests compare exact text for every status
of every value and every accuracy; NaN, infinities and overflow; negative
temperatures and the rounding of -0.0; a buffer exactly large enough and one
byte short, for the text, the writer, the topics, the payload and the
configs; illegal and over-long prefixes and ids; which entities each build
provides; that the display precision is the screen's. `mqtt_messages_parse`
runs `tests/unit/mqtt_messages_dump.c` and parses its 55 messages with
Python's `json`, strictly -- no NaN, no duplicate key -- and checks what the
C tests cannot see: that every state message has the same keys, every
template names only keys the state message has, `~` expands to the device's
own topics, escaped text comes back as it went in. Seen failing: with
quotes left unescaped (80 failures), a stale value published (6), and an
attributes template naming a key the state lacks (3).

**What phase 4b inherits**:

- On connect: the last will on the status topic, `online`, then all nine
  configs -- the empty ones too -- and again whenever `<discovery
  prefix>/status` says `online`.
- A heartbeat, and the `expire_after` that follows from it.
- esp-mqtt's buffer defaults to 1024 bytes (`CONFIG_MQTT_BUFFER_SIZE`, its
  Kconfig in ESP-IDF 5.5), and its client sends a longer message in
  fragments (read in `mqtt_client.c`, not tried). A config message is up to
  969 bytes, with a topic of up to 112: see one go through whole.
- The device id from the MAC, within `[A-Za-z0-9_-]{1,32}`.

**Costs.**

- Home Assistant's documentation says an attributes topic implies
  `force_update`: every state message may be written to HA's database for
  each of the six measured entities, changed or not. The heartbeat bounds
  it.
- While a reading is stale or warming up, HA's history has a gap.
- The keys, the entity object ids and the `unique_id` scheme are a contract
  from the first message a real Home Assistant receives; changing one later
  means migrating entities, so it needs an ADR of its own.

**Not verified**: nothing has been seen in a real Home Assistant -- the
simulator is to show up there first, in phase 4b (`docs/architecture.md`
section 10).
