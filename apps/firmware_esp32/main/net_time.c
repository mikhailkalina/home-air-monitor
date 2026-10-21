// SPDX-License-Identifier: Apache-2.0

#include "net_time.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "adp_clock.h"
#include "board_rtc_int.h"
#include "domain/civil_time.h"
#include "pcf8563.h"
#include "wall_clock_limits.h"

#define TAG "net_time"

#define MS_PER_S 1000u
#define US_PER_MS 1000u

// "2026-10-25T01:00:00.000Z" and its terminator.
#define UTC_MS_TEXT_LEN 25u

// What the other tasks leave for the event loop, under s_lock.
typedef struct {
    bool got_ip;
    bool answer;  // an answer believed and set, not yet taken
    uint64_t sntp_ms;
    uint64_t before_ms;  // the system time just before; 0 when it had none
    bool refused;        // an answer refused, not yet logged
    uint64_t refused_ms;
    bool forget;
} mailbox_t;

typedef struct {
    net_time_config_t cfg;
    bool initialised;
    esp_event_handler_instance_t ip_handler;
    rtc_sync_t sync;          // the event loop's
    mailbox_t box;            // under s_lock
    net_time_status_t shown;  // under s_lock
} net_time_t;

static net_time_t s_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void log_write(log_level_t level, const char *message, const log_field_t *fields,
                      size_t count)
{
    port_log_write(s_t.cfg.log, level, TAG, message, fields, count);
}

static void wake(void)
{
    if (s_t.initialised && s_t.cfg.wake != NULL) {
        s_t.cfg.wake();
    }
}

static uint64_t ms_of(const struct timeval *tv)
{
    if (tv->tv_sec < 0) {
        return 0u;
    }
    return (uint64_t)tv->tv_sec * MS_PER_S + (uint64_t)tv->tv_usec / US_PER_MS;
}

// lwIP's SNTP, on lwIP's task, with a server's answer: replaces ESP-IDF's
// weak default (components/lwip/apps/sntp/sntp.c), which sets the system
// time at once and calls a notification callback. This sets it the same
// way, if the time is one this firmware believes -- a server not yet
// synchronised itself can answer with anything, and the RTC is written from
// what is set here -- and leaves the rest to the event loop. It must not
// wait: nothing here touches the bus, the log or the card.
void sntp_sync_time(struct timeval *tv)
{
    uint64_t before_ms = 0u;
    struct timeval before;
    if (gettimeofday(&before, NULL) == 0) {
        const uint64_t ms = ms_of(&before);
        before_ms = (ms >= ADP_CLOCK_WALL_EARLIEST_MS) ? ms : 0u;
    }
    const uint64_t sntp_ms = ms_of(tv);
    const bool believed = wall_clock_believable_ms(sntp_ms, ADP_CLOCK_WALL_EARLIEST_MS);
    if (believed) {
        (void)settimeofday(tv, NULL);
        sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    }
    taskENTER_CRITICAL(&s_lock);
    if (believed) {
        s_t.box.answer = true;
        s_t.box.sntp_ms = sntp_ms;
        s_t.box.before_ms = before_ms;
    } else {
        s_t.box.refused = true;
        s_t.box.refused_ms = sntp_ms;
    }
    taskEXIT_CRITICAL(&s_lock);
    wake();
}

// ESP-IDF's event task: the station has an address. The event loop starts
// SNTP at the first.
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    taskENTER_CRITICAL(&s_lock);
    s_t.box.got_ip = true;
    taskEXIT_CRITICAL(&s_lock);
    wake();
}

