// SPDX-License-Identifier: Apache-2.0

#include "adp_net.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define TAG "adp_net"

// adp_net_pure.h names the driver's reasons without its headers; here they
// are held to ESP-IDF's own.
_Static_assert(ADP_NET_REASON_UNSPECIFIED == WIFI_REASON_UNSPECIFIED, "reason");
_Static_assert(ADP_NET_REASON_AUTH_EXPIRE == WIFI_REASON_AUTH_EXPIRE, "reason");
_Static_assert(ADP_NET_REASON_AUTH_LEAVE == WIFI_REASON_AUTH_LEAVE, "reason");
_Static_assert(ADP_NET_REASON_DISASSOC_DUE_TO_INACTIVITY == WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY,
               "reason");
_Static_assert(ADP_NET_REASON_ASSOC_TOOMANY == WIFI_REASON_ASSOC_TOOMANY, "reason");
_Static_assert(ADP_NET_REASON_ASSOC_LEAVE == WIFI_REASON_ASSOC_LEAVE, "reason");
_Static_assert(ADP_NET_REASON_MIC_FAILURE == WIFI_REASON_MIC_FAILURE, "reason");
_Static_assert(ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT,
               "reason");
_Static_assert(ADP_NET_REASON_802_1X_AUTH_FAILED == WIFI_REASON_802_1X_AUTH_FAILED, "reason");
_Static_assert(ADP_NET_REASON_BEACON_TIMEOUT == WIFI_REASON_BEACON_TIMEOUT, "reason");
_Static_assert(ADP_NET_REASON_NO_AP_FOUND == WIFI_REASON_NO_AP_FOUND, "reason");
_Static_assert(ADP_NET_REASON_AUTH_FAIL == WIFI_REASON_AUTH_FAIL, "reason");
_Static_assert(ADP_NET_REASON_ASSOC_FAIL == WIFI_REASON_ASSOC_FAIL, "reason");
_Static_assert(ADP_NET_REASON_HANDSHAKE_TIMEOUT == WIFI_REASON_HANDSHAKE_TIMEOUT, "reason");
_Static_assert(ADP_NET_REASON_CONNECTION_FAIL == WIFI_REASON_CONNECTION_FAIL, "reason");
_Static_assert(ADP_NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ==
                   WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY,
               "reason");
_Static_assert(ADP_NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ==
                   WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD,
               "reason");
_Static_assert(ADP_NET_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD ==
                   WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD,
               "reason");

// The Wi-Fi driver's code stays in flash (sdkconfig.defaults): its IRAM
// optimisations cost 17.6 KB of internal RAM the board needs more than a
// message a minute needs the throughput (docs/adr/0032-wifi-on-the-board.md,
// "Its buffers in internal RAM"). A choice in sdkconfig.defaults does not reach
// an sdkconfig that already exists (CLAUDE.md), so the build refuses rather
// than the heap running short.
#if CONFIG_ESP_WIFI_IRAM_OPT || CONFIG_ESP_WIFI_RX_IRAM_OPT
#error "adp_net expects the Wi-Fi IRAM optimisations off (sdkconfig.defaults) -- delete this \
build's sdkconfig, or run idf.py reconfigure, so the default reaches it"
#endif

// The station's fields are the driver's sizes: a name that fills them has no
// terminator there, which the driver allows.
_Static_assert(sizeof(((wifi_sta_config_t *)0)->ssid) == PORT_NET_SSID_MAX, "ssid");
_Static_assert(sizeof(((wifi_sta_config_t *)0)->password) == ADP_NET_PASSWORD_MAX, "password");

// The callers, ESP-IDF's event task and the adapter's task share what the
// port shows under this lock, held for a few bytes' copy at a time. One
// station, one lock.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_station_taken;

// Set by the shutdown handler: the chip is restarting through esp_restart()
// -- the console's restart, phase 6's OTA; a panic resets without the
// handlers -- and ESP-IDF's own shutdown handler is about to stop the
// driver. What the driver says from then on is its stop,
// not a lost link: nothing is sought again, nothing is logged.
static volatile bool s_shutting_down;

