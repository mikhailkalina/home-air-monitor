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
// the ids and priorities below, the same as the simulator's. Phase 3d
// (docs/adr/0020-bsec-iaq.md): with CONFIG_HAC_BSEC the BME68x is measured as
// Bosch's BSEC2 asks instead, and IAQ joins its readings; without it, as
// before, the IAQ cell shows raw gas resistance. Phase 3e
// (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md): the PCF8563
// on the same bus gives the system time at boot, the console's time and
// time_set read and set it, and the footer shows, in Vilnius time, when the
// values on the screen were measured. Phase 3f
// (docs/adr/0024-sd-card-recordings-and-debug-log.md): each source's samples
// and the debug log go to the microSD card, written by a task of their own
// (card_writer.c) -- the event loop only posts what changed -- and the
// console's sd, sd_mount, sd_eject and rec manage it. No card is a normal
// state: recording is simply off. Phase 3g
// (docs/adr/0025-touch-input-gt911.md): the GT911 behind port_input
// (adp_input.c, its interrupt and a task of its own), read into
// core/app/screen_controller.c by the event loop: a tap switches between the
// home screen and a diagnostics screen; the console's touch, touch_sleep and
// touch_wake look at it and put it to sleep. Phase 4e
// (docs/adr/0032-wifi-on-the-board.md): the Wi-Fi station behind port_net
// (adp_net.c), for the network sdkconfig.credentials names -- none, and the
// firmware runs as before. Its driver is set up with the rest; its radio
// starts once the first frame is on the glass (start_radio()), and the
// console's wifi shows it; phase 4f's time and 4g's MQTT use the link. Phase 4f
// (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md, "Amendment
// (phase 4f): time from the network"): once Wi-Fi first has an address,
// SNTP sets the system time, and after each answer the event loop measures
// the RTC by its alarm's tick on INT -- stamped in an interrupt on IO9 -- and
// writes it to a second's start (net_time.c, rtc_sync.c): the drift, logged
// every hour. The console's time shows it, and rtc_stop leaves the RTC with
// no time for a boot to find. Phase 4g (docs/adr/0033-mqtt-on-the-board.md):
// with a broker in sdkconfig.credentials as well, esp-mqtt behind
// port_telemetry (adp_mqtt.c, a task of its own that alone calls esp-mqtt)
// and core/reporting/reporter.c on the event loop: the device announces
// itself to Home Assistant, publishes what the screen shows, and the status
// bar and the diagnostics screen show the link. The console's mqtt shows it.
// The card as a USB disk (docs/adr/0036-the-card-as-a-usb-disk.md): the
// console's usb_disk, or the button held two seconds, lends the card to the
// PC -- the USB PHY taken from USB-Serial-JTAG for TinyUSB's mass storage,
// run by the card's task -- until the disk is ejected; then the console
// comes back and recording resumes. Firmware from the card
// (docs/adr/0037-firmware-from-the-card.md): home_air_monitor.bin in its root
// is installed into the other OTA slot at the next mount, deleted, and booted;
// the new image confirms itself at its first frame, or the bootloader rolls it
// back.
//
// What runs, in this order: the log's tee into a ring for the card
// (log_tee.c, first, so the boot's own lines reach the file), adp_log and
// adp_clock, the port_clock and port_log contract suites run once against the
// real adapters (docs/adr/0005), the real display adapter (or the null one,
// CONFIG_HAC_DISPLAY_NULL -- see main/Kconfig.projbuild), adp_i2c and the
// boot-time bus scan -- after the display, whose init powers the sensors (see
// bring_up_i2c()) -- then the system time from the RTC (set_clock_from_rtc()),
// then the touch controller and its task (start_touch()), the Wi-Fi
// station with its radio off (start_network())
// and the wait for its first address, the RTC's INT interrupting
// (start_net_time()), the MQTT client's task and the reporter
// (start_telemetry()), which connect once the radio's link is up,
// optionally the whole-panel bring-up
// diagnostic
// (CONFIG_HAC_EPD_TEST_PATTERN, which costs the panel's minimum
// full-refresh interval before the first rendered frame -- see
// run_epd_test_pattern below), and the FreeRTOS pump from event_loop.c
// driving the sensor manager and update_policy, the card's SPI bus and
// task (start_card(), after the pump so a boot update can report progress),
// and last the serial console
// (see start_console() for why last).
//
// CONFIG_HAC_EPD_CALIBRATION (phase 2c, off by default) replaces the pump
// with epd_calibration.c's measurement run: the same adapters, no event loop
// and no sensors.

#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"  // CONFIG_HAC_*: which display and which BME68x source

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