static void format_utc_ms(char *dst, size_t cap, uint64_t unix_ms)
{
    const civil_time_t t = civil_time_from_unix_ms(unix_ms);
    (void)snprintf(dst, cap, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", (int)t.year, (unsigned)t.month,
                   (unsigned)t.day, (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second,
                   (unsigned)(unix_ms % MS_PER_S));
}

static void start_sntp(void)
{
    esp_sntp_set_sync_interval(NET_TIME_SYNC_INTERVAL_MS);
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_t.cfg.server);
    c.wait_for_sync = false;  // nothing waits: the answer comes to sntp_sync_time()
    c.start = true;
    const esp_err_t err = esp_netif_sntp_init(&c);

    taskENTER_CRITICAL(&s_lock);
    s_t.shown.sntp_running = (err == ESP_OK);
    s_t.shown.sntp_err = (int32_t)err;
    taskEXIT_CRITICAL(&s_lock);

    const log_field_t fields[] = {
        log_str("server", s_t.cfg.server),
        log_u64("interval_s", NET_TIME_SYNC_INTERVAL_MS / MS_PER_S),
        log_i64("esp_err", (int64_t)err),
    };
    log_write((err == ESP_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR,
              (err == ESP_OK) ? "sntp started" : "sntp not started: no time from the network",
              fields, sizeof(fields) / sizeof(fields[0]));
}

// An answer: what the network said and how far the system time had been
// from it; the source to SNTP, and the RTC's sync from the beginning.
static void on_answer(uint64_t sntp_ms, uint64_t before_ms, uint64_t now_ms)
{
    const wall_clock_source_t was = *s_t.cfg.wall_source;
    *s_t.cfg.wall_source = WALL_CLOCK_SOURCE_SNTP;

    taskENTER_CRITICAL(&s_lock);
    const uint32_t syncs = ++s_t.shown.syncs;
    s_t.shown.last_sync_ms = now_ms;
    s_t.shown.last_sync_utc_ms = sntp_ms;
    s_t.shown.step_known = before_ms != 0u;
    s_t.shown.system_minus_sntp_ms = (int64_t)before_ms - (int64_t)sntp_ms;
    taskEXIT_CRITICAL(&s_lock);

    char utc[UTC_MS_TEXT_LEN];
    format_utc_ms(utc, sizeof(utc), sntp_ms);
    const log_field_t fields[] = {
        log_str("utc", utc),
        (before_ms != 0u) ? log_i64("system_minus_sntp_ms", (int64_t)before_ms - (int64_t)sntp_ms)
                          : log_str("system_minus_sntp_ms", "none"),
        log_str("source_was", wall_clock_source_name(was)),
        log_u64("syncs", syncs),
    };
    log_write(LOG_LEVEL_INFO, "synced", fields, sizeof(fields) / sizeof(fields[0]));
    rtc_sync_begin(&s_t.sync);
}

static void on_refused(uint64_t sntp_ms)
{
    taskENTER_CRITICAL(&s_lock);
    const uint32_t refused = ++s_t.shown.refused;
    taskEXIT_CRITICAL(&s_lock);
    const log_field_t fields[] = {
        log_u64("server_unix_s", sntp_ms / MS_PER_S),
        log_u64("refused", refused),
    };
    log_write(LOG_LEVEL_WARN, "time refused: before 2026 or from 2100 on, nothing set", fields,
              sizeof(fields) / sizeof(fields[0]));
}

static void log_compared(const rtc_sync_note_t *n)
{
    const rtc_sync_tick_t *mark = &s_t.sync.mark;  // still the last write's
    const bool since_known = mark->known && n->tick.wall_ms >= mark->wall_ms;
    const log_field_t fields[] = {
        log_i64("rtc_minus_sntp_ms", n->tick.rtc_minus_wall_ms),
        log_u64("uncertainty_ms", n->tick.uncertainty_ms),
        since_known ? log_u64("since_write_s", (n->tick.wall_ms - mark->wall_ms) / MS_PER_S)
                    : log_str("since_write_s", "-"),
        n->drift.known ? log_f32("drift_ppm", n->drift.ppm) : log_str("drift_ppm", "-"),
        n->drift.known ? log_f32("drift_uncertainty_ppm", n->drift.uncertainty_ppm)
                       : log_str("drift_uncertainty_ppm", "-"),
    };
    log_write(LOG_LEVEL_INFO, "rtc against sntp", fields, sizeof(fields) / sizeof(fields[0]));
}

static void log_written(const rtc_sync_note_t *n)
{
    // The time at the first tick: the second written, plus the one it ticks.
    char utc[UTC_MS_TEXT_LEN];
    format_utc_ms(utc, sizeof(utc), n->second_ms + MS_PER_S);
    const log_field_t fields[] = {
        log_str("utc", utc),
        log_i64("first_tick_ms", n->first_tick_ms),
        log_i64("start_late_ms", n->start_late_ms),
        log_u64("stopped_ms", n->stopped_ms),
        log_u64("rearms", n->rearms),
        log_u64("start_window_ms", n->start_window_ms),
    };
    log_write((n->status == HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_WARN,
              (n->status == HAL_OK) ? "rtc written" : "rtc written late: its moment missed", fields,
              sizeof(fields) / sizeof(fields[0]));
}

static void log_landed(const rtc_sync_note_t *n)
{
    const log_field_t fields[] = {
        log_i64("residual_ms", n->tick.rtc_minus_wall_ms),
        log_u64("uncertainty_ms", n->tick.uncertainty_ms),
        log_i64("first_tick_ms", n->first_tick_ms),
        log_bool("learned", n->first_tick_believed),
    };
    log_write(LOG_LEVEL_INFO, "rtc landed", fields, sizeof(fields) / sizeof(fields[0]));
}

// One note to the log and to what the console shows.
static void take_note(const rtc_sync_note_t *n)
{
    switch (n->kind) {
        case RTC_SYNC_NOTE_COMPARED:
            log_compared(n);
            break;
        case RTC_SYNC_NOTE_NOT_COMPARED: {
            const log_field_t fields[] = {log_str("why", n->why)};
            log_write(LOG_LEVEL_WARN, "rtc against sntp: nothing to compare", fields, 1u);
            break;
        }
        case RTC_SYNC_NOTE_WRITTEN:
            log_written(n);
            break;
        case RTC_SYNC_NOTE_WRITE_FAILED: {
            const log_field_t fields[] = {
                log_str("why", n->why),
                log_i64("status", (int64_t)n->status),
            };
            log_write(LOG_LEVEL_ERROR, "rtc write failed", fields,
                      sizeof(fields) / sizeof(fields[0]));
            break;
        }
        case RTC_SYNC_NOTE_LANDED:
            log_landed(n);
            break;
        case RTC_SYNC_NOTE_NOT_LANDED: {
            const log_field_t fields[] = {log_str("why", n->why)};
            log_write(LOG_LEVEL_WARN, "rtc landed: not seen", fields, 1u);
            break;
        }
    }

    taskENTER_CRITICAL(&s_lock);
    net_time_status_t *st = &s_t.shown;
    switch (n->kind) {
        case RTC_SYNC_NOTE_COMPARED:
            st->offset_known = true;
            st->rtc_minus_sntp_ms = n->tick.rtc_minus_wall_ms;
            st->offset_uncertainty_ms = n->tick.uncertainty_ms;
            st->drift_known = n->drift.known;
            st->drift_ppm = n->drift.ppm;
            st->drift_uncertainty_ppm = n->drift.uncertainty_ppm;
            break;
        case RTC_SYNC_NOTE_NOT_COMPARED:
            st->offset_known = false;
            st->drift_known = false;
            break;
        case RTC_SYNC_NOTE_WRITTEN:
            st->writes++;
            break;
        case RTC_SYNC_NOTE_WRITE_FAILED:
            st->write_failures++;
            break;
        case RTC_SYNC_NOTE_LANDED:
            st->landed_known = true;
            st->residual_ms = n->tick.rtc_minus_wall_ms;
            break;
        case RTC_SYNC_NOTE_NOT_LANDED:
            st->landed_known = false;
            break;
    }
    taskEXIT_CRITICAL(&s_lock);
}

hal_status_t net_time_init(const net_time_config_t *cfg)
{
    memset(&s_t, 0, sizeof(s_t));
    s_t.cfg = *cfg;
    s_t.shown.server = cfg->server;
    rtc_sync_init(&s_t.sync, cfg->bus, cfg->rtc_address, cfg->clock);
    s_t.shown.first_tick_ms = s_t.sync.first_tick_ms;

    // A reset in a sync leaves the alarm armed and INT low: a later arming
    // would see no edge fall.
    const hal_status_t off = pcf8563_alarm_off(cfg->bus, cfg->rtc_address);
    (void)board_rtc_int_start(cfg->log);

    const esp_err_t err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                              on_got_ip, NULL, &s_t.ip_handler);
    if (err != ESP_OK) {
        const log_field_t fields[] = {log_i64("esp_err", (int64_t)err)};
        log_write(LOG_LEVEL_ERROR, "cannot wait for an address: no time from the network", fields,
                  1u);
        return HAL_ERR_IO;
    }
    s_t.initialised = true;
    s_t.shown.started = true;
    const log_field_t fields[] = {
        log_str("server", cfg->server),
        log_u64("interval_s", NET_TIME_SYNC_INTERVAL_MS / MS_PER_S),
        log_i64("rtc_alarm_off", (int64_t)off),
    };
    log_write(LOG_LEVEL_INFO, "waiting for the network's first address", fields,
              sizeof(fields) / sizeof(fields[0]));
    return HAL_OK;
}

uint64_t net_time_step(void *ctx, uint64_t now_ms)
{
    (void)ctx;
    if (!s_t.initialised) {
        return RTC_SYNC_NO_DEADLINE;
    }
    taskENTER_CRITICAL(&s_lock);
    const mailbox_t box = s_t.box;
    s_t.box.got_ip = false;  // a later address tries again, if this start fails
    s_t.box.answer = false;
    s_t.box.refused = false;
    s_t.box.forget = false;
    const bool sntp_running = s_t.shown.sntp_running;
    taskEXIT_CRITICAL(&s_lock);

    if (box.forget) {
        rtc_sync_abort(&s_t.sync);
    }
    if (box.got_ip && !sntp_running) {
        start_sntp();
    }
    if (box.refused) {
        on_refused(box.refused_ms);
    }
    if (box.answer) {
        on_answer(box.sntp_ms, box.before_ms, now_ms);
    }

    board_rtc_int_edge_t stamped;
    board_rtc_int_edge(&stamped);
    // esp_timer's microseconds to port_clock's now_ms(), which is the same
    // counter in milliseconds (adp_clock.c).
    const rtc_sync_edge_t edge = {.count = stamped.count, .at_ms = stamped.at_us / US_PER_MS};
    rtc_sync_note_t notes[RTC_SYNC_NOTES_MAX];
    size_t n = 0u;
    const uint64_t due = rtc_sync_step(&s_t.sync, &edge, now_ms, notes, &n);
    for (size_t i = 0u; i < n; ++i) {
        take_note(&notes[i]);
    }

    taskENTER_CRITICAL(&s_lock);
    s_t.shown.phase = s_t.sync.phase;
    s_t.shown.int_edges = stamped.count;
    s_t.shown.first_tick_ms = s_t.sync.first_tick_ms;
    s_t.shown.first_tick_learned = s_t.sync.first_tick_learned;
    taskEXIT_CRITICAL(&s_lock);
    return due;
}

void net_time_forget_rtc_mark(void)
{
    if (!s_t.initialised) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s_t.box.forget = true;
    taskEXIT_CRITICAL(&s_lock);
    wake();
}

void net_time_status(net_time_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_t.shown;
    taskEXIT_CRITICAL(&s_lock);
    out->int_low = s_t.initialised && board_rtc_int_low();
}
