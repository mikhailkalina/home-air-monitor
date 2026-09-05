# Home Air Monitor on the T5-ePaper-S3

## 1. Purpose of this document

This document captures the technical summary of a home air quality monitor project built on the **LILYGO T5-ePaper-S3** board with external sensors for temperature, humidity, pressure, CO₂ and air quality.

It is intended for the team responsible for hardware design, firmware architecture and smart home integrations.

---

## 2. System goal

Build an autonomous or semi-autonomous device that monitors indoor air parameters, displays them locally on an e-paper screen, and can forward measurements to a smart home system.

The device must measure and display:

- temperature;
- relative humidity;
- barometric pressure;
- CO₂ concentration;
- an air quality / VOC / IAQ indicator;
- battery or power status;
- network / smart home connection status.

---

## 3. Base hardware platform

The first hardware revision will use:

**LILYGO T5-ePaper-S3**

Expected platform characteristics:

- MCU: ESP32-S3;
- integrated e-paper display;
- touch panel;
- Wi-Fi support;
- deep sleep support;
- PSRAM;
- USB and/or battery power;
- I²C for external sensors;
- local UI on e-paper.

---

## 4. Proposed sensor set

### 4.1 BME680 / BME688

Used to measure:

- temperature;
- humidity;
- pressure;
- gas resistance / VOC-related signal;
- IAQ when used with Bosch BSEC/BSEC2.

Recommended role:

```text
Temperature  -> BME680/BME688
Humidity     -> BME680/BME688
Pressure     -> BME680/BME688
VOC / IAQ    -> BME680/BME688 + BSEC/BSEC2
```

Important: the BME680/BME688 is **not a true CO₂ sensor**. Its estimated eCO₂ must not be treated as an actual CO₂ concentration.

### 4.2 SCD41

Used to measure:

- true CO₂ in ppm;
- temperature;
- humidity.

Recommended role:

```text
CO₂ -> SCD41
```

Temperature and humidity from the SCD41 can be used for diagnostics, compensation and cross-checking against the BME680/BME688, but the primary temperature and humidity values should come from the BME680/BME688 or a dedicated SHT sensor, once the thermal influence of the enclosure has been verified.

### 4.3 Final data source assignment

```text
CO₂          -> SCD41
Temperature  -> BME680/BME688 or another dedicated temp/RH sensor
Humidity     -> BME680/BME688 or another dedicated temp/RH sensor
Pressure     -> BME680/BME688
VOC / IAQ    -> BME680/BME688 + Bosch BSEC/BSEC2
Battery      -> ADC / fuel gauge / PMU depending on board revision
```

---

## 5. Recommended software framework

### 5.1 Primary recommendation

The main firmware should be built on:

```text
ESP-IDF 5.x
```

Reasons:

- better control over power, sleep/wake and peripherals;
- easier to build a long-lived architecture;
- better suited to Wi-Fi, MQTT, Matter, OTA, NVS and low-power scenarios;
- easier control over PSRAM and memory layout;
- less dependence on the Arduino wrapper layer and legacy libraries.

### 5.2 Role of the Arduino examples

The Arduino examples for the T5-ePaper-S3 are useful for:

- quick board bring-up;
- verifying the display;
- verifying touch;
- confirming the pinout;
- reference implementation of the display initialization sequence;
- reference implementation of peripheral power control.

The production firmware should nonetheless be built on ESP-IDF.

Recommended approach:

```text
1. Run the official Arduino example to verify the board.
2. Confirm the display, touch, I²C, power and sensors all work.
3. Move project development to ESP-IDF.
4. Use the Arduino code as reference only.
```

---

## 6. Drivers and libraries

### 6.1 E-paper display

Recommended sources:

- the official LilyGO repository with board examples;
- EPDiy for ESP-IDF;
- IDF ports of the existing LilyGO examples.

Recommended strategy:

```text
Display backend -> EPDiy or a proven LilyGO IDF port
UI layer        -> a lightweight in-house renderer for the first phase
LVGL            -> defer to a later version
```

LVGL should not be introduced in the first phase if the UI is simple. The priority is a stable measure / render / sleep cycle.

### 6.2 Touch panel

If the board revision includes a touch panel, the likely controller is:

```text
GT911 over I²C
```

Touch should be isolated in its own component:

```text
components/
  gt911/
    gt911.c
    gt911.h
```

Minimum functionality:

- init;
- reset sequence;
- coordinate readout;
- handling of one or two touch points;
- optional interrupt;
- optional touch-to-wake.