#include "adp_clock.h"
#include "adp_display.h"
#include "adp_display_null.h"
#include "adp_i2c.h"
#include "adp_input.h"
#include "adp_log.h"
#include "adp_mqtt.h"
#include "adp_net.h"
#include "adp_sd.h"
#include "board_button.h"
#include "board_config.h"
#include "board_gpio_isr.h"
#include "board_power.h"

#include "app/link_hold.h"
#include "app/screen_controller.h"
#include "app/sensor_manager.h"
#include "app/update_policy.h"
#include "domain/civil_time.h"
#include "recording/log_file.h"
#include "recording/recorder.h"
#include "reporting/ha_discovery.h"
#include "reporting/reporter.h"
#include "reporting/topics.h"
#if CONFIG_HAC_BSEC
#include "bme68x_bsec_source.h"
#else
#include "bme68x_source.h"
#endif
#include "domain/measurement.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_log.h"
#include "port_net.h"
#include "port_sensor.h"
#include "scd4x_source.h"

#include "port_clock_contract.h"
#include "port_log_contract.h"

#include "card_writer.h"
#include "console.h"
#include "epd_calibration.h"
#include "event_loop.h"
#include "fw_update.h"
#include "log_tee.h"
#include "net_time.h"
#include "wall_clock.h"

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

// The card's task (card_writer.c): FATFS and the SD driver under the
// recorder's and the log's formatting. 6 KB until its own low-water mark,
// which it logs, says otherwise. Below the event loop, on the other core:
// whatever the card makes it wait, the screen and the sensors do not.
#define CARD_WRITER_TASK_STACK_BYTES 6144u
#define CARD_WRITER_TASK_PRIORITY 3
#define CARD_WRITER_TASK_CORE 0

// The touch task (adp_input.c): the GT911 driver under adp_i2c, its log
// lines -- and, when a transfer fails, ESP-IDF's I2C driver's three error
// lines from inside the call, the deepest thing anything on this task does.
// 4 KB had 1,408 bytes left at its lowest from its own log lines alone
// (2026-10-17), less than such a failure needs: app_main's 3.5 KB overflowed
// on exactly that (2026-10-20, docs/hardware/board_notes.md). 6 KB until its
// low-water mark, which it logs, says otherwise. Above the event loop and the
// card, so that a finger is read -- and stamped -- when it lands, whatever
// they are doing; on core 0 beside the card. EPDiy's render tasks outrank
// everything, on both cores, while a refresh drives the glass: a finger
// during a full refresh is read between its frames.
#define TOUCH_TASK_STACK_BYTES 6144u
#define TOUCH_TASK_PRIORITY 6
#define TOUCH_TASK_CORE 0

// The Wi-Fi station's task (adp_net.c): it starts the radio, connects, backs
// off and logs, with ESP-IDF's driver calls under its log lines. 6 KB, as the
// touch task's, until its own low-water mark, which it logs, says otherwise.
// On the protocol core, beside ESP-IDF's own Wi-Fi task (priority 23) and
// lwIP's (18, sdkconfig.defaults), below both: it only tells the driver what
// to do. Above the card, below touch.
#define NET_TASK_STACK_BYTES 6144u
#define NET_TASK_PRIORITY 4
#define NET_TASK_CORE 0

// The MQTT client's task (adp_mqtt.c), the only caller of esp-mqtt: it makes,
// starts, stops and destroys a client per session and hands it what the
// reporter published, with its log lines and esp-mqtt's under them. 6 KB, as
// the station's, until its own low-water mark, which it logs, says
// otherwise. On the protocol core beside esp-mqtt's own task (5, its
// default, adp_mqtt_limits.h), below it and level with the station's: it
// waits for esp-mqtt, never the other way round.
#define MQTT_TASK_STACK_BYTES 6144u
#define MQTT_TASK_PRIORITY 4
#define MQTT_TASK_CORE 0

// The sources' ids, which every measurement carries as its source_id and the
// priorities below are written in -- the same two the simulator gives the
// same two sensors (apps/simulator/main.c).
#define SCD4X_SOURCE_ID 1u
#define BME68X_SOURCE_ID 2u

// The tags the two sources' recordings carry in their names
// (core/recording/record_files.h): data/<day>_scd4x.csv,
// data/<day>_bme68x.csv. The simulator's --recording reads a file's source
// from them (apps/simulator/main.c), so they and the ids above go together.
#define SCD4X_RECORDING_TAG "scd4x"
#define BME68X_RECORDING_TAG "bme68x"

