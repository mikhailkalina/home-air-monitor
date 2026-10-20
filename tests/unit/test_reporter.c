// SPDX-License-Identifier: Apache-2.0
//
// core/reporting/reporter.h on a fake clock, against the loopback broker
// model and the simulated network: the first connection, a network outage and
// the reconnection, a session dropped with the network up, Home Assistant
// restarting, a refused publish, refused and unanswered attempts, the
// heartbeat, and a day's count of state messages.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adp_net_sim.h"
#include "adp_telemetry_loopback.h"
#include "fake_clock.h"
#include "hac_test.h"
#include "reporting/report_limits.h"
#include "reporting/reporter.h"

#define TS 1000u
#define CYCLE_MS SENSOR_MANAGER_DEFAULT_INTERVAL_MS
#define LOG_MAX 512u
#define SCD4X_CAPS (SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY)
#define BME68X_CAPS \
    (SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_GAS_RES)

// What the broker saw, in order (the first LOG_MAX events), and counts.
typedef struct {
    loopback_event_t event;
    char client[PORT_TELEMETRY_CLIENT_ID_MAX + 1u];
    char topic[PORT_TELEMETRY_TOPIC_MAX];
    char payload[MQTT_PAYLOAD_STATE_MAX];  // a config is cut here: only its length matters
    size_t len;
    bool retain;
    uint64_t at_ms;
} seen_t;

typedef struct {
    port_log_t port;
    unsigned warns;
    unsigned births;
} cap_log_t;

static struct {
    fake_clock_t clock;
    loopback_broker_t broker;
    adp_telemetry_loopback_t device;
    adp_telemetry_loopback_t ha;  // Home Assistant, another client of the broker
    adp_net_sim_t net;
    mqtt_topics_t topics;
    reporter_t rep;
    cap_log_t log;
    seen_t seen[LOG_MAX];
    size_t n_seen;
    unsigned states;
    unsigned configs;
    unsigned statuses;
    char last_state[MQTT_PAYLOAD_STATE_MAX];
    bool woken;
} g;

static bool ends_with(const char *s, const char *tail)
{
    const size_t n = strlen(s);
    const size_t t = strlen(tail);
    return n >= t && strcmp(s + n - t, tail) == 0;
}

static void tap(void *ctx, loopback_event_t event, const char *client, const loopback_message_t *m,
                uint64_t at_ms)
{
    (void)ctx;
    if (event == LOOPBACK_EVENT_MESSAGE && strcmp(client, "test-device") == 0) {
        if (ends_with(m->topic, "/state")) {
            g.states++;
            const size_t n = (m->len < sizeof(g.last_state)) ? m->len : sizeof(g.last_state) - 1u;
            memcpy(g.last_state, m->payload, n);
            g.last_state[n] = '\0';
        } else if (ends_with(m->topic, "/config")) {
            g.configs++;
        } else if (ends_with(m->topic, "/status")) {
            g.statuses++;
        }
    }
    if (g.n_seen >= LOG_MAX) {
        return;
    }
    seen_t *s = &g.seen[g.n_seen++];
    memset(s, 0, sizeof(*s));
    s->event = event;
    snprintf(s->client, sizeof(s->client), "%s", client);
    s->at_ms = at_ms;
    if (m != NULL) {
        snprintf(s->topic, sizeof(s->topic), "%s", m->topic);
        const size_t n = (m->len < sizeof(s->payload)) ? m->len : sizeof(s->payload) - 1u;
        memcpy(s->payload, m->payload, n);
        s->len = m->len;
        s->retain = m->retain;
    }
}

static void cap_write(const port_log_t *self, log_level_t level, const char *tag, const char *msg,
                      const log_field_t *fields, size_t n)
{
    (void)tag;
    (void)fields;
    (void)n;
    cap_log_t *l = (cap_log_t *)self->impl;
    if (level == LOG_LEVEL_WARN && msg != NULL && strstr(msg, "no session") != NULL) {
        l->warns++;
    }
    if (msg != NULL && strstr(msg, "home assistant started") != NULL) {
        l->births++;
    }
}

static void waker(void *ctx)
{
    (void)ctx;
    g.woken = true;
}

