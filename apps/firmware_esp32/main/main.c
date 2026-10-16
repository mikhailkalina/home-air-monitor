// SPDX-License-Identifier: Apache-2.0
//
// Composition root for the ESP32-S3 firmware (docs/architecture.md 6.2 wires
// adapters here). Phase 2a brought the board up with no panel driven and no
// sensors attached; phase 2b (docs/adr/0007-epaper-backend.md) adds the real
// display -- EPDiy driving the ED047TC1 through its row-by-row i80 engine,
// behind platform/esp32_t5s3/adp_display.c. Phase 2d
// (docs/adr/0011-i2c-bus-adapter.md) brings up the shared I2C bus behind
// platform/esp32_t5s3/adp_i2c.c, scans it once at boot and offers the
// i2c_scan / i2c_recover console commands. Phase 3c
// (docs/adr/0017-sensor-manager.md) runs both sensors -- the SCD4x and the
// BME68x drivers over that bus -- through core/app/sensor_manager.c, with
// the ids and priorities below, the same as the simulator's.
//
// What runs, in this order: adp_log and adp_clock, the port_clock and
// port_log contract suites run once against the real adapters
// (docs/adr/0005), the real display adapter (or the null one,
// CONFIG_HAC_DISPLAY_NULL -- see main/Kconfig.projbuild), adp_i2c and the
// boot-time bus scan -- after the display, whose init powers the sensors (see
// bring_up_i2c()) -- optionally the whole-panel bring-up diagnostic
// (CONFIG_HAC_EPD_TEST_PATTERN, which costs the panel's minimum
// full-refresh interval before the first rendered frame -- see
// run_epd_test_pattern below), and the FreeRTOS pump from event_loop.c
// driving the sensor manager and update_policy, and last the serial console
// (see start_console() for why last).
//
// CONFIG_HAC_EPD_CALIBRATION (phase 2c, off by default) replaces the pump
// with epd_calibration.c's measurement run: the same adapters, no event loop
// and no sensors.

#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

#include "adp_clock.h"
#include "adp_display.h"
#include "adp_display_null.h"
#include "adp_i2c.h"
#include "adp_log.h"
#include "board_config.h"
#include "board_power.h"

#include "app/sensor_manager.h"
#include "app/update_policy.h"
#include "bme68x_source.h"
#include "domain/measurement.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_log.h"
#include "port_sensor.h"
#include "scd4x_source.h"

#include "port_clock_contract.h"
#include "port_log_contract.h"

#include "console.h"
#include "epd_calibration.h"
#include "event_loop.h"

// Phase 2a's figure was 6144: ~2 KB IDF/vsnprintf baseline, ~400 B
// view_model_t, ~400 B through the render call chain, doubled. Phase 3c puts
// both drivers on this task -- Bosch's and Sensirion's calls, the I2C
// adapter under them -- and logs floats (newlib's float formatting is the
// deepest thing on this stack), so 8 KB until the board says otherwise:
// event_loop.c logs the task's free-stack low-water mark at each new low, and
// this number is revisited from that line rather than kept out of
// superstition.
#define EVENT_LOOP_TASK_STACK_BYTES 8192u

// Off the protocol core (0), where ESP-IDF runs Wi-Fi when phase 4 brings it.
#define EVENT_LOOP_TASK_PRIORITY 5
#define EVENT_LOOP_TASK_CORE 1

// The sources' ids, which every measurement carries as its source_id and the
// priorities below are written in -- the same two the simulator gives the
// same two sensors (apps/simulator/main.c).
#define SCD4X_SOURCE_ID 1u
#define BME68X_SOURCE_ID 2u

// CHOICE: how often each source's latest sample is logged, besides what the
// sensor manager logs on every change of state -- often enough to compare
// the BME68x's temperature with the SCD41's and a reference thermometer
// (docs/requirements.md 13.2), seldom enough not to bury the rest.
#define SAMPLE_LOG_INTERVAL_MS 60000u