// The BME68x source this image measures with: the one Bosch's BSEC2 drives,
// in a build with the library (components/hac_drivers/Kconfig), or the
// forced-mode one with a heater profile of its own. Never both: they would
// fight over the sensor. BSEC sets its own pace -- its low-power period,
// which the sensor manager is told -- where the forced-mode source keeps the
// SCD4x's.
#if CONFIG_HAC_BSEC
typedef bme68x_bsec_source_t bme68x_composed_t;
typedef bme68x_bsec_source_config_t bme68x_composed_config_t;
#define BME68X_INTERVAL_MS HAC_BSEC_PERIOD_MS
#define SENSORS_TEXT "SCD4x and BME68x (BSEC2 IAQ) through the sensor manager"
// BSEC rates its IAQ (docs/adr/0021): the recording keeps the verdict.
#define BME68X_ACCURACY_CAPS SENS_CAP_IAQ
#else
typedef bme68x_source_t bme68x_composed_t;
typedef bme68x_source_config_t bme68x_composed_config_t;
#define BME68X_INTERVAL_MS SENSOR_MANAGER_DEFAULT_INTERVAL_MS
#define SENSORS_TEXT "SCD4x and BME68x through the sensor manager"
#define BME68X_ACCURACY_CAPS 0
#endif

// CHOICE: the local time the footer and the console show is Vilnius's -- EET,
// UTC+2, with the EU's summer time (core/domain/civil_time.h,
// time_zone_eu()). The simulator shows the same (apps/simulator/main.c).
#define LOCAL_ZONE_NAME "Vilnius"
#define LOCAL_STANDARD_OFFSET_MIN 120

// CHOICES: what Home Assistant calls the board, and what it says about it
// (core/reporting/ha_discovery.h). The project built it, on LILYGO's board;
// the simulator is "Home Air Monitor (simulator)" (apps/simulator/main.c),
// so the two appear side by side. The image's version is its app
// description's, git describe at build time.
#define HA_DEVICE_NAME "Home Air Monitor"
#define HA_MANUFACTURER "home-air-monitor"
#define HA_MODEL "LILYGO T5 4.7\" e-Paper S3"
#define HA_ORIGIN_NAME "home-air-monitor"
#define HA_SUPPORT_URL "https://github.com/mikhailkalina/home-air-monitor"

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
// is the vendor's clear -- the panel visibly cycles to white -- followed by a
// redraw of the buffer from white, so the panel should settle on solid black. That isolates "does
// the panel respond to a drive sequence at all" from "is screen_home's rendering correct": a panel
// that stays blank through this is a driver or wiring problem, while one that
// goes black has been driven correctly and any later wrong image is a
// rendering problem instead. Off by default since phase 2c's calibration run
// drove this panel through every refresh mode; kept for bringing up another
// unit.
//
// Deliberately REFRESH_FULL rather than REFRESH_CLEAR, which is what this
// used while FastEPD was the backend. Under EPDiy, REFRESH_CLEAR is
// the clear alone: a dedicated multi-cycle drive to white that ignores the
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

// The system time from the PCF8563, once the bus is up: a time the chip says
// is valid, and no earlier than ADP_CLOCK_WALL_EARLIEST_MS, sets it; VL set,
// a stopped or undecodable clock, an implausible year or a chip that does not
// answer leaves it as it was -- 0 after a chip reset, whatever a previous
// boot set after a software reset (adp_clock.h), which is then what the
// screen shows. The RTC holds UTC. Logged either way, with the chip's own
// time, so a boot log says what the clock was set from.
static wall_clock_source_t set_clock_from_rtc(adp_i2c_t *i2c, adp_clock_t *clock,
                                              const port_log_t *log)
{
    const bool kept = clock->port.wall_ms(&clock->port) != 0u;
    wall_clock_rtc_t rtc;
    wall_clock_read_rtc(adp_i2c_port(i2c), BOARD_RTC_I2C_ADDRESS, ADP_CLOCK_WALL_EARLIEST_MS, &rtc);

    wall_clock_source_t source = kept ? WALL_CLOCK_SOURCE_KEPT : WALL_CLOCK_SOURCE_NONE;
    if (rtc.usable && adp_clock_set_wall_ms(clock, rtc.unix_ms) == HAL_OK) {
        source = WALL_CLOCK_SOURCE_RTC;
    }

    const log_field_t fields[] = {
        log_str("source", wall_clock_source_name(source)),
        log_str("rtc", wall_clock_rtc_reason(&rtc)),
        log_u64("rtc_unix_ms", rtc.decoded ? rtc.unix_ms : 0u),
        log_bool("kept_across_reset", kept),
        log_u64("wall_ms", clock->port.wall_ms(&clock->port)),
    };
    port_log_write(log, (source == WALL_CLOCK_SOURCE_NONE) ? LOG_LEVEL_WARN : LOG_LEVEL_INFO,
                   "main", "wall clock", fields, sizeof(fields) / sizeof(fields[0]));
    return source;
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
static void start_console(const console_deps_t *deps)
{
    (void)console_start(deps);
}

// The touch controller, once the bus is up: on VDD3V3, not on PWR_EN, so
// the rail does not matter to it -- but the bus does, and the bus is only
// usable once the rail is up (bring_up_i2c()). Starts the touch task, which
// wakes the controller, finds it, reads its configuration and turns its
// interrupt on -- about 90 ms, logged as `adp_input: touch ready took_ms=` --
// while app_main goes on: the first frame does not wait for it. Without a
// controller, touch is off, looked for every minute, and the rest runs.
static void start_touch(adp_input_t *touch, adp_i2c_t *i2c, const port_clock_t *clock,
                        const port_log_t *log)
{
    // The GPIO ISR service, here on app_main before the touch task or the
    // RTC's INT asks for it: installed once, no second call logged as an
    // error (board_gpio_isr.h).
    const esp_err_t isr = board_gpio_isr_service();
    if (isr != ESP_OK) {
        const log_field_t fields[] = {log_i64("esp_err", (int64_t)isr)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "GPIO ISR service not installed", fields, 1u);
    }
    const adp_input_config_t cfg = {
        .bus = adp_i2c_port(i2c),
        .clock = clock,
        .log = log,
        .task_stack_bytes = TOUCH_TASK_STACK_BYTES,
        .task_priority = TOUCH_TASK_PRIORITY,
        .task_core = TOUCH_TASK_CORE,
    };
    const hal_status_t st = adp_input_init(touch, &cfg);
    if (st != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)st)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "touch not started", fields, 1u);
    }
}