static air_reading_t reading(float co2)
{
    air_reading_t r;
    air_reading_init(&r);
    r.temperature_c = measurement_ok(22.0f, TS, 2u);
    r.humidity_rh = measurement_ok(45.0f, TS, 2u);
    r.pressure_hpa = measurement_ok(1006.0f, TS, 2u);
    r.co2_ppm = measurement_ok(co2, TS, 1u);
    r.gas_resistance_ohm = measurement_ok(150000.0f, TS, 2u);
    r.uptime_s = 60u;
    return r;
}

static reporter_deps_t deps_for(bool with_net)
{
    reporter_deps_t d;
    memset(&d, 0, sizeof(d));
    d.telemetry = adp_telemetry_loopback_port(&g.device);
    d.net = with_net ? adp_net_sim_port(&g.net) : NULL;
    d.log = &g.log.port;
    d.topics = &g.topics;
    d.discovery.device.name = "Test Monitor";
    d.discovery.caps = (sensor_caps_t)(SCD4X_CAPS | BME68X_CAPS);
    d.discovery.has_rssi = with_net;
    return d;
}

static void rig_up(bool with_net)
{
    memset(&g, 0, sizeof(g));
    fake_clock_init(&g.clock);
    const port_clock_t *clock = fake_clock_port(&g.clock);
    loopback_broker_init(&g.broker, clock);
    loopback_broker_set_tap(&g.broker, tap, NULL);
    HAC_CHECK_EQ_INT(adp_telemetry_loopback_init(&g.device, &g.broker), HAL_OK);
    HAC_CHECK_EQ_INT(adp_telemetry_loopback_init(&g.ha, &g.broker), HAL_OK);
    adp_telemetry_loopback_set_waker(&g.device, waker, NULL);
    adp_net_sim_init(&g.net, clock);
    adp_net_sim_set_listener(&g.net, adp_telemetry_loopback_net_listener, &g.device);
    g.log.port.write = cap_write;
    g.log.port.impl = &g.log;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&g.topics, "homeair", "test-device", "homeassistant"),
                     HAL_OK);
    const reporter_deps_t deps = deps_for(with_net);
    const reporter_config_t cfg = reporter_config_default();
    HAC_CHECK_EQ_INT(reporter_init(&g.rep, &deps, &cfg), HAL_OK);
}

static uint64_t now(void)
{
    return g.clock.now_ms;
}

static uint64_t step(const air_reading_t *r, reporter_note_t *note)
{
    return reporter_step(&g.rep, r, now(), note);
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
    return (a < b) ? a : b;
}

// A loop's life until `end_ms`: a step at every sensor cycle and at every
// deadline the reporter, the network or the broker names, the reading from
// `at` (NULL: the same one throughout).
static void drive(uint64_t end_ms, air_reading_t (*at)(uint64_t t_ms), const air_reading_t *fixed)
{
    for (;;) {
        const air_reading_t r = (at != NULL) ? at(now()) : *fixed;
        const uint64_t due = step(&r, NULL);
        const uint64_t cycle = (now() / CYCLE_MS + 1u) * CYCLE_MS;
        uint64_t next = min_u64(min_u64(due, cycle), adp_net_sim_next_ms(&g.net));
        next = min_u64(next, loopback_broker_next_ms(&g.broker));
        if (next <= now()) {
            next = now() + 1u;
        }
        if (next > end_ms) {
            fake_clock_advance(&g.clock, end_ms - now());
            return;
        }
        fake_clock_advance(&g.clock, next - now());
    }
}

// Brings the network up and steps until the session is there.
static void connect_now(const air_reading_t *r)
{
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    drive(now() + ADP_NET_SIM_CONNECT_MS, NULL, r);
    (void)step(r, NULL);
    HAC_CHECK_EQ_INT(g.rep.state, REPORTER_ONLINE);
}

static long co2_of(const char *state)
{
    const char *p = strstr(state, "\"co2\":");
    return (p != NULL) ? strtol(p + 6, NULL, 10) : -1;
}

// --- the first connection ---------------------------------------------------------------