typedef struct {
    const port_log_t *log;
    const char *tag;
} contract_ctx_t;

static void contract_report(void *ctx_v, const char *check, bool ok)
{
    const contract_ctx_t *ctx = (const contract_ctx_t *)ctx_v;
    const log_field_t fields[] = {log_bool("ok", ok)};
    port_log_write(ctx->log, ok ? LOG_LEVEL_DEBUG : LOG_LEVEL_ERROR, ctx->tag, check, fields, 1u);
}

#if CONFIG_HAC_EPD_TEST_PATTERN
// Bring-up diagnostic (phase 2b/2c, CONFIG_HAC_EPD_TEST_PATTERN -- see
// main/Kconfig.projbuild), run once at startup before the event loop: fill the
// framebuffer solid black and flush it as a full refresh. On this backend that
// is epd_clear() -- the panel visibly cycles to white -- followed by a
// MODE_GC16 redraw of the buffer, so the panel should settle on solid black. That isolates "does
// the panel respond to a drive sequence at all" from "is screen_home's rendering correct": a panel
// that stays blank through this is a driver or wiring problem, while one that
// goes black has been driven correctly and any later wrong image is a
// rendering problem instead. Off by default since phase 2c's calibration run
// drove this panel through every refresh mode; kept for bringing up another
// unit.
//
// Deliberately REFRESH_FULL rather than REFRESH_CLEAR, which is what this
// used while FastEPD was the backend. Under EPDiy, REFRESH_CLEAR is
// epd_clear() alone: a dedicated multi-cycle drive to white that ignores the
// framebuffer by design, which would make a black fill meaningless. Going
// through the ordinary full-refresh path also means this probe exercises the
// same mapping every later frame uses, rather than a path of its own.
//
// ONE whole-panel drive, and then a wait.
// port_display_t::min_full_refresh_interval_ms is what this adapter reports
// to the core as the shortest time the panel tolerates between whole-panel
// refreshes, and core/app/update_policy.c defers every later refresh to
// honour it -- so a boot-time diagnostic that issued two of them two seconds
// apart, and then handed over to a policy that immediately issued a third,
// would be the one place in the firmware contradicting a limit the rest of
// the code treats as the panel's own. The light direction is proved by the
// event loop's own first frame: screen_home renders on a white ground.
static void run_epd_test_pattern(port_display_t *display, const port_clock_t *clock,
                                 const port_log_t *log)
{
    framebuffer_t *fb = display->get_framebuffer(display);
    if (fb == NULL) {
        port_log_write(log, LOG_LEVEL_ERROR, "main", "test_pattern: get_framebuffer returned NULL",
                       NULL, 0u);
        return;
    }

    framebuffer_clear(fb, 0x00u);  // VM_INK_PRIMARY -- black
    const hal_status_t status = display->flush(display, NULL, REFRESH_FULL);
    const log_field_t fields[] = {log_i64("status", (int64_t)status)};
    port_log_write(log, LOG_LEVEL_INFO, "main",
                   "test_pattern: solid black flushed (REFRESH_FULL, whole panel)", fields, 1u);

    // update_policy_init() starts with have_full_refresh = false, so the
    // policy's first frame is due immediately and it cannot space itself
    // from a refresh it never saw. Waiting here is what keeps that first
    // frame outside the interval this diagnostic just consumed. The delay is
    // the declared constant, not a shorter convenience value: the point is
    // to honour the limit, not to approximate it.
    const log_field_t wait_fields[] = {
        log_u64("wait_ms", BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main",
                   "test_pattern: waiting out the panel's minimum full-refresh interval before "
                   "the first rendered frame",
                   wait_fields, sizeof(wait_fields) / sizeof(wait_fields[0]));
    clock->delay_ms(clock, BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS);
}
#endif  // CONFIG_HAC_EPD_TEST_PATTERN

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external_pin";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "other_watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "POWER_GLITCH";
        case ESP_RST_CPU_LOCKUP:
            return "cpu_lockup";
        default:
            return "unknown";
    }
}

