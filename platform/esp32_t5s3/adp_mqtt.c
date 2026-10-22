// SPDX-License-Identifier: Apache-2.0

#include "adp_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#define TAG "adp_mqtt"

// TLS is left out (docs/adr/0033-mqtt-on-the-board.md): the URI's scheme is
// refused, and esp-mqtt's TLS and websocket transports are not linked
// (sdkconfig.defaults). A default does not reach an sdkconfig that already
// exists (CLAUDE.md), so the build refuses one that still has them rather
// than carry mbedTLS's transport unused.
#if CONFIG_MQTT_TRANSPORT_SSL || CONFIG_MQTT_TRANSPORT_WEBSOCKET
#error "adp_mqtt expects esp-mqtt's TLS and websocket transports off (sdkconfig.defaults) -- \
delete this build's sdkconfig, or run idf.py reconfigure, so the default reaches it"
#endif

// esp-mqtt's task on the protocol core, beside Wi-Fi's and lwIP's: core 1 is
// the event loop's and EPDiy's row output's (ADR 0032).
#if !CONFIG_MQTT_TASK_CORE_SELECTION_ENABLED || !CONFIG_MQTT_USE_CORE_0
#error "adp_mqtt expects esp-mqtt's task pinned to core 0 (sdkconfig.defaults) -- delete this \
build's sdkconfig, or run idf.py reconfigure, so the default reaches it"
#endif

// What a CONNACK and a received message carry fits the port, and the queues'
// counters fit their indices.
_Static_assert(ADP_MQTT_OUT_SLOTS <= UINT16_MAX, "ring");
_Static_assert(ADP_MQTT_INBOX_SLOTS <= UINT16_MAX, "ring");
_Static_assert(ADP_MQTT_INBOX_SLOTS >= PORT_TELEMETRY_MIN_INBOX, "the port's inbox minimum");
_Static_assert(ADP_MQTT_MAX_SUBSCRIPTIONS >= PORT_TELEMETRY_MIN_SUBSCRIPTIONS,
               "the port's subscription minimum");
_Static_assert(ADP_MQTT_OUT_BUFFER_BYTES >= PORT_TELEMETRY_PAYLOAD_MAX + PORT_TELEMETRY_TOPIC_MAX,
               "esp-mqtt's out buffer holds the longest message whole");

#define US_PER_MS 1000

// The callers, esp-mqtt's task and the adapter's task share the queues'
// indices and the counters under this lock, held for a few words at a time
// -- never across a copy, so no copy is made with interrupts off.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// A session's parameters, as connect() was given them.
typedef struct {
    char client_id[PORT_TELEMETRY_CLIENT_ID_MAX + 1u];
    uint16_t keepalive_s;
    bool has_will;
    char will_topic[PORT_TELEMETRY_TOPIC_MAX];
    uint8_t will_payload[PORT_TELEMETRY_PAYLOAD_MAX];
    size_t will_len;
    uint8_t will_qos;
    bool will_retain;
} session_params_t;

// Everything sized by a message, in PSRAM -- about 27 KB -- as the recorder's
// buffers are (main.c's start_card()). Nothing of the Wi-Fi driver's own is
// here: those stay internal (ADR 0032).
typedef struct {
    // The caller fills, the adapter's task empties.
    adp_mqtt_out_t out[ADP_MQTT_OUT_SLOTS];
    // esp-mqtt's task fills, poll() empties.
    telemetry_message_t inbox[ADP_MQTT_INBOX_SLOTS];
    // esp-mqtt's task's alone.
    adp_mqtt_assembly_t assembly;
    // The caller's alone: its subscriptions, and connect()'s arguments.
    adp_mqtt_subs_t subs;
    session_params_t given;
    // What connect() asked for, under the lock; and the adapter's task's
    // copy, for the client it starts.
    session_params_t asked;
    session_params_t started;
} mem_t;

static mem_t *mem_of(const adp_mqtt_t *m)
{
    return (mem_t *)m->mem;
}

static void log_write(const adp_mqtt_t *m, log_level_t level, const char *message,
                      const log_field_t *fields, size_t count)
{
    port_log_write(m->cfg.log, level, TAG, message, fields, count);
}

static uint64_t now_ms(const adp_mqtt_t *m)
{
    return m->cfg.clock->now_ms(m->cfg.clock);
}

static bool network_up(const adp_mqtt_t *m)
{
    return m->cfg.net == NULL || m->cfg.net->state(m->cfg.net) == NET_UP;
}