// The MQTT client, for the console's mqtt: NULL without telemetry.
static adp_mqtt_t *s_console_mqtt;

// The touch task has queued input: the event loop looks now.
static void wake_event_loop(void *ctx)
{
    (void)ctx;
    event_loop_post_input();
}

// The station's state changed, or the MQTT session's, or a message arrived:
// the event loop looks now, and the reporter with it.
static void wake_event_loop_network(void *ctx)
{
    (void)ctx;
    event_loop_post_network();
}

// The device's name on the network: MQTT's device id, "hac-" and the station
// MAC's twelve hex digits (core/reporting/topics.h) -- also what DHCP tells
// the router, so the router's client list shows the same name Home Assistant
// will. Read from the eFuses, so it exists whether or not Wi-Fi does.
static void name_device(char *id, size_t cap, const port_log_t *log)
{
    uint8_t mac[ADP_NET_MAC_LEN];
    const hal_status_t st = adp_net_station_mac(mac);
    if (st != HAL_OK || mqtt_device_id_from_mac(mac, id, cap) >= cap) {
        id[0] = '\0';
    }
    const log_field_t fields[] = {
        log_str("device_id", (id[0] != '\0') ? id : "-"),
        log_i64("status", (int64_t)st),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main", "device", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// The Wi-Fi station for the network sdkconfig.credentials names, its driver
// set up and its radio off: the radio's start is start_radio()'s, once the
// first frame is on the glass. NULL without a network in this image -- the
// firmware then runs as before -- and the port, answering NET_FAILED at up(),
// when the station could not be set up: the console says why.
static port_net_t *start_network(adp_net_t *net, const char *hostname, const port_clock_t *clock,
                                 const port_log_t *log)
{
    if (CONFIG_HAC_WIFI_SSID[0] == '\0') {
        port_log_write(log, LOG_LEVEL_INFO, "main",
                       "Wi-Fi off: no network in this image (sdkconfig.credentials)", NULL, 0u);
        return NULL;
    }
    const adp_net_config_t cfg = {
        .ssid = CONFIG_HAC_WIFI_SSID,
        .password = CONFIG_HAC_WIFI_PASSWORD,
        .hostname = (hostname[0] != '\0') ? hostname : NULL,
        .clock = clock,
        .log = log,
        .task_stack_bytes = NET_TASK_STACK_BYTES,
        .task_priority = NET_TASK_PRIORITY,
        .task_core = NET_TASK_CORE,
    };
    const hal_status_t st = adp_net_init(net, &cfg);
    if (st != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)st)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "Wi-Fi not set up: no network", fields, 1u);
    }
    adp_net_set_waker(net, wake_event_loop_network, NULL);
    return adp_net_port(net);
}

// Time from the network, for an image with one: the RTC's alarm taken
// back, its INT interrupting, and the wait for the station's first address,
// when SNTP starts; the rest is the event loop's, through net_time_step().
// The condition is a constant, as start_network()'s: without a network
// nothing here is called, and the linker drops SNTP with the rest of the
// stack. True when the event loop is to step it.
static bool start_net_time(port_net_t *net, adp_i2c_t *i2c, const port_clock_t *clock,
                           const port_log_t *log, wall_clock_source_t *wall_source)
{
    if (CONFIG_HAC_WIFI_SSID[0] == '\0' || net == NULL) {
        return false;
    }
    const net_time_config_t cfg = {
        .bus = adp_i2c_port(i2c),
        .rtc_address = BOARD_RTC_I2C_ADDRESS,
        .clock = clock,
        .log = log,
        .wall_source = wall_source,
        .server = CONFIG_HAC_SNTP_SERVER,
        .wake = event_loop_post_time,
    };
    return net_time_init(&cfg) == HAL_OK;
}