// Why the chip last reset, in the app's own log. The ROM prints the same as
// its first line ("rst:0x.. (...)"), but over USB-Serial-JTAG that line is
// usually lost: a chip reset drops the USB device, and the terminal is not
// listening again until the ROM is long done -- every boot of the phase-3a
// reboot loop (2026-10-15) arrived without it. `rom_code` is the ROM's own
// number, as in that line: 1 power-on, 12 esp_restart(), 15 brownout, 21 a
// reset from the USB host (DTR/RTS), 23 power glitch.
static void log_reset_reason(const port_log_t *log)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    const log_field_t fields[] = {
        log_str("reason", reset_reason_name(reason)),
        log_i64("rom_code", (int64_t)esp_rom_get_reset_reason(0)),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main", "previous reset", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// That memory budget is what every phase from 2b onward has to fit inside --
// see the phase-2a plan's "Deliverable" section.
static void log_memory_budget(const port_log_t *log)
{
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    const log_field_t fields[] = {
        log_u64("free_heap_bytes", free_heap),
        log_u64("free_internal_bytes", free_internal),
        log_u64("free_psram_bytes", free_psram),
        log_u64("largest_free_psram_block_bytes", largest_psram_block),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main", "memory budget at startup", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// Runs port_clock_contract_run()/port_log_contract_run() against the real
// adapters this firmware actually uses, at boot, because there is no
// idf.py -T test-app scaffold yet (docs/adr/0005-contract-suites-run-at-boot.md).
// The bodies are the exact object code tests/contract/test_port_clock_contract.c
// and test_port_log_contract.c exercise on the host: same source, two
// runners, via cmake/sources_contract.cmake.
static void run_contract_suites(const port_clock_t *clock, const port_log_t *log)
{
    contract_ctx_t clock_ctx = {.log = log, .tag = "contract.port_clock"};
    const unsigned clock_failures = port_clock_contract_run(clock, contract_report, &clock_ctx);

    contract_ctx_t log_ctx = {.log = log, .tag = "contract.port_log"};
    const unsigned log_failures = port_log_contract_run(log, contract_report, &log_ctx);

    const log_field_t tally_fields[] = {
        log_u64("port_clock_failures", clock_failures),
        log_u64("port_log_failures", log_failures),
    };
    const log_level_t level =
        (clock_failures == 0u && log_failures == 0u) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    port_log_write(log, level, "contract", "contract suites complete", tally_fields,
                   sizeof(tally_fields) / sizeof(tally_fields[0]));
}

// Phase 2d's deliverable: the bus comes up and the boot log shows what
// answers on it beside what board_config.h expects. A bus that fails to come
// up is logged and the rest of the firmware runs regardless: the sensor
// manager then finds both sensors failing and keeps retrying them at its
// backoff, and the console's i2c_recover is the way to retry the bus.
//
// Called after the switched 3V3 is up, never before: the sensors run from
// it, and until it rises they sit on the bus unpowered and hold both lines
// low -- the RTC and the touch controller lose the bus with them, and the
// scan stops at its first address. Seen on the board 2026-10-15
// (docs/hardware/board_notes.md, open item "A reboot loop when the switched
// 3V3 comes up"). In the normal build EPDiy's init raises the rail (PWR_EN);
// a CONFIG_HAC_DISPLAY_NULL build raises it itself, through
// board_power_switched_3v3_on(). Either way BOARD_SWITCHED_3V3_SETTLE_MS
// passes first, and each driver's init() waits its own sensor's power-up time
// again before its first transfer: 30 ms for the SCD4x (its datasheet,
// Table 7), 2 ms for the BME68x (Table 1).
//
// The port_i2c contract suite is deliberately NOT run here, unlike port_clock
// and port_log above: it hangs and stalls the shared bus on purpose, which
// has no place in an ordinary boot. It runs in the test app (ADR 0011).
static void bring_up_i2c(adp_i2c_t *i2c, const port_clock_t *clock, const port_log_t *log)
{
    clock->delay_ms(clock, BOARD_SWITCHED_3V3_SETTLE_MS);
    const hal_status_t status = adp_i2c_init(i2c, log);
    if (status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)status)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "adp_i2c_init failed", fields, 1u);
    }
    console_print_i2c_scan(i2c);
}

