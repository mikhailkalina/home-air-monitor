// SPDX-License-Identifier: Apache-2.0

#include "reporting/reporter.h"

#include <string.h>

#include "reporting/report_limits.h"

#define REPORTER_TAG "reporter"

// What a session sends fits what the port carries.
_Static_assert(MQTT_TOPIC_MAX <= PORT_TELEMETRY_TOPIC_MAX, "a topic the port cannot take");
_Static_assert(HA_DISCOVERY_CONFIG_MAX <= PORT_TELEMETRY_PAYLOAD_MAX,
               "a discovery config the port cannot take");
_Static_assert(MQTT_PAYLOAD_STATE_MAX <= HA_DISCOVERY_CONFIG_MAX,
               "the state message must fit the reporter's buffer");
_Static_assert((unsigned)HA_ENTITY_COUNT <= 32u, "need_discovery has a bit per entity");

#define ALL_DISCOVERY ((uint32_t)((1ull << (unsigned)HA_ENTITY_COUNT) - 1ull))

#define MS_PER_S 1000u

reporter_config_t reporter_config_default(void)
{
    reporter_config_t cfg;
    cfg.keepalive_s = (uint16_t)REPORT_KEEPALIVE_S;
    cfg.connect_timeout_ms = REPORT_CONNECT_TIMEOUT_MS;
    cfg.backoff_first_ms = REPORT_BACKOFF_FIRST_MS;
    cfg.backoff_max_ms = REPORT_BACKOFF_MAX_MS;
    cfg.retry_ms = REPORT_RETRY_MS;
    cfg.policy = publish_policy_config_default();
    return cfg;
}

hal_status_t reporter_init(reporter_t *r, const reporter_deps_t *deps, const reporter_config_t *cfg)
{
    memset(r, 0, sizeof(*r));
    if (deps == NULL || cfg == NULL || deps->telemetry == NULL || deps->topics == NULL ||
        cfg->policy.heartbeat_ms < MS_PER_S || cfg->policy.heartbeat_ms % MS_PER_S != 0u ||
        cfg->policy.heartbeat_ms / MS_PER_S > HA_DISCOVERY_HEARTBEAT_MAX_S) {
        return HAL_ERR_INVALID_ARG;
    }
    r->telemetry = deps->telemetry;
    r->net = deps->net;
    r->log = deps->log;
    r->topics = deps->topics;
    r->cfg = *cfg;
    r->discovery = deps->discovery;
    r->discovery.topics = deps->topics;
    r->discovery.heartbeat_s = cfg->policy.heartbeat_ms / MS_PER_S;

    // Every config, once: one that cannot be built would otherwise be found
    // in the middle of a session, and could never be sent.
    for (unsigned e = 0u; e < (unsigned)HA_ENTITY_COUNT; ++e) {
        size_t len = 0u;
        if (ha_discovery_topic(r->topic, sizeof(r->topic), &r->discovery, (ha_entity_t)e) == 0u ||
            !ha_discovery_config(r->buf, sizeof(r->buf), &r->discovery, (ha_entity_t)e, &len)) {
            memset(r, 0, sizeof(*r));
            return HAL_ERR_INVALID_ARG;
        }
    }

    publish_policy_init(&r->policy, &cfg->policy);
    air_reading_init(&r->latest);
    r->state = REPORTER_OFFLINE;
    return HAL_OK;
}

static bool network_up(const reporter_t *r)
{
    return r->net == NULL || r->net->state(r->net) == NET_UP;
}

static void log_status(const reporter_t *r, log_level_t level, const char *msg, hal_status_t st)
{
    const log_field_t fields[] = {
        log_i64("status", (int64_t)st),
        log_u64("session", r->sessions),
    };
    port_log_write(r->log, level, REPORTER_TAG, msg, fields, sizeof(fields) / sizeof(fields[0]));
}

// --- what arrives ------------------------------------------------------------

static bool payload_is(const telemetry_message_t *m, const char *word)
{
    const size_t n = strlen(word);
    return !m->truncated && m->len == n && memcmp(m->payload, word, n) == 0;
}