static bool has_text(const char *s)
{
    return s != NULL && s[0] != '\0';
}

static void wake_task(const adp_mqtt_t *m)
{
    if (m->task != NULL) {
        xTaskNotifyGive((TaskHandle_t)m->task);
    }
}

// The event loop, through the composition's waker.
static void wake_loop(adp_mqtt_t *m)
{
    portENTER_CRITICAL(&s_lock);
    const adp_mqtt_waker_fn fn = m->waker;
    void *ctx = m->waker_ctx;
    portEXIT_CRITICAL(&s_lock);
    if (fn != NULL) {
        fn(ctx);
    }
}

// The longest a port call kept its caller -- the event loop's longest post.
static void note_call(adp_mqtt_t *m, const char *call, int64_t started_us)
{
    const int64_t took = esp_timer_get_time() - started_us;
    const uint32_t us = (took > 0) ? (uint32_t)took : 0u;
    portENTER_CRITICAL(&s_lock);
    if (us > m->post_max_us) {
        m->post_max_us = us;
        m->post_max_call = call;
    }
    portEXIT_CRITICAL(&s_lock);
}

// --- the port: the caller's side ------------------------------------------------------

static adp_mqtt_t *impl_of(const port_telemetry_t *port)
{
    return (adp_mqtt_t *)port->impl;
}

static bool session_ok(const telemetry_session_t *s)
{
    if (s == NULL || !port_telemetry_client_id_ok(s->client_id)) {
        return false;
    }
    if (s->will_topic == NULL) {
        return true;
    }
    return port_telemetry_topic_ok(s->will_topic) && s->will_qos <= 2u &&
           s->will_len < PORT_TELEMETRY_PAYLOAD_MAX &&
           (s->will_payload != NULL || s->will_len == 0u);
}

// Under the lock: what is read for a client is what was asked, whole.
static void copy_params(session_params_t *dst, const session_params_t *src)
{
    memcpy(dst->client_id, src->client_id, sizeof(dst->client_id));
    dst->keepalive_s = src->keepalive_s;
    dst->has_will = src->has_will;
    memcpy(dst->will_topic, src->will_topic, sizeof(dst->will_topic));
    if (src->will_len > 0u) {
        memcpy(dst->will_payload, src->will_payload, src->will_len);
    }
    dst->will_len = src->will_len;
    dst->will_qos = src->will_qos;
    dst->will_retain = src->will_retain;
}

static void params_of(session_params_t *dst, const telemetry_session_t *s)
{
    memset(dst->client_id, 0, sizeof(dst->client_id));
    memcpy(dst->client_id, s->client_id, strlen(s->client_id));
    dst->keepalive_s = s->keepalive_s;
    dst->has_will = s->will_topic != NULL;
    memset(dst->will_topic, 0, sizeof(dst->will_topic));
    dst->will_len = 0u;
    dst->will_qos = 0u;
    dst->will_retain = false;
    if (dst->has_will) {
        memcpy(dst->will_topic, s->will_topic, strlen(s->will_topic));
        if (s->will_len > 0u) {
            memcpy(dst->will_payload, s->will_payload, s->will_len);
        }
        dst->will_len = s->will_len;
        dst->will_qos = s->will_qos;
        dst->will_retain = s->will_retain;
    }
}

