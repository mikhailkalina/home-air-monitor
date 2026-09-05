# Home Air Monitor — codebase architecture

**Companion to** `requirements.md`
**Scope:** repository layout, layering, port interfaces, dual build for ESP32 and native PC, the simulator, testing strategy, and the procedure for porting to new platforms.

---

## 1. Analysis of the requirements document

### 1.1 What the requirements get right and we keep unchanged

| Decision in the requirements | Assessment |
|---|---|
| A single normalized `air_reading_t` as the contract between layers | The key decision. Kept and extended |
| Sensors unaware of consumers; UI does not poll sensors; MQTT does not read drivers | This is the foundation for platform abstraction |
| Reporting isolated (MQTT / Matter / HTTP as interchangeable backends) | Already almost a port — we formalize it |
| CO₂ (SCD41) separated from VOC/IAQ (BME68x) | Correct, and it shapes the data source model |
| Dirty flag and thresholds for e-paper updates | Pure business logic — moved into the core and covered by tests |
| ESP-IDF 5.x as the primary framework | Agreed, but it must be **one adapter among several**, not the frame of the whole project |

### 1.2 What needs to change

**1. The project tree in §7.1 of the requirements hard-wires ESP-IDF into the repository structure.**
`main/` + `components/` is an ESP-IDF project layout. If the repository root coincides with the ESP-IDF project layout, ESP-IDF stops being an implementation detail and becomes load-bearing: building the logic on a PC becomes impossible, and so does adding a second platform. The relationship must be inverted — a platform-independent core at the top level, with the ESP-IDF project as one application under `apps/`.

**2. `air_reading_t` carries no validity status.**
Yet §15.1 requires that "a sensor error must be shown on screen and in MQTT" and that "the device must not hang when a single sensor fails", and §9.2 requires a stale-data indicator. With the current structure, an SCD41 failure is indistinguishable from `co2_ppm = 0`. Every value needs its own `status` and `timestamp`.

**3. No execution model is defined.**
§15.3 says "split FreeRTOS tasks by responsibility" — but FreeRTOS tasks inside business logic make it both non-portable and untestable. The core should be a single-threaded run-to-completion state machine; concurrency belongs in the adapters.

**4. No testing strategy.**
The requirements say nothing about unit tests, CI or static analysis. Meanwhile, behaviours such as "CO₂ rose by 50 ppm → refresh the display", "one Wi-Fi window every 10 minutes", "data is stale → show the indicator" take hours to verify on hardware and milliseconds on a PC.

**5. Time (`timestamp_ms`, `uptime_s`, intervals) must not come from the system inside the core.**
Otherwise the test "what happens after 8 hours on battery" is impossible to write. The clock is a port.

**6. Deep sleep on the ESP32-S3 is effectively a reboot.**
The core must be able to serialize its state before sleeping and restore it afterwards. This is an architectural requirement, not a detail of §11, and the simulator must be able to emulate that cold start.

---

## 2. Architectural principle

**Hexagonal architecture (ports and adapters).**

```
                    ┌──────────────────────────────────────────┐
                    │                  CORE                    │
                    │   domain · app · ui · policy · scheduler  │
   ports (.h)  ◄────┤   plain C11: no OS, no malloc,           ├────►  ports (.h)
                    │   no esp_*, no stdio, no time()          │
                    └──────────────────────────────────────────┘
                          ▲                              ▲
             ┌────────────┴────────────┐    ┌────────────┴────────────┐
             │  platform/esp32_t5s3    │    │   platform/host         │
             │  ESP-IDF, FreeRTOS,     │    │   SDL2, files, stdio,   │
             │  EPDiy, NVS, esp-mqtt   │    │   libmosquitto, fake HW │
             └─────────────────────────┘    └─────────────────────────┘
                          ▲                              ▲
                  apps/firmware_esp32              apps/simulator
                                                  apps/tests (fakes)
```

### 2.1 Dependency rule (enforced in CI)

1. `core/` depends **only** on `ports/*.h` and a subset of the C standard library (`stdint`, `stdbool`, `string`, `math`). Forbidden: `stdio.h`, `time.h`, `malloc`, `esp_*`, `freertos/*`, `SDL*`.
2. `ports/` is headers only. Not a single `.c` file. No dependencies at all.
3. `platform/<name>/` implements the ports. It contains no business logic and calls no core functions other than publishing events.
4. `drivers/` (BME68x, SCD4x, GT911, EPD) depend only on `port_i2c.h` and `port_clock.h`, and therefore build and test on a PC.
5. `apps/<name>/` is the only place where **composition** (wiring) happens: adapters are constructed and handed to the core.

Enforced in CI by a simple grep rule:

```bash
# the core must know nothing about platforms
! grep -rEn '#include\s+[<"](esp_|freertos/|driver/|SDL|stdio\.h|time\.h)' core/
```

### 2.2 Why C11 rather than C++

- ESP-IDF, BSEC2, and the Sensirion and Bosch drivers are all C; avoiding wrappers reduces friction.
- Interfaces are hand-written vtable structs: explicit, predictable in size, easy to substitute in tests.
- No exceptions, RTTI or hidden allocations — which matters for deep sleep and for static memory analysis.

**Exception:** the simulator GUI (`platform/host/sim_gui/`) may be written in C++17, since it is never built for the target. The core knows nothing about it.

---

## 3. Repository layout