typedef enum {
    MSG_WAKE = 0,      // a request: look at `wanted`
    MSG_STARTED,       // WIFI_EVENT_STA_START
    MSG_ASSOCIATED,    // WIFI_EVENT_STA_CONNECTED
    MSG_DISCONNECTED,  // WIFI_EVENT_STA_DISCONNECTED
    MSG_GOT_IP,        // IP_EVENT_STA_GOT_IP
    MSG_LOST_IP,       // IP_EVENT_STA_LOST_IP
} msg_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t reason;
    uint8_t channel;
    int8_t rssi;
    uint8_t bssid[ADP_NET_MAC_LEN];
    uint8_t ip[PORT_NET_IPV4_LEN];
    uint8_t gateway[PORT_NET_IPV4_LEN];
} net_msg_t;

// The task's own copies of what it publishes.
typedef struct {
    uint8_t ip[PORT_NET_IPV4_LEN];
    uint8_t gateway[PORT_NET_IPV4_LEN];
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[ADP_NET_MAC_LEN];
    uint32_t stack_free_min_bytes;
} task_view_t;

static task_view_t s_view;

static uint64_t now_ms(const adp_net_t *a)
{
    return a->cfg.clock->now_ms(a->cfg.clock);
}

static void log_write(const adp_net_t *a, log_level_t level, const char *message,
                      const log_field_t *fields, size_t count)
{
    port_log_write(a->cfg.log, level, TAG, message, fields, count);
}

static hal_status_t status_of(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return HAL_OK;
        case ESP_ERR_NO_MEM:
            return HAL_ERR_NO_MEM;
        case ESP_ERR_INVALID_ARG:
            return HAL_ERR_INVALID_ARG;
        default:
            return HAL_ERR_IO;
    }
}

static void octets(const esp_ip4_addr_t *ip, uint8_t out[PORT_NET_IPV4_LEN])
{
    for (size_t i = 0u; i < PORT_NET_IPV4_LEN; ++i) {
        out[i] = esp_ip4_addr_get_byte(ip, i);
    }
}

// "192.0.2.10" -- for the log, which keeps no fixed-point text of its own.
#define ADDRESS_TEXT_MAX 20u
// "aa:bb:cc:dd:ee:ff"
#define MAC_TEXT_MAX 20u

static void address_text(char *dst, size_t cap, const uint8_t ip[PORT_NET_IPV4_LEN])
{
    (void)snprintf(dst, cap, "%u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2],
                   (unsigned)ip[3]);
}