static hal_status_t mq_connect(port_telemetry_t *port, const telemetry_session_t *s)
{
    const int64_t t0 = esp_timer_get_time();
    adp_mqtt_t *m = impl_of(port);
    if (!session_ok(s)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (m->init_status != HAL_OK) {
        return HAL_ERR_IO;
    }
    // A copy outside the lock first, into the caller's own: the copy under
    // it is a few hundred bytes from there, never from the caller's
    // arguments.
    params_of(&mem_of(m)->given, s);
    const bool net = network_up(m);
    portENTER_CRITICAL(&s_lock);
    const hal_status_t st = adp_mqtt_session_connect(&m->session, net);
    if (st == HAL_OK) {
        copy_params(&mem_of(m)->asked, &mem_of(m)->given);
        m->stats.attempts++;
        // What esp-mqtt says next is about this session.
        m->last_connack = -1;
        m->last_errno = 0;
        m->last_esp_err = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    if (st == HAL_OK) {
        wake_task(m);
    }
    note_call(m, "connect", t0);
    return st;
}

static hal_status_t mq_disconnect(port_telemetry_t *port)
{
    const int64_t t0 = esp_timer_get_time();
    adp_mqtt_t *m = impl_of(port);
    portENTER_CRITICAL(&s_lock);
    adp_mqtt_session_disconnect(&m->session);
    portEXIT_CRITICAL(&s_lock);
    wake_task(m);
    note_call(m, "disconnect", t0);
    return HAL_OK;
}

// The session usable, under the lock -- and a network gone noticed, which
// ends it: the adapter's task is told, to stop its client.
static bool usable_locked(adp_mqtt_t *m, bool net, bool *ended)
{
    const bool was_over = m->session.over;
    const bool ok = adp_mqtt_session_usable(&m->session, net);
    *ended = !was_over && m->session.over;
    if (*ended) {
        m->stats.network++;
    }
    return ok;
}

// One slot of the ring for the caller's message or subscription: NOT_READY
// without a session, NO_MEM with the ring full. The slot is filled outside
// the lock and committed under it.
static hal_status_t enqueue(adp_mqtt_t *m, uint8_t kind, const char *topic, const uint8_t *payload,
                            size_t len, uint8_t qos, bool retain)
{
    const bool net = network_up(m);
    bool ended = false;
    portENTER_CRITICAL(&s_lock);
    const bool ok = usable_locked(m, net, &ended);
    const uint32_t gen = m->session.gen;
    int slot = -1;
    if (ok) {
        slot = adp_mqtt_ring_reserve(&m->out_ring);
        if (slot < 0) {
            m->stats.refused_calls++;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (ended) {
        wake_task(m);
    }
    if (!ok) {
        return HAL_ERR_NOT_READY;
    }
    if (slot < 0) {
        return HAL_ERR_NO_MEM;
    }

    adp_mqtt_out_t *o = &mem_of(m)->out[slot];
    const size_t topic_len = strlen(topic);
    o->session = gen;
    o->kind = kind;
    o->qos = qos;
    o->retain = retain;
    o->len = (uint16_t)len;
    memcpy(o->topic, topic, topic_len + 1u);
    if (len > 0u) {
        memcpy(o->payload, payload, len);
    }

    portENTER_CRITICAL(&s_lock);
    adp_mqtt_ring_commit(&m->out_ring);
    m->queued_bytes += (uint32_t)(topic_len + len);
    if (kind == ADP_MQTT_OUT_SUBSCRIBE) {
        m->stats.subscribed++;
    } else {
        m->stats.published++;
    }
    portEXIT_CRITICAL(&s_lock);
    wake_task(m);
    return HAL_OK;
}

static hal_status_t mq_publish(port_telemetry_t *port, const char *topic, const uint8_t *payload,
                               size_t len, uint8_t qos, bool retain)
{
    const int64_t t0 = esp_timer_get_time();
    adp_mqtt_t *m = impl_of(port);
    if (!port_telemetry_topic_ok(topic) || qos > 2u || len >= PORT_TELEMETRY_PAYLOAD_MAX ||
        (payload == NULL && len > 0u)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (m->init_status != HAL_OK) {
        return HAL_ERR_NOT_READY;
    }
    const hal_status_t st = enqueue(m, ADP_MQTT_OUT_PUBLISH, topic, payload, len, qos, retain);
    note_call(m, "publish", t0);
    return st;
}

static hal_status_t mq_subscribe(port_telemetry_t *port, const char *filter, uint8_t qos)
{
    const int64_t t0 = esp_timer_get_time();
    adp_mqtt_t *m = impl_of(port);
    if (!port_telemetry_filter_ok(filter) || qos > 2u) {
        return HAL_ERR_INVALID_ARG;
    }
    if (m->init_status != HAL_OK) {
        return HAL_ERR_NOT_READY;
    }
    const bool net = network_up(m);
    bool ended = false;
    portENTER_CRITICAL(&s_lock);
    const bool ok = usable_locked(m, net, &ended);
    const uint32_t gen = m->session.gen;
    portEXIT_CRITICAL(&s_lock);
    if (ended) {
        wake_task(m);
    }
    hal_status_t st = HAL_ERR_NOT_READY;
    if (ok) {
        // The subscriptions' table is the caller's alone.
        st = adp_mqtt_subs_take(&mem_of(m)->subs, gen, filter)
                 ? enqueue(m, ADP_MQTT_OUT_SUBSCRIBE, filter, NULL, 0u, qos, false)
                 : HAL_ERR_NO_MEM;
    }
    note_call(m, "subscribe", t0);
    return st;
}

static bool mq_is_connected(const port_telemetry_t *port)
{
    const int64_t t0 = esp_timer_get_time();
    adp_mqtt_t *m = impl_of(port);
    if (m->init_status != HAL_OK) {
        return false;
    }
    const bool net = network_up(m);
    bool ended = false;
    portENTER_CRITICAL(&s_lock);
    const bool ok = usable_locked(m, net, &ended);
    portEXIT_CRITICAL(&s_lock);
    if (ended) {
        wake_task(m);
    }
    note_call(m, "is_connected", t0);
    return ok;
}

static bool mq_poll(port_telemetry_t *port, telemetry_message_t *out)
{
    const int64_t t0 = esp_timer_get_time();
    adp_mqtt_t *m = impl_of(port);
    if (m->init_status != HAL_OK) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    const int slot = adp_mqtt_ring_peek(&m->in_ring);
    portEXIT_CRITICAL(&s_lock);
    if (slot >= 0) {
        *out = mem_of(m)->inbox[slot];
        portENTER_CRITICAL(&s_lock);
        adp_mqtt_ring_release(&m->in_ring);
        portEXIT_CRITICAL(&s_lock);
    }
    note_call(m, "poll", t0);
    return slot >= 0;
}

// --- esp-mqtt's events, on esp-mqtt's task -----------------------------------------------

static void note_mqtt_stack(adp_mqtt_t *m)
{
    // Bytes on ESP-IDF's Xtensa port (event_loop.c).
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&s_lock);
    if (free_bytes < m->mqtt_stack_free_min) {
        m->mqtt_stack_free_min = free_bytes;
    }
    portEXIT_CRITICAL(&s_lock);
}

// A message, put together from its pieces, into the inbox -- while the
// caller holds the session it came in.
static bool receive(adp_mqtt_t *m, uint32_t gen, const esp_mqtt_event_t *e)
{
    mem_t *mem = mem_of(m);
    const adp_mqtt_piece_t piece = {
        .topic = e->topic,
        .topic_len = (e->topic_len > 0) ? (size_t)e->topic_len : 0u,
        .data = (const uint8_t *)e->data,
        .data_len = (e->data_len > 0) ? (size_t)e->data_len : 0u,
        .offset = (e->current_data_offset > 0) ? (size_t)e->current_data_offset : 0u,
        .total = (e->total_data_len > 0) ? (size_t)e->total_data_len : 0u,
        .retain = e->retain,
    };
    const adp_mqtt_piece_result_t r = adp_mqtt_assemble(&mem->assembly, &piece);
    if (r == ADP_MQTT_PIECE_MORE || r == ADP_MQTT_PIECE_STRAY) {
        return false;
    }
    int slot = -1;
    portENTER_CRITICAL(&s_lock);
    if (r == ADP_MQTT_PIECE_DONE && adp_mqtt_session_holds(&m->session, gen)) {
        slot = adp_mqtt_ring_reserve(&m->in_ring);
    }
    if (slot < 0) {
        m->stats.inbox_dropped++;
    }
    portEXIT_CRITICAL(&s_lock);
    if (slot < 0) {
        return false;
    }
    mem->inbox[slot] = mem->assembly.msg;
    portENTER_CRITICAL(&s_lock);
    adp_mqtt_ring_commit(&m->in_ring);
    m->stats.received++;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    adp_mqtt_t *m = (adp_mqtt_t *)arg;
    const esp_mqtt_event_t *e = (const esp_mqtt_event_t *)data;
    const uint32_t gen = m->handler_gen;
    note_mqtt_stack(m);
    bool loop = false;
    bool wake = false;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            portENTER_CRITICAL(&s_lock);
            loop = adp_mqtt_session_accepted(&m->session, gen);
            if (loop) {
                m->stats.sessions++;
            }
            portEXIT_CRITICAL(&s_lock);
            wake = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            portENTER_CRITICAL(&s_lock);
            loop = adp_mqtt_session_ended(&m->session, gen, m->refused_gen == gen);
            if (loop) {
                switch (m->session.end) {
                    case ADP_MQTT_END_LOST:
                        m->stats.lost++;
                        break;
                    case ADP_MQTT_END_REFUSED:
                        m->stats.refused++;
                        break;
                    default:
                        m->stats.no_connection++;
                        break;
                }
            }
            portEXIT_CRITICAL(&s_lock);
            wake = true;
            break;
        case MQTT_EVENT_ERROR:
            if (e->error_handle != NULL) {
                portENTER_CRITICAL(&s_lock);
                if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    m->refused_gen = gen;
                    m->last_connack = (int)e->error_handle->connect_return_code;
                } else if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                    m->last_errno = e->error_handle->esp_transport_sock_errno;
                    m->last_esp_err = (int)e->error_handle->esp_tls_last_esp_err;
                }
                portEXIT_CRITICAL(&s_lock);
            }
            break;
        case MQTT_EVENT_PUBLISHED:
            portENTER_CRITICAL(&s_lock);
            m->stats.acknowledged++;
            portEXIT_CRITICAL(&s_lock);
            wake = true;  // room in esp-mqtt's outbox, for what waits
            break;
        case MQTT_EVENT_DATA:
            loop = receive(m, gen, e);
            break;
        default:
            break;
    }
    if (loop) {
        wake_loop(m);
    }
    if (wake) {
        wake_task(m);
    }
}

// --- the adapter's task -----------------------------------------------------------------

static const char *user_of(const adp_mqtt_t *m)
{
    return has_text(m->cfg.username) ? m->cfg.username : NULL;
}

// A client for session `gen`, made and started: esp-mqtt's task connects.
static void start_client(adp_mqtt_t *m, uint32_t gen)
{
    mem_t *mem = mem_of(m);
    portENTER_CRITICAL(&s_lock);
    copy_params(&mem->started, &mem->asked);
    portEXIT_CRITICAL(&s_lock);
    m->started_gen = gen;
    const session_params_t *p = &mem->started;

    esp_mqtt_client_config_t c;
    memset(&c, 0, sizeof(c));
    c.broker.address.hostname = m->broker.host;
    c.broker.address.port = m->broker.port;
    c.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    c.credentials.client_id = p->client_id;
    c.credentials.username = user_of(m);
    c.credentials.authentication.password =
        (user_of(m) != NULL && has_text(m->cfg.password)) ? m->cfg.password : NULL;
    c.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    c.session.keepalive = (int)p->keepalive_s;
    c.session.disable_keepalive = p->keepalive_s == 0u;
    if (p->has_will) {
        c.session.last_will.topic = p->will_topic;
        // Of a length, not a string: an empty will is "", which esp-mqtt
        // copies as such.
        c.session.last_will.msg = (p->will_len > 0u) ? (const char *)p->will_payload : "";
        c.session.last_will.msg_len = (int)p->will_len;
        c.session.last_will.qos = (int)p->will_qos;
        c.session.last_will.retain = p->will_retain ? 1 : 0;
    }
    c.network.disable_auto_reconnect = true;
    c.network.timeout_ms = (int)ADP_MQTT_NETWORK_TIMEOUT_MS;
    c.network.reconnect_timeout_ms = (int)ADP_MQTT_IDLE_POLL_MS;
    c.task.priority = ADP_MQTT_TASK_PRIORITY;
    c.task.stack_size = (int)ADP_MQTT_TASK_STACK_BYTES;
    c.buffer.size = (int)ADP_MQTT_IN_BUFFER_BYTES;
    c.buffer.out_size = (int)ADP_MQTT_OUT_BUFFER_BYTES;
    c.outbox.limit = ADP_MQTT_OUTBOX_LIMIT_BYTES;

    // The previous client's task is gone: the handler is this client's now.
    m->handler_gen = gen;
    adp_mqtt_assembly_reset(&mem->assembly);
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&c);
    esp_err_t err = (client != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, on_event, m);
    }
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(client);
    }
    if (err != ESP_OK) {
        if (client != NULL) {
            (void)esp_mqtt_client_destroy(client);
        }
        portENTER_CRITICAL(&s_lock);
        adp_mqtt_session_not_started(&m->session, gen);
        m->stats.not_started++;
        portEXIT_CRITICAL(&s_lock);
        const log_field_t fields[] = {
            log_i64("esp_err", (int64_t)err),
            log_u64("free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        };
        log_write(m, LOG_LEVEL_ERROR, "no client: esp-mqtt would not make or start one", fields,
                  sizeof(fields) / sizeof(fields[0]));
        wake_loop(m);
        return;
    }
    m->client = client;
    m->client_gen = gen;
}

// The client stopped -- a DISCONNECT if esp-mqtt still thinks it connected,
// then its task ended -- and freed with everything it held. Waits as long as
// esp-mqtt waits for the network.
static void stop_client(adp_mqtt_t *m)
{
    const uint64_t start = now_ms(m);
    (void)esp_mqtt_client_destroy((esp_mqtt_client_handle_t)m->client);
    m->client = NULL;
    m->client_gen = 0u;
    m->waiting_for_room = false;
    portENTER_CRITICAL(&s_lock);
    m->outbox_bytes = 0u;
    portEXIT_CRITICAL(&s_lock);
    const uint64_t took = now_ms(m) - start;
    const log_field_t fields[] = {log_u64("took_ms", took)};
    // A second and more is esp-mqtt waiting on the network: worth a line.
    log_write(m, (took >= 1000u) ? LOG_LEVEL_INFO : LOG_LEVEL_DEBUG, "client stopped", fields, 1u);
}

// Under the lock: the client runs the session the caller holds, and the
// broker has accepted it.
static bool live_locked(const adp_mqtt_t *m)
{
    return m->client != NULL && m->client_gen == m->session.gen &&
           adp_mqtt_session_holds(&m->session, m->client_gen) && m->session.accepted;
}

// What waits in the ring, to the client while its session is the one held
// and accepted; anything of another session dropped. Stops at the first
// message esp-mqtt's outbox has no room for: it waits for an acknowledgement.
static void forward(adp_mqtt_t *m)
{
    mem_t *mem = mem_of(m);
    m->waiting_for_room = false;
    for (unsigned guard = 0u; guard < 2u * ADP_MQTT_OUT_SLOTS; ++guard) {
        portENTER_CRITICAL(&s_lock);
        const int slot = adp_mqtt_ring_peek(&m->out_ring);
        const bool live = live_locked(m);
        portEXIT_CRITICAL(&s_lock);
        if (slot < 0) {
            return;
        }
        const adp_mqtt_out_t *o = &mem->out[slot];
        const size_t bytes = strlen(o->topic) + o->len;
        bool sent = false;
        if (live && o->session == m->client_gen) {
            esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)m->client;
            int rc = 0;
            if (o->kind == ADP_MQTT_OUT_SUBSCRIBE) {
                rc = esp_mqtt_client_subscribe_single(client, o->topic, (int)o->qos);
            } else {
                // An empty payload goes as NULL: a length of 0 with data
                // means "as long as the string" to esp-mqtt.
                rc = esp_mqtt_client_enqueue(client, o->topic,
                                             (o->len > 0u) ? (const char *)o->payload : NULL,
                                             (int)o->len, (int)o->qos, o->retain ? 1 : 0, true);
            }
            if (rc == -2) {
                portENTER_CRITICAL(&s_lock);
                m->stats.outbox_full++;
                portEXIT_CRITICAL(&s_lock);
                m->waiting_for_room = true;
                return;
            }
            sent = rc >= 0;  // -1: the client's session is ending -- dropped with it
        }
        portENTER_CRITICAL(&s_lock);
        adp_mqtt_ring_release(&m->out_ring);
        m->queued_bytes -= (bytes <= m->queued_bytes) ? (uint32_t)bytes : m->queued_bytes;
        if (sent) {
            m->stats.forwarded++;
        } else {
            m->stats.discarded++;
        }
        portEXIT_CRITICAL(&s_lock);
    }
}