```
home-air-monitor/
├── CMakeLists.txt                  # root: host build (core + drivers + sim + tests)
├── CMakePresets.json               # host-debug, host-asan, host-release, ci
├── cmake/
│   ├── sources_core.cmake          # THE source list for the core, shared by host and IDF
│   ├── sources_drivers.cmake
│   └── warnings.cmake              # -Wall -Wextra -Werror -Wconversion ...
│
├── ports/                          # ★ HEADERS ONLY. The platform contract.
│   ├── hal_status.h
│   ├── port_clock.h
│   ├── port_log.h
│   ├── port_sync.h                 # critical sections / mutex
│   ├── port_i2c.h
│   ├── port_display.h
│   ├── port_input.h
│   ├── port_net.h
│   ├── port_telemetry.h
│   ├── port_storage.h
│   ├── port_power.h
│   └── port_sensor.h               # sensor_source_t — abstraction ABOVE the drivers
│
├── core/                           # ★ ALL BUSINESS LOGIC. Platform independent.
│   ├── domain/
│   │   ├── measurement.h/.c        # measurement_f32_t, air_reading_t, merging, validation
│   │   ├── air_quality.h/.c        # CO₂/IAQ thresholds → Good/Moderate/Poor levels
│   │   └── device_state.h/.c       # state that must survive deep sleep
│   ├── app/
│   │   ├── app_core.h/.c           # state machine: event → actions; next_deadline()
│   │   ├── sensor_manager.h/.c     # polling, source priority, fault tolerance
│   │   ├── update_policy.h/.c      # display refresh and publish thresholds
│   │   ├── power_policy.h/.c       # duty cycle: usb_mode / battery_mode
│   │   └── app_deps.h              # dependency injection struct
│   ├── ui/
│   │   ├── framebuffer.h/.c        # platform-neutral buffer (4bpp gray / 1bpp)
│   │   ├── gfx.h/.c                # primitives: line, rect, blit, glyph
│   │   ├── fonts/                  # generated bitmap fonts (.c)
│   │   ├── view_model.h/.c         # air_reading_t → what actually gets drawn
│   │   ├── screen_home.c
│   │   ├── screen_status.c
│   │   ├── screen_error.c
│   │   └── screen_graphs.c
│   ├── reporting/
│   │   ├── reporter.h              # reporter interface (internal, not a port)
│   │   ├── mqtt_payload.c          # JSON assembly — a pure function, tested in isolation
│   │   └── ha_discovery.c          # Home Assistant Discovery — pure generation
│   └── config/
│       ├── app_config.h/.c         # defaults + overrides from storage
│       └── config_parse.c
│
├── drivers/                        # ★ Drivers over port_i2c — they build on a PC too
│   ├── bme68x/
│   ├── scd4x/
│   ├── gt911/
│   ├── epd_ed047tc1/
│   └── bq27220/
│
├── platform/
│   ├── esp32_t5s3/
│   │   ├── board_config.h          # pinout, revision, I²C addresses, panel resolution
│   │   ├── adp_clock.c
│   │   ├── adp_i2c.c               # ESP-IDF i2c_master + recovery
│   │   ├── adp_display.c           # EPDiy / LilyGo-EPD47 backend, PSRAM buffer
│   │   ├── adp_input.c             # GT911 + IRQ → event queue
│   │   ├── adp_net.c               # wifi_manager
│   │   ├── adp_telemetry_mqtt.c    # esp-mqtt
│   │   ├── adp_storage.c           # NVS + LittleFS
│   │   ├── adp_power.c             # deep sleep, RTC memory, BQ25896/BQ27220
│   │   ├── adp_log.c               # ESP_LOG
│   │   └── adp_sensors.c           # wraps drivers as sensor_source_t
│   │
│   ├── host/
│   │   ├── adp_clock_virtual.c     # virtual clock with time acceleration
│   │   ├── adp_i2c_fake.c          # register-level model of I²C devices (for drivers)
│   │   ├── adp_display_sdl.c       # 960×540 window at 1:1 + e-paper emulation
│   │   ├── adp_display_png.c       # headless: dump frames to PNG (for CI)
│   │   ├── adp_input_sdl.c         # mouse/keyboard → touch events
│   │   ├── adp_net_sim.c           # controllable link quality and availability
│   │   ├── adp_telemetry_mqtt.c    # libmosquitto → a real broker
│   │   ├── adp_telemetry_stdout.c  # sink to log/file
│   │   ├── adp_storage_fs.c        # KV in a JSON file, log in CSV
│   │   ├── adp_power_sim.c         # deep sleep emulated as a core restart
│   │   ├── adp_sensors_replay.c    # ★ sensor data replayed from files
│   │   ├── replay/
│   │   │   ├── replay_source.c     # parser and playback loop
│   │   │   └── fault_inject.c      # fault injection
│   │   └── sim_gui/                # C++17: control panel, sliders, timeline
│   │
│   └── posix_headless/             # optional: build for Raspberry Pi / Linux device
│
├── apps/
│   ├── firmware_esp32/             # ESP-IDF project
│   │   ├── CMakeLists.txt
│   │   ├── sdkconfig.defaults
│   │   ├── partitions.csv
│   │   ├── main/
│   │   │   ├── CMakeLists.txt
│   │   │   └── main.c              # composition: adapters → app_core
│   │   └── components/             # thin wrappers over core/ drivers/ platform/
│   │       ├── hac_core/CMakeLists.txt
│   │       ├── hac_drivers/CMakeLists.txt
│   │       └── hac_platform/CMakeLists.txt
│   │
│   ├── simulator/
│   │   ├── CMakeLists.txt
│   │   └── main.c                  # composition + CLI parsing
│   │
│   └── tools/
│       ├── scenario_gen/           # scenario file generator
│       └── font_gen/
│
├── tests/
│   ├── unit/                       # pure functions: thresholds, JSON, parsers, view_model
│   ├── contract/                   # ★ tests run against ANY implementation of a port
│   ├── scenario/                   # a full day pushed through the core in seconds
│   ├── golden/                     # reference PNGs of each screen
│   └── fuzz/                       # libFuzzer: config and sensor frame parsers
│
├── scenarios/                      # ★ data for the simulator
│   ├── normal_day.csv
│   ├── co2_spike_meeting.csv
│   ├── sensor_scd41_fault.csv
│   ├── battery_drain.csv
│   └── wifi_flapping.csv
│
└── docs/
    ├── architecture.md             # this document
    ├── porting_guide.md
    └── adr/                        # Architecture Decision Records
```

