// SPDX-License-Identifier: Apache-2.0

#include "console.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "adp_clock.h"
#include "adp_net.h"
#include "adp_net_pure.h"
#include "board_config.h"
#include "card_writer.h"
#include "domain/civil_time.h"
#include "gt911_touch.h"
#include "i2c_scan.h"
#include "net_time.h"
#include "pcf8563.h"
#include "port_clock.h"
#include "port_net.h"
#include "wall_clock.h"

// No trailing space: esp_console_setup_prompt() appends one itself.
#define CONSOLE_PROMPT "hac>"

// How long `restart` lets its last line drain to USB-Serial-JTAG before the
// reset cuts the port off. A line takes well under a millisecond at USB full
// speed; usleep() sleeps whole 10 ms ticks, so this is five of them.
#define RESTART_DRAIN_US 50000u

// What board_config.h says is on the bus, each entry printed with the name of
// the constant it came from so a surprise in the scan points straight at the
// line to correct. The GT911's second address is a candidate: it latches one
// of the two at power-up, and only 0x5D has been seen (board_config.h,
// "touch panel"). So is the BME68x's: SDO picks one of two, and this module
// answers at 0x77 (board_config.h, "sensors"). A candidate is printed only
// when it answers. The sensors are required, so a board without them wired
// says MISSING -- which is the point of the scan.
//
// EXPECT() expands to the fields only; the braces stay at the use site. A
// braced initializer as a macro body is laid out differently by clang-format
// 18 (CI's, Ubuntu 24.04) and 21, with no layout both accept.
#define EXPECT(addr_constant, expect_kind, description) \
    .addr = (addr_constant), .kind = (expect_kind), .what = description " (" #addr_constant ")"

static const i2c_expected_t EXPECTED[] = {
    {EXPECT(BOARD_RTC_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "rtc: PCF8563")},
    {EXPECT(BOARD_TOUCH_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "touch: GT911, INT low at reset")},
    {EXPECT(BOARD_TOUCH_ALT_I2C_ADDRESS, I2C_EXPECT_CANDIDATE,
            "touch: GT911, if INT was high at reset")},
    {EXPECT(BOARD_SCD4X_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "co2: SCD4x, its only address")},
    {EXPECT(BOARD_BME68X_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "env: BME68x, SDO high")},
    {EXPECT(BOARD_BME68X_ALT_I2C_ADDRESS, I2C_EXPECT_CANDIDATE, "env: BME68x, if SDO is low")},
};
#define EXPECTED_COUNT (sizeof EXPECTED / sizeof EXPECTED[0])

// Every expected address plus every address a scan can find. Static rather
// than on the stack: ~1.4 KB is a third of the console task's stack, and the
// boot-time scan and the console never run at the same time.
#define REPORT_ROWS (EXPECTED_COUNT + (I2C_SCAN_LAST_ADDR - I2C_SCAN_FIRST_ADDR + 1u))
static i2c_scan_row_t s_rows[REPORT_ROWS];

// What the commands work with, copied by console_start().
static console_deps_t s_deps;

// "YYYY-MM-DDTHH:MM:SSZ" and its terminator; "YYYY-MM-DD HH:MM:SS UTC+hh:mm".
#define UTC_TEXT_LEN (CIVIL_ISO8601_UTC_LEN + 1u)
#define LOCAL_TEXT_LEN 32u
// A signed 64-bit number of milliseconds as text, or "none".
#define STEP_TEXT_LEN 24u
#define MINUTES_PER_HOUR 60
static const char *hal_status_text(hal_status_t s)
{
    switch (s) {
        case HAL_OK:
            return "ok";
        case HAL_ERR_TIMEOUT:
            return "timeout";
        case HAL_ERR_IO:
            return "io";
        case HAL_ERR_NOT_FOUND:
            return "not_found";
        case HAL_ERR_INVALID_ARG:
            return "invalid_arg";
        case HAL_ERR_NO_MEM:
            return "no_mem";
        case HAL_ERR_UNSUPPORTED:
            return "unsupported";
        case HAL_ERR_BUSY:
            return "busy";
        case HAL_ERR_NOT_READY:
            return "not_ready";
        default:
            return "?";
    }
}

static const char *level_text(bool high)
{
    return high ? "high" : "LOW";
}