static void log_end(adp_mqtt_t *m, const char *message, const char *why, int64_t code)
{
    char broker[ADP_MQTT_HOST_MAX + 8u];
    (void)snprintf(broker, sizeof(broker), "%s:%u", m->broker.host, (unsigned)m->broker.port);
    const log_field_t fields[] = {
        log_str("broker", broker),
        log_str("why", why),
        log_i64("code", code),
    };
    log_write(m, LOG_LEVEL_WARN, message, fields, sizeof(fields) / sizeof(fields[0]));
}

// Why the session ended, once per session, and each reason once until a
// session works again: a reporter retrying an absent broker every two
// minutes does not fill the card's log with the same line. And, at the first
// session, what it costs.
static void log_news(adp_mqtt_t *m)
{
    portENTER_CRITICAL(&s_lock);
    const adp_mqtt_session_t s = m->session;
    const int connack = m->last_connack;
    const int err = m->last_errno;
    const int esp_err = m->last_esp_err;
    const uint32_t sessions = m->stats.sessions;
    const uint32_t mqtt_stack = m->mqtt_stack_free_min;
    portEXIT_CRITICAL(&s_lock);

    if (s.accepted && !s.over) {
        m->last_noted = 0;  // a failure after this one is news again
    }
    if (sessions > 0u && !m->memory_logged) {
        m->memory_logged = true;
        const log_field_t fields[] = {
            log_u64("free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            log_u64("min_free_internal", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
            log_u64("free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            log_u64("mqtt_stack_free_min", mqtt_stack),
        };
        log_write(m, LOG_LEVEL_INFO, "memory with a session", fields,
                  sizeof(fields) / sizeof(fields[0]));
    }
    if (mqtt_stack < m->mqtt_stack_logged) {
        m->mqtt_stack_logged = mqtt_stack;
        const log_field_t fields[] = {log_u64("stack_free_min_bytes", mqtt_stack)};
        log_write(m, LOG_LEVEL_INFO, "esp-mqtt's task: stack low-water mark", fields, 1u);
    }

    if (!s.over || s.gen == m->logged_gen) {
        return;
    }
    m->logged_gen = s.gen;
    const long key = (long)s.end * 100000L + (long)connack * 1000L + (long)err;
    if (key == m->last_noted) {
        return;
    }
    m->last_noted = key;
    switch (s.end) {
        case ADP_MQTT_END_REFUSED:
            log_end(m, "the broker refused the session", adp_mqtt_connack_name(connack),
                    (int64_t)connack);
            return;
        case ADP_MQTT_END_NO_CONNECTION:
            log_end(m, "no connection to the broker",
                    (err != 0)       ? strerror(err)
                    : (esp_err != 0) ? esp_err_to_name(esp_err)
                                     : "no answer within ADP_MQTT_NETWORK_TIMEOUT_MS",
                    (int64_t)err);
            return;
        case ADP_MQTT_END_LOST:
            log_end(m, "the session was lost",
                    (err != 0) ? strerror(err) : "the broker closed it, or the keepalive",
                    (int64_t)err);
            return;
        case ADP_MQTT_END_NETWORK:
            log_end(m, "the session was lost", "the network went", 0);
            return;
        case ADP_MQTT_END_NOT_STARTED:
        case ADP_MQTT_END_NONE:
            return;
    }
}

static void track_stack(adp_mqtt_t *m)
{
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&s_lock);
    const bool lower = free_bytes < m->task_stack_free_min;
    if (lower) {
        m->task_stack_free_min = free_bytes;
    }
    portEXIT_CRITICAL(&s_lock);
    if (lower) {
        const log_field_t fields[] = {log_u64("stack_free_min_bytes", free_bytes)};
        log_write(m, LOG_LEVEL_INFO, "stack low-water mark", fields, 1u);
    }
}

static void mqtt_task(void *arg)
{
    adp_mqtt_t *m = (adp_mqtt_t *)arg;
    for (;;) {
        const TickType_t wait =
            m->waiting_for_room ? pdMS_TO_TICKS(ADP_MQTT_FORWARD_RETRY_MS) + 1u : portMAX_DELAY;
        (void)ulTaskNotifyTake(pdTRUE, wait);

        // Until there is nothing more to do: a stop, then a start, then
        // what waits for the new session.
        for (unsigned guard = 0u; guard < 4u; ++guard) {
            portENTER_CRITICAL(&s_lock);
            const adp_mqtt_plan_t plan =
                adp_mqtt_session_plan(&m->session, m->client_gen, m->started_gen);
            const uint32_t gen = m->session.gen;
            portEXIT_CRITICAL(&s_lock);
            if (plan == ADP_MQTT_PLAN_STOP) {
                stop_client(m);
            } else if (plan == ADP_MQTT_PLAN_START) {
                start_client(m, gen);
            } else {
                break;
            }
        }
        // Forwarded while the session is live, dropped while it is not.
        forward(m);
        portENTER_CRITICAL(&s_lock);
        const bool live = live_locked(m);
        portEXIT_CRITICAL(&s_lock);
        if (live) {
            // Takes the client's lock, which esp-mqtt's task holds while it
            // waits for the network: read only while the session works.
            const int outbox = esp_mqtt_client_get_outbox_size((esp_mqtt_client_handle_t)m->client);
            portENTER_CRITICAL(&s_lock);
            m->outbox_bytes = (outbox > 0) ? (uint32_t)outbox : 0u;
            portEXIT_CRITICAL(&s_lock);
        }
        log_news(m);
        track_stack(m);
    }
}

// --- set-up ---------------------------------------------------------------------------

hal_status_t adp_mqtt_init(adp_mqtt_t *m, const adp_mqtt_config_t *cfg)
{
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    m->port.connect = mq_connect;
    m->port.disconnect = mq_disconnect;
    m->port.publish = mq_publish;
    m->port.subscribe = mq_subscribe;
    m->port.is_connected = mq_is_connected;
    m->port.poll = mq_poll;
    m->port.impl = m;
    m->init_status = HAL_ERR_NOT_READY;
    m->last_connack = -1;
    m->post_max_call = "-";
    m->task_stack_free_min = UINT32_MAX;
    m->mqtt_stack_free_min = UINT32_MAX;
    m->mqtt_stack_logged = UINT32_MAX;
    adp_mqtt_session_init(&m->session);
    adp_mqtt_ring_init(&m->out_ring, (uint16_t)ADP_MQTT_OUT_SLOTS);
    adp_mqtt_ring_init(&m->in_ring, (uint16_t)ADP_MQTT_INBOX_SLOTS);

    const uint64_t start = now_ms(m);
    const char *why = adp_mqtt_parse_uri(cfg->broker_uri, &m->broker);
    if (why == NULL && has_text(cfg->password) && !has_text(cfg->username)) {
        why = "a password without a user name: MQTT sends none (3.1.2.9)";
    }
    if (why != NULL) {
        log_write(m, LOG_LEVEL_ERROR, why, NULL, 0u);
        m->init_status = HAL_ERR_INVALID_ARG;
        return m->init_status;
    }

    m->mem = heap_caps_calloc(1u, sizeof(mem_t), MALLOC_CAP_SPIRAM);
    TaskHandle_t task = NULL;
    if (m->mem == NULL || xTaskCreatePinnedToCore(mqtt_task, "hac_mqtt", cfg->task_stack_bytes, m,
                                                  (UBaseType_t)cfg->task_priority, &task,
                                                  (BaseType_t)cfg->task_core) != pdPASS) {
        log_write(m, LOG_LEVEL_ERROR, "no PSRAM for the queues, or no task: no MQTT", NULL, 0u);
        m->init_status = HAL_ERR_NO_MEM;
        return m->init_status;
    }
    adp_mqtt_subs_init(&mem_of(m)->subs);
    m->task = task;
    m->init_status = HAL_OK;

    // The user, never the password; its length says whether there is one.
    char broker[ADP_MQTT_HOST_MAX + 8u];
    (void)snprintf(broker, sizeof(broker), "%s:%u", m->broker.host, (unsigned)m->broker.port);
    const log_field_t fields[] = {
        log_str("broker", broker),
        log_str("user", has_text(cfg->username) ? cfg->username : "-"),
        log_u64("password_len", has_text(cfg->password) ? strlen(cfg->password) : 0u),
        log_u64("took_ms", now_ms(m) - start),
        log_u64("free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        log_u64("psram_bytes", sizeof(mem_t)),
    };
    log_write(m, LOG_LEVEL_INFO, "client ready: plain MQTT, no TLS", fields,
              sizeof(fields) / sizeof(fields[0]));
    return HAL_OK;
}

port_telemetry_t *adp_mqtt_port(adp_mqtt_t *m)
{
    return &m->port;
}

void adp_mqtt_set_waker(adp_mqtt_t *m, adp_mqtt_waker_fn fn, void *ctx)
{
    portENTER_CRITICAL(&s_lock);
    m->waker_ctx = ctx;
    m->waker = fn;
    portEXIT_CRITICAL(&s_lock);
}

void adp_mqtt_status(adp_mqtt_t *m, adp_mqtt_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->init_status = m->init_status;
    out->broker = m->broker;
    out->username = has_text(m->cfg.username) ? m->cfg.username : "";
    portENTER_CRITICAL(&s_lock);
    out->state = adp_mqtt_session_state(&m->session);
    out->end = m->session.end;
    out->attempt = m->session.gen;
    out->last_connack = m->last_connack;
    out->last_errno = m->last_errno;
    out->last_esp_err = m->last_esp_err;
    out->stats = m->stats;
    out->queued_messages = m->out_ring.count;
    out->queued_bytes = m->queued_bytes;
    out->outbox_bytes = m->outbox_bytes;
    out->inbox_waiting = m->in_ring.count;
    out->post_max_us = m->post_max_us;
    out->post_max_call = m->post_max_call;
    out->task_stack_free_min_bytes = m->task_stack_free_min;
    out->mqtt_stack_free_min_bytes = m->mqtt_stack_free_min;
    portEXIT_CRITICAL(&s_lock);
}