// MQTT, for an image with a network and a broker: the topics from the
// overlay's prefix and the device id, the client and its task, and the
// reporter, which connects once the station's link is up -- the radio starts
// after the first frame (start_radio()), so the first session comes some
// seconds after it. The condition is a constant, as start_network()'s:
// without a broker nothing here is called, and the linker drops esp-mqtt.
// NULL without telemetry; the event loop then runs without a reporter, and
// the status bar says nothing about the link.
static reporter_t *start_telemetry(port_net_t *net, const char *device_id, sensor_caps_t caps,
                                   const port_clock_t *clock, const port_log_t *log)
{
    static adp_mqtt_t s_mqtt;
    static mqtt_topics_t s_topics;
    static reporter_t s_reporter;
    if (CONFIG_HAC_WIFI_SSID[0] == '\0' || CONFIG_HAC_MQTT_BROKER_URI[0] == '\0' || net == NULL) {
        port_log_write(log, LOG_LEVEL_INFO, "main",
                       "MQTT off: no broker in this image (sdkconfig.credentials)", NULL, 0u);
        return NULL;
    }
    if (mqtt_topics_init(&s_topics, CONFIG_HAC_MQTT_TOPIC_PREFIX, device_id,
                         MQTT_DISCOVERY_PREFIX_DEFAULT) != HAL_OK) {
        const log_field_t fields[] = {
            log_str("prefix", CONFIG_HAC_MQTT_TOPIC_PREFIX),
            log_str("device_id", (device_id[0] != '\0') ? device_id : "-"),
        };
        port_log_write(log, LOG_LEVEL_ERROR, "main",
                       "MQTT off: the topic prefix or the device id is not valid "
                       "(core/reporting/topics.h)",
                       fields, sizeof(fields) / sizeof(fields[0]));
        return NULL;
    }
    const adp_mqtt_config_t cfg = {
        .broker_uri = CONFIG_HAC_MQTT_BROKER_URI,
        .username = CONFIG_HAC_MQTT_USER,
        .password = CONFIG_HAC_MQTT_PASSWORD,
        .net = net,
        .clock = clock,
        .log = log,
        .task_stack_bytes = MQTT_TASK_STACK_BYTES,
        .task_priority = MQTT_TASK_PRIORITY,
        .task_core = MQTT_TASK_CORE,
    };
    if (adp_mqtt_init(&s_mqtt, &cfg) != HAL_OK) {
        return NULL;  // logged by the adapter
    }
    adp_mqtt_set_waker(&s_mqtt, wake_event_loop_network, NULL);

    const esp_app_desc_t *app = esp_app_get_description();
    reporter_deps_t deps = {
        .telemetry = adp_mqtt_port(&s_mqtt),
        .net = net,
        .log = log,
        .topics = &s_topics,
    };
    deps.discovery.device.name = HA_DEVICE_NAME;
    deps.discovery.device.manufacturer = HA_MANUFACTURER;
    deps.discovery.device.model = HA_MODEL;
    deps.discovery.device.hw_version = BOARD_SILKSCREEN_REVISION;
    deps.discovery.device.sw_version = app->version;
    deps.discovery.origin.name = HA_ORIGIN_NAME;
    deps.discovery.origin.sw_version = app->version;
    deps.discovery.origin.support_url = HA_SUPPORT_URL;
    deps.discovery.caps = caps;
    deps.discovery.has_rssi = true;
    const reporter_config_t rcfg = reporter_config_default();
    if (reporter_init(&s_reporter, &deps, &rcfg) != HAL_OK) {
        port_log_write(log, LOG_LEVEL_ERROR, "main",
                       "MQTT off: the reporter would not start (a discovery config too long?)",
                       NULL, 0u);
        return NULL;
    }
    const log_field_t fields[] = {
        log_str("topics", s_topics.base),
        log_str("discovery", s_topics.discovery_prefix),
        log_str("sw_version", app->version),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main", "MQTT: the reporter waits for the link", fields,
                   sizeof(fields) / sizeof(fields[0]));
    s_console_mqtt = &s_mqtt;
    return &s_reporter;
}

// event_loop.h's first-frame hook: the radio on, now that the switched rail
// has long settled and the panel has stopped being driven. up() only asks;
// the station's task starts the radio -- its RF calibration the current this
// waits for -- and joins. The next panel drive is a sensor cycle away at
// least (update_policy's min_redraw_interval_ms), unless someone taps.
static void start_radio(void *ctx)
{
    port_net_t *net = (port_net_t *)ctx;
    if (net != NULL) {
        (void)net->up(net);
    }
}