### 6.3 Sensors

Use the ESP-IDF component ecosystem and existing IDF drivers wherever possible.

Approximate structure:

```text
components/
  sensors/
    sensor_manager.c
    sensor_bme680.c
    sensor_scd41.c
    sensor_common.h
```

For the BME680/BME688:

- basic readings can be obtained through an open source driver;
- IAQ/VOC index requires Bosch BSEC/BSEC2;
- BSEC/BSEC2 may require separate integration of a binary library, with licensing implications.

For the SCD41:

- use an SCD4x driver;
- account for the warm-up period;
- account for single-shot vs periodic measurement modes;
- review the automatic self-calibration settings;
- provide state save/restore where applicable.

---

## 7. Proposed firmware architecture

### 7.1 Overall project structure

```text
main/
  app_main.c
  app_config.h

components/
  board_t5_epaper_s3/
    board_pins.h
    board_power.c
    board_i2c.c
    board_display.c
    board_touch.c
    board_battery.c

  sensors/
    sensor_manager.c
    sensor_bme680.c
    sensor_scd41.c
    sensor_common.h

  ui/
    ui_home_screen.c
    ui_status_bar.c
    ui_graphs.c
    ui_common.h

  reporting/
    reporter_common.h
    mqtt_reporter.c
    matter_reporter.c
    local_http_reporter.c

  storage/
    nvs_config.c
    measurement_log.c

  connectivity/
    wifi_manager.c
    provisioning.c

  ota/
    ota_manager.c
```

### 7.2 Separation of concerns

Sensors must not know where their data is sent.

The UI must not poll sensors directly.

MQTT and Matter must not read hardware drivers directly.

Recommended model:

```text
sensor drivers
    ↓
sensor_manager
    ↓
normalized air_reading_t
    ↓
ui / mqtt / matter / logger
```

### 7.3 Unified data structure

Recommended structure:

```c
typedef struct {
    float temperature_c;
    float humidity_rh;
    float pressure_hpa;

    uint16_t co2_ppm;

    float iaq;
    float voc_index;
    uint32_t gas_resistance_ohm;

    uint8_t battery_percent;
    uint16_t battery_mv;

    int8_t wifi_rssi_dbm;

    uint32_t uptime_s;
    uint64_t timestamp_ms;
} air_reading_t;
```

---

## 8. Main device cycle

For the first version, the following cycle is recommended:

```text
1. Wake up / start
2. Init board peripherals
3. Init I²C
4. Read sensors
5. Update internal measurement state
6. Update e-paper display if needed
7. Connect Wi-Fi if reporting is enabled
8. Publish data via MQTT / HTTP / Matter
9. Store optional local log
10. Enter sleep / wait until next cycle
```

A USB-powered device may run in continuous mode:

```text
1. Periodic sensor read
2. Periodic display update
3. Periodic network report
4. Optional local web UI / Matter active session
```

A battery-powered device should use a duty cycle:

```text
Measure often
Display less often
Wi-Fi rarely or only on significant changes
Sleep aggressively
```

---

## 9. Working with the e-paper display

### 9.1 Guidelines

- avoid frequent full refreshes;
- use partial refresh where it is reliably supported;
- cap the screen update rate;
- decouple sensor sampling from display updates;
- maintain a screen dirty flag;
- update only when data changes beyond a threshold.

Example:

```text
CO₂ changed by > 50 ppm        -> update display
Temperature changed by > 0.2°C -> update display
Humidity changed by > 1%       -> update display
Time since last update > 2 min -> update display
```

### 9.2 First-phase UI

Minimum screen:

```text
CO₂:          738 ppm
Temperature: 23.4 °C
Humidity:    45 %
Pressure:    1012 hPa
IAQ:         Good / 84
Battery:     87 %
Wi-Fi:       Connected / Offline
```

Additionally:

- air quality icons;
- a warning at high CO₂;
- timestamp of the last measurement;
- sensor error indicator;
- stale data indicator.

---

## 10. Smart home support

### 10.1 Recommended MVP: MQTT + Home Assistant Discovery

The first version should implement:

```text
MQTT reporter
Home Assistant MQTT Discovery
```

Advantages:

- fast to implement;
- easy to debug;
- no Matter commissioning required;
- a good fit for ESP-IDF;
- convenient to test with mosquitto, MQTT Explorer and Home Assistant.

Example topics:

```text
homeair/t5s3/state
homeair/t5s3/status
homeair/t5s3/config
```

Example state payload:

```json
{
  "temperature": 23.4,
  "humidity": 45.2,
  "pressure": 1012.8,
  "co2": 738,
  "iaq": 84,
  "voc_index": 112,
  "gas_resistance": 182000,
  "battery": 87,
  "rssi": -61
}
```

Recommended Home Assistant entity classes:

```text
temperature      -> device_class: temperature, unit: °C
humidity         -> device_class: humidity, unit: %
pressure         -> device_class: pressure, unit: hPa
co2              -> device_class: carbon_dioxide, unit: ppm
battery          -> device_class: battery, unit: %
signal strength  -> device_class: signal_strength, unit: dBm
```

### 10.2 Matter

Matter can be added in the second version.

Recommended implementation:

```text
ESP-IDF + esp-matter
Matter over Wi-Fi
Device type: Air Quality Sensor
```

A possible Matter model:

```text
Endpoint 0: Root node

Endpoint 1: Air Quality Sensor
  ├─ Air Quality cluster
  ├─ Temperature Measurement cluster
  ├─ Relative Humidity Measurement cluster
  ├─ Pressure Measurement cluster
  ├─ Carbon Dioxide Concentration Measurement cluster
  └─ TVOC / VOC-related cluster if supported
```

Alternatively the parameters can be exposed as several separate endpoints:

```text
Endpoint 1: Temperature Sensor
Endpoint 2: Humidity Sensor
Endpoint 3: Pressure Sensor
Endpoint 4: Air Quality Sensor
```

Matter should be isolated in its own module:

```text
components/
  reporting/
    matter_reporter.c
    matter_reporter.h
```

This allows Matter to be added without rewriting the sensor manager or the UI.

### 10.3 Matter limitations

Matter is more complex than MQTT:

- higher memory requirements;
- commissioning;
- mDNS;
- secure sessions;
- ecosystems may present the Air Quality Sensor differently;
- higher power consumption over Wi-Fi;
- harder to debug.

For a battery-powered device, Matter over Wi-Fi may be less favourable than MQTT with short Wi-Fi windows.

---

## 11. Power and energy consumption

### 11.1 General guidelines

- use ESP32-S3 deep sleep wherever possible;
- disable Wi-Fi between transmissions;
- do not refresh the e-paper more often than necessary;
- account for SCD41 consumption;
- account for BME680/BME688 gas sensor heating;
- account for the fact that BSEC may require a regular sampling interval;
- separate high-power events from accurate temperature measurements.

### 11.2 Operating modes

#### USB / permanent power

```text
Sensor read:       every 5–30 seconds
Display update:    every 30–120 seconds
MQTT publish:      every 30–60 seconds
Matter:            enabled
Wi-Fi:             always on or mostly on
```

#### Battery mode

```text
Sensor read:       every 30–300 seconds
Display update:    every 60–300 seconds
MQTT publish:      every 60–600 seconds or on change
Matter:            optional / not recommended for the MVP
Wi-Fi:             short wake window
Sleep:             aggressive deep sleep
```

---

## 12. Enclosure and sensor placement

Measurement quality depends heavily on the mechanical design.

Guidelines:

- do not place the BME680/BME688 and SCD41 near the ESP32-S3, DC/DC converters, battery charging circuitry or e-paper power circuitry;
- provide ventilation openings;
- avoid an enclosed volume around the sensors;
- do not direct heat flow from the board towards the sensors;
- where possible, move the sensors to the board edge or onto a separate small sensor board;
- account for SCD41 self-heating;
- account for heating during Wi-Fi transmission;
- do not refresh the display immediately before a temperature measurement if this affects thermal conditions.

---

## 13. Calibration and data quality

### 13.1 CO₂

The SCD41 requires:

- warm-up handling;
- verification of automatic self-calibration;
- the ability to enable/disable ASC;
- forced recalibration where needed;
- persistence of user settings;
- range validation of values.

Example baseline thresholds for the UI:

```text
< 800 ppm       -> Good
800–1200 ppm    -> Moderate
1200–2000 ppm   -> Poor
> 2000 ppm      -> Very poor
```

### 13.2 Temperature and humidity

The following must be compared:

```text
BME680/BME688 temp/RH
SCD41 temp/RH
external reference if available
```

The primary source is chosen after testing.

### 13.3 VOC / IAQ

For the BME680/BME688:

- raw gas resistance is not directly a human-readable IAQ figure;
- Bosch BSEC/BSEC2 is preferable for IAQ;
- burn-in and stabilization must be taken into account;
- BSEC state must be persisted across restarts if BSEC is used;
- VOC/IAQ must never be labelled "CO₂".

---

## 14. Minimum technical requirements for the MVP

### 14.1 Hardware MVP

- T5-ePaper-S3 board;
- BME680 or BME688;
- SCD41;
- I²C sensor connection;
- USB power;
- optional battery;
- an enclosure or a ventilated test frame.

### 14.2 Firmware MVP

- ESP-IDF project;
- board abstraction layer;
- I²C init;
- BME680/BME688 driver;
- SCD41 driver;
- normalized measurement structure;
- e-paper display output;
- Wi-Fi connection;
- MQTT reporting;
- NVS config;
- serial logging;
- basic error handling.

### 14.3 Smart Home MVP

- MQTT publish;
- Home Assistant discovery;
- retained state topic;
- availability topic;
- unique device identifiers.

### 14.4 UI MVP

- CO₂ ppm;
- temperature;
- humidity;
- pressure;
- IAQ/VOC indicator;
- battery/status;
- last update time;
- error indication.

---

## 15. Non-functional requirements

### 15.1 Reliability

- the device must not hang when a single sensor fails;
- a sensor error must be shown on screen and in MQTT;
- Wi-Fi failure must not block measurements;
- MQTT failure must not block the UI;
- the watchdog must be enabled;
- errors must be logged.

### 15.2 Extensibility

The architecture must allow adding:

- Matter;
- a local HTTP API;
- OTA;
- BLE provisioning;
- a PM2.5 sensor;
- an additional VOC sensor;
- on-screen graphs;
- a touch menu;
- a battery-optimized mode.

### 15.3 Performance

- use PSRAM for display buffers where needed;
- keep heap usage under control;
- avoid large stack allocations;
- split FreeRTOS tasks by responsibility;
- do not keep Wi-Fi enabled unnecessarily in battery mode.

### 15.4 Maintainability

- pinout and board revision must live in the board config;
- all magic numbers belong in config;
- drivers must be separated from application logic;
- logging levels must be configurable;
- MQTT topics must be configurable.

---

## 16. Recommended implementation phases

### Phase 0 — board verification

Goal: confirm the board is functional.

Tasks:

- run the official LilyGO Arduino example;
- verify the display;
- verify touch, if present;
- verify I²C;
- verify power;
- verify PSRAM;
- verify serial logging.

Result:

```text
Board acceptance confirmed
```

### Phase 1 — ESP-IDF bring-up

Goal: create a minimal ESP-IDF project.

Tasks:

- create the ESP-IDF project;
- enable PSRAM;
- configure the partition table;
- configure logging;
- configure I²C;
- integrate the display driver;
- render static text on the e-paper.

Result:

```text
ESP-IDF firmware can render text on display
```

### Phase 2 — sensors

Goal: obtain correct measurements.

Tasks:

- integrate the SCD41;
- integrate the BME680/BME688;
- print values to the UART log;
- render values on the e-paper;
- handle missing-sensor errors;
- verify I²C recovery.

Result:

```text
Device shows local air measurements
```

### Phase 3 — MQTT / Home Assistant

Goal: deliver measurements to the smart home.

Tasks:

- implement the Wi-Fi manager;
- implement the MQTT reporter;
- add an availability topic;
- add Home Assistant discovery;
- verify long-term statistics in Home Assistant;
- add retained state.

Result:

```text
Device appears in Home Assistant automatically
```

### Phase 4 — power and sleep

Goal: optimize energy consumption.

Tasks:

- measure baseline current;
- configure light sleep / deep sleep;
- disable Wi-Fi between transmissions;
- optimize the e-paper refresh rate;
- assess the impact of BME680/BSEC sampling on sleep;
- verify SCD41 modes.

Result:

```text
Power profile documented and optimized
```

### Phase 5 — UX and reliability

Goal: make the device usable and robust.

Tasks:

- add a status screen;
- add an error screen;
- add stale data indication;
- add a touch menu if needed;
- add NVS settings;
- add OTA;
- add the watchdog;
- add crash diagnostics.

Result:

```text
Usable prototype
```

### Phase 6 — Matter

Goal: add Matter support as an alternative backend.

Tasks:

- integrate esp-matter;
- create an Air Quality Sensor endpoint;
- add measurement clusters;
- synchronize Matter attributes from `air_reading_t`;
- test with Home Assistant Matter;
- test with Apple Home / Google Home if required;
- assess power consumption.

Result:

```text
Matter-compatible prototype
```

---

## 17. Principal technical risks

### 17.1 Display

Risks:

- complex e-paper initialization;
- differing board revisions;
- incompatible examples;
- high RAM/PSRAM demand;
- partial refresh artifacts.

Mitigation:

- start from the official examples;
- pin down the board revision;
- use a proven display backend;
- enable PSRAM;
- document the pinout.

### 17.2 BME680/BME688 IAQ

Risks:

- BSEC/BSEC2 integration complexity;
- binary library licensing;
- the need for state persistence;
- long burn-in;
- misinterpretation of eCO₂.

Mitigation:

- show raw gas resistance or a simple VOC indicator in the MVP;
- add BSEC as a second step;
- keep CO₂ explicitly separate from VOC/IAQ.

### 17.3 SCD41

Risks:

- self-heating;
- calibration;
- incorrect ASC behaviour in a poorly ventilated room;
- power consumption.

Mitigation:

- design the enclosure carefully;
- expose ASC settings;
- add a forced calibration option;
- measure current draw in each mode.

### 17.4 Matter

Risks:

- more complex development;
- higher memory requirements;
- higher power consumption;
- inconsistent Air Quality Sensor support across ecosystems.

Mitigation:

- keep Matter out of the MVP;
- implement MQTT first;
- isolate the reporting abstraction;
- add Matter as a second backend.

---

## 18. Recommended first-version stack

```text
Framework:       ESP-IDF 5.x
Display:         EPDiy / LilyGO IDF-compatible display driver
Touch:           GT911 component if present
Sensors:         SCD41 + BME680/BME688
CO₂ source:      SCD41
IAQ/VOC source:  BME680/BME688 + optional BSEC/BSEC2
Storage:         NVS
Logging:         UART first, optional file/log later
Smart home:      MQTT + Home Assistant Discovery
Matter:          later phase
UI:              custom lightweight e-paper UI
OTA:             later MVP+ phase
```

---

## 19. Recommended initial development checklist

- [ ] Confirm exact T5-ePaper-S3 board revision.
- [ ] Run official LilyGO example.
- [ ] Verify display.
- [ ] Verify touch panel, if available.
- [ ] Verify PSRAM.
- [ ] Create ESP-IDF project.
- [ ] Add board pin configuration.
- [ ] Add I²C bus abstraction.
- [ ] Add SCD41 driver.
- [ ] Add BME680/BME688 driver.
- [ ] Define `air_reading_t`.
- [ ] Display readings on e-paper.
- [ ] Add Wi-Fi connection.
- [ ] Add MQTT publish.
- [ ] Add Home Assistant Discovery.
- [ ] Add NVS configuration.
- [ ] Add error handling.
- [ ] Measure power consumption.
- [ ] Tune display update interval.
- [ ] Tune sensor sampling interval.
- [ ] Decide whether BSEC/BSEC2 is required.
- [ ] Plan Matter integration as separate milestone.

---

## 20. Key engineering decisions

### Decision 1

Use ESP-IDF as the primary framework.

### Decision 2

Use the Arduino/LilyGO examples for bring-up and reference only.

### Decision 3

Use the SCD41 as the sole source of true CO₂.

### Decision 4

Use the BME680/BME688 for temperature, humidity, pressure and VOC/IAQ, but not for CO₂.

### Decision 5

Begin smart home integration with MQTT + Home Assistant Discovery.

### Decision 6

Implement Matter later, as an additional reporting backend.

### Decision 7

Design the firmware around a shared `air_reading_t` structure from the outset, so that the UI, MQTT, Matter and logging all consume a single source of data.

---

## 21. Summary for the team

The first version of the device is to be implemented as ESP-IDF firmware for the T5-ePaper-S3 with a BME680/BME688 and an SCD41 attached. The SCD41 provides true CO₂; the BME680/BME688 provides temperature, humidity, pressure and VOC/IAQ. The local UI is rendered on e-paper. Smart home integration in the MVP uses MQTT with Home Assistant Discovery. Matter is not required for the first version and should be added later through a separate reporting backend built on esp-matter.

The core architectural principle: sensors, UI and reporting must be decoupled through a single normalized measurement structure. This makes it possible to add Matter, an HTTP API, OTA, battery mode, a touch UI and additional sensors without rewriting the core firmware.