static void the_first_session_sends_online_every_config_and_the_state(void)
{
    rig_up(true);
    const air_reading_t r = reading(800.0f);
    reporter_note_t note;

    // No network: nothing tried, nothing to wake for.
    HAC_CHECK_EQ_U64(step(&r, &note), REPORTER_NO_DEADLINE);
    HAC_CHECK(!note.attempted);
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    HAC_CHECK_EQ_U64(step(&r, &note), REPORTER_NO_DEADLINE);  // associating
    HAC_CHECK_EQ_INT((int)g.n_seen, 0);

    fake_clock_advance(&g.clock, ADP_NET_SIM_CONNECT_MS);
    const uint64_t due = step(&r, &note);
    HAC_CHECK(note.attempted && note.session_started && note.online_published);
    HAC_CHECK_EQ_INT((int)note.session, 1);
    HAC_CHECK_EQ_INT((int)note.discovery_published, HA_ENTITY_COUNT);
    HAC_CHECK(note.state_published);
    HAC_CHECK_EQ_INT(note.reason, PUBLISH_REASON_SESSION);
    HAC_CHECK_EQ_U64(due, ADP_NET_SIM_CONNECT_MS + REPORT_HEARTBEAT_MS);

    // On the wire: connected, online, the nine configs in order, the state.
    HAC_CHECK_EQ_INT((int)g.n_seen, 2 + HA_ENTITY_COUNT + 1);
    HAC_CHECK_EQ_INT(g.seen[0].event, LOOPBACK_EVENT_CONNECTED);
    HAC_CHECK(strcmp(g.seen[1].topic, "homeair/test-device/status") == 0 &&
              strcmp(g.seen[1].payload, "online") == 0 && g.seen[1].retain);
    for (unsigned e = 0u; e < (unsigned)HA_ENTITY_COUNT; ++e) {
        char topic[MQTT_TOPIC_MAX];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/test-device/%s/config",
                 ha_entity_object_id((ha_entity_t)e));
        const seen_t *s = &g.seen[2u + e];
        hac_test_check(strcmp(s->topic, topic) == 0 && s->retain, topic, __FILE__, __LINE__);
        // No BSEC in this build: IAQ and its accuracy get the empty config
        // that removes them.
        const bool empty = (e == HA_ENTITY_IAQ || e == HA_ENTITY_IAQ_ACCURACY);
        hac_test_check(empty == (s->len == 0u), topic, __FILE__, __LINE__);
    }
    const seen_t *state = &g.seen[2u + HA_ENTITY_COUNT];
    HAC_CHECK(strcmp(state->topic, "homeair/test-device/state") == 0 && state->retain);
    HAC_CHECK(strstr(state->payload, "\"co2\":800,") != NULL);
    HAC_CHECK(strstr(state->payload, "\"rssi\":-61,") != NULL);

    // The broker keeps the status, the state and the seven configs.
    const loopback_message_t *status =
        loopback_broker_retained(&g.broker, "homeair/test-device/status");
    HAC_CHECK(status != NULL && status->len == 6u && memcmp(status->payload, "online", 6u) == 0);
    HAC_CHECK_EQ_INT((int)g.broker.retained_count, 2 + HA_ENTITY_COUNT - 2);
    // The session's last will is "offline" on the status topic, retained.
    HAC_CHECK(g.device.has_will && strcmp(g.device.will.topic, "homeair/test-device/status") == 0 &&
              g.device.will.len == 7u && g.device.will.retain);
}

static void the_heartbeat_sends_the_state_every_minute(void)
{
    rig_up(true);
    const air_reading_t r = reading(800.0f);
    connect_now(&r);
    const uint64_t start = now();
    const unsigned before = g.states;
    drive(start + 10u * REPORT_HEARTBEAT_MS, NULL, &r);
    (void)step(&r, NULL);
    HAC_CHECK_EQ_INT((int)(g.states - before), 10);
    HAC_CHECK_EQ_INT((int)g.rep.state_by_reason[PUBLISH_REASON_HEARTBEAT], 10);
}

// --- losing it ------------------------------------------------------------------------------

static air_reading_t rising(uint64_t t_ms)
{
    return reading(600.0f + (float)(t_ms / CYCLE_MS) * 10.0f);
}