void console_print_i2c_scan(adp_i2c_t *bus)
{
    bool sda_high = false;
    bool scl_high = false;
    adp_i2c_lines(bus, &sda_high, &scl_high);
    printf("I2C_SCAN bus sda_gpio=%d scl_gpio=%d scl_hz=%u, idle lines sda=%s scl=%s\n",
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, (unsigned)BOARD_I2C_SCL_HZ, level_text(sda_high),
           level_text(scl_high));

    // Timed because the first boot with the scan showed about a second between
    // "bus up" and the console starting -- ~9 ms per address if it is all the
    // scan, which nothing in the driver's probe path explains (ADR 0011).
    i2c_scan_result_t result;
    const int64_t start_us = esp_timer_get_time();
    i2c_scan(adp_i2c_port(bus), BOARD_I2C_PROBE_TIMEOUT_MS, &result);
    const int64_t scan_us = esp_timer_get_time() - start_us;
    const size_t n = i2c_scan_report(&result, EXPECTED, EXPECTED_COUNT, s_rows, REPORT_ROWS);

    unsigned missing = 0u;
    unsigned unexpected = 0u;
    for (size_t i = 0; i < n; i++) {
        const i2c_scan_row_t *row = &s_rows[i];
        missing += (row->status == I2C_ROW_MISSING) ? 1u : 0u;
        unexpected += (row->status == I2C_ROW_UNEXPECTED) ? 1u : 0u;
        // A candidate that did not answer says no more than an address the
        // table never names: it is listed only so that, if it ever answers,
        // its row names the device instead of calling it UNEXPECTED. Not
        // printed since 2026-10-16. A required address that did not answer
        // is still printed, as MISSING, and so is anything not scanned.
        if (row->status == I2C_ROW_ABSENT) {
            continue;
        }
        printf("I2C_SCAN 0x%02X %-11s %s\n", row->addr, i2c_scan_row_status_name(row->status),
               row->what != NULL ? row->what : "not in board_config.h");
    }

    if (result.status != HAL_OK) {
        printf("I2C_SCAN stopped at 0x%02X: %s -- addresses from there on were not probed; "
               "try i2c_recover\n",
               result.scanned_end, hal_status_text(result.status));
    }
    printf("I2C_SCAN_RESULT answered=%u missing=%u unexpected=%u scan=%s scan_ms=%lld\n",
           (unsigned)i2c_scan_count(&result), missing, unexpected,
           result.status == HAL_OK ? "complete" : "INCOMPLETE", (long long)(scan_us / 1000));
}

// --- time ------------------------------------------------------------------------