// The console is started last, once the event loop is running. Creating the
// REPL runs linenoise's terminal probe in the CALLER's task
// (esp_console_setup_prompt(), components/console/esp_console_common.c): it
// asks the terminal for a status report and polls for the answer 50 times
// with usleep(10 ms), which at this firmware's 10 ms tick sleeps ~20 ms a
// time. Against a terminal that never answers -- YAT, which this project's
// logs come from -- that is a second of app_main blocked. Measured
// 2026-10-13 when it still ran before the display: the first frame came a
// second late (docs/adr/0011-i2c-bus-adapter.md). Here it blocks nothing
// else; the event loop is already drawing on its own task.
//
// The early returns above -- a display that fails to initialise, and the
// calibration mode -- leave the console off; their logs still carry the boot
// scan.
static void start_console(adp_i2c_t *i2c, const port_log_t *log)
{
    (void)console_start(i2c, log);
}

// The board's two sensors, set up on the bus and handed to the sensor
// manager with the priorities of docs/architecture.md 6.3: temperature and
// humidity from the BME68x, the SCD41 as fallback; CO2 from the SCD41 alone;
// pressure and gas from the BME68x alone. Nothing touches the bus here: the
// event loop's first poll initialises both, on its own task.
static void compose_sensors(sensor_manager_t *mgr, scd4x_source_t *scd4x, bme68x_source_t *bme68x,
                            adp_i2c_t *i2c, const port_clock_t *clock, const port_log_t *log)
{
    const scd4x_source_config_t scd4x_cfg = {
        .bus = adp_i2c_port(i2c),
        .clock = clock,
        .log = log,
        .address = BOARD_SCD4X_I2C_ADDRESS,
        .id = SCD4X_SOURCE_ID,
        .mode = SCD4X_MODE_PERIODIC,  // USB power (requirements 11.2); single shot is phase 5's
    };
    const bme68x_source_config_t bme68x_cfg = {
        .bus = adp_i2c_port(i2c),
        .clock = clock,
        .log = log,
        .address = BOARD_BME68X_I2C_ADDRESS,
        .id = BME68X_SOURCE_ID,
    };
    (void)scd4x_source_setup(scd4x, &scd4x_cfg);
    (void)bme68x_source_setup(bme68x, &bme68x_cfg);

    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(mgr, clock, log, &cfg);
    (void)sensor_manager_add(mgr, &scd4x->port, SENSOR_MANAGER_DEFAULT_INTERVAL_MS);
    (void)sensor_manager_add(mgr, &bme68x->port, SENSOR_MANAGER_DEFAULT_INTERVAL_MS);

    static const uint8_t prefer_bme68x[] = {BME68X_SOURCE_ID, SCD4X_SOURCE_ID};
    static const uint8_t only_bme68x[] = {BME68X_SOURCE_ID};
    static const uint8_t only_scd4x[] = {SCD4X_SOURCE_ID};
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_TEMPERATURE, prefer_bme68x, 2u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_HUMIDITY, prefer_bme68x, 2u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_PRESSURE, only_bme68x, 1u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_CO2, only_scd4x, 1u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_GAS_RESISTANCE, only_bme68x, 1u);
}

static const char *status_text(value_status_t status)
{
    switch (status) {
        case VAL_OK:
            return "ok";
        case VAL_WARMUP:
            return "warmup";
        case VAL_STALE:
            return "stale";
        case VAL_ERROR:
            return "error";
        case VAL_UNAVAILABLE:
        default:
            return "n/a";
    }
}