// Everything the adapter holds. Only Home Assistant's birth means anything:
// sent live, not from the broker's store -- a retained "online" says HA was
// up before this session, which the session's own discovery already covers.
static void drain(reporter_t *r, reporter_note_t *note)
{
    // Bounded: an adapter that never runs dry must not hold the loop.
    for (unsigned i = 0u; i < 4u * PORT_TELEMETRY_MIN_INBOX; ++i) {
        if (!r->telemetry->poll(r->telemetry, &r->rx)) {
            return;
        }
        if (strcmp(r->rx.topic, r->topics->ha_status) != 0 || r->rx.retained ||
            !payload_is(&r->rx, MQTT_STATUS_ONLINE)) {
            continue;
        }
        r->births++;
        note->birth = true;
        if (r->state == REPORTER_ONLINE) {
            r->need_discovery = ALL_DISCOVERY;
        }
        const log_field_t fields[] = {log_u64("session", r->sessions)};
        port_log_write(r->log, LOG_LEVEL_INFO, REPORTER_TAG,
                       "home assistant started: discovery again", fields, 1u);
    }
}

// --- the session -----------------------------------------------------------------

static void begin_session(reporter_t *r, uint64_t now_ms, reporter_note_t *note)
{
    r->state = REPORTER_ONLINE;
    r->sessions++;
    r->need_subscribe = true;
    r->need_online = true;
    r->need_discovery = ALL_DISCOVERY;
    r->retry_publish_at_ms = 0u;
    r->refusal_logged = false;
    publish_policy_reset(&r->policy);
    note->session_started = true;
    note->session = r->sessions;
    const log_field_t fields[] = {
        log_u64("session", r->sessions),
        log_u64("attempts", r->attempts),
        log_u64("now_ms", now_ms),
    };
    port_log_write(r->log, LOG_LEVEL_INFO, REPORTER_TAG, "session up", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

static void attempt_failed(reporter_t *r, hal_status_t st, uint64_t now_ms);

// The session is gone -- the adapter noticed a drop, or the network went. The
// next attempt is at once if the session had got as far as "online": it was
// working a moment ago. One that ended before that, with the network still
// up, counts as a failed attempt -- the backoff is cleared only by "online"
// going out -- so an adapter that accepts sessions and refuses everything in
// them is tried less and less often, not in a loop.
static void end_session(reporter_t *r, bool network_lost, uint64_t now_ms, reporter_note_t *note)
{
    (void)r->telemetry->disconnect(r->telemetry);
    r->state = REPORTER_OFFLINE;
    r->sessions_lost++;
    if (r->need_online && !network_lost) {
        attempt_failed(r, HAL_ERR_NOT_READY, now_ms);
    } else {
        r->retry_at_ms = now_ms;
    }
    note->session_ended = true;
    note->network_lost = network_lost;
    const log_field_t fields[] = {
        log_u64("session", r->sessions),
        log_str("why", network_lost ? "network" : "dropped"),
        log_u64("now_ms", now_ms),
    };
    port_log_write(r->log, LOG_LEVEL_INFO, REPORTER_TAG, "session lost", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

static void attempt_failed(reporter_t *r, hal_status_t st, uint64_t now_ms)
{
    r->attempts_failed++;
    r->state = REPORTER_OFFLINE;
    if (r->backoff_ms == 0u) {
        r->backoff_ms = r->cfg.backoff_first_ms;
    } else if (r->backoff_ms < r->cfg.backoff_max_ms) {
        r->backoff_ms = (r->backoff_ms > r->cfg.backoff_max_ms / 2u) ? r->cfg.backoff_max_ms
                                                                     : r->backoff_ms * 2u;
    }
    r->retry_at_ms = now_ms + r->backoff_ms;
    if (!r->attempt_failure_logged) {
        // Once per episode: an absent broker would otherwise fill the log.
        const log_field_t fields[] = {
            log_i64("status", (int64_t)st),
            log_u64("attempts", r->attempts),
            log_u64("retry_in_ms", r->backoff_ms),
        };
        port_log_write(r->log, LOG_LEVEL_WARN, REPORTER_TAG,
                       "no session: trying again, less often each time", fields,
                       sizeof(fields) / sizeof(fields[0]));
        r->attempt_failure_logged = true;
    }
}

static void attempt(reporter_t *r, uint64_t now_ms, reporter_note_t *note)
{
    const uint8_t offline[] = MQTT_STATUS_OFFLINE;
    const telemetry_session_t session = {
        .client_id = r->topics->device_id,
        .keepalive_s = r->cfg.keepalive_s,
        .will_topic = r->topics->status,
        .will_payload = offline,
        .will_len = sizeof(offline) - 1u,
        .will_qos = (uint8_t)MQTT_STATUS_QOS,
        .will_retain = MQTT_STATUS_RETAIN,
    };
    r->attempts++;
    const hal_status_t st = r->telemetry->connect(r->telemetry, &session);
    note->attempted = true;
    note->attempt_status = st;
    if (st != HAL_OK) {
        attempt_failed(r, st, now_ms);
        return;
    }
    r->state = REPORTER_CONNECTING;
    r->attempt_deadline_ms = now_ms + r->cfg.connect_timeout_ms;
}

// Where the session is, after this step's news.
static void advance_session(reporter_t *r, uint64_t now_ms, reporter_note_t *note)
{
    const bool link = network_up(r);

    if (r->state == REPORTER_ONLINE && (!link || !r->telemetry->is_connected(r->telemetry))) {
        end_session(r, !link, now_ms, note);
    }
    if (r->state == REPORTER_CONNECTING) {
        if (!link) {
            (void)r->telemetry->disconnect(r->telemetry);
            r->state = REPORTER_OFFLINE;
            r->retry_at_ms = now_ms;
        } else if (r->telemetry->is_connected(r->telemetry)) {
            begin_session(r, now_ms, note);
        } else if (now_ms >= r->attempt_deadline_ms) {
            (void)r->telemetry->disconnect(r->telemetry);
            note->attempt_given_up = true;
            attempt_failed(r, HAL_ERR_TIMEOUT, now_ms);
        }
    }
    if (r->state == REPORTER_OFFLINE && link && now_ms >= r->retry_at_ms) {
        attempt(r, now_ms, note);
        // An adapter that connects at once is online in this same step.
        if (r->state == REPORTER_CONNECTING && r->telemetry->is_connected(r->telemetry)) {
            begin_session(r, now_ms, note);
        }
    }
}

// --- what is owed -------------------------------------------------------------------

// A refused call: the session gone (the adapter noticed before is_connected()
// told), or the adapter not taking anything now.
static void refused(reporter_t *r, hal_status_t st, uint64_t now_ms, reporter_note_t *note)
{
    note->refused = st;
    if (st == HAL_ERR_NOT_READY) {
        end_session(r, !network_up(r), now_ms, note);
        return;
    }
    r->refusals++;
    r->retry_publish_at_ms = now_ms + r->cfg.retry_ms;
    if (!r->refusal_logged) {
        log_status(r, LOG_LEVEL_WARN, "a message refused: trying again", st);
        r->refusal_logged = true;
    }
}

static hal_status_t publish(reporter_t *r, const char *topic, const char *payload, size_t len,
                            uint8_t qos, bool retain)
{
    const hal_status_t st =
        r->telemetry->publish(r->telemetry, topic, (const uint8_t *)payload, len, qos, retain);
    if (st == HAL_OK) {
        r->refusal_logged = false;
    }
    return st;
}

// The lowest entity still owed.
static ha_entity_t next_discovery(uint32_t owed)
{
    unsigned e = 0u;
    while ((owed & (1u << e)) == 0u) {
        ++e;
    }
    return (ha_entity_t)e;
}

static bool send_discovery(reporter_t *r, uint64_t now_ms, reporter_note_t *note)
{
    while (r->need_discovery != 0u) {
        const ha_entity_t e = next_discovery(r->need_discovery);
        size_t len = 0u;
        if (ha_discovery_topic(r->topic, sizeof(r->topic), &r->discovery, e) == 0u ||
            !ha_discovery_config(r->buf, sizeof(r->buf), &r->discovery, e, &len)) {
            // reporter_init() built every one: unreachable, and never answered
            // with an empty message, which would remove the entity.
            log_status(r, LOG_LEVEL_ERROR, "a discovery config could not be built", HAL_ERR_NO_MEM);
            r->need_discovery &= ~(1u << (unsigned)e);
            continue;
        }
        const hal_status_t st =
            publish(r, r->topic, r->buf, len, (uint8_t)MQTT_DISCOVERY_QOS, MQTT_DISCOVERY_RETAIN);
        if (st != HAL_OK) {
            refused(r, st, now_ms, note);
            return false;
        }
        r->need_discovery &= ~(1u << (unsigned)e);
        r->discovery_messages++;
        note->discovery_published++;
    }
    return true;
}

static void send_state(reporter_t *r, uint64_t now_ms, reporter_note_t *note)
{
    publish_decision_t d;
    publish_policy_evaluate(&r->policy, &r->latest, now_ms, &d);
    if (!d.due) {
        return;
    }
    const size_t len = mqtt_payload_state(r->buf, sizeof(r->buf), &r->latest);
    if (len == 0u) {
        // MQTT_PAYLOAD_STATE_MAX holds every message: unreachable.
        log_status(r, LOG_LEVEL_ERROR, "the state message could not be built", HAL_ERR_NO_MEM);
        return;
    }
    const hal_status_t st =
        publish(r, r->topics->state, r->buf, len, (uint8_t)MQTT_STATE_QOS, MQTT_STATE_RETAIN);
    if (st != HAL_OK) {
        refused(r, st, now_ms, note);
        return;
    }
    publish_policy_commit(&r->policy, &r->latest, now_ms);
    r->state_messages++;
    r->state_by_reason[d.reason]++;
    note->state_published = true;
    note->reason = d.reason;
}

// In order, stopping at the first refusal: what follows waits for it.
static void work(reporter_t *r, uint64_t now_ms, reporter_note_t *note)
{
    if (now_ms < r->retry_publish_at_ms) {
        return;
    }
    if (r->need_subscribe) {
        const hal_status_t st =
            r->telemetry->subscribe(r->telemetry, r->topics->ha_status, (uint8_t)MQTT_STATUS_QOS);
        if (st != HAL_OK) {
            refused(r, st, now_ms, note);
            return;
        }
        r->need_subscribe = false;
    }
    if (r->need_online) {
        const hal_status_t st =
            publish(r, r->topics->status, MQTT_STATUS_ONLINE, sizeof(MQTT_STATUS_ONLINE) - 1u,
                    (uint8_t)MQTT_STATUS_QOS, MQTT_STATUS_RETAIN);
        if (st != HAL_OK) {
            refused(r, st, now_ms, note);
            return;
        }
        r->need_online = false;
        note->online_published = true;
        // The session works: the next failure starts the backoff afresh.
        r->backoff_ms = 0u;
        r->attempt_failure_logged = false;
    }
    if (!send_discovery(r, now_ms, note)) {
        return;
    }
    send_state(r, now_ms, note);
}

static uint64_t next_deadline(const reporter_t *r, uint64_t now_ms)
{
    switch (r->state) {
        case REPORTER_OFFLINE:
            // Without the network, its adapter wakes the loop when it is back.
            return network_up(r) ? r->retry_at_ms : REPORTER_NO_DEADLINE;
        case REPORTER_CONNECTING:
            return r->attempt_deadline_ms;
        case REPORTER_ONLINE:
            break;
    }
    if (r->need_subscribe || r->need_online || r->need_discovery != 0u) {
        return (r->retry_publish_at_ms > now_ms) ? r->retry_publish_at_ms : now_ms;
    }
    publish_decision_t d;
    publish_policy_evaluate(&r->policy, &r->latest, now_ms, &d);
    if (d.due && r->retry_publish_at_ms > now_ms) {
        return r->retry_publish_at_ms;
    }
    return d.next_deadline_ms;
}

uint64_t reporter_step(reporter_t *r, const air_reading_t *reading, uint64_t now_ms,
                       reporter_note_t *note)
{
    reporter_note_t scratch;
    if (note == NULL) {
        note = &scratch;
    }
    memset(note, 0, sizeof(*note));
    note->refused = HAL_OK;
    note->attempt_status = HAL_OK;
    if (r->telemetry == NULL) {
        return REPORTER_NO_DEADLINE;  // never initialised
    }

    // The screen's reading, with the network's part -- which the screen's
    // copy never gets.
    r->latest = *reading;
    const bool has_link = r->net != NULL && r->net->state(r->net) == NET_UP;
    r->latest.net_connected = has_link;
    r->latest.wifi_rssi_dbm = has_link ? r->net->rssi_dbm(r->net) : (int8_t)PORT_NET_RSSI_NONE;
    r->latest.telemetry_connected = true;  // only ever published within a session

    drain(r, note);
    advance_session(r, now_ms, note);
    if (r->state == REPORTER_ONLINE) {
        work(r, now_ms, note);
    }
    note->session = r->sessions;
    return next_deadline(r, now_ms);
}

const char *reporter_state_name(reporter_state_t state)
{
    switch (state) {
        case REPORTER_OFFLINE:
            return "offline";
        case REPORTER_CONNECTING:
            return "connecting";
        case REPORTER_ONLINE:
            return "online";
    }
    return "?";
}