Two points deserve emphasis:

- `apps/firmware_esp32/components/hac_core/CMakeLists.txt` contains **no copy of the source list** — it includes the same `cmake/sources_core.cmake`. One list of files feeds both builds, so they cannot drift apart.
- The simulator is not "a second firmware" but an application that performs composition, exactly like the firmware. `apps/simulator/main.c` is roughly the same length as `apps/firmware_esp32/main/main.c`.

---

## 4. Data model

### 4.1 A measurement with explicit status

```c
/* core/domain/measurement.h */

typedef enum {
    VAL_UNAVAILABLE = 0,  /* sensor absent, or does not provide this quantity */
    VAL_WARMUP,           /* sensor warming up (SCD41, BME68x burn-in)        */
    VAL_OK,
    VAL_STALE,            /* value older than the allowed age                 */
    VAL_ERROR,            /* read failure or physically implausible value     */
} value_status_t;

typedef struct {
    float          value;
    value_status_t status;
    uint64_t       ts_ms;      /* monotonic acquisition time */
    uint8_t        source_id;  /* which sensor_source produced it */
} measurement_f32_t;

typedef struct {
    measurement_f32_t temperature_c;
    measurement_f32_t humidity_rh;
    measurement_f32_t pressure_hpa;
    measurement_f32_t co2_ppm;
    measurement_f32_t iaq;
    measurement_f32_t voc_index;
    measurement_f32_t gas_resistance_ohm;

    measurement_f32_t battery_percent;
    measurement_f32_t battery_mv;
    bool              charging;

    int8_t   wifi_rssi_dbm;
    bool     net_connected;
    bool     telemetry_connected;

    uint32_t uptime_s;
    uint32_t boot_count;
    uint64_t wall_time_ms;     /* real time, if synchronized */
} air_reading_t;
```

What this buys directly against the requirements:

- §15.1 "a sensor error must be shown on screen and in MQTT" — `status` is both rendered in the UI and serialized into the payload.
- §9.2 "stale data indicator" — `VAL_STALE` is derived in the core from `ts_ms` and `now_ms`, and tested with a fake clock.
- §13.1 "SCD41 warm-up" — `VAL_WARMUP` instead of garbage values at start-up.
- §4.3 "data sources" — `source_id` records whether temperature came from the BME68x or, in a degraded mode, from the SCD41.

### 4.2 State that survives deep sleep

```c
/* core/domain/device_state.h — lives in RTC slow memory on the ESP32 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint32_t boot_count;
    uint64_t last_display_update_ms;
    uint64_t last_publish_ms;
    air_reading_t last_published;     /* for the "changed by N" thresholds */
    uint8_t  bsec_state[BSEC_STATE_MAX];
    uint16_t bsec_state_len;
    uint32_t crc32;
} persisted_state_t;
```

The core receives this block at start-up and hands it back before sleeping, through `port_storage`. Where it physically lives — RTC memory, NVS or a file — is the adapter's decision.

---

## 5. Ports (the platform contract)

The substantive parts follow. The common style is a vtable plus an opaque `impl`.

### 5.1 Shared types

```c
/* ports/hal_status.h */
typedef enum {
    HAL_OK = 0,
    HAL_ERR_TIMEOUT,
    HAL_ERR_IO,
    HAL_ERR_NOT_FOUND,
    HAL_ERR_INVALID_ARG,
    HAL_ERR_NO_MEM,
    HAL_ERR_UNSUPPORTED,
    HAL_ERR_BUSY,
    HAL_ERR_NOT_READY,
} hal_status_t;
```

`esp_err_t` never reaches the core; the mapping happens in `platform/esp32_t5s3/adp_*.c`.

### 5.2 The clock — the most important port

```c
/* ports/port_clock.h */
typedef struct port_clock_s port_clock_t;
struct port_clock_s {
    uint64_t (*now_ms)(const port_clock_t *self);       /* monotonic */
    uint64_t (*wall_ms)(const port_clock_t *self);      /* UTC, 0 if unsynchronized */
    void     (*delay_ms)(const port_clock_t *self, uint32_t ms);
    void *impl;
};
```

The core **never** calls `esp_timer_get_time()`, `xTaskGetTickCount()` or `time()`. Consequences:

- the test "there must be exactly N publishes over six hours" runs in milliseconds;
- the simulator supports `--time-scale 3600`, replaying a full day in 24 seconds;
- no flaky tests caused by wall-clock timing.

### 5.3 The sensor abstraction (the seam that files plug into)

```c
/* ports/port_sensor.h */
typedef enum {
    SENS_CAP_TEMPERATURE = 1u << 0,
    SENS_CAP_HUMIDITY    = 1u << 1,
    SENS_CAP_PRESSURE    = 1u << 2,
    SENS_CAP_CO2         = 1u << 3,
    SENS_CAP_VOC         = 1u << 4,
    SENS_CAP_IAQ         = 1u << 5,
    SENS_CAP_GAS_RES     = 1u << 6,
    SENS_CAP_PM25        = 1u << 7,   /* provision for §15.2 */
} sensor_caps_t;

typedef struct {
    measurement_f32_t temperature_c;
    measurement_f32_t humidity_rh;
    measurement_f32_t pressure_hpa;
    measurement_f32_t co2_ppm;
    measurement_f32_t iaq;
    measurement_f32_t voc_index;
    measurement_f32_t gas_resistance_ohm;
} sensor_sample_t;

typedef struct sensor_source_s sensor_source_t;
struct sensor_source_s {
    const char   *name;
    uint8_t       id;
    sensor_caps_t caps;

    hal_status_t (*init)(sensor_source_t *self);
    hal_status_t (*start)(sensor_source_t *self);              /* begin a measurement */
    uint32_t     (*ready_in_ms)(const sensor_source_t *self);  /* when to read        */
    hal_status_t (*read)(sensor_source_t *self, sensor_sample_t *out);
    hal_status_t (*suspend)(sensor_source_t *self);            /* before deep sleep   */
    void         (*deinit)(sensor_source_t *self);

    void *impl;
};
```