// A value if it is VAL_OK, else its status, under one key.
static log_field_t value_field(const char *key, const measurement_f32_t *m)
{
    return (m->status == VAL_OK) ? log_f32(key, m->value) : log_str(key, status_text(m->status));
}

// One line per source: its own latest sample, whichever field the merge took
// from it -- so the SCD41's temperature shows beside the BME68x's even while
// the BME68x's is the one on the screen.
static void log_samples(const sensor_manager_t *sensors, const port_log_t *log)
{
    for (size_t i = 0u; i < SENSOR_MANAGER_MAX_SOURCES; ++i) {
        const sensor_manager_slot_t *slot = sensor_manager_slot(sensors, i);
        if (slot == NULL) {
            return;
        }
        const sensor_caps_t caps = slot->source->caps;
        const sensor_sample_t *s = &slot->sample;
        log_field_t fields[LOG_FIELDS_MAX];
        size_t n = 0u;
        fields[n++] = log_str("state", sensor_manager_slot_state_name(slot->state));
        if ((caps & SENS_CAP_CO2) != 0u) {
            fields[n++] = value_field("co2_ppm", &s->co2_ppm);
        }
        if ((caps & SENS_CAP_TEMPERATURE) != 0u) {
            fields[n++] = value_field("t_c", &s->temperature_c);
        }
        if ((caps & SENS_CAP_HUMIDITY) != 0u) {
            fields[n++] = value_field("rh", &s->humidity_rh);
        }
        if ((caps & SENS_CAP_PRESSURE) != 0u) {
            fields[n++] = value_field("p_hpa", &s->pressure_hpa);
        }
        if ((caps & SENS_CAP_GAS_RES) != 0u && n < LOG_FIELDS_MAX) {
            fields[n++] = value_field("gas_ohm", &s->gas_resistance_ohm);
        }
        port_log_write(log, LOG_LEVEL_INFO, slot->source->name, "sample", fields, n);
    }
}

typedef struct {
    const port_log_t *log;
#if !CONFIG_HAC_DISPLAY_NULL
    adp_display_t *display;
#endif
    uint64_t next_sample_log_ms;
} reading_hook_ctx_t;

// event_loop.h's hook, after every poll: the measured temperature to the
// display adapter for EPDiy's waveform selection -- through the adapter's own
// function, not port_display (adp_display.h) -- and, once a minute, every
// source's sample to the log.
static void on_reading(void *ctx_v, const sensor_manager_t *sensors, const air_reading_t *reading,
                       uint64_t now_ms)
{
    reading_hook_ctx_t *ctx = (reading_hook_ctx_t *)ctx_v;
#if !CONFIG_HAC_DISPLAY_NULL
    adp_display_set_temperature(ctx->display, reading->temperature_c.status == VAL_OK,
                                reading->temperature_c.value);
#else
    (void)reading;
#endif
    if (now_ms >= ctx->next_sample_log_ms) {
        log_samples(sensors, ctx->log);
        ctx->next_sample_log_ms = now_ms + SAMPLE_LOG_INTERVAL_MS;
    }
}