// event_loop.h's first-frame hook, whole: an image that got this far after an
// update from the card is confirmed, so the bootloader keeps it
// (docs/adr/0037-firmware-from-the-card.md) -- the boot, the rail, the
// sensors' bring-up and a full refresh behind it -- and then the radio.
static void after_first_frame(void *ctx)
{
    fw_update_confirm_running();
    start_radio(ctx);
}

// The diagnostics screen's word for where the wall clock came from, short
// enough for its line; NULL for none.
static const char *clock_word(wall_clock_source_t source)
{
    switch (source) {
        case WALL_CLOCK_SOURCE_RTC:
            return "rtc";
        case WALL_CLOCK_SOURCE_CONSOLE:
            return "console";
        case WALL_CLOCK_SOURCE_KEPT:
            return "kept";
        case WALL_CLOCK_SOURCE_SNTP:
            return "sntp";
        case WALL_CLOCK_SOURCE_NONE:
            break;
    }
    return NULL;
}

// event_loop.h's facts hook: what the diagnostics screen shows that only the
// composition knows. Read on the event loop's task; the card's task and
// the console write them, a word each, and net_time_step() on this task.
static void screen_facts(void *ctx, diag_recorder_t *recorder, const char **clock_source)
{
    const wall_clock_source_t *wall_source = (const wall_clock_source_t *)ctx;
    recorder_state_t state = RECORDER_WAITING;
    recorder->present = card_writer_recorder_state(&state);
    recorder->state = state;
    *clock_source = clock_word(*wall_source);
}

// The board's two sensors, set up on the bus and handed to the sensor
// manager with the priorities of docs/architecture.md 6.3: temperature and
// humidity from the BME68x, the SCD41 as fallback; CO2 from the SCD41 alone;
// pressure, gas and IAQ from the BME68x alone -- IAQ only where BSEC is
// linked, and never CO2 from it. Nothing touches the bus here: the event
// loop's first poll initialises both, on its own task.
static void compose_sensors(sensor_manager_t *mgr, scd4x_source_t *scd4x, bme68x_composed_t *bme68x,
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
    const bme68x_composed_config_t bme68x_cfg = {
        .bus = adp_i2c_port(i2c),
        .clock = clock,
        .log = log,
        .address = BOARD_BME68X_I2C_ADDRESS,
        .id = BME68X_SOURCE_ID,
    };
    (void)scd4x_source_setup(scd4x, &scd4x_cfg);
#if CONFIG_HAC_BSEC
    (void)bme68x_bsec_source_setup(bme68x, &bme68x_cfg);
#else
    (void)bme68x_source_setup(bme68x, &bme68x_cfg);
#endif

    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(mgr, clock, log, &cfg);
    (void)sensor_manager_add(mgr, &scd4x->port, SENSOR_MANAGER_DEFAULT_INTERVAL_MS);
    (void)sensor_manager_add(mgr, &bme68x->port, BME68X_INTERVAL_MS);

    static const uint8_t prefer_bme68x[] = {BME68X_SOURCE_ID, SCD4X_SOURCE_ID};
    static const uint8_t only_bme68x[] = {BME68X_SOURCE_ID};
    static const uint8_t only_scd4x[] = {SCD4X_SOURCE_ID};
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_TEMPERATURE, prefer_bme68x, 2u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_HUMIDITY, prefer_bme68x, 2u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_PRESSURE, only_bme68x, 1u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_CO2, only_scd4x, 1u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_GAS_RESISTANCE, only_bme68x, 1u);
    (void)sensor_manager_set_priority(mgr, SENSOR_FIELD_IAQ, only_bme68x, 1u);
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
        if ((caps & SENS_CAP_IAQ) != 0u && n < LOG_FIELDS_MAX) {
            fields[n++] = value_field("iaq", &s->iaq);
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
    recorder_watch_t watch;
    recorder_event_t events[SENSOR_MANAGER_MAX_SOURCES];
} reading_hook_ctx_t;