This is the **substitution point for the simulator**: on hardware, `impl` is the SCD41 driver over I²C; on a PC it is CSV playback. The core sees the same `sensor_source_t` either way.

### 5.4 The I²C bus — a second, lower substitution point

```c
/* ports/port_i2c.h */
typedef struct port_i2c_s port_i2c_t;
struct port_i2c_s {
    hal_status_t (*write)(port_i2c_t*, uint8_t addr, const uint8_t *d, size_t n, uint32_t to_ms);
    hal_status_t (*read )(port_i2c_t*, uint8_t addr, uint8_t *d, size_t n, uint32_t to_ms);
    hal_status_t (*write_read)(port_i2c_t*, uint8_t addr,
                               const uint8_t *w, size_t wn,
                               uint8_t *r, size_t rn, uint32_t to_ms);
    hal_status_t (*recover)(port_i2c_t*);   /* §17: I²C bus recovery */
    void *impl;
};
```

**Why two levels of abstraction.** They solve different problems:

| Level | What is substituted | What it lets you test |
|---|---|---|
| `sensor_source_t` | the whole sensor | business logic: thresholds, fault tolerance, UI, publishing |
| `port_i2c_t` | the bus | the drivers themselves: Sensirion CRC, BME68x commands, NACK/timeout handling |

Both modes are available on a PC: `--sensors replay` (fast, data from a file) and `--sensors fake-i2c` (the real drivers against a register-level device model). The second mode catches driver bugs without hardware.

### 5.5 Display

```c
/* ports/port_display.h */
typedef enum { PIXFMT_GRAY4 = 0, PIXFMT_MONO1 } pixel_format_t;

typedef enum {
    REFRESH_FULL,      /* full refresh with inversion */
    REFRESH_PARTIAL,   /* partial, accumulates ghosting */
    REFRESH_FAST_MONO, /* fast black-and-white mode */
    REFRESH_CLEAR,     /* clear / panel recovery */
} refresh_mode_t;

typedef struct { uint16_t x, y, w, h; } rect_t;

typedef struct port_display_s port_display_t;
struct port_display_s {
    uint16_t       width;         /* 960 */
    uint16_t       height;        /* 540 */
    pixel_format_t format;        /* GRAY4 — 16 levels on the ED047TC1 */
    uint32_t       min_full_refresh_interval_ms;
    uint32_t       max_partial_refreshes_before_full;  /* burn-in protection */

    framebuffer_t *(*get_framebuffer)(port_display_t*);
    hal_status_t (*flush)(port_display_t*, const rect_t *area, refresh_mode_t mode);
    hal_status_t (*power)(port_display_t*, bool on);
    void *impl;
};
```