static void an_outage_publishes_nothing_and_the_next_session_everything(void)
{
    rig_up(true);
    char err[96];
    HAC_CHECK_EQ_INT(adp_net_sim_add_script(&g.net, "100:down;700:up", err, sizeof(err)), HAL_OK);
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    drive(1000000u, rising, NULL);

    HAC_CHECK_EQ_INT((int)g.rep.sessions, 2);
    HAC_CHECK_EQ_INT((int)g.rep.sessions_lost, 1);
    size_t dropped = LOG_MAX;
    size_t will = LOG_MAX;
    size_t second = LOG_MAX;
    for (size_t i = 0u; i < g.n_seen; ++i) {
        const seen_t *s = &g.seen[i];
        if (s->event == LOOPBACK_EVENT_DROPPED && dropped == LOG_MAX) {
            dropped = i;
            HAC_CHECK_EQ_U64(s->at_ms, 100000u);
        } else if (s->event == LOOPBACK_EVENT_WILL) {
            will = i;
            // One and a half keepalives after the network went.
            HAC_CHECK_EQ_U64(s->at_ms, 100000u + REPORT_KEEPALIVE_S * 1500u);
            HAC_CHECK(strcmp(s->payload, "offline") == 0 && s->retain);
        } else if (s->event == LOOPBACK_EVENT_CONNECTED && i > 0u) {
            second = i;
            HAC_CHECK_EQ_U64(s->at_ms, 700000u + ADP_NET_SIM_CONNECT_MS);
        } else if (s->event == LOOPBACK_EVENT_MESSAGE && s->at_ms > 100000u && s->at_ms < 700000u) {
            hac_test_check(false, "nothing published during the outage", s->topic, __LINE__);
        }
    }
    HAC_CHECK(dropped < will && will < second && second < LOG_MAX);
    // The second session: online, every config, and the newest state --
    // never one held from the outage.
    HAC_CHECK(strcmp(g.seen[second + 1u].payload, "online") == 0);
    unsigned configs = 0u;
    for (size_t i = second + 2u; i < second + 2u + HA_ENTITY_COUNT; ++i) {
        configs += ends_with(g.seen[i].topic, "/config") ? 1u : 0u;
    }
    HAC_CHECK_EQ_INT((int)configs, HA_ENTITY_COUNT);
    const seen_t *state = &g.seen[second + 2u + HA_ENTITY_COUNT];
    HAC_CHECK(ends_with(state->topic, "/state"));
    HAC_CHECK_EQ_INT((int)co2_of(state->payload),
                     (int)rising(700000u + ADP_NET_SIM_CONNECT_MS).co2_ppm.value);
    const loopback_message_t *status =
        loopback_broker_retained(&g.broker, "homeair/test-device/status");
    HAC_CHECK(status != NULL && status->len == 6u);
}

static void a_session_dropped_with_the_network_up_comes_back_at_once(void)
{
    rig_up(true);
    const air_reading_t r = reading(800.0f);
    connect_now(&r);
    const size_t mark = g.n_seen;

    // A TCP connection reset: the network never went down.
    adp_telemetry_loopback_set_link(&g.device, false, now());
    adp_telemetry_loopback_set_link(&g.device, true, now());
    HAC_CHECK(g.woken);
    reporter_note_t note;
    (void)step(&r, &note);
    HAC_CHECK(note.session_ended && !note.network_lost);
    HAC_CHECK(note.session_started && note.session == 2u);
    HAC_CHECK_EQ_INT((int)note.discovery_published, HA_ENTITY_COUNT);

    // The new connection took the id over: the old session's will went out
    // then, before the new "online".
    HAC_CHECK_EQ_INT(g.seen[mark].event, LOOPBACK_EVENT_DROPPED);
    HAC_CHECK_EQ_INT(g.seen[mark + 1u].event, LOOPBACK_EVENT_WILL);
    HAC_CHECK_EQ_INT(g.seen[mark + 2u].event, LOOPBACK_EVENT_CONNECTED);
    HAC_CHECK(strcmp(g.seen[mark + 3u].payload, "online") == 0);
    const loopback_message_t *status =
        loopback_broker_retained(&g.broker, "homeair/test-device/status");
    HAC_CHECK(status != NULL && memcmp(status->payload, "online", 6u) == 0);
}

// --- Home Assistant -----------------------------------------------------------------------