// event_loop.h's hook, after every poll: each new sample or failure to the
// card's task, which records it -- a copy into its queue, never a wait on the
// card; the measured temperature to the display adapter for EPDiy's waveform
// selection -- through the adapter's own function, not port_display
// (adp_display.h) -- and, once a minute, every source's sample to the log.
static void on_reading(void *ctx_v, const sensor_manager_t *sensors, const air_reading_t *reading,
                       uint64_t now_ms)
{
    reading_hook_ctx_t *ctx = (reading_hook_ctx_t *)ctx_v;
    const size_t n = recorder_watch(&ctx->watch, sensors, now_ms, reading->wall_time_ms,
                                    ctx->events, SENSOR_MANAGER_MAX_SOURCES);
    card_writer_post(ctx->events, n);
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

// The card: its SPI bus, the recorder with the two sources' tags, the log's
// files, and the task that owns them all. The recorder and the log file hold
// their buffers -- some 13 KB -- in PSRAM; only the task's stack is internal.
// Without the memory or the task, recording is off and the rest runs.
//
// Called once the switched 3V3 is up and settled, never before, though the
// card itself runs from the always-on VDD3V3: whatever the card's task does
// touches PSRAM (FATFS's buffers) and the card's current is on VDD3V3, and
// neither belongs in the rail's inrush. Not, as first read, the cause of the
// boot resets of 2026-10-16: an image that started the card here still reset
// at the same point, with nothing of the card running yet -- the log's hook
// writing PSRAM in the inrush is the reading that fits both images, and
// log_tee_hold() is its answer (docs/hardware/board_notes.md, open item "A
// reboot loop when the switched 3V3 comes up"). The log loses only the lines
// held then, and says how many.
static void start_card(adp_sd_t *sd, const port_clock_t *clock, const port_log_t *log,
                       const time_zone_t *zone)
{
    (void)adp_sd_init(sd, log);
    recorder_t *rec = heap_caps_calloc(1u, sizeof(recorder_t), MALLOC_CAP_SPIRAM);
    log_file_t *log_file = heap_caps_calloc(1u, sizeof(log_file_t), MALLOC_CAP_SPIRAM);
    if (rec == NULL || log_file == NULL) {
        port_log_write(log, LOG_LEVEL_ERROR, "main", "no memory for the recorder: recording off",
                       NULL, 0u);
        return;
    }
    const recorder_config_t cfg = recorder_config_default();
    recorder_init(rec, adp_sd_port(sd), log, zone, &cfg);
    (void)recorder_add_source(rec, SCD4X_SOURCE_ID, SCD4X_RECORDING_TAG, 0);
    (void)recorder_add_source(rec, BME68X_SOURCE_ID, BME68X_RECORDING_TAG, BME68X_ACCURACY_CAPS);
    log_file_init(log_file, adp_sd_port(sd), zone);
    // The button the card's task samples for the USB disk mode (ADR 0036).
    const esp_err_t button = board_button_init();
    if (button != ESP_OK) {
        const log_field_t fields[] = {log_i64("esp_err", (int64_t)button)};
        port_log_write(log, LOG_LEVEL_WARN, "main",
                       "button not set up: usb_disk at the console only", fields, 1u);
    }

    const card_writer_deps_t deps = {
        .clock = clock,
        .log = log,
        .sd = sd,
        .recorder = rec,
        .log_file = log_file,
        .update_progress = event_loop_firmware_update,
    };
    (void)card_writer_start(&deps, CARD_WRITER_TASK_STACK_BYTES, CARD_WRITER_TASK_PRIORITY,
                            CARD_WRITER_TASK_CORE);
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
    static link_hold_t s_link;
    static scd4x_source_t s_scd4x;
    static bme68x_composed_t s_bme68x;
    static sensor_manager_t s_sensors;
    static reading_hook_ctx_t s_hook;
    static time_zone_t s_zone;
    static wall_clock_source_t s_wall_source;
    static adp_sd_t s_sd;
    static adp_input_t s_touch;
    static screen_controller_t s_screens;
    static adp_net_t s_net;
    static char s_device_id[MQTT_DEVICE_ID_MAC_LEN + 1u];

    // Before anything logs: every line from here on reaches the card's file.
    const hal_status_t tee = log_tee_start((esp_log_level_t)CONFIG_HAC_LOG_FILE_LEVEL);

    adp_log_init(&s_log);
    const port_log_t *log = adp_log_port(&s_log);

    adp_clock_init(&s_clock, log);
    const port_clock_t *clock = adp_clock_port(&s_clock);

    // The zone first: the card's task names its files by the local day.
    s_zone = time_zone_eu(LOCAL_STANDARD_OFFSET_MIN);

    port_log_write(log, LOG_LEVEL_INFO, "main",
#if CONFIG_HAC_DISPLAY_NULL
                   "phase 4g: null display (CONFIG_HAC_DISPLAY_NULL), PWR_EN raised without "
                   "EPDiy, " SENSORS_TEXT ", the RTC, the microSD card, touch, Wi-Fi, SNTP, MQTT",
#elif CONFIG_HAC_EPD_CALIBRATION
                   "phase 2c: panel calibration mode (CONFIG_HAC_EPD_CALIBRATION), no event loop, "
                   "no sensors",
#else
                   "phase 4g: EPDiy driving the panel, " SENSORS_TEXT
                   ", the RTC, the microSD card, touch, Wi-Fi, SNTP, MQTT",
#endif
                   NULL, 0u);

    log_reset_reason(log);
    fw_update_boot(log);  // which slot runs; an update rolled back (docs/adr/0037)
    log_memory_budget(log);
    name_device(s_device_id, sizeof(s_device_id), log);
    if (tee != HAL_OK) {
        port_log_write(log, LOG_LEVEL_ERROR, "main", "no memory for the log's ring: no log file",
                       NULL, 0u);
    }
    run_contract_suites(clock, log);

#if CONFIG_HAC_DISPLAY_NULL
    const hal_status_t display_status =
        adp_display_null_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    log_tee_hold(true);  // nothing in PSRAM while the rail comes up: see log_tee_hold()
    board_power_switched_3v3_on(log);  // what EPDiy's init does for the sensors otherwise
    bring_up_i2c(&s_i2c, clock, log);  // after the rail: see bring_up_i2c()
    log_tee_hold(false);
    s_wall_source = set_clock_from_rtc(&s_i2c, &s_clock, log);
    start_touch(&s_touch, &s_i2c, clock, log);
    if (display_status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)display_status)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "adp_display_null_init failed", fields, 1u);
        return;  // nothing else can run without a framebuffer
    }
    port_display_t *display = adp_display_null_port(&s_display);