Note the split: `max_partial_refreshes_before_full` and `min_full_refresh_interval_ms` are **properties of the panel**, reported by the adapter to the core, while the policy that applies them (§9.1 of the requirements, plus LilyGO's warning about artifacts from prolonged partial refresh) lives in `core/app/update_policy.c` and is tested on a PC.

A 960×540 buffer at 4 bpp is **253 KB** — it fits in PSRAM on the ESP32-S3 (8 MB available), and is an ordinary `malloc` inside the adapter on a PC. The core never allocates: `get_framebuffer()` returns a buffer the platform has already provided.

### 5.6 Input, network, telemetry, storage, power

```c
/* ports/port_input.h */
typedef enum { INPUT_TOUCH_DOWN, INPUT_TOUCH_UP, INPUT_TOUCH_MOVE, INPUT_BUTTON } input_kind_t;
typedef struct { input_kind_t kind; uint16_t x, y; uint8_t id; uint64_t ts_ms; } input_event_t;

typedef struct port_input_s port_input_t;
struct port_input_s {
    bool (*poll)(port_input_t*, input_event_t *out);  /* non-blocking */
    void *impl;
};

/* ports/port_net.h */
typedef enum { NET_DOWN, NET_CONNECTING, NET_UP, NET_FAILED } net_state_t;
typedef struct port_net_s port_net_t;
struct port_net_s {
    hal_status_t (*up)(port_net_t*);
    hal_status_t (*down)(port_net_t*);
    net_state_t  (*state)(const port_net_t*);
    int8_t       (*rssi_dbm)(const port_net_t*);
    void *impl;
};

/* ports/port_telemetry.h */
typedef struct port_telemetry_s port_telemetry_t;
struct port_telemetry_s {
    hal_status_t (*connect)(port_telemetry_t*);
    hal_status_t (*publish)(port_telemetry_t*, const char *topic,
                            const uint8_t *payload, size_t len,
                            uint8_t qos, bool retain);
    hal_status_t (*subscribe)(port_telemetry_t*, const char *topic);
    bool         (*is_connected)(const port_telemetry_t*);
    hal_status_t (*disconnect)(port_telemetry_t*);
    void *impl;
};

/* ports/port_storage.h */
typedef struct port_storage_s port_storage_t;
struct port_storage_s {
    hal_status_t (*kv_get)(port_storage_t*, const char *k, void *buf, size_t *len);
    hal_status_t (*kv_set)(port_storage_t*, const char *k, const void *buf, size_t len);
    hal_status_t (*kv_erase)(port_storage_t*, const char *k);
    hal_status_t (*retained_get)(port_storage_t*, void *buf, size_t *len);  /* RTC memory */
    hal_status_t (*retained_set)(port_storage_t*, const void *buf, size_t len);
    hal_status_t (*log_append)(port_storage_t*, const void *rec, size_t len);
    void *impl;
};

/* ports/port_power.h */
typedef enum { POWER_USB, POWER_BATTERY, POWER_CHARGING } power_source_t;
typedef struct port_power_s port_power_t;
struct port_power_s {
    power_source_t (*source)(const port_power_t*);
    hal_status_t   (*battery)(const port_power_t*, uint16_t *mv, uint8_t *percent);
    void           (*deep_sleep_ms)(port_power_t*, uint32_t ms);  /* DOES NOT RETURN */
    hal_status_t   (*light_sleep_ms)(port_power_t*, uint32_t ms);
    void *impl;
};
```

`deep_sleep_ms` is marked as non-returning, which forces the logic to be written correctly: anything that must be preserved is saved before the call. The simulator behaves identically — it destroys the core object and recreates it from the retained block.

---

## 6. The core: execution model

### 6.1 A run-to-completion state machine instead of FreeRTOS tasks

```c
/* core/app/app_core.h */
typedef enum {
    EV_TICK, EV_SENSOR_READY, EV_INPUT, EV_NET_STATE, EV_TELEMETRY_STATE,
    EV_TELEMETRY_MESSAGE, EV_POWER_CHANGED, EV_CONFIG_CHANGED,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint64_t     ts_ms;
    union {
        input_event_t input;
        net_state_t   net;
        uint8_t       sensor_id;
    } data;
} app_event_t;

typedef struct {
    uint64_t next_deadline_ms;   /* when the core wants to wake up  */
    bool     allow_sleep;        /* whether deep sleep is permitted */
} app_step_result_t;

app_core_t *app_core_create(const app_deps_t *deps, const app_config_t *cfg);
void        app_core_handle(app_core_t*, const app_event_t *ev, app_step_result_t *out);
void        app_core_prepare_sleep(app_core_t*);   /* serialize state */
```

The core spawns no threads, never blocks and never sleeps. It responds to an event and states when it wants to be called again. Implementing the wait is the platform's job:

| Platform | Loop implementation |
|---|---|
| ESP32, USB power | a FreeRTOS task, `xQueueReceive(queue, timeout = next_deadline - now)` |
| ESP32, battery | `app_core_prepare_sleep()` → `esp_deep_sleep(next_deadline - now)` |
| Simulator | the SDL event loop; with `--time-scale N` the virtual clock jumps forward without real waiting |
| Tests | `while (t < end) { app_core_handle(...); t = next_deadline; }` — a full day in milliseconds |

The same business logic code runs in all four modes.

### 6.2 Dependency injection

```c
/* core/app/app_deps.h */
typedef struct {
    const port_clock_t   *clock;
    port_display_t       *display;
    port_input_t         *input;
    port_net_t           *net;
    port_telemetry_t     *telemetry;
    port_storage_t       *storage;
    port_power_t         *power;
    const port_log_t     *log;

    sensor_source_t     **sensors;
    size_t                sensor_count;
} app_deps_t;
```

Composition for the firmware:

```c
/* apps/firmware_esp32/main/main.c */
void app_main(void) {
    board_init();
    static port_i2c_t i2c;            adp_i2c_init(&i2c, &BOARD_I2C_CFG);
    static sensor_source_t s_scd41;   scd41_source_init(&s_scd41, &i2c, /*id=*/1);
    static sensor_source_t s_bme;     bme68x_source_init(&s_bme, &i2c, /*id=*/2);
    static sensor_source_t *sensors[] = { &s_scd41, &s_bme };

    app_deps_t deps = {
        .clock = adp_clock(), .display = adp_display_epdiy(),
        .input = adp_input_gt911(&i2c), .net = adp_net_wifi(),
        .telemetry = adp_telemetry_mqtt(), .storage = adp_storage_nvs(),
        .power = adp_power_bq(&i2c), .log = adp_log(),
        .sensors = sensors, .sensor_count = 2,
    };
    run_event_loop(app_core_create(&deps, load_config()));
}
```

Composition for the simulator:

```c
/* apps/simulator/main.c */
int main(int argc, char **argv) {
    sim_args_t a = sim_parse_args(argc, argv);

    static sensor_source_t s_scd41, s_bme;
    replay_source_init(&s_scd41, a.scenario_co2,  SENS_CAP_CO2, 1);
    replay_source_init(&s_bme,   a.scenario_env,
                       SENS_CAP_TEMPERATURE|SENS_CAP_HUMIDITY|
                       SENS_CAP_PRESSURE|SENS_CAP_GAS_RES, 2);
    static sensor_source_t *sensors[] = { &s_scd41, &s_bme };

    app_deps_t deps = {
        .clock = adp_clock_virtual(a.time_scale),
        .display = a.headless ? adp_display_png(a.out_dir)
                              : adp_display_sdl(960, 540, a.zoom),
        .input = adp_input_sdl(), .net = adp_net_sim(&a.net_profile),
        .telemetry = a.broker ? adp_telemetry_mqtt(a.broker)
                              : adp_telemetry_stdout(),
        .storage = adp_storage_fs(a.state_dir),
        .power = adp_power_sim(&a.battery_profile), .log = adp_log_console(),
        .sensors = sensors, .sensor_count = 2,
    };
    return sim_run(app_core_create(&deps, load_config()));
}
```

The difference between firmware and simulator lives entirely in **these two files**.

### 6.3 Sensor manager and fault tolerance

`core/app/sensor_manager.c` implements §4.3 of the requirements declaratively:

```c
static const source_priority_t PRIORITY[] = {
    { .field = FIELD_CO2,         .order = { 1, 0 } },    /* SCD41 only              */
    { .field = FIELD_TEMPERATURE, .order = { 2, 1, 0 } }, /* BME68x, SCD41 on failure */
    { .field = FIELD_HUMIDITY,    .order = { 2, 1, 0 } },
    { .field = FIELD_PRESSURE,    .order = { 2, 0 } },
    { .field = FIELD_IAQ,         .order = { 2, 0 } },
};
```

The logic: poll the sources, take for each field the first source reporting `VAL_OK`, mark the rest `VAL_ERROR`/`VAL_STALE`, and record `source_id`. The §15.1 requirement that "the device must not hang when a single sensor fails" becomes a 20-line unit test instead of a field trial involving a desoldered sensor.

---

## 7. The simulator

### 7.1 Replaying data from files

**Scenario format (CSV with a header — human-readable and editable in a spreadsheet):**

```csv
# scenarios/co2_spike_meeting.csv
# t_offset_s — offset from the start of the scenario; playback loops after the last row
t_offset_s,temperature_c,humidity_rh,pressure_hpa,co2_ppm,gas_res_ohm,status
0,22.1,44.0,1013.2,620,180000,ok
60,22.2,44.3,1013.1,680,179500,ok
120,22.6,46.8,1013.0,940,175000,ok
180,23.1,49.2,1012.9,1380,168000,ok
240,23.4,50.1,1012.8,1720,164000,ok
300,23.5,50.4,1012.8,2050,162000,ok
360,23.2,48.0,1012.9,1400,170000,ok
420,22.8,45.5,1013.0,850,176000,ok
480,22.4,44.2,1013.1,640,179000,ok
```

Playback engine rules:

- **Looping** — after the last row, time resets to the start; an eight-minute scenario runs indefinitely, portraying a "meeting then ventilation" cycle.
- **Interpolation** between rows (linear, disableable by flag) — a full day can be described in twenty rows rather than 1440.
- **The `status` column** drives faults: `ok`, `error`, `stale`, `warmup`, `nan`, `out_of_range`. A row with `co2_ppm=0,status=error` exercises the §15.1 requirement.
- **A missing column** means `VAL_UNAVAILABLE`, modelling a sensor that is not installed.
- **Alternative sources**: JSONL (for recordings from a real device), generators (`--generator sine:co2:400:2000:900s`), and **recording live data from the device** over MQTT into the same CSV format, so a real-world episode can be replayed on a PC.

Multiple files are supported: `--scenario-co2 a.csv --scenario-env b.csv`. The sources run independently, with their own intervals and their own loop points, which is closer to reality than a single combined file.

### 7.2 The simulator window

```
┌──────────────────────────────────────────────┬─────────────────────────┐
│                                              │ SENSORS                 │
│                                              │  CO₂   [====|===] 1380  │
│         Pixel-for-pixel display area         │  Temp  [===|====] 23.1  │
│                 960 × 540                    │  RH    [==|=====] 49.2  │
│           (zoom 1x / 0.5x / 2x)              │  ☐ override manual      │
│            16 levels of grey                 │                         │
│           e-paper emulation:                 │ FAULTS                  │
│            · refresh latency                 │  [x] SCD41 NACK         │
│            · flash on full refresh           │  [ ] I²C timeout        │
│            · ghosting from partial refresh   │  [ ] Wi-Fi drop         │
│                                              │  [ ] Broker down        │
│                                              │  [ ] Low battery 8%     │
│                                              │                         │
├──────────────────────────────────────────────┤ TIME                    │
│ [▶] [⏸] [⏭]  ×1 ×10 ×60 ×3600   t=04:12:30   │  virtual clock          │
├──────────────────────────────────────────────┤ POWER                   │
│ LOG                                          │  ● awake  ○ deep sleep  │
│ 04:12:28 sensor scd41 -> 1380 ppm            │  wake in: 118 s         │
│ 04:12:29 policy: Δco2=440 > 50 -> refresh    │  [Force cold boot]      │
│ 04:12:30 display: PARTIAL 0,0 960x540 (82ms) │  battery: 74 %          │
│ 04:12:30 mqtt: homeair/t5s3/state (retain)   │                         │
└──────────────────────────────────────────────┴─────────────────────────┘
```

What emulating e-paper physics in the window gives you:

- **Refresh latency** (full ≈ 300–1000 ms, partial ≈ 80–250 ms) makes it immediately obvious when a redraw strategy leaves the UI feeling sluggish.
- **Ghosting accumulation** across partial refreshes, with a forced full refresh after N of them, reproduces LilyGO's warning about artifacts and irreversible panel damage from prolonged partial refreshing.
- **A 16-level grey palette** shows at once when a design is unreadable at 4 bpp — before the enclosure is ordered.
- **Highlighting the flushed region** on each update reveals when a supposedly "partial" update is in fact redrawing the entire screen.

Touch: a mouse click becomes `INPUT_TOUCH_DOWN`/`UP` in panel coordinates, with two-point support (Shift+click) to match the GT911.

### 7.3 Emulating deep sleep

Via the `Force cold boot` button, and automatically whenever the core calls `deep_sleep_ms()`, the simulator:

1. captures the retained block,
2. destroys the core object,
3. advances the virtual clock by the sleep duration,
4. recreates the core and hands back the retained block.

This catches, on a PC, the most expensive class of battery-mode bugs: garbage on the screen after sleep, a `boot_count` that never increments, BSEC state lost so IAQ restarts from zero, thresholds compared against an empty `last_published`.

### 7.4 Headless mode for CI

`--headless --frames out/` writes frames to PNG instead of opening an SDL window. The same binary serves scenario tests and golden comparisons. SDL becomes an optional dependency (`-DSIM_GUI=OFF`), so CI does not need graphics.

---

## 8. Build system

### 8.1 A single source list

```cmake
# cmake/sources_core.cmake
set(HAC_CORE_SOURCES
    ${HAC_ROOT}/core/domain/measurement.c
    ${HAC_ROOT}/core/domain/air_quality.c
    ${HAC_ROOT}/core/app/app_core.c
    ${HAC_ROOT}/core/app/sensor_manager.c
    ${HAC_ROOT}/core/app/update_policy.c
    ${HAC_ROOT}/core/ui/framebuffer.c
    # ...
)
set(HAC_CORE_INCLUDES ${HAC_ROOT}/core ${HAC_ROOT}/ports)
```

Host build:

```cmake
# CMakeLists.txt (root)
cmake_minimum_required(VERSION 3.20)
project(home_air_monitor C CXX)
set(HAC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(cmake/sources_core.cmake)
include(cmake/warnings.cmake)

add_library(hac_core STATIC ${HAC_CORE_SOURCES})
target_include_directories(hac_core PUBLIC ${HAC_CORE_INCLUDES})
hac_apply_warnings(hac_core)      # -Wall -Wextra -Werror -Wconversion

add_subdirectory(drivers)
add_subdirectory(platform/host)
add_subdirectory(apps/simulator)
enable_testing()
add_subdirectory(tests)
```

The ESP-IDF build uses **the same file**:

```cmake
# apps/firmware_esp32/components/hac_core/CMakeLists.txt
set(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
include(${HAC_ROOT}/cmake/sources_core.cmake)
idf_component_register(SRCS ${HAC_CORE_SOURCES}
                       INCLUDE_DIRS ${HAC_CORE_INCLUDES})
```

Adding a file to the core automatically reaches both builds. This eliminates the classic dual-build failure mode where something compiles on a PC but not on the device.

### 8.2 Presets

```jsonc
// CMakePresets.json
{
  "configurePresets": [
    { "name": "host-debug",   "binaryDir": "build/host-debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "SIM_GUI": "ON" } },
    { "name": "host-asan",    "inherits": "host-debug",
      "cacheVariables": { "HAC_SANITIZERS": "address,undefined" } },
    { "name": "ci-headless",  "inherits": "host-debug",
      "cacheVariables": { "SIM_GUI": "OFF" } }
  ]
}
```

```bash
cmake --preset host-debug && cmake --build build/host-debug
./build/host-debug/apps/simulator/simulator \
    --scenario-co2 scenarios/co2_spike_meeting.csv \
    --scenario-env scenarios/normal_day.csv \
    --time-scale 60 --broker mqtt://localhost:1883
```

Firmware: `cd apps/firmware_esp32 && idf.py build flash monitor`.

---

## 9. Testing

### 9.1 Levels

| Level | Where | What it verifies | Speed |
|---|---|---|---|
| Unit | host | CO₂/IAQ thresholds, JSON and HA Discovery generation, source merging, `view_model`, config parsers | ms |
| Golden UI | host | a rendered screen compared against a reference PNG | ms |
| Contract | host + on-target | that every implementation of a port behaves identically | ms / s |
| Scenario | host | a day of operation in seconds: the sequence of refreshes, publishes and sleeps | s |
| Fuzz | host | the scenario CSV parser, MQTT commands, config read from NVS | minutes |
| On-target smoke | hardware | I²C, panel timing, current draw, Wi-Fi | minutes |

### 9.2 Contract tests — the mechanism that makes portability real

One test suite, parameterized by the implementation under test:

```c
/* tests/contract/test_storage_contract.c */
void run_storage_contract(port_storage_t *s) {
    uint8_t buf[64]; size_t len = sizeof buf;
    TEST_ASSERT_EQUAL(HAL_ERR_NOT_FOUND, s->kv_get(s, "absent", buf, &len));
    TEST_ASSERT_EQUAL(HAL_OK, s->kv_set(s, "k", "v", 2));
    len = sizeof buf;
    TEST_ASSERT_EQUAL(HAL_OK, s->kv_get(s, "k", buf, &len));
    TEST_ASSERT_EQUAL(2, len);
    /* a key longer than the NVS limit (15 chars) must behave the same everywhere */
    TEST_ASSERT_EQUAL(HAL_ERR_INVALID_ARG, s->kv_set(s, LONG_KEY_16, "v", 2));
}
```

The same file runs:
- on a PC against `adp_storage_fs`, as an ordinary CTest;
- on the device against `adp_storage_nvs`, via `idf.py -T contract test`.

This is what makes "it works in the simulator" mean "it will work on hardware". Without contract tests, abstractions inevitably diverge at the edges — NVS key length limits, behaviour on a full partition, I²C error codes.

### 9.3 Scenario tests

```c
TEST(scenario, battery_mode_wifi_budget) {
    sim_env_t env = sim_env_make(.scenario = "scenarios/normal_day.csv",
                                 .power    = POWER_BATTERY,
                                 .duration_h = 24);
    sim_run(&env);
    /* §11.2: in battery mode, publish no more than once per 60 s */
    ASSERT_LE(env.stats.publish_count, 24 * 60);
    /* §9.1: a full refresh no more than once every 10 minutes */
    ASSERT_LE(env.stats.full_refresh_count, 24 * 6);
    /* §15.1: a sensor failure does not stop the cycle */
    ASSERT_GT(env.stats.wake_count, 0);
    ASSERT_EQ(env.stats.watchdog_resets, 0);
}
```

A test like this verifies in one second what would otherwise take a day of observation on the device.

### 9.4 CI

```yaml
jobs:
  host:      cmake --preset ci-headless && ctest --output-on-failure
  asan:      cmake --preset host-asan   && ctest
  firmware:  idf.py build                 # confirms the core still compiles for the S3
  format:    clang-format --dry-run --Werror; clang-tidy; cppcheck
  static:    scripts/check_sources.sh     # SPDX header on every first-party file;
                                          # no platform includes in core/
  golden:    simulator --headless --frames out && compare-png tests/golden
```

ASan and UBSan on a PC catch classes of bug — a framebuffer overrun, a use-after-free in the event queue — that on an ESP32 surface as rare hangs and take days to track down.

---

## 10. Order of work

The difference from §16 of the requirements: a substantial amount of work happens before any hardware exists, and the UI is finished entirely on a PC.

| Phase | Content | Result |
|---|---|---|
| **0. Skeleton** | repository, CMake dual build, CI, `hal_status`, `port_clock`, domain types, first unit tests | `ctest` green; the core compiles for both host and S3 |
| **1. Simulator** | CSV replay, 960×540 SDL window, `framebuffer` + `gfx` + fonts, Home/Status/Error screens, `update_policy` | The full UI and threshold logic finished **without hardware** |
| **2. Board bring-up** | `board_config.h` for the specific revision, `adp_i2c`, `adp_display` (EPDiy/ED047TC1), `adp_clock`, `adp_log` | The same UI seen on the PC appears on the panel. The core is unchanged |
| **3. Sensors** | BME68x and SCD4x drivers over `port_i2c`, wrapped as `sensor_source_t`, plus a fake-i2c model for the PC | Real measurements; drivers covered by tests on a PC |
| **4. Telemetry** | `mqtt_payload`, `ha_discovery` (pure functions, tested), `adp_telemetry_mqtt` for both platforms | The simulator appears in Home Assistant as a device, before the hardware is ready |
| **5. Power** | `power_policy`, `persisted_state_t`, `adp_power`, cold-boot emulation in the simulator | The battery duty cycle is debugged in accelerated time, then measured on hardware |
| **6. Reliability and UX** | touch menu, error screens, NVS config, OTA, watchdog, on-device contract tests | A prototype meeting §14 |
| **7. Matter** | `platform/esp32_t5s3/adp_telemetry_matter.c` as a second reporter | The core and UI are untouched |

Note that phases 1 and 2 can proceed in parallel with two people: one builds the UI and logic on a PC, the other brings up the board. Their meeting point is the `port_display` interface.

---

## 11. Porting to a new platform

Checklist (`docs/porting_guide.md`):

1. Create `platform/<name>/` and implement the ports: `clock`, `log`, `sync`, `display`, `input`, `net`, `telemetry`, `storage`, `power`, plus either `i2c` or ready-made `sensor_source_t` instances.
2. Run `tests/contract/` against the new adapters. Until those pass, the platform is not ready.
3. Create `apps/<name>/main.c` — composition and the wait loop, nothing more.
4. Add a `board_config.h` with the pinout and panel parameters.
5. The core, drivers, UI and reporters are **not touched**.

The work for a new platform is on the order of 1500–2500 lines of adapters. By comparison, under the layout in §7.1 of the requirements, porting would mean rewriting almost everything.

Realistic reuse candidates: the ESP32-C6 (Thread/Matter), a different e-paper board (a different resolution touches only `board_config.h`), a Raspberry Pi Zero with an HDMI panel, or a headless aggregation agent collecting from several devices.

---

## 12. Architectural risks and limitations

| Risk | How it shows up | Mitigation |
|---|---|---|
| **Simulator ≠ hardware** | green on a PC, but I²C timeouts, insufficient RAM or panel timing issues on the device | Contract tests on the device; an on-target smoke suite in each phase's acceptance criteria; the rule that the simulator verifies logic, not timing and not power |
| **Leaky abstractions** | BSEC requires an opaque state blob; e-paper has panel-specific waveform modes | The port passes state as an opaque buffer; `refresh_mode_t` is the minimal common subset; panel specifics stay in the adapter, which reports only numeric constraints to the core |
| **Over-abstraction** | a port for everything, forty vtables, nobody can follow the control flow | The rule: a port is created when there are **two** real implementations (hardware plus simulator). Otherwise it is just a function |
| **BSEC unavailable for the host architecture** | IAQ cannot be computed on a PC | `iaq_engine` is an interface; on the host, use a simplified implementation or replay IAQ directly from the CSV. §17.2 of the requirements already suggests showing raw gas resistance in the MVP |
| **vtable overhead** | an indirect call instead of a direct one | Negligible at a measurement rate of 0.03–0.2 Hz. In hot paths (`gfx` primitives) the calls are direct; the port is only crossed at `flush` |
| **Drift between the two builds** | a file added to only one of them | The shared `sources_core.cmake`; the firmware build runs in CI on every PR |

---

## 13. Summary

The original requirements already contain the right central idea — decoupling through a normalized measurement structure. The architecture proposed here carries it through to its conclusion: **ESP-IDF, EPDiy, NVS, esp-mqtt and the sensors themselves become replaceable parts rather than the frame of the project.**

Three decisions determine everything else:

1. **The clock, display, sensors, network, storage and power are ports.** The core does not know where it is running.
2. **The core is a single-threaded state machine of the form "event → actions plus next wake time".** Waiting is implemented by the platform: deep sleep on the device, accelerated virtual time in the simulator, instantaneous jumps in tests.
3. **There are two substitution points for sensor data** — `sensor_source_t` (scenario files, for exercising business logic) and `port_i2c_t` (a register-level model, for exercising the drivers themselves).

The practical effect: the UI, e-paper refresh thresholds, fault tolerance, the battery duty cycle and the Home Assistant integration are all developed and debugged on a PC, leaving for the hardware only what genuinely requires hardware — I²C timing, panel waveforms and current measurement.