static void ha_publishes(const char *topic, const char *payload, bool retain)
{
    port_telemetry_t *ha = adp_telemetry_loopback_port(&g.ha);
    if (!ha->is_connected(ha)) {
        const telemetry_session_t s = {.client_id = "home-assistant", .keepalive_s = 60u};
        HAC_CHECK_EQ_INT(ha->connect(ha, &s), HAL_OK);
    }
    HAC_CHECK_EQ_INT(ha->publish(ha, topic, (const uint8_t *)payload, strlen(payload), 1u, retain),
                     HAL_OK);
}

static void home_assistant_starting_brings_discovery_again(void)
{
    rig_up(true);
    const air_reading_t r = reading(800.0f);
    connect_now(&r);
    const unsigned configs = g.configs;
    const unsigned states = g.states;

    g.woken = false;
    ha_publishes("homeassistant/status", "online", false);
    HAC_CHECK(g.woken);  // the adapter wakes the loop
    reporter_note_t note;
    (void)step(&r, &note);
    HAC_CHECK(note.birth);
    HAC_CHECK_EQ_INT((int)note.discovery_published, HA_ENTITY_COUNT);
    HAC_CHECK_EQ_INT((int)(g.configs - configs), HA_ENTITY_COUNT);
    HAC_CHECK_EQ_INT((int)(g.states - states), 0);  // the state is kept: HA reads it
    HAC_CHECK_EQ_INT((int)g.log.births, 1);

    // Its will, and a word on another topic: nothing.
    ha_publishes("homeassistant/status", "offline", false);
    ha_publishes("homeassistant/other", "online", false);
    (void)step(&r, &note);
    HAC_CHECK(!note.birth && note.discovery_published == 0u);
}

static void a_kept_birth_is_not_a_restart(void)
{
    rig_up(true);
    ha_publishes("homeassistant/status", "online", true);  // HA's birth, retained
    const air_reading_t r = reading(800.0f);
    connect_now(&r);
    reporter_note_t note;
    fake_clock_advance(&g.clock, 1000u);
    (void)step(&r, &note);
    // The subscription brought it marked retained: the session's own
    // discovery covers it.
    HAC_CHECK(!note.birth);
    HAC_CHECK_EQ_INT((int)g.configs, HA_ENTITY_COUNT);
    HAC_CHECK_EQ_INT((int)g.rep.births, 0);
}

// --- refusals ---------------------------------------------------------------------------------

static void a_refused_publish_is_tried_again_with_the_newest_reading(void)
{
    rig_up(true);
    air_reading_t r = reading(800.0f);
    connect_now(&r);
    loopback_broker_stall(&g.broker, true);

    // A CO2 past its delta every cycle: the queue takes 16, then says no.
    reporter_note_t note;
    bool refused = false;
    float co2 = 800.0f;
    for (unsigned i = 0u; i < 20u && !refused; ++i) {
        fake_clock_advance(&g.clock, CYCLE_MS);
        co2 += 100.0f;
        r = reading(co2);
        const uint64_t due = step(&r, &note);
        if (note.refused == HAL_ERR_NO_MEM) {
            refused = true;
            HAC_CHECK_EQ_U64(due, now() + REPORT_RETRY_MS);
        }
    }
    HAC_CHECK(refused);
    HAC_CHECK_EQ_INT((int)g.device.out_count, ADP_TELEMETRY_LOOPBACK_OUTBOX);

    // Still refused at the retry; the reading moves on meanwhile.
    fake_clock_advance(&g.clock, REPORT_RETRY_MS);
    r = reading(co2 + 1000.0f);
    (void)step(&r, &note);
    HAC_CHECK_EQ_INT(note.refused, HAL_ERR_NO_MEM);

    loopback_broker_stall(&g.broker, false);
    fake_clock_advance(&g.clock, REPORT_RETRY_MS);
    r = reading(co2 + 2000.0f);
    (void)step(&r, &note);
    HAC_CHECK(note.state_published && note.refused == HAL_OK);
    // What the queue had took its turn first; the last word is the newest.
    HAC_CHECK_EQ_INT((int)co2_of(g.last_state), (int)(co2 + 2000.0f));
    const loopback_message_t *kept =
        loopback_broker_retained(&g.broker, "homeair/test-device/state");
    HAC_CHECK(kept != NULL);
    HAC_CHECK(g.rep.refusals >= 2u);
}