#else
    // EPDiy's init raises PWR_EN and logs a line microseconds later, in the
    // rail's inrush: nothing in PSRAM until the rail has settled (see
    // log_tee_hold()), which bring_up_i2c() waits out first.
    log_tee_hold(true);
    const hal_status_t display_status =
        adp_display_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    bring_up_i2c(&s_i2c, clock, log);  // after the display: see bring_up_i2c()
    log_tee_hold(false);
    s_wall_source = set_clock_from_rtc(&s_i2c, &s_clock, log);
    start_touch(&s_touch, &s_i2c, clock, log);
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

    // After the rail, the log's release and touch; the radio itself
    // waits for the first frame (start_radio()).
    port_net_t *net = start_network(&s_net, s_device_id, clock, log);
    const bool net_time = start_net_time(net, &s_i2c, clock, log, &s_wall_source);

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
    // After the sensors, whose capabilities say which entities discovery
    // announces; before the event loop, which steps the reporter.
    reporter_t *reporter = start_telemetry(
        net, s_device_id, (sensor_caps_t)(s_scd4x.port.caps | s_bme68x.port.caps), clock, log);
    s_hook.log = log;
#if !CONFIG_HAC_DISPLAY_NULL
    s_hook.display = &s_display;
#endif
    s_hook.next_sample_log_ms = 0u;
    const screen_controller_config_t screens_cfg = screen_controller_config_default();
    screen_controller_init(&s_screens, &screens_cfg);
    adp_input_set_waker(&s_touch, wake_event_loop, NULL);
    // Without a reporter the link is LINK_OFF: the bar says nothing about it
    // (docs/adr/0031-the-network-on-the-screen.md); with one, what the link
    // has held for a minute.
    const link_hold_config_t link_cfg = link_hold_config_default();
    link_hold_init(&s_link, &link_cfg);

    event_loop_deps_t deps = {
        .clock = clock,
        .display = display,
        .log = log,
        .policy = &s_policy,
        .sensors = &s_sensors,
        .zone = &s_zone,
        .on_reading = on_reading,
        .on_reading_ctx = &s_hook,
        .input = adp_input_port(&s_touch),
        .screens = &s_screens,
        .facts = screen_facts,
        .facts_ctx = &s_wall_source,
        .reporter = reporter,  // NULL without a broker: no telemetry
        .link = &s_link,
        .after_first_frame = after_first_frame,
        .after_first_frame_ctx = net,
        // The constant first, as in start_net_time(): without a network the
        // step is not referenced at all.
        .step = (CONFIG_HAC_WIFI_SSID[0] != '\0' && net_time) ? net_time_step : NULL,
        .step_ctx = NULL,
    };
    if (!event_loop_start(&deps, EVENT_LOOP_TASK_STACK_BYTES, EVENT_LOOP_TASK_PRIORITY,
                          EVENT_LOOP_TASK_CORE)) {
        return;
    }
    // The pump must exist before the card can find an image: update progress
    // waits for its display acknowledgement, including at the boot's mount.
    start_card(&s_sd, clock, log, &s_zone);
    const console_deps_t console = {
        .bus = &s_i2c,
        .clock = &s_clock,
        .zone = &s_zone,
        .zone_name = LOCAL_ZONE_NAME,
        .wall_source = &s_wall_source,
        .touch = &s_touch,
        .net = (net != NULL) ? &s_net : NULL,
        .mqtt = s_console_mqtt,
        .device_id = s_device_id,
        .log = log,
    };
    start_console(&console);
}