static void format_utc(char *dst, size_t cap, uint64_t unix_ms)
{
    const civil_time_t t = civil_time_from_unix_ms(unix_ms);
    (void)snprintf(dst, cap, "%04d-%02u-%02uT%02u:%02u:%02uZ", (int)t.year, (unsigned)t.month,
                   (unsigned)t.day, (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
}

static void format_local(char *dst, size_t cap, uint64_t unix_ms)
{
    const civil_time_t t = civil_time_local(unix_ms, s_deps.zone);
    const int32_t offset = time_zone_offset_min(s_deps.zone, unix_ms);
    const int32_t magnitude = (offset < 0) ? -offset : offset;
    (void)snprintf(dst, cap, "%04d-%02u-%02u %02u:%02u:%02u UTC%c%02d:%02d", (int)t.year,
                   (unsigned)t.month, (unsigned)t.day, (unsigned)t.hour, (unsigned)t.minute,
                   (unsigned)t.second, (offset < 0) ? '-' : '+',
                   (int)(magnitude / MINUTES_PER_HOUR), (int)(magnitude % MINUTES_PER_HOUR));
}

// What the network has said (phase 4f, net_time.h): where SNTP stands, its
// last answer, where the RTC's sync is and what it measured, and the RTC's
// INT on IO9 -- its edges so far and its level, high while no alarm is due.
static void print_sntp(void)
{
    net_time_status_t st;
    net_time_status(&st);
    if (!st.started) {
        printf("TIME_SNTP off: %s\n",
               (s_deps.net == NULL) ? "no network in this image" : "net_time did not start");
        return;
    }
    printf("TIME_SNTP rtc sync: %s; INT on IO9 %s, %u edges; next write planned with "
           "first_tick_ms=%u (%s)\n",
           rtc_sync_phase_name(st.phase), st.int_low ? "low" : "high", (unsigned)st.int_edges,
           (unsigned)st.first_tick_ms, st.first_tick_learned ? "measured" : "the datasheet's");
    if (!st.sntp_running) {
        if (st.sntp_err != 0) {
            printf("TIME_SNTP not started: esp_netif_sntp_init() esp_err=%d\n", (int)st.sntp_err);
        } else {
            printf("TIME_SNTP waiting for Wi-Fi's first address; then asks %s\n", st.server);
        }
        return;
    }
    if (st.syncs == 0u) {
        printf("TIME_SNTP asking %s, no answer yet; refused=%u\n", st.server, (unsigned)st.refused);
        return;
    }
    const port_clock_t *clock = adp_clock_port(s_deps.clock);
    const uint64_t now = clock->now_ms(clock);
    char last[UTC_TEXT_LEN];
    format_utc(last, sizeof(last), st.last_sync_utc_ms);
    char step[STEP_TEXT_LEN] = "none";
    if (st.step_known) {
        (void)snprintf(step, sizeof(step), "%lld", (long long)st.system_minus_sntp_ms);
    }
    printf("TIME_SNTP server=%s syncs=%u last=%s (%llu s ago) system_minus_sntp_ms=%s "
           "refused=%u\n",
           st.server, (unsigned)st.syncs, last,
           (unsigned long long)((now - st.last_sync_ms) / 1000u), step, (unsigned)st.refused);
    if (st.offset_known) {
        printf("TIME_SNTP before the last write: rtc_minus_sntp_ms=%lld (+-%u)",
               (long long)st.rtc_minus_sntp_ms, (unsigned)st.offset_uncertainty_ms);
    } else {
        printf("TIME_SNTP before the last write: no tick to compare");
    }
    if (st.drift_known) {
        printf(" drift_ppm=%.2f (+-%.2f)\n", (double)st.drift_ppm,
               (double)st.drift_uncertainty_ppm);
    } else {
        printf(" drift: not known -- it needs this boot's last write ten minutes back\n");
    }
    printf("TIME_SNTP writes=%u failed=%u", (unsigned)st.writes, (unsigned)st.write_failures);
    if (st.landed_known) {
        printf(" landed residual_ms=%lld", (long long)st.residual_ms);
    }
    printf("\n");
}

// The system time and where it came from, or why there is none; what SNTP
// has said; then what the RTC holds now, with its registers, so a VL bit or
// a stopped clock shows as it is on the chip.
static void print_time(void)
{
    wall_clock_rtc_t rtc;
    wall_clock_read_rtc(adp_i2c_port(s_deps.bus), BOARD_RTC_I2C_ADDRESS, ADP_CLOCK_WALL_EARLIEST_MS,
                        &rtc);

    const uint64_t wall_ms = s_deps.clock->port.wall_ms(&s_deps.clock->port);
    if (wall_ms != 0u) {
        char utc[UTC_TEXT_LEN];
        char local[LOCAL_TEXT_LEN];
        format_utc(utc, sizeof(utc), wall_ms);
        format_local(local, sizeof(local), wall_ms);
        printf("TIME utc=%s %s=\"%s\" source=\"%s\"\n", utc, s_deps.zone_name, local,
               wall_clock_source_name(*s_deps.wall_source));
    } else {
        printf("TIME none: %s -- set it with time_set YYYY-MM-DDTHH:MM:SSZ (UTC)%s\n",
               wall_clock_rtc_reason(&rtc),
               (s_deps.net != NULL) ? ", or wait for the network's" : "");
    }
    print_sntp();

    if (rtc.chip.verdict == PCF8563_TIME_NOT_ANSWERING) {
        printf("TIME_RTC %s (bus: %s)\n", wall_clock_rtc_reason(&rtc),
               hal_status_text(rtc.chip.bus));
        return;
    }
    char held[UTC_TEXT_LEN] = "?";
    if (rtc.decoded) {
        format_utc(held, sizeof(held), rtc.unix_ms);
    }
    const uint8_t *g = rtc.chip.regs;
    printf("TIME_RTC holds %s, %s; registers 00-08: %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
           held, wall_clock_rtc_reason(&rtc), g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8]);
}

static int cmd_time(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    print_time();
    return 0;
}

static int cmd_time_set(void *context, int argc, char **argv)
{
    (void)context;
    if (argc != 2) {
        printf("TIME_SET usage: time_set YYYY-MM-DDTHH:MM:SSZ -- UTC, e.g. "
               "time_set 2026-10-16T12:00:00Z\n");
        return 1;
    }
    civil_time_t t;
    uint64_t unix_ms = 0u;
    if (!civil_time_parse_iso8601_utc(argv[1], &t) || !civil_time_to_unix_ms(&t, &unix_ms)) {
        printf("TIME_SET refused: \"%s\" is not YYYY-MM-DDTHH:MM:SSZ (UTC, a real date)\n",
               argv[1]);
        return 1;
    }
    if (unix_ms < ADP_CLOCK_WALL_EARLIEST_MS) {
        printf("TIME_SET refused: %s is before 2026, earlier than any clock this firmware sets\n",
               argv[1]);
        return 1;
    }

    const hal_status_t rtc =
        wall_clock_write_rtc(adp_i2c_port(s_deps.bus), BOARD_RTC_I2C_ADDRESS, unix_ms);
    // Written to the second, not to its start: no drift from the last mark.
    net_time_forget_rtc_mark();
    const hal_status_t system = adp_clock_set_wall_ms(s_deps.clock, unix_ms);
    if (system == HAL_OK) {
        *s_deps.wall_source = WALL_CLOCK_SOURCE_CONSOLE;
    }
    printf("TIME_SET %s rtc=%s system=%s\n", argv[1], hal_status_text(rtc),
           hal_status_text(system));
    if (rtc != HAL_OK) {
        printf("TIME_SET the RTC was not written: this time will not survive a reset\n");
    }

    const log_field_t fields[] = {
        log_str("utc", argv[1]),
        log_str("rtc", hal_status_text(rtc)),
        log_str("system", hal_status_text(system)),
    };
    port_log_write(s_deps.log,
                   (rtc == HAL_OK && system == HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR,
                   "console", "time_set", fields, sizeof(fields) / sizeof(fields[0]));

    if (s_deps.net != NULL) {
        printf("TIME_SET until the network's next answer, which sets both again\n");
    }
    print_time();
    return (rtc == HAL_OK && system == HAL_OK) ? 0 : 1;
}

// The RTC with no time for a boot to find: its STOP bit set, as a clock
// that stopped would read ("rtc: stopped"). For the check that a boot with
// nothing in the RTC still shows the right time once the network answers
// (docs/hardware/board_notes.md, open item "Time from the network on the
// board"). The system time is left as it is; time_set, or SNTP's next
// answer, starts the RTC again.
static int cmd_rtc_stop(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    const hal_status_t st = pcf8563_stop(adp_i2c_port(s_deps.bus), BOARD_RTC_I2C_ADDRESS);
    net_time_forget_rtc_mark();
    printf("TIME_RTC_STOP %s: the RTC stands still -- a boot now finds no time in it; "
           "time_set or the network's next answer starts it again\n",
           hal_status_text(st));
    const log_field_t fields[] = {log_str("status", hal_status_text(st))};
    port_log_write(s_deps.log, (st == HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, "console",
                   "rtc_stop", fields, 1u);
    print_time();
    return (st == HAL_OK) ? 0 : 1;
}

// --- the card ------------------------------------------------------------------------

// How long a console command waits for the card's task: the slowest thing it
// does is `sd`'s first count of the free space after a mount, a scan of the
// whole FAT on a card whose FSINFO does not hold it -- about 5 s on a 16 GB
// card (adp_sd_card_info()).
#define CARD_REQUEST_TIMEOUT_MS 30000u

#define BYTES_PER_KB 1024u
#define BYTES_PER_MB (1024u * 1024u)

// Static: the card's task fills it after the request returns, should it time
// out (card_writer_request()).
static card_writer_status_t s_card;

static bool ask_card(card_writer_cmd_t cmd, const char *prefix)
{
    const hal_status_t status = card_writer_request(cmd, &s_card, CARD_REQUEST_TIMEOUT_MS);
    if (status != HAL_OK) {
        printf("%s the card's task did not answer: %s\n", prefix, hal_status_text(status));
        return false;
    }
    return true;
}

static void print_card(void)
{
    if (!s_card.have_card) {
        printf("SD %s, last mount esp_err=0x%x, %lu attempts, %lu mounts since boot\n",
               adp_sd_state_name(s_card.sd_state), (unsigned)s_card.last_mount_err,
               (unsigned long)s_card.mount_attempts, (unsigned long)s_card.mounts);
        return;
    }
    printf("SD mounted card=%s %s %llu MB, spi %lu kHz\n", s_card.card.name,
           s_card.card.sdhc ? "SDHC/SDXC" : "SDSC",
           (unsigned long long)(s_card.card.capacity_bytes / BYTES_PER_MB),
           (unsigned long)s_card.card.freq_khz);
    if (s_card.card.free_known) {
        printf("SD %llu MB free\n", (unsigned long long)(s_card.card.free_bytes / BYTES_PER_MB));
    }
    for (size_t i = 0u; i < CARD_WRITER_DIRS; ++i) {
        printf("SD %s/ %lu files, %llu KB\n", s_card.dirs[i].dir,
               (unsigned long)s_card.dirs[i].files,
               (unsigned long long)(s_card.dirs[i].bytes / BYTES_PER_KB));
    }
}

static int cmd_sd(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    if (!ask_card(CARD_WRITER_SPACE, "SD")) {
        return 1;
    }
    print_card();
    return 0;
}

static int cmd_sd_mount(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    if (!ask_card(CARD_WRITER_MOUNT, "SD_MOUNT")) {
        return 1;
    }
    printf("SD_MOUNT %s\n", hal_status_text(s_card.result));
    print_card();
    return (s_card.result == HAL_OK) ? 0 : 1;
}

static int cmd_sd_eject(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    if (!ask_card(CARD_WRITER_EJECT, "SD_EJECT")) {
        return 1;
    }
    printf("SD_EJECT done: every file flushed and closed, the card unmounted -- safe to remove. "
           "sd_mount to use it again\n");
    return 0;
}

static int cmd_rec(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    if (!ask_card(CARD_WRITER_STATUS, "REC")) {
        return 1;
    }
    printf("REC %s (%s), card %s, %lu old files removed by the bound\n",
           (s_card.rec_state == RECORDER_ON) ? "on" : "off", recorder_state_name(s_card.rec_state),
           adp_sd_state_name(s_card.sd_state), (unsigned long)s_card.rec_files_removed);
    for (size_t i = 0u; i < s_card.source_count; ++i) {
        printf("REC source %u %s: %s, %lu rows, %llu bytes, %lu dropped since boot\n",
               (unsigned)s_card.sources[i].id, s_card.sources[i].tag,
               (s_card.sources[i].path[0] != '\0') ? s_card.sources[i].path : "no file open",
               (unsigned long)s_card.sources[i].rows_written,
               (unsigned long long)s_card.sources[i].bytes_written,
               (unsigned long)s_card.sources[i].rows_dropped);
    }
    printf("REC log %s: %s, %lu lines, %llu bytes, %lu dropped since boot; ring %lu in use, "
           "%lu at most, %lu overflowed\n",
           recorder_state_name(s_card.log_state),
           (s_card.log_path[0] != '\0') ? s_card.log_path : "no file open",
           (unsigned long)s_card.log_lines_written, (unsigned long long)s_card.log_bytes_written,
           (unsigned long)s_card.log_lines_dropped, (unsigned long)s_card.ring_used,
           (unsigned long)s_card.ring_high_water, (unsigned long)s_card.ring_dropped_total);
    printf("REC event loop: %lu events posted, %lu dropped, the longest post %lu us; the card: "
           "slowest write %lu us, slowest flush %lu us; writer stack %lu bytes free at least\n",
           (unsigned long)s_card.events_posted, (unsigned long)s_card.events_dropped,
           (unsigned long)s_card.post_max_us, (unsigned long)s_card.slowest_write_us,
           (unsigned long)s_card.slowest_flush_us, (unsigned long)s_card.stack_free_min_bytes);
    return 0;
}

// --- touch ------------------------------------------------------------------------

// Static: about 0.5 KB, and only the console's task uses it.
static adp_input_status_t s_touch;

static int cmd_touch(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    adp_input_status(s_deps.touch, &s_touch);
    const gt911_info_t *i = &s_touch.info;
    if (!s_touch.init_done) {
        printf("TOUCH the touch task is still looking for the controller\n");
        return 1;
    }
    if (!s_touch.found) {
        printf("TOUCH not found: %s -- looked for again every minute, touch_wake now; %lu looks, "
               "%lu losses\n",
               hal_status_text(s_touch.init_status), (unsigned long)s_touch.stats.init_attempts,
               (unsigned long)s_touch.stats.losses);
        return 1;
    }
    printf("TOUCH %s at 0x%02X, product \"%s\", firmware 0x%04X, vendor 0x%02X%s\n",
           s_touch.asleep ? "ASLEEP" : "awake", i->address, i->product_id,
           (unsigned)i->firmware_version, (unsigned)i->vendor_id,
           (s_touch.init_status == HAL_OK) ? "" : " -- interrupt NOT on");
    printf("TOUCH config version 0x%02X: x_max %u y_max %u (in use %ux%u), touch number %u, "
           "module_switch1 0x%02X, trigger %s, checksum %s (0x%02X) -- read, never written\n",
           (unsigned)i->config_version, (unsigned)i->x_max, (unsigned)i->y_max,
           (unsigned)i->x_resolution, (unsigned)i->y_resolution, (unsigned)i->touch_number,
           (unsigned)i->module_switch1, gt911_trigger_name(i->trigger),
           i->config_checksum_ok ? "ok" : "BAD", (unsigned)i->config_checksum);
    printf("TOUCH map swap_xy=%d mirror_x=%d mirror_y=%d onto %ux%u (board_config.h)\n",
           BOARD_TOUCH_SWAP_XY, BOARD_TOUCH_MIRROR_X, BOARD_TOUCH_MIRROR_Y,
           (unsigned)BOARD_EPD_WIDTH, (unsigned)BOARD_EPD_HEIGHT);
    const gt911_input_stats_t *st = &s_touch.stats;
    printf("TOUCH since boot: %lu interrupts, %lu reads, %lu reports, %lu empty, %lu failed "
           "(last %s), %lu events, %lu dropped, %lu lifts timed out; task stack %lu bytes free "
           "at least\n",
           (unsigned long)s_touch.interrupts, (unsigned long)st->reads, (unsigned long)st->reports,
           (unsigned long)st->empty_reads, (unsigned long)st->read_errors,
           hal_status_text(st->last_error), (unsigned long)st->events,
           (unsigned long)(st->dropped + s_touch.queue_dropped), (unsigned long)st->timed_out_lifts,
           (unsigned long)s_touch.stack_free_min_bytes);
    printf("TOUCH lost %lu times, found again %lu, %lu looks in all\n", (unsigned long)st->losses,
           (unsigned long)st->refound, (unsigned long)st->init_attempts);
    for (size_t k = 0u; k < s_touch.history_count; ++k) {
        const gt911_input_record_t *r = &s_touch.history[k];
        printf("TOUCH event %-4s id %u raw %4u,%4u -> panel %3u,%3u at %llu ms\n",
               gt911_touch_kind_name(r->kind), (unsigned)r->id, (unsigned)r->raw_x,
               (unsigned)r->raw_y, (unsigned)r->x, (unsigned)r->y, (unsigned long long)r->ts_ms);
    }
    return 0;
}

static int cmd_touch_sleep(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    const hal_status_t st = adp_input_sleep(s_deps.touch);
    printf("TOUCH_SLEEP %s%s\n", hal_status_text(st),
           (st == HAL_OK) ? ": INT held low, nothing reported until touch_wake or a reset" : "");
    return (st == HAL_OK) ? 0 : 1;
}

static int cmd_touch_wake(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    const int64_t start = esp_timer_get_time();
    const hal_status_t st = adp_input_wake(s_deps.touch);
    printf("TOUCH_WAKE %s in %lld ms\n", hal_status_text(st),
           (long long)((esp_timer_get_time() - start) / 1000));
    return (st == HAL_OK) ? 0 : 1;
}

// --- wifi -------------------------------------------------------------------------

#define MS_PER_S 1000u
#define S_PER_MIN 60u
#define MIN_PER_H 60u

// The status, static: about 0.2 KB, and only the console's task uses it.
static adp_net_status_t s_net;

static const char *net_state_word(net_state_t state)
{
    switch (state) {
        case NET_UP:
            return "UP";
        case NET_CONNECTING:
            return "JOINING";
        case NET_FAILED:
            return "FAILED";
        case NET_DOWN:
            break;
    }
    return "DOWN";
}

// "1 h 02 min", "3 min 07 s", "12 s".
static void print_duration(uint64_t ms)
{
    const uint64_t s = ms / MS_PER_S;
    const uint64_t min = s / S_PER_MIN;
    if (min >= MIN_PER_H) {
        printf("%llu h %02llu min", (unsigned long long)(min / MIN_PER_H),
               (unsigned long long)(min % MIN_PER_H));
    } else if (min > 0u) {
        printf("%llu min %02llu s", (unsigned long long)min, (unsigned long long)(s % S_PER_MIN));
    } else {
        printf("%llu s", (unsigned long long)s);
    }
}

static int cmd_wifi(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    const char *device =
        (s_deps.device_id != NULL && s_deps.device_id[0] != '\0') ? s_deps.device_id : "-";
    if (s_deps.net == NULL) {
        printf("WIFI off: no network in this image -- set CONFIG_HAC_WIFI_SSID in "
               "apps/firmware_esp32/sdkconfig.credentials (sdkconfig.credentials.example); "
               "device id %s\n",
               device);
        return 1;
    }
    adp_net_status(s_deps.net, &s_net);
    const port_clock_t *clock = adp_clock_port(s_deps.clock);
    const uint64_t now = clock->now_ms(clock);
    const adp_net_machine_stats_t *st = &s_net.stats;

    if (s_net.init_status != HAL_OK) {
        printf("WIFI FAILED: the station was not set up (%s) -- see the boot log's adp_net "
               "lines\n",
               hal_status_text(s_net.init_status));
    } else if (s_net.state == NET_UP) {
        printf("WIFI UP \"%s\" %d dBm, address %u.%u.%u.%u, gateway %u.%u.%u.%u, channel %u, "
               "AP %02x:%02x:%02x:%02x:%02x:%02x, up for ",
               s_net.ssid, (int)s_net.rssi_dbm, (unsigned)s_net.ipv4[0], (unsigned)s_net.ipv4[1],
               (unsigned)s_net.ipv4[2], (unsigned)s_net.ipv4[3], (unsigned)s_net.gateway[0],
               (unsigned)s_net.gateway[1], (unsigned)s_net.gateway[2], (unsigned)s_net.gateway[3],
               (unsigned)s_net.channel, (unsigned)s_net.bssid[0], (unsigned)s_net.bssid[1],
               (unsigned)s_net.bssid[2], (unsigned)s_net.bssid[3], (unsigned)s_net.bssid[4],
               (unsigned)s_net.bssid[5]);
        print_duration(now - st->up_since_ms);
        printf("\n");
    } else {
        printf("WIFI %s \"%s\" (%s)", net_state_word(s_net.state), s_net.ssid,
               adp_net_phase_name(s_net.phase));
        if (s_net.state == NET_CONNECTING) {
            printf(", sought for ");
            print_duration(now - st->sought_since_ms);
            printf(", %lu attempts failed in a row", (unsigned long)st->failures_in_a_row);
            if (s_net.next_attempt_ms != ADP_NET_NO_DEADLINE) {
                printf(", the next in ");
                print_duration((s_net.next_attempt_ms > now) ? s_net.next_attempt_ms - now : 0u);
            }
        }
        printf("\n");
        if (st->refusals_in_a_row >= 2u) {
            printf("WIFI refused %lu times in a row: is the password in sdkconfig.credentials "
                   "right?\n",
                   (unsigned long)st->refusals_in_a_row);
        }
    }
    printf("WIFI since boot: %lu connections, %lu lost, %lu attempts, %lu failed (%lu timed out); "
           "the last reason: %s (%u)\n",
           (unsigned long)st->connections, (unsigned long)st->losses, (unsigned long)st->attempts,
           (unsigned long)st->attempts_failed, (unsigned long)st->timeouts,
           adp_net_reason_name(st->last_reason), (unsigned)st->last_reason);
    printf("WIFI station %02x:%02x:%02x:%02x:%02x:%02x, device id %s; task stack %lu bytes free "
           "at least; %lu events dropped\n",
           (unsigned)s_net.mac[0], (unsigned)s_net.mac[1], (unsigned)s_net.mac[2],
           (unsigned)s_net.mac[3], (unsigned)s_net.mac[4], (unsigned)s_net.mac[5], device,
           (unsigned long)s_net.stack_free_min_bytes, (unsigned long)s_net.events_dropped);
    return (s_net.state == NET_UP) ? 0 : 1;
}

// --- vbat -------------------------------------------------------------------------
//
// The battery's divider on GPIO14, an ADC2 input, which ESP-IDF documents as
// shared with Wi-Fi: adc_oneshot_read() arbitrates, and refuses a read it
// cannot make with ESP_ERR_TIMEOUT (docs/hardware/board_notes.md, open item on
// GPIO14). This answers whether it gives usable readings with Wi-Fi up --
// phase 5's battery adapter is the reader, not this.

// CHOICES: reads per command, and the wait between two, so a burst of Wi-Fi
// traffic and its quiet both fall inside one command.
#define VBAT_READS 32u
#define VBAT_READ_GAP_US 20000u

static int cmd_vbat(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t err = adc_oneshot_io_to_channel(BOARD_BATTERY_ADC_GPIO, &unit, &channel);
    adc_oneshot_unit_handle_t adc = NULL;
    if (err == ESP_OK) {
        const adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        err = adc_oneshot_new_unit(&unit_cfg, &adc);
    }
    if (err == ESP_OK) {
        const adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_oneshot_config_channel(adc, channel, &chan_cfg);
    }
    if (err != ESP_OK) {
        printf("VBAT the ADC on GPIO%d did not start: esp_err 0x%x\n", BOARD_BATTERY_ADC_GPIO,
               (unsigned)err);
        if (adc != NULL) {
            (void)adc_oneshot_del_unit(adc);
        }
        return 1;
    }
    adc_cali_handle_t cali = NULL;
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    const bool calibrated = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) == ESP_OK;

    unsigned ok = 0u;
    unsigned refused = 0u;
    int raw_min = INT_MAX;
    int raw_max = INT_MIN;
    int mv_min = INT_MAX;
    int mv_max = INT_MIN;
    long long mv_sum = 0;
    for (unsigned i = 0u; i < VBAT_READS; ++i) {
        int raw = 0;
        const esp_err_t r = adc_oneshot_read(adc, channel, &raw);
        if (r != ESP_OK) {
            ++refused;
        } else {
            ++ok;
            raw_min = (raw < raw_min) ? raw : raw_min;
            raw_max = (raw > raw_max) ? raw : raw_max;
            int mv = 0;
            if (calibrated && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
                mv_min = (mv < mv_min) ? mv : mv_min;
                mv_max = (mv > mv_max) ? mv : mv_max;
                mv_sum += mv;
            }
        }
        usleep(VBAT_READ_GAP_US);
    }
    if (calibrated) {
        (void)adc_cali_delete_scheme_curve_fitting(cali);
    }
    (void)adc_oneshot_del_unit(adc);

    const net_state_t wifi =
        (s_deps.net != NULL) ? s_deps.net->port.state(&s_deps.net->port) : NET_DOWN;
    printf("VBAT GPIO%d = ADC%d channel %d, Wi-Fi %s: %u reads, %u ok, %u refused (Wi-Fi held "
           "ADC2)\n",
           BOARD_BATTERY_ADC_GPIO, (int)unit + 1, (int)channel,
           (s_deps.net != NULL) ? net_state_word(wifi) : "off", VBAT_READS, ok, refused);
    if (ok == 0u) {
        return 1;
    }
    if (!calibrated || mv_min > mv_max) {
        printf("VBAT raw %d..%d of 4095, no calibration to millivolts\n", raw_min, raw_max);
        return 0;
    }
    const long long mean = mv_sum / (long long)ok;
    printf("VBAT raw %d..%d; the pin %d..%d mV, mean %lld; the battery x%u: %lld mV\n", raw_min,
           raw_max, mv_min, mv_max, mean, (unsigned)BOARD_BATTERY_DIVIDER_RATIO,
           mean * (long long)BOARD_BATTERY_DIVIDER_RATIO);
    return 0;
}

// --- i2c ----------------------------------------------------------------------------

static int cmd_i2c_scan(void *context, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    console_print_i2c_scan((adp_i2c_t *)context);
    return 0;
}

static int cmd_i2c_recover(void *context, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    adp_i2c_t *bus = (adp_i2c_t *)context;
    port_i2c_t *port = adp_i2c_port(bus);

    const hal_status_t status = port->recover(port);
    bool sda_high = false;
    bool scl_high = false;
    adp_i2c_lines(bus, &sda_high, &scl_high);
    printf("I2C_RECOVER status=%s pulses=%u lines sda=%s scl=%s\n", hal_status_text(status),
           bus->last_recover_pulses, level_text(sda_high), level_text(scl_high));
    return status == HAL_OK ? 0 : 1;
}

// A software reset on demand: what a panic or the watchdog would do, minus
// the fault. It was added to answer one question by eye -- does a software
// reset drop the switched 3V3 that powers the sensors, as pressing RST does?
// The blue LED sits on that rail (docs/hardware/board_notes.md, "Power rails
// and connectors"). It does not: the LED stayed lit (2026-10-16), so after
// such a reboot the SCD41 is found still measuring. Kept for checking
// exactly that path once the sensor drivers run.
static int cmd_restart(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    printf("RESTART esp_restart() now -- watch the blue LED: dark for a moment = the sensors' "
           "3V3 drops as with RST; lit throughout = it stays up\n");
    fflush(stdout);
    usleep(RESTART_DRAIN_US);
    esp_restart();
    return 0;  // not reached
}

static esp_err_t register_commands(adp_i2c_t *bus)
{
    const esp_console_cmd_t scan = {
        .command = "i2c_scan",
        .help = "Probe every I2C address and compare with board_config.h",
        .func_w_context = cmd_i2c_scan,
        .context = bus,
    };
    const esp_console_cmd_t recover = {
        .command = "i2c_recover",
        .help = "Clock a stuck I2C slave free (up to nine SCL pulses, then STOP)",
        .func_w_context = cmd_i2c_recover,
        .context = bus,
    };
    const esp_console_cmd_t restart = {
        .command = "restart",
        .help = "Software reset (esp_restart): does the sensors' 3V3 drop, as with RST?",
        .func_w_context = cmd_restart,
        .context = NULL,
    };
    const esp_console_cmd_t time_cmd = {
        .command = "time",
        .help = "The system time (UTC and local), where it came from, and what the RTC holds",
        .func_w_context = cmd_time,
        .context = NULL,
    };
    const esp_console_cmd_t time_set_cmd = {
        .command = "time_set",
        .help = "Set the RTC and the system time to a UTC time: time_set YYYY-MM-DDTHH:MM:SSZ",
        .hint = "<YYYY-MM-DDTHH:MM:SSZ>",
        .func_w_context = cmd_time_set,
        .context = NULL,
    };
    const esp_console_cmd_t rtc_stop_cmd = {
        .command = "rtc_stop",
        .help = "Stop the RTC, so that a boot finds no time in it (time_set or SNTP restarts it)",
        .func_w_context = cmd_rtc_stop,
        .context = NULL,
    };
    const esp_console_cmd_t sd_cmd = {
        .command = "sd",
        .help = "The microSD card: mounted or not and why, its size and free space, the files",
        .func_w_context = cmd_sd,
        .context = NULL,
    };
    const esp_console_cmd_t sd_mount_cmd = {
        .command = "sd_mount",
        .help = "Mount the card now, showing ESP-IDF's SD lines if it fails",
        .func_w_context = cmd_sd_mount,
        .context = NULL,
    };
    const esp_console_cmd_t sd_eject_cmd = {
        .command = "sd_eject",
        .help = "Flush and close every file and unmount the card: safe to remove",
        .func_w_context = cmd_sd_eject,
        .context = NULL,
    };
    const esp_console_cmd_t rec_cmd = {
        .command = "rec",
        .help = "Recording: on or off, the files, rows and bytes since boot, the log file",
        .func_w_context = cmd_rec,
        .context = NULL,
    };
    const esp_console_cmd_t touch_cmd = {
        .command = "touch",
        .help = "The touch controller: product ID, firmware, its configuration, the last events",
        .func_w_context = cmd_touch,
        .context = NULL,
    };
    const esp_console_cmd_t touch_sleep_cmd = {
        .command = "touch_sleep",
        .help = "Put the touch controller to sleep (INT held low) until touch_wake or a reset",
        .func_w_context = cmd_touch_sleep,
        .context = NULL,
    };
    const esp_console_cmd_t touch_wake_cmd = {
        .command = "touch_wake",
        .help = "Wake the touch controller (INT pulsed high), or look for it again",
        .func_w_context = cmd_touch_wake,
        .context = NULL,
    };
    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Wi-Fi: state, network, signal, address, reconnections, the last failure",
        .func_w_context = cmd_wifi,
        .context = NULL,
    };
    const esp_console_cmd_t vbat_cmd = {
        .command = "vbat",
        .help = "Read the battery's divider on GPIO14 (ADC2, shared with Wi-Fi) 32 times",
        .func_w_context = cmd_vbat,
        .context = NULL,
    };
    esp_err_t err = esp_console_cmd_register(&scan);
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&recover);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&restart);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&time_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&time_set_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&rtc_stop_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&sd_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&sd_mount_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&sd_eject_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&rec_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&touch_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&touch_sleep_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&touch_wake_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&wifi_cmd);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&vbat_cmd);
    }
    return err;
}

hal_status_t console_start(const console_deps_t *deps)
{
    s_deps = *deps;
    adp_i2c_t *bus = deps->bus;
    const port_log_t *log = deps->log;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = CONSOLE_PROMPT;
    const esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err == ESP_OK) {
        err = esp_console_register_help_command();
    }
    if (err == ESP_OK) {
        err = register_commands(bus);
    }
    if (err == ESP_OK) {
        err = esp_console_start_repl(repl);
    }

    const log_field_t fields[] = {
        log_str("prompt", CONSOLE_PROMPT),
        log_str("commands", "help i2c_scan i2c_recover restart time time_set rtc_stop sd "
                            "sd_mount sd_eject rec touch touch_sleep touch_wake wifi vbat"),
        log_i64("esp_err", (int64_t)err),
    };
    port_log_write(log, err == ESP_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, "console",
                   err == ESP_OK ? "console ready on USB-Serial-JTAG" : "console failed to start",
                   fields, sizeof(fields) / sizeof(fields[0]));

    return err == ESP_OK ? HAL_OK : HAL_ERR_IO;
}