static void mac_text(char *dst, size_t cap, const uint8_t mac[ADP_NET_MAC_LEN])
{
    (void)snprintf(dst, cap, "%02x:%02x:%02x:%02x:%02x:%02x", (unsigned)mac[0], (unsigned)mac[1],
                   (unsigned)mac[2], (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

// The driver may report a slightly positive RSSI for a very strong signal
// (its header says so); the port's range is -127..-1.
static int8_t clamp_rssi(int rssi)
{
    if (rssi >= 0) {
        return -1;
    }
    return (rssi < -127) ? (int8_t)-127 : (int8_t)rssi;
}

// --- what the port shows ---------------------------------------------------------

// Under s_lock. What was asked, until the task has acted on it.
static net_state_t shown_state_locked(const adp_net_t *a)
{
    if (a->requests != a->served) {
        if (!a->wanted) {
            return NET_DOWN;
        }
        return (a->shown.state == NET_UP) ? NET_UP : NET_CONNECTING;
    }
    return a->shown.state;
}

static net_state_t net_state(const port_net_t *port)
{
    adp_net_t *a = (adp_net_t *)port->impl;
    portENTER_CRITICAL(&s_lock);
    const net_state_t s = shown_state_locked(a);
    portEXIT_CRITICAL(&s_lock);
    return s;
}

static int8_t net_rssi(const port_net_t *port)
{
    adp_net_t *a = (adp_net_t *)port->impl;
    portENTER_CRITICAL(&s_lock);
    const int8_t rssi =
        (shown_state_locked(a) == NET_UP) ? a->shown.rssi_dbm : (int8_t)PORT_NET_RSSI_NONE;
    portEXIT_CRITICAL(&s_lock);
    return rssi;
}

static void net_info(const port_net_t *port, net_info_t *out)
{
    adp_net_t *a = (adp_net_t *)port->impl;
    memcpy(out->ssid, a->ssid, sizeof(out->ssid));  // fixed since init
    portENTER_CRITICAL(&s_lock);
    if (shown_state_locked(a) == NET_UP) {
        memcpy(out->ipv4, a->shown.ipv4, sizeof(out->ipv4));
    } else {
        memset(out->ipv4, 0, sizeof(out->ipv4));
    }
    portEXIT_CRITICAL(&s_lock);
}

// A message for the task; one that does not fit is counted. A request's
// wake-up need not fit: the task, busy with what fills the queue, looks at
// the requests every time it wakes.
static void post(adp_net_t *a, const net_msg_t *m)
{
    if (a->queue == NULL) {
        return;
    }
    if (xQueueSend((QueueHandle_t)a->queue, m, 0) != pdTRUE && m->kind != MSG_WAKE) {
        portENTER_CRITICAL(&s_lock);
        a->shown.events_dropped++;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void request(adp_net_t *a, bool wanted)
{
    portENTER_CRITICAL(&s_lock);
    a->wanted = wanted;
    a->requests++;
    portEXIT_CRITICAL(&s_lock);
    net_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = MSG_WAKE;
    post(a, &m);
}

static hal_status_t net_up(port_net_t *port)
{
    adp_net_t *a = (adp_net_t *)port->impl;
    if (a->init_status != HAL_OK) {
        portENTER_CRITICAL(&s_lock);
        a->shown.state = NET_FAILED;
        portEXIT_CRITICAL(&s_lock);
        return HAL_ERR_NOT_READY;
    }
    request(a, true);
    return HAL_OK;
}

static hal_status_t net_down(port_net_t *port)
{
    adp_net_t *a = (adp_net_t *)port->impl;
    if (a->init_status != HAL_OK) {
        portENTER_CRITICAL(&s_lock);
        a->shown.state = NET_DOWN;
        portEXIT_CRITICAL(&s_lock);
        return HAL_OK;
    }
    request(a, false);
    return HAL_OK;
}

// --- ESP-IDF's events, on its event task ----------------------------------------

// esp_restart() calls the shutdown handlers last registered first, so this
// one, registered after esp_netif_create_default_wifi_sta() registered
// esp_wifi_stop(), runs before the driver stops.
static void on_shutdown(void)
{
    s_shutting_down = true;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    if (s_shutting_down) {
        return;
    }
    adp_net_t *a = (adp_net_t *)arg;
    net_msg_t m;
    memset(&m, 0, sizeof(m));
    if (id == WIFI_EVENT_STA_START) {
        m.kind = MSG_STARTED;
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)data;
        m.kind = MSG_ASSOCIATED;
        m.channel = e->channel;
        memcpy(m.bssid, e->bssid, sizeof(m.bssid));
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        m.kind = MSG_DISCONNECTED;
        m.reason = e->reason;
        m.rssi = e->rssi;
    } else {
        return;
    }
    post(a, &m);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    if (s_shutting_down) {
        return;
    }
    adp_net_t *a = (adp_net_t *)arg;
    net_msg_t m;
    memset(&m, 0, sizeof(m));
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        m.kind = MSG_GOT_IP;
        octets(&e->ip_info.ip, m.ip);
        octets(&e->ip_info.gw, m.gateway);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        m.kind = MSG_LOST_IP;
    } else {
        return;
    }
    post(a, &m);
}

// --- the task ----------------------------------------------------------------------

static void log_memory(const adp_net_t *a, const char *message, uint64_t took_ms, esp_err_t err)
{
    const log_field_t fields[] = {
        log_i64("esp_err", (int64_t)err),
        log_u64("took_ms", took_ms),
        log_u64("free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        log_u64("min_free_internal", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        log_u64("free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
    };
    log_write(a, (err == ESP_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, message, fields,
              sizeof(fields) / sizeof(fields[0]));
}

// The radio on: the RF calibration's current, the reason up() waits for the
// first frame (adp_net.h).
static adp_net_action_t start_radio(adp_net_t *a)
{
    const uint64_t start = now_ms(a);
    const esp_err_t err = esp_wifi_start();
    log_memory(a, (err == ESP_OK) ? "radio started" : "radio did not start: given up until up()",
               now_ms(a) - start, err);
    return (err == ESP_OK) ? ADP_NET_ACT_NONE
                           : adp_net_machine_started(&a->machine, false, now_ms(a));
}

// What the machine answered, done; what that answers in turn, done too.
static void act(adp_net_t *a, adp_net_action_t action)
{
    // A chain is two long at most: an error's own answer is never another
    // call into the driver.
    for (unsigned guard = 0u; action != ADP_NET_ACT_NONE && guard < 4u; ++guard) {
        adp_net_action_t next = ADP_NET_ACT_NONE;
        esp_err_t err = ESP_OK;
        switch (action) {
            case ADP_NET_ACT_START:
                next = start_radio(a);
                break;
            case ADP_NET_ACT_CONNECT:
                err = esp_wifi_connect();
                if (err != ESP_OK) {
                    const log_field_t fields[] = {log_i64("esp_err", (int64_t)err)};
                    log_write(a, LOG_LEVEL_WARN, "esp_wifi_connect refused: a failed attempt",
                              fields, 1u);
                    next = adp_net_machine_connect_error(&a->machine, now_ms(a));
                }
                break;
            case ADP_NET_ACT_DISCONNECT:
                (void)esp_wifi_disconnect();
                break;
            case ADP_NET_ACT_STOP:
                err = esp_wifi_stop();
                {
                    const log_field_t fields[] = {log_i64("esp_err", (int64_t)err)};
                    log_write(a, LOG_LEVEL_INFO, "radio stopped", fields, 1u);
                }
                break;
            case ADP_NET_ACT_NONE:
                break;
        }
        action = next;
    }
}

// An attempt that ended without an address. The first of a run, and any
// whose reason differs from the last one logged, at INFO; the rest at DEBUG:
// an access point gone for a day is two lines in the card's log, not
// thousands.
static void log_failure(adp_net_t *a, uint16_t reason, int8_t rssi)
{
    const adp_net_machine_stats_t *st = &a->machine.stats;
    const bool first = st->failures_in_a_row == 1u;
    const log_level_t level =
        (first || reason != a->logged_reason) ? LOG_LEVEL_INFO : LOG_LEVEL_DEBUG;
    if (level == LOG_LEVEL_INFO) {
        a->logged_reason = reason;
    }
    const uint64_t now = now_ms(a);
    const uint64_t next = adp_net_machine_next_ms(&a->machine);
    const log_field_t fields[] = {
        log_str("reason", adp_net_reason_name(reason)),
        log_u64("code", reason),
        log_i64("rssi", rssi),
        log_u64("in_a_row", st->failures_in_a_row),
        log_u64("refused_in_a_row", st->refusals_in_a_row),
        log_u64("next_in_ms", (next > now && next != ADP_NET_NO_DEADLINE) ? next - now : 0u),
    };
    log_write(a, level,
              (st->refusals_in_a_row >= 2u && adp_net_reason_is_refusal(reason))
                  ? "attempt refused: is the password right?"
                  : "attempt failed",
              fields, sizeof(fields) / sizeof(fields[0]));
}

static void read_rssi(adp_net_t *a, uint64_t now)
{
    int rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
        s_view.rssi = clamp_rssi(rssi);
    }
    a->rssi_due_ms = now + ADP_NET_RSSI_REFRESH_MS;
}

static void handle(adp_net_t *a, const net_msg_t *m)
{
    const uint64_t now = now_ms(a);
    const adp_net_phase_t before = a->machine.phase;
    switch ((msg_kind_t)m->kind) {
        case MSG_WAKE:
            break;
        case MSG_STARTED:
            act(a, adp_net_machine_started(&a->machine, true, now));
            break;
        case MSG_ASSOCIATED:
            adp_net_machine_associated(&a->machine, now);
            if (a->machine.phase == ADP_NET_PHASE_JOINING) {
                s_view.channel = m->channel;
                memcpy(s_view.bssid, m->bssid, sizeof(s_view.bssid));
                char bssid[MAC_TEXT_MAX];
                mac_text(bssid, sizeof(bssid), m->bssid);
                const log_field_t fields[] = {
                    log_str("bssid", bssid),
                    log_u64("channel", m->channel),
                    log_u64("attempt", a->machine.stats.failures_in_a_row + 1u),
                };
                log_write(a, LOG_LEVEL_INFO, "associated", fields,
                          sizeof(fields) / sizeof(fields[0]));
            }
            break;
        case MSG_DISCONNECTED: {
            const uint64_t up_since = a->machine.stats.up_since_ms;
            act(a, adp_net_machine_disconnected(&a->machine, m->reason, now));
            if (before == ADP_NET_PHASE_UP) {
                const log_field_t fields[] = {
                    log_str("reason", adp_net_reason_name(m->reason)),
                    log_u64("code", m->reason),
                    log_i64("rssi", m->rssi),
                    log_u64("was_up_ms", now - up_since),
                    log_u64("losses", a->machine.stats.losses),
                };
                log_write(a, LOG_LEVEL_WARN, "link lost: sought again at once", fields,
                          sizeof(fields) / sizeof(fields[0]));
                a->logged_reason = ADP_NET_REASON_NONE;
            } else if (before == ADP_NET_PHASE_JOINING) {
                log_failure(a, m->reason, m->rssi);
            }
            break;
        }
        case MSG_GOT_IP: {
            const uint64_t sought_since = a->machine.stats.sought_since_ms;
            const uint32_t failed = a->machine.stats.failures_in_a_row;
            act(a, adp_net_machine_got_address(&a->machine, now));
            if (a->machine.phase != ADP_NET_PHASE_UP) {
                break;  // not this attempt's
            }
            const bool changed = memcmp(s_view.ip, m->ip, sizeof(s_view.ip)) != 0;
            memcpy(s_view.ip, m->ip, sizeof(s_view.ip));
            memcpy(s_view.gateway, m->gateway, sizeof(s_view.gateway));
            if (before == ADP_NET_PHASE_UP && !changed) {
                break;  // a lease renewed
            }
            read_rssi(a, now);
            char address[ADDRESS_TEXT_MAX];
            char gateway[ADDRESS_TEXT_MAX];
            address_text(address, sizeof(address), m->ip);
            address_text(gateway, sizeof(gateway), m->gateway);
            // Five fields at most: a sixth of this length lays the list out
            // in columns, which clang-format's versions do not agree on.
            // The channel is in the "associated" line just before.
            const log_field_t fields[] = {
                log_str("address", address),
                log_str("gateway", gateway),
                log_i64("rssi", s_view.rssi),
                log_u64("failed_attempts", failed),
                log_u64("took_ms", now - sought_since),
            };
            log_write(a, LOG_LEVEL_INFO, (before == ADP_NET_PHASE_UP) ? "address changed" : "up",
                      fields, sizeof(fields) / sizeof(fields[0]));
            if (a->machine.stats.connections == 1u && before != ADP_NET_PHASE_UP) {
                // What the link costs once it carries traffic: the figure
                // board_notes.md compares with phase 3g's 176,143.
                const log_field_t memory[] = {
                    log_u64("free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                    log_u64("min_free_internal",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                    log_u64("free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                };
                log_write(a, LOG_LEVEL_INFO, "memory with the link up", memory,
                          sizeof(memory) / sizeof(memory[0]));
            }
            a->logged_reason = ADP_NET_REASON_NONE;
            break;
        }
        case MSG_LOST_IP:
            act(a, adp_net_machine_lost_address(&a->machine, now));
            if (before == ADP_NET_PHASE_UP) {
                log_write(a, LOG_LEVEL_WARN, "address lost: waiting for it again", NULL, 0u);
            }
            break;
    }
}

// up() and down(), as asked since the task last looked: the latest wins.
// Returns the request count it acted on.
static uint32_t serve_requests(adp_net_t *a)
{
    portENTER_CRITICAL(&s_lock);
    const bool wanted = a->wanted;
    const uint32_t requests = a->requests;
    const uint32_t served = a->served;
    portEXIT_CRITICAL(&s_lock);
    if (requests == served) {
        return served;
    }
    const uint64_t now = now_ms(a);
    const adp_net_phase_t before = a->machine.phase;
    if (wanted) {
        if (before == ADP_NET_PHASE_IDLE || before == ADP_NET_PHASE_FAILED) {
            const log_field_t fields[] = {log_str("from", adp_net_phase_name(before))};
            log_write(a, LOG_LEVEL_INFO, "asked up: starting the radio", fields, 1u);
            a->logged_reason = ADP_NET_REASON_NONE;
        }
        act(a, adp_net_machine_up(&a->machine, now));
    } else {
        act(a, adp_net_machine_down(&a->machine, now));
    }
    return requests;
}

static void track_stack(adp_net_t *a)
{
    // Bytes on ESP-IDF's Xtensa port (event_loop.c).
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (free_bytes >= s_view.stack_free_min_bytes) {
        return;
    }
    s_view.stack_free_min_bytes = free_bytes;
    const log_field_t fields[] = {log_u64("stack_free_min_bytes", free_bytes)};
    log_write(a, LOG_LEVEL_INFO, "stack low-water mark", fields, 1u);
}

// What the task decided, where the port and the console read it; the event
// loop woken when the state shown changed.
static void publish(adp_net_t *a, uint32_t served)
{
    const net_state_t state = adp_net_machine_state(&a->machine);
    const bool up = state == NET_UP;
    portENTER_CRITICAL(&s_lock);
    a->served = served;
    a->shown.state = state;
    a->shown.phase = a->machine.phase;
    a->shown.stats = a->machine.stats;
    a->shown.next_attempt_ms = (a->machine.phase == ADP_NET_PHASE_WAITING)
                                   ? a->machine.deadline_ms
                                   : (uint64_t)ADP_NET_NO_DEADLINE;
    if (up) {
        memcpy(a->shown.ipv4, s_view.ip, sizeof(a->shown.ipv4));
        memcpy(a->shown.gateway, s_view.gateway, sizeof(a->shown.gateway));
        a->shown.rssi_dbm = s_view.rssi;
    } else {
        memset(a->shown.ipv4, 0, sizeof(a->shown.ipv4));
        memset(a->shown.gateway, 0, sizeof(a->shown.gateway));
        a->shown.rssi_dbm = (int8_t)PORT_NET_RSSI_NONE;
    }
    a->shown.channel = s_view.channel;
    memcpy(a->shown.bssid, s_view.bssid, sizeof(a->shown.bssid));
    a->shown.stack_free_min_bytes = s_view.stack_free_min_bytes;
    const net_state_t effective = shown_state_locked(a);
    portEXIT_CRITICAL(&s_lock);

    if (effective != a->woken_for) {
        a->woken_for = effective;
        if (a->waker != NULL) {
            a->waker(a->waker_ctx);
        }
    }
}

static TickType_t ticks_until(const adp_net_t *a, uint64_t deadline)
{
    if (deadline == ADP_NET_NO_DEADLINE) {
        return portMAX_DELAY;
    }
    const uint64_t now = now_ms(a);
    const uint64_t ms = (deadline > now) ? deadline - now : 0u;
    const uint32_t capped = (ms > UINT32_MAX / 2u) ? UINT32_MAX / 2u : (uint32_t)ms;
    // Whole ticks, rounded up, and at least one: a deadline is never early.
    return (TickType_t)(pdMS_TO_TICKS(capped) + 1u);
}

static void net_task(void *arg)
{
    adp_net_t *a = (adp_net_t *)arg;
    s_view.stack_free_min_bytes = UINT32_MAX;
    s_view.rssi = (int8_t)PORT_NET_RSSI_NONE;
    uint64_t deadline = ADP_NET_NO_DEADLINE;
    for (;;) {
        net_msg_t msg;
        const bool got =
            xQueueReceive((QueueHandle_t)a->queue, &msg, ticks_until(a, deadline)) == pdTRUE;
        if (s_shutting_down) {
            deadline = ADP_NET_NO_DEADLINE;  // the chip resets in a moment
            continue;
        }
        // The requests first: a down() makes whatever the driver says next
        // moot (adp_net_pure.h).
        const uint32_t served = serve_requests(a);
        if (got) {
            handle(a, &msg);
        }
        const uint64_t now = now_ms(a);
        const adp_net_phase_t before = a->machine.phase;
        const uint32_t timeouts = a->machine.stats.timeouts;
        act(a, adp_net_machine_tick(&a->machine, now));
        if (a->machine.stats.timeouts != timeouts) {
            log_failure(a, ADP_NET_REASON_ATTEMPT_TIMEOUT, (int8_t)PORT_NET_RSSI_NONE);
        } else if (before == ADP_NET_PHASE_STARTING && a->machine.phase == ADP_NET_PHASE_FAILED) {
            log_write(a, LOG_LEVEL_ERROR, "the radio's start event never came: given up until up()",
                      NULL, 0u);
        }
        if (a->machine.phase == ADP_NET_PHASE_UP && now >= a->rssi_due_ms) {
            read_rssi(a, now);
        }
        publish(a, served);

        deadline = adp_net_machine_next_ms(&a->machine);
        if (a->machine.phase == ADP_NET_PHASE_UP && a->rssi_due_ms < deadline) {
            deadline = a->rssi_due_ms;
        }
        track_stack(a);
    }
}

// --- set-up --------------------------------------------------------------------------

static hal_status_t check_config(const adp_net_t *a, const adp_net_config_t *cfg)
{
    const size_t ssid_len = (cfg->ssid != NULL) ? strlen(cfg->ssid) : 0u;
    const size_t pass_len = (cfg->password != NULL) ? strlen(cfg->password) : 0u;
    const char *why = NULL;
    if (ssid_len == 0u) {
        why = "no SSID: nothing to join";
    } else if (ssid_len > PORT_NET_SSID_MAX) {
        why = "the SSID is longer than 32 octets";
    } else if (pass_len > ADP_NET_PASSWORD_MAX) {
        why = "the password is longer than 64 characters";
    } else if (pass_len > 0u && pass_len < ADP_NET_PASSWORD_MIN) {
        why = "the password is shorter than 8 characters, which WPA2 does not take";
    }
    if (why != NULL) {
        const log_field_t fields[] = {
            log_u64("ssid_len", ssid_len),
            log_u64("password_len", pass_len),
        };
        log_write(a, LOG_LEVEL_ERROR, why, fields, sizeof(fields) / sizeof(fields[0]));
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_OK;
}

// The flash's NVS partition, which ESP-IDF's PHY keeps the radio's
// calibration in: opened, never erased -- a partition this firmware cannot
// open costs a full calibration at every start, which the log says, not the
// data some later phase may keep there.
static void open_nvs(const adp_net_t *a)
{
    const esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        const log_field_t fields[] = {log_i64("esp_err", (int64_t)err)};
        log_write(a, LOG_LEVEL_WARN,
                  "NVS not opened: the radio is calibrated in full at every start", fields, 1u);
    }
}

static esp_err_t set_up_driver(adp_net_t *a, const adp_net_config_t *cfg, const char *password)
{
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;  // someone made it already
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    a->netif = netif;
    if (cfg->hostname != NULL) {
        // A name for the router's list, not a reason to have no network.
        const esp_err_t named = esp_netif_set_hostname(netif, cfg->hostname);
        if (named != ESP_OK) {
            const log_field_t fields[] = {log_i64("esp_err", (int64_t)named)};
            log_write(a, LOG_LEVEL_WARN, "hostname not set: DHCP gives ESP-IDF's", fields, 1u);
        }
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    // The driver keeps no copy of the station's configuration in NVS
    // (adp_net.h), whatever this build's CONFIG_ESP_WIFI_NVS_ENABLED says.
    init.nvs_enable = 0;
    err = esp_wifi_init(&init);
    if (err == ESP_OK) {
        // Belt and braces over nvs_enable = 0, which already keeps the
        // configuration out of NVS: its refusal is logged, not fatal.
        const esp_err_t stored = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (stored != ESP_OK) {
            const log_field_t fields[] = {log_i64("esp_err", (int64_t)stored)};
            log_write(a, LOG_LEVEL_WARN, "esp_wifi_set_storage(RAM) refused", fields, 1u);
        }
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        return err;
    }
    // After the default handlers' esp_wifi_stop(), so that it runs first
    // (on_shutdown()). Without it a restart logs a lost link: noise, not harm.
    const esp_err_t shutdown = esp_register_shutdown_handler(on_shutdown);
    if (shutdown != ESP_OK) {
        const log_field_t fields[] = {log_i64("esp_err", (int64_t)shutdown)};
        log_write(a, LOG_LEVEL_WARN, "shutdown handler not registered", fields, 1u);
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    const size_t ssid_len = strlen(cfg->ssid);
    const size_t pass_len = strlen(password);
    memcpy(wc.sta.ssid, cfg->ssid, ssid_len);
    memcpy(wc.sta.password, password, pass_len);
    // CHOICES: every channel scanned and the strongest access point of the
    // name taken, which a home with a repeater or a mesh needs; nothing
    // weaker than WPA2 unless the network has no password at all; WPA3's
    // SAE in both its derivations, and protected management frames when the
    // network offers them.
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = (pass_len == 0u) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        return err;
    }

    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, a,
                                              &wifi_handler);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, a,
                                                  &ip_handler);
    }
    a->wifi_handler = wifi_handler;
    a->ip_handler = ip_handler;
    return err;
}

hal_status_t adp_net_init(adp_net_t *a, const adp_net_config_t *cfg)
{
    const char *password = (cfg->password != NULL) ? cfg->password : "";
    memset(a, 0, sizeof(*a));
    a->cfg = *cfg;
    a->cfg.password = NULL;  // handed to the driver below, and kept nowhere here
    a->port.up = net_up;
    a->port.down = net_down;
    a->port.state = net_state;
    a->port.rssi_dbm = net_rssi;
    a->port.info = net_info;
    a->port.impl = a;
    a->init_status = HAL_ERR_NOT_READY;
    a->shown.state = NET_DOWN;
    a->shown.rssi_dbm = (int8_t)PORT_NET_RSSI_NONE;
    a->shown.next_attempt_ms = ADP_NET_NO_DEADLINE;
    a->woken_for = NET_DOWN;
    const adp_net_machine_config_t mcfg = adp_net_machine_config_default();
    adp_net_machine_init(&a->machine, &mcfg);
    if (cfg->ssid != NULL) {
        strncpy(a->ssid, cfg->ssid, PORT_NET_SSID_MAX);  // terminated: a->ssid is one longer
    }
    (void)adp_net_station_mac(a->mac);

    if (s_station_taken) {
        log_write(a, LOG_LEVEL_ERROR, "a second station: ESP-IDF's Wi-Fi is one", NULL, 0u);
        a->init_status = HAL_ERR_BUSY;
        return a->init_status;
    }
    const hal_status_t valid = check_config(a, cfg);
    if (valid != HAL_OK) {
        a->init_status = valid;
        return valid;
    }
    s_station_taken = true;

    const uint64_t start = now_ms(a);
    open_nvs(a);
    const esp_err_t err = set_up_driver(a, cfg, password);
    if (err != ESP_OK) {
        log_memory(a, "station not set up: no Wi-Fi", now_ms(a) - start, err);
        a->init_status = status_of(err);
        return a->init_status;
    }

    a->queue = xQueueCreate(ADP_NET_QUEUE_LEN, sizeof(net_msg_t));
    TaskHandle_t task = NULL;
    if (a->queue == NULL || xTaskCreatePinnedToCore(net_task, "hac_net", cfg->task_stack_bytes, a,
                                                    (UBaseType_t)cfg->task_priority, &task,
                                                    (BaseType_t)cfg->task_core) != pdPASS) {
        log_write(a, LOG_LEVEL_ERROR, "queue or task not created: no Wi-Fi", NULL, 0u);
        a->init_status = HAL_ERR_NO_MEM;
        return a->init_status;
    }
    a->task = task;
    a->init_status = HAL_OK;

    // The name, never the password; its length says whether there is one.
    const log_field_t fields[] = {
        log_str("ssid", a->ssid),
        log_u64("password_len", strlen(password)),
        log_str("hostname", (cfg->hostname != NULL) ? cfg->hostname : "-"),
        log_u64("took_ms", now_ms(a) - start),
        log_u64("free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        log_u64("free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
    };
    log_write(a, LOG_LEVEL_INFO, "station ready, the radio off until up()", fields,
              sizeof(fields) / sizeof(fields[0]));
    return HAL_OK;
}

port_net_t *adp_net_port(adp_net_t *a)
{
    return &a->port;
}

void adp_net_set_waker(adp_net_t *a, adp_net_waker_fn fn, void *ctx)
{
    portENTER_CRITICAL(&s_lock);
    a->waker_ctx = ctx;
    a->waker = fn;
    portEXIT_CRITICAL(&s_lock);
}

hal_status_t adp_net_station_mac(uint8_t out[ADP_NET_MAC_LEN])
{
    return status_of(esp_read_mac(out, ESP_MAC_WIFI_STA));
}

void adp_net_status(adp_net_t *a, adp_net_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = a->shown;
    out->state = shown_state_locked(a);
    portEXIT_CRITICAL(&s_lock);
    out->init_status = a->init_status;
    memcpy(out->ssid, a->ssid, sizeof(out->ssid));
    memcpy(out->mac, a->mac, sizeof(out->mac));
    if (out->state != NET_UP) {
        memset(out->ipv4, 0, sizeof(out->ipv4));
        out->rssi_dbm = (int8_t)PORT_NET_RSSI_NONE;
    }
}