void app_main(void)
{
    static adp_log_t s_log;
    static adp_clock_t s_clock;
#if CONFIG_HAC_DISPLAY_NULL
    static adp_display_null_t s_display;
#else
    static adp_display_t s_display;
#endif
    static adp_i2c_t s_i2c;
    static update_policy_t s_policy;
    static scd4x_source_t s_scd4x;
    static bme68x_source_t s_bme68x;
    static sensor_manager_t s_sensors;
    static reading_hook_ctx_t s_hook;

    adp_log_init(&s_log);
    const port_log_t *log = adp_log_port(&s_log);

    adp_clock_init(&s_clock, log);
    const port_clock_t *clock = adp_clock_port(&s_clock);

    port_log_write(log, LOG_LEVEL_INFO, "main",
#if CONFIG_HAC_DISPLAY_NULL
                   "phase 3c: null display (CONFIG_HAC_DISPLAY_NULL), PWR_EN raised without "
                   "EPDiy, SCD4x and BME68x through the sensor manager",
#elif CONFIG_HAC_EPD_CALIBRATION
                   "phase 2c: panel calibration mode (CONFIG_HAC_EPD_CALIBRATION), no event loop, "
                   "no sensors",
#else
                   "phase 3c: EPDiy driving the panel, SCD4x and BME68x through the sensor manager",
#endif
                   NULL, 0u);

    log_reset_reason(log);
    log_memory_budget(log);
    run_contract_suites(clock, log);

#if CONFIG_HAC_DISPLAY_NULL
    const hal_status_t display_status =
        adp_display_null_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    board_power_switched_3v3_on(log);  // what EPDiy's init does for the sensors otherwise
    bring_up_i2c(&s_i2c, clock, log);  // after the rail: see bring_up_i2c()
    if (display_status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)display_status)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "adp_display_null_init failed", fields, 1u);
        return;  // nothing else can run without a framebuffer
    }
    port_display_t *display = adp_display_null_port(&s_display);
#else
    const hal_status_t display_status =
        adp_display_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    bring_up_i2c(&s_i2c, clock, log);  // after the display: see bring_up_i2c()
    if (display_status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)display_status)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "adp_display_init failed", fields, 1u);
        return;  // nothing else can run without a framebuffer
    }
    port_display_t *display = adp_display_port(&s_display);
#if CONFIG_HAC_EPD_TEST_PATTERN
    run_epd_test_pattern(display, clock, log);
#endif
#if CONFIG_HAC_EPD_CALIBRATION
    const epd_calibration_config_t calibration = {
        .iterations = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_ITERATIONS,
#if CONFIG_HAC_EPD_CALIBRATION_GHOST_SHIFT
        .ghost_pattern = EPD_CAL_GHOST_SHIFT,
#else
        .ghost_pattern = EPD_CAL_GHOST_SAWTOOTH,
#endif
        .ghost_partials = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_GHOST_PARTIALS,
        .ghost_pause_ms = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_GHOST_PAUSE_MS,
        .residue_normal_fulls = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_RESIDUE_NORMAL_FULLS,
        .residue_deep_events = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_RESIDUE_DEEP_EVENTS,
        .residue_extra_clears = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_RESIDUE_EXTRA_CLEARS,
    };
    epd_calibration_run(display, clock, log, &calibration);
    return;  // the calibration run replaces the event loop; the panel keeps its last image
#endif
#endif

    const update_policy_config_t cfg = update_policy_config_default();
    const update_policy_limits_t limits = update_policy_limits_from_display(display);
    update_policy_init(&s_policy, clock, &cfg, &limits);

    const log_field_t panel_fields[] = {
        log_u64("width", display->width),
        log_u64("height", display->height),
        log_u64("min_full_refresh_interval_ms", limits.min_full_refresh_interval_ms),
        log_u64("max_partial_refreshes_before_full", limits.max_partial_refreshes_before_full),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main", "panel and policy limits", panel_fields,
                   sizeof(panel_fields) / sizeof(panel_fields[0]));

    compose_sensors(&s_sensors, &s_scd4x, &s_bme68x, &s_i2c, clock, log);
    s_hook.log = log;
#if !CONFIG_HAC_DISPLAY_NULL
    s_hook.display = &s_display;
#endif
    s_hook.next_sample_log_ms = 0u;

    event_loop_deps_t deps = {
        .clock = clock,
        .display = display,
        .log = log,
        .policy = &s_policy,
        .sensors = &s_sensors,
        .on_reading = on_reading,
        .on_reading_ctx = &s_hook,
    };
    event_loop_start(&deps, EVENT_LOOP_TASK_STACK_BYTES, EVENT_LOOP_TASK_PRIORITY,
                     EVENT_LOOP_TASK_CORE);
    start_console(&s_i2c, log);
}