static void refused_attempts_back_off_and_are_logged_once(void)
{
    rig_up(true);
    loopback_broker_refuse(&g.broker, LOOPBACK_REFUSE, 3u);
    const air_reading_t r = reading(800.0f);
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    fake_clock_advance(&g.clock, ADP_NET_SIM_CONNECT_MS);

    uint64_t attempts_at[4];
    unsigned n = 0u;
    while (n < 4u && now() < 600000u) {
        reporter_note_t note;
        const uint64_t due = step(&r, &note);
        if (note.attempted) {
            attempts_at[n++] = now();
        }
        if (due == REPORTER_NO_DEADLINE || due <= now()) {
            break;
        }
        fake_clock_advance(&g.clock, (due < now() + CYCLE_MS ? due : now() + CYCLE_MS) - now());
    }
    HAC_CHECK_EQ_INT((int)n, 4);
    const uint64_t t0 = ADP_NET_SIM_CONNECT_MS;
    HAC_CHECK_EQ_U64(attempts_at[0], t0);
    HAC_CHECK_EQ_U64(attempts_at[1], t0 + REPORT_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_U64(attempts_at[2], t0 + 3u * REPORT_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_U64(attempts_at[3], t0 + 7u * REPORT_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_INT(g.rep.state, REPORTER_ONLINE);
    HAC_CHECK_EQ_INT((int)g.rep.attempts_failed, 3);
    HAC_CHECK_EQ_INT((int)g.log.warns, 1);
}

static void the_backoff_stops_at_its_ceiling(void)
{
    rig_up(true);
    loopback_broker_refuse(&g.broker, LOOPBACK_REFUSE, LOOPBACK_BROKER_REFUSE_ALWAYS);
    const air_reading_t r = reading(800.0f);
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    drive(3600000u, NULL, &r);
    // 5, 10, 20, 40, 80, then 120 s each: 6 attempts in the first 155 s,
    // one every two minutes after.
    const uint64_t after_ramp = 3600000u - ADP_NET_SIM_CONNECT_MS - 155000u;
    HAC_CHECK_EQ_INT((int)g.rep.attempts, (int)(6u + after_ramp / REPORT_BACKOFF_MAX_MS));
    HAC_CHECK_EQ_INT((int)g.log.warns, 1);
}

// Sessions the adapter accepts and then refuses everything in: each counts as
// a failed attempt, and the backoff grows as for refused connections.
static port_telemetry_t g_inner;
static hal_status_t refusing_publish(port_telemetry_t *port, const char *topic,
                                     const uint8_t *payload, size_t len, uint8_t qos, bool retain)
{
    (void)port;
    (void)topic;
    (void)payload;
    (void)len;
    (void)qos;
    (void)retain;
    return HAL_ERR_NOT_READY;
}

static void sessions_that_take_nothing_back_off(void)
{
    rig_up(true);
    g_inner = *adp_telemetry_loopback_port(&g.device);
    g_inner.publish = refusing_publish;
    reporter_deps_t deps = deps_for(true);
    deps.telemetry = &g_inner;
    const reporter_config_t cfg = reporter_config_default();
    HAC_CHECK_EQ_INT(reporter_init(&g.rep, &deps, &cfg), HAL_OK);

    const air_reading_t r = reading(800.0f);
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    fake_clock_advance(&g.clock, ADP_NET_SIM_CONNECT_MS);
    uint64_t sessions_at[4];
    unsigned n = 0u;
    while (n < 4u && now() < 600000u) {
        reporter_note_t note;
        const uint64_t due = step(&r, &note);
        if (note.session_started) {
            HAC_CHECK(note.session_ended);  // in the same step: "online" refused
            sessions_at[n++] = now();
        }
        fake_clock_advance(&g.clock, (due > now() ? due : now() + 1u) - now());
    }
    HAC_CHECK_EQ_INT((int)n, 4);
    const uint64_t t0 = ADP_NET_SIM_CONNECT_MS;
    HAC_CHECK_EQ_U64(sessions_at[0], t0);
    HAC_CHECK_EQ_U64(sessions_at[1], t0 + REPORT_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_U64(sessions_at[2], t0 + 3u * REPORT_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_U64(sessions_at[3], t0 + 7u * REPORT_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_INT((int)g.log.warns, 1);
}

static void an_attempt_never_answered_is_given_up(void)
{
    rig_up(true);
    loopback_broker_refuse(&g.broker, LOOPBACK_SILENT, 1u);
    const air_reading_t r = reading(800.0f);
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    fake_clock_advance(&g.clock, ADP_NET_SIM_CONNECT_MS);
    reporter_note_t note;
    uint64_t due = step(&r, &note);
    HAC_CHECK(note.attempted && note.attempt_status == HAL_OK);
    HAC_CHECK_EQ_INT(g.rep.state, REPORTER_CONNECTING);
    HAC_CHECK_EQ_U64(due, now() + REPORT_CONNECT_TIMEOUT_MS);

    fake_clock_advance(&g.clock, REPORT_CONNECT_TIMEOUT_MS);
    due = step(&r, &note);
    HAC_CHECK(note.attempt_given_up);
    HAC_CHECK_EQ_INT(g.rep.state, REPORTER_OFFLINE);
    HAC_CHECK_EQ_U64(due, now() + REPORT_BACKOFF_FIRST_MS);

    fake_clock_advance(&g.clock, REPORT_BACKOFF_FIRST_MS);
    (void)step(&r, &note);
    HAC_CHECK(note.session_started);
}

// --- a day ------------------------------------------------------------------------------------

static air_reading_t quiet(uint64_t t_ms)
{
    (void)t_ms;
    return reading(800.0f);
}

// A day like normal_day.csv's -- a temperature swinging 3 C and CO2 200 ppm,
// slowly -- and a window opened at 08:00, CO2 down 400 ppm in five minutes.
// The slow swings never pass a delta between two heartbeats: each message
// moves the baseline. Only the airing does.
static air_reading_t ordinary(uint64_t t_ms)
{
    const float h = (float)t_ms / 3600000.0f;
    float co2 = 700.0f + 200.0f * (h < 12.0f ? h / 12.0f : (24.0f - h) / 12.0f);
    if (h >= 8.0f && h < 8.0f + 5.0f / 60.0f) {
        co2 -= 400.0f * (h - 8.0f) * 12.0f;
    } else if (h >= 8.0f + 5.0f / 60.0f && h < 9.0f) {
        co2 -= 400.0f;
    }
    air_reading_t r = reading(co2);
    r.temperature_c.value = 20.5f + 3.0f * (h < 14.0f ? h / 14.0f : (24.0f - h) / 10.0f);
    return r;
}

// Past a delta every cycle.
static air_reading_t restless(uint64_t t_ms)
{
    return reading(((t_ms / CYCLE_MS) % 2u == 0u) ? 600.0f : 900.0f);
}

static unsigned day_of(air_reading_t (*at)(uint64_t t_ms))
{
    rig_up(true);
    port_net_t *net = adp_net_sim_port(&g.net);
    (void)net->up(net);
    drive(86400000u, at, NULL);
    return g.states;
}

static void a_days_state_messages_stay_within_bounds(void)
{
    // The heartbeat's floor: the session's first, then one a minute.
    const unsigned floor_count = 1u + (86400000u - ADP_NET_SIM_CONNECT_MS) / REPORT_HEARTBEAT_MS;
    HAC_CHECK_EQ_INT((int)day_of(quiet), (int)floor_count);

    const unsigned ordinary_count = day_of(ordinary);
    printf("an ordinary day: %u state messages (%u temperature, %u co2)\n", ordinary_count,
           g.rep.state_by_reason[PUBLISH_REASON_TEMPERATURE],
           g.rep.state_by_reason[PUBLISH_REASON_CO2]);
    HAC_CHECK(g.rep.state_by_reason[PUBLISH_REASON_CO2] >= 4u);  // the airing, and its end
    HAC_CHECK_EQ_INT((int)g.rep.state_by_reason[PUBLISH_REASON_TEMPERATURE], 0);
    HAC_CHECK(ordinary_count > floor_count);
    HAC_CHECK(ordinary_count <= floor_count + floor_count / 4u);

    // The ceiling: one a sensor cycle.
    const unsigned restless_count = day_of(restless);
    HAC_CHECK(restless_count <= 1u + 86400000u / CYCLE_MS);
    HAC_CHECK(restless_count >= 86400000u / CYCLE_MS - 1u);
}

// --- odds and ends ------------------------------------------------------------------------------

static void without_a_network_port_the_link_is_assumed_and_rssi_is_null(void)
{
    rig_up(false);
    const air_reading_t r = reading(800.0f);
    reporter_note_t note;
    (void)step(&r, &note);
    HAC_CHECK(note.session_started);
    HAC_CHECK(strstr(g.last_state, "\"rssi\":null,") != NULL);
}

static void init_refuses_what_could_never_be_published(void)
{
    rig_up(true);
    reporter_t r;
    reporter_config_t cfg = reporter_config_default();
    reporter_deps_t deps = deps_for(true);

    cfg.policy.heartbeat_ms = 0u;
    HAC_CHECK_EQ_INT(reporter_init(&r, &deps, &cfg), HAL_ERR_INVALID_ARG);
    cfg.policy.heartbeat_ms = 1500u;  // discovery speaks whole seconds
    HAC_CHECK_EQ_INT(reporter_init(&r, &deps, &cfg), HAL_ERR_INVALID_ARG);
    cfg.policy.heartbeat_ms = (HA_DISCOVERY_HEARTBEAT_MAX_S + 1u) * 1000u;
    HAC_CHECK_EQ_INT(reporter_init(&r, &deps, &cfg), HAL_ERR_INVALID_ARG);

    cfg = reporter_config_default();
    deps.discovery.device.name = NULL;  // no config can be built
    HAC_CHECK_EQ_INT(reporter_init(&r, &deps, &cfg), HAL_ERR_INVALID_ARG);
    deps = deps_for(true);
    deps.topics = NULL;
    HAC_CHECK_EQ_INT(reporter_init(&r, &deps, &cfg), HAL_ERR_INVALID_ARG);
    deps = deps_for(true);
    deps.telemetry = NULL;
    HAC_CHECK_EQ_INT(reporter_init(&r, &deps, &cfg), HAL_ERR_INVALID_ARG);

    // A reporter that refused to start does nothing at all.
    const air_reading_t reading_ = reading(800.0f);
    HAC_CHECK_EQ_U64(reporter_step(&r, &reading_, 0u, NULL), REPORTER_NO_DEADLINE);

    // The heartbeat discovery announces is the policy's.
    cfg = reporter_config_default();
    deps = deps_for(true);
    HAC_CHECK_EQ_INT(reporter_init(&r, &deps, &cfg), HAL_OK);
    HAC_CHECK_EQ_U64(r.discovery.heartbeat_s, REPORT_HEARTBEAT_MS / 1000u);
}

static void states_have_names(void)
{
    HAC_CHECK(strcmp(reporter_state_name(REPORTER_OFFLINE), "offline") == 0);
    HAC_CHECK(strcmp(reporter_state_name(REPORTER_CONNECTING), "connecting") == 0);
    HAC_CHECK(strcmp(reporter_state_name(REPORTER_ONLINE), "online") == 0);
}

int main(void)
{
    HAC_RUN(the_first_session_sends_online_every_config_and_the_state);
    HAC_RUN(the_heartbeat_sends_the_state_every_minute);
    HAC_RUN(an_outage_publishes_nothing_and_the_next_session_everything);
    HAC_RUN(a_session_dropped_with_the_network_up_comes_back_at_once);
    HAC_RUN(home_assistant_starting_brings_discovery_again);
    HAC_RUN(a_kept_birth_is_not_a_restart);
    HAC_RUN(a_refused_publish_is_tried_again_with_the_newest_reading);
    HAC_RUN(refused_attempts_back_off_and_are_logged_once);
    HAC_RUN(the_backoff_stops_at_its_ceiling);
    HAC_RUN(sessions_that_take_nothing_back_off);
    HAC_RUN(an_attempt_never_answered_is_given_up);
    HAC_RUN(a_days_state_messages_stay_within_bounds);
    HAC_RUN(without_a_network_port_the_link_is_assumed_and_rssi_is_null);
    HAC_RUN(init_refuses_what_could_never_be_published);
    HAC_RUN(states_have_names);
    return hac_test_summary();
}
