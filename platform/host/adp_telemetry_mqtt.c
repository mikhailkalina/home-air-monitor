// SPDX-License-Identifier: Apache-2.0
//
// Threads, mutexes, the monotonic clock and strerror() are POSIX, not C11:
// with CMAKE_C_EXTENSIONS OFF glibc hides them unless a feature-test macro
// asks, before the first system header (platform/host/host_time.c).
#define _POSIX_C_SOURCE 200809L

#include "adp_telemetry_mqtt.h"

#include <errno.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "mqtt"

// mosquitto_loop_forever()'s wait on the socket. Anything to send wakes it at
// once, so this only bounds how late a keepalive ping goes out.
#define LOOP_TIMEOUT_MS 1000

#define MS_PER_S 1000u
#define NS_PER_MS 1000000L
#define NS_PER_S 1000000000L

// The largest port number, and how many digits it has.
#define URL_PORT_MAX 65535u
#define URL_PORT_DIGITS 5u

// last_noted's value for "every client in use", apart from every session's.
#define NOTED_NO_CLIENT (-1L)

typedef enum {
    PHASE_FREE = 0,
    PHASE_CONNECTING,  // its thread waits for an earlier DISCONNECT, or is in mosquitto_connect()
    PHASE_LOOPING,     // its thread is in mosquitto_loop_forever()
    PHASE_ENDED,       // its thread is done, or about to be: join it
    PHASE_SEVERED,     // its thread stopped by the network going; the socket still open
} phase_t;

// Why a session ended, for the log.
typedef enum {
    END_NONE = 0,       // disconnect(), the network, or not over
    END_NO_CONNECTION,  // no TCP connection, or it closed before the broker answered
    END_REFUSED,        // a CONNACK with an error
    END_LOST,           // a session that worked ended by itself
} end_kind_t;

typedef struct {
    adp_telemetry_mqtt_t *owner;
    struct mosquitto *mosq;
    pthread_t thread;
    uint16_t keepalive_s;

    // Under the owner's lock: the network thread writes them.
    phase_t phase;
    bool accepted;   // the broker said yes
    bool gone;       // over, as far as the port is concerned
    bool abandoned;  // disconnect(), the network or destroy() let it go
    bool closing;    // disconnect() let it go while it looped: its DISCONNECT is owed
    unsigned in_flight;
    end_kind_t end;
    int end_code;   // a MOSQ_ERR_*, or the CONNACK's code
    int end_errno;  // with MOSQ_ERR_ERRNO

    // The caller's thread's alone.
    bool joined;
    bool end_noted;
    char subs[ADP_TELEMETRY_MQTT_MAX_SUBSCRIPTIONS][PORT_TELEMETRY_TOPIC_MAX];
    size_t sub_count;
} client_t;

struct adp_telemetry_mqtt_s {
    port_telemetry_t port;

    char host[ADP_TELEMETRY_MQTT_HOST_MAX + 1u];
    uint16_t port_number;
    char username[ADP_TELEMETRY_MQTT_USER_MAX + 1u];
    char password[ADP_TELEMETRY_MQTT_PASSWORD_MAX + 1u];
    bool has_username;
    bool has_password;
    const port_log_t *log;
    adp_telemetry_mqtt_waker_fn waker;
    void *waker_ctx;

    pthread_mutex_t lock;
    pthread_cond_t closed;  // a closing client's DISCONNECT went out

    // Under the lock.
    unsigned closing;  // clients whose DISCONNECT is owed
    telemetry_message_t inbox[ADP_TELEMETRY_MQTT_INBOX];
    size_t in_head;
    size_t in_count;
    adp_telemetry_mqtt_stats_t stats;

    // The caller's thread's alone. The slots' phases are under the lock.
    client_t clients[ADP_TELEMETRY_MQTT_CLIENTS];
    client_t *current;  // the port's session or attempt; NULL: none
    bool link;
    long last_noted;  // the failure last logged, so that each is logged once
};

// --- small things --------------------------------------------------------------

static void say(char *err, size_t cap, const char *text)
{
    if (err != NULL && cap > 0u) {
        (void)snprintf(err, cap, "%s", text);
    }
}

// Cancellation is held off wherever the lock is held or the waker runs:
// only the waits inside libmosquitto, and its connect, may be cut short.
static int cancel_off(void)
{
    int old = PTHREAD_CANCEL_ENABLE;
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
    return old;
}

static void cancel_restore(int state)
{
    int unused = 0;
    (void)pthread_setcancelstate(state, &unused);
}

static void wake(const adp_telemetry_mqtt_t *m)
{
    if (m->waker != NULL) {
        m->waker(m->waker_ctx);
    }
}

static phase_t phase_of(adp_telemetry_mqtt_t *m, const client_t *c)
{
    pthread_mutex_lock(&m->lock);
    const phase_t p = c->phase;
    pthread_mutex_unlock(&m->lock);
    return p;
}

static void set_phase(adp_telemetry_mqtt_t *m, client_t *c, phase_t p)
{
    pthread_mutex_lock(&m->lock);
    c->phase = p;
    pthread_mutex_unlock(&m->lock);
}

static hal_status_t from_mosq(int rc)
{
    switch (rc) {
        case MOSQ_ERR_SUCCESS:
            return HAL_OK;
        case MOSQ_ERR_NO_CONN:
        case MOSQ_ERR_CONN_LOST:
            return HAL_ERR_NOT_READY;
        case MOSQ_ERR_NOMEM:
            return HAL_ERR_NO_MEM;
        case MOSQ_ERR_INVAL:
        case MOSQ_ERR_PAYLOAD_SIZE:
        case MOSQ_ERR_OVERSIZE_PACKET:
        case MOSQ_ERR_MALFORMED_UTF8:
        case MOSQ_ERR_QOS_NOT_SUPPORTED:
            return HAL_ERR_INVALID_ARG;
        default:
            return HAL_ERR_IO;
    }
}

// --- libmosquitto's callbacks, on the network thread -----------------------------

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    const int cs = cancel_off();
    client_t *c = (client_t *)obj;
    adp_telemetry_mqtt_t *m = c->owner;
    pthread_mutex_lock(&m->lock);
    if (rc == 0) {
        c->accepted = true;
        m->stats.sessions++;
    } else {
        c->end = END_REFUSED;
        c->end_code = rc;
        c->gone = true;
        m->stats.refused++;
    }
    pthread_mutex_unlock(&m->lock);
    wake(m);
    cancel_restore(cs);
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    const int cs = cancel_off();
    client_t *c = (client_t *)obj;
    adp_telemetry_mqtt_t *m = c->owner;
    if (rc != 0) {
        // libmosquitto's loop would connect again by itself; the port never
        // does. Asking for a disconnect is what makes the loop return.
        (void)mosquitto_disconnect(mosq);
    }
    pthread_mutex_lock(&m->lock);
    if (rc != 0 && c->end == END_NONE && !c->abandoned) {
        c->end = c->accepted ? END_LOST : END_NO_CONNECTION;
        c->end_code = rc;
        if (c->accepted) {
            m->stats.lost++;
        } else {
            m->stats.no_connection++;
        }
    }
    c->gone = true;
    pthread_mutex_unlock(&m->lock);
    wake(m);
    cancel_restore(cs);
}

static void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
    (void)mosq;
    (void)mid;
    const int cs = cancel_off();
    client_t *c = (client_t *)obj;
    adp_telemetry_mqtt_t *m = c->owner;
    pthread_mutex_lock(&m->lock);
    if (c->in_flight > 0u) {
        c->in_flight--;
    }
    m->stats.acknowledged++;
    pthread_mutex_unlock(&m->lock);
    cancel_restore(cs);
}

static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    (void)mosq;
    const int cs = cancel_off();
    client_t *c = (client_t *)obj;
    adp_telemetry_mqtt_t *m = c->owner;
    bool queued = false;
    pthread_mutex_lock(&m->lock);
    // What a session let go of still receives is not the port's: the next
    // session starts with nothing (ports/port_telemetry.h, subscribe()).
    if (!c->abandoned) {
        const size_t topic_len = (msg->topic != NULL) ? strlen(msg->topic) : 0u;
        if (topic_len == 0u || topic_len >= PORT_TELEMETRY_TOPIC_MAX ||
            m->in_count >= ADP_TELEMETRY_MQTT_INBOX) {
            m->stats.inbox_dropped++;
        } else {
            telemetry_message_t *out =
                &m->inbox[(m->in_head + m->in_count) % ADP_TELEMETRY_MQTT_INBOX];
            memset(out, 0, sizeof(*out));
            memcpy(out->topic, msg->topic, topic_len);
            const size_t len = (msg->payloadlen > 0) ? (size_t)msg->payloadlen : 0u;
            out->truncated = len > sizeof(out->payload);
            out->len = out->truncated ? sizeof(out->payload) : len;
            if (out->len > 0u) {
                memcpy(out->payload, msg->payload, out->len);
            }
            out->retained = msg->retain;
            m->in_count++;
            m->stats.received++;
            queued = true;
        }
    }
    pthread_mutex_unlock(&m->lock);
    if (queued) {
        wake(m);
    }
    cancel_restore(cs);
}

// --- the network thread ------------------------------------------------------------

static struct timespec deadline_after(uint32_t ms)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += (time_t)(ms / MS_PER_S);
    t.tv_nsec += (long)(ms % MS_PER_S) * NS_PER_MS;
    if (t.tv_nsec >= NS_PER_S) {
        t.tv_sec += 1;
        t.tv_nsec -= NS_PER_S;
    }
    return t;
}

static void *client_main(void *arg)
{
    client_t *c = (client_t *)arg;
    adp_telemetry_mqtt_t *m = c->owner;
    (void)cancel_off();

    // A session disconnect() ended says goodbye before this one says hello.
    pthread_mutex_lock(&m->lock);
    const struct timespec until = deadline_after(ADP_TELEMETRY_MQTT_CLOSE_WAIT_MS);
    while (m->closing > 0u && !c->abandoned) {
        if (pthread_cond_timedwait(&m->closed, &m->lock, &until) == ETIMEDOUT) {
            break;
        }
    }
    const bool go = !c->abandoned;
    pthread_mutex_unlock(&m->lock);

    int rc = MOSQ_ERR_SUCCESS;
    int err = 0;
    if (go) {
        cancel_restore(PTHREAD_CANCEL_ENABLE);
        // Blocking, here: the name lookup and the TCP handshake. The CONNECT
        // is only queued; the loop sends it.
        rc = mosquitto_connect(c->mosq, m->host, (int)m->port_number, (int)c->keepalive_s);
        err = errno;
        (void)cancel_off();
    }

    pthread_mutex_lock(&m->lock);
    const bool loop = go && rc == MOSQ_ERR_SUCCESS && !c->abandoned;
    if (go && rc != MOSQ_ERR_SUCCESS) {
        c->end = END_NO_CONNECTION;
        c->end_code = rc;
        c->end_errno = err;
        m->stats.no_connection++;
    }
    if (!loop) {
        // An attempt let go of before it got this far leaves a socket that
        // sent nothing: closing it later tells the broker nothing.
        c->gone = true;
        c->phase = PHASE_ENDED;
    } else {
        c->phase = PHASE_LOOPING;
    }
    pthread_mutex_unlock(&m->lock);
    if (!loop) {
        wake(m);
        return NULL;
    }

    cancel_restore(PTHREAD_CANCEL_ENABLE);
    (void)mosquitto_loop_forever(c->mosq, LOOP_TIMEOUT_MS, 1);
    (void)cancel_off();

    pthread_mutex_lock(&m->lock);
    c->gone = true;
    if (c->closing) {
        c->closing = false;
        m->closing--;
        pthread_cond_broadcast(&m->closed);
    }
    c->phase = PHASE_ENDED;
    pthread_mutex_unlock(&m->lock);
    wake(m);
    return NULL;
}

// --- the caller's side ---------------------------------------------------------------

static adp_telemetry_mqtt_t *impl_of(const port_telemetry_t *port)
{
    return (adp_telemetry_mqtt_t *)port->impl;
}

static void log_end(const adp_telemetry_mqtt_t *m, const char *msg, const char *why)
{
    char broker[ADP_TELEMETRY_MQTT_HOST_MAX + 8u];
    (void)snprintf(broker, sizeof(broker), "%s:%u", m->host, (unsigned)m->port_number);
    const log_field_t fields[] = {log_str("broker", broker), log_str("why", why)};
    port_log_write(m->log, LOG_LEVEL_WARN, LOG_TAG, msg, fields, 2u);
}

// Why a session that is over ended, once per session, and each reason once
// until a session works again: a reporter retrying an absent broker every two
// minutes does not fill the log with the same line.
static void note_end(adp_telemetry_mqtt_t *m, client_t *c)
{
    if (c->end_noted) {
        return;
    }
    pthread_mutex_lock(&m->lock);
    const bool over = c->gone || c->phase == PHASE_ENDED || c->phase == PHASE_SEVERED;
    const end_kind_t kind = c->end;
    const int code = c->end_code;
    const int err = c->end_errno;
    pthread_mutex_unlock(&m->lock);
    if (!over) {
        return;
    }
    c->end_noted = true;
    const long key = (long)kind * 100000L + (long)code * 1000L + (long)err;
    if (kind == END_NONE || key == m->last_noted) {
        return;
    }
    m->last_noted = key;
    switch (kind) {
        case END_NO_CONNECTION:
            log_end(m, "no connection to the broker",
                    (code == MOSQ_ERR_ERRNO) ? strerror(err) : mosquitto_strerror(code));
            return;
        case END_REFUSED:
            log_end(m, "the broker refused the session", mosquitto_connack_string(code));
            return;
        case END_LOST:
            log_end(m, "the session was lost", mosquitto_strerror(code));
            return;
        case END_NONE:
            return;
    }
}

static void release(adp_telemetry_mqtt_t *m, client_t *c)
{
    if (c->mosq != NULL) {
        mosquitto_destroy(c->mosq);
        c->mosq = NULL;
    }
    set_phase(m, c, PHASE_FREE);
}

// Joins and frees what has ended, apart from the port's own session, whose
// end the next connect() or disconnect() lets go of.
static void reap(adp_telemetry_mqtt_t *m)
{
    for (size_t i = 0u; i < ADP_TELEMETRY_MQTT_CLIENTS; ++i) {
        client_t *c = &m->clients[i];
        const phase_t p = phase_of(m, c);
        if (p == PHASE_FREE) {
            continue;
        }
        note_end(m, c);
        if (c == m->current || p != PHASE_ENDED) {
            continue;
        }
        if (!c->joined) {
            // It said it ended just before returning: this waits microseconds.
            (void)pthread_join(c->thread, NULL);
            c->joined = true;
        }
        release(m, c);
    }
}

static bool usable(adp_telemetry_mqtt_t *m, const client_t *c)
{
    if (c == NULL) {
        return false;
    }
    pthread_mutex_lock(&m->lock);
    const bool ok = c->accepted && !c->gone && !c->abandoned;
    pthread_mutex_unlock(&m->lock);
    return ok;
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

static client_t *free_slot(adp_telemetry_mqtt_t *m)
{
    for (size_t i = 0u; i < ADP_TELEMETRY_MQTT_CLIENTS; ++i) {
        if (phase_of(m, &m->clients[i]) == PHASE_FREE) {
            return &m->clients[i];
        }
    }
    return NULL;
}

// Lets the port's session go: it is no longer the port's, whatever its
// thread is still doing.
static void let_go(adp_telemetry_mqtt_t *m, client_t *c)
{
    pthread_mutex_lock(&m->lock);
    c->abandoned = true;
    c->gone = true;
    pthread_mutex_unlock(&m->lock);
    if (m->current == c) {
        m->current = NULL;
    }
}

static hal_status_t mq_connect(port_telemetry_t *port, const telemetry_session_t *s)
{
    adp_telemetry_mqtt_t *m = impl_of(port);
    if (!session_ok(s)) {
        return HAL_ERR_INVALID_ARG;
    }
    reap(m);
    if (m->current != NULL) {
        pthread_mutex_lock(&m->lock);
        const bool busy = !m->current->gone;
        pthread_mutex_unlock(&m->lock);
        if (busy) {
            return HAL_ERR_BUSY;
        }
        let_go(m, m->current);  // over: the next reap() frees it
    }
    if (!m->link) {
        return HAL_ERR_NOT_READY;
    }
    client_t *c = free_slot(m);
    if (c == NULL) {
        if (m->last_noted != NOTED_NO_CLIENT) {
            m->last_noted = NOTED_NO_CLIENT;
            log_end(m, "no connection attempt: earlier ones are still ending",
                    "every client is in use (ADP_TELEMETRY_MQTT_CLIENTS)");
        }
        return HAL_ERR_IO;
    }

    memset(c, 0, sizeof(*c));
    c->owner = m;
    c->keepalive_s = s->keepalive_s;
    if (c->keepalive_s > 0u && c->keepalive_s < ADP_TELEMETRY_MQTT_MIN_KEEPALIVE_S) {
        c->keepalive_s = (uint16_t)ADP_TELEMETRY_MQTT_MIN_KEEPALIVE_S;
    }
    c->mosq = mosquitto_new(s->client_id, true, c);
    if (c->mosq == NULL) {
        return HAL_ERR_IO;
    }
    (void)mosquitto_threaded_set(c->mosq, true);
    (void)mosquitto_int_option(c->mosq, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
    mosquitto_connect_callback_set(c->mosq, on_connect);
    mosquitto_disconnect_callback_set(c->mosq, on_disconnect);
    mosquitto_publish_callback_set(c->mosq, on_publish);
    mosquitto_message_callback_set(c->mosq, on_message);
    int rc = MOSQ_ERR_SUCCESS;
    if (s->will_topic != NULL) {
        rc = mosquitto_will_set(c->mosq, s->will_topic, (int)s->will_len,
                                (s->will_len > 0u) ? s->will_payload : NULL, (int)s->will_qos,
                                s->will_retain);
    }
    if (rc == MOSQ_ERR_SUCCESS && m->has_username) {
        rc = mosquitto_username_pw_set(c->mosq, m->username, m->has_password ? m->password : NULL);
    }
    if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(c->mosq);
        c->mosq = NULL;
        return from_mosq(rc);
    }

    set_phase(m, c, PHASE_CONNECTING);
    if (pthread_create(&c->thread, NULL, client_main, c) != 0) {
        release(m, c);
        return HAL_ERR_IO;
    }
    m->current = c;
    pthread_mutex_lock(&m->lock);
    m->stats.attempts++;
    pthread_mutex_unlock(&m->lock);
    return HAL_OK;
}

static hal_status_t mq_disconnect(port_telemetry_t *port)
{
    adp_telemetry_mqtt_t *m = impl_of(port);
    reap(m);
    client_t *c = m->current;
    if (c == NULL) {
        return HAL_OK;
    }
    m->current = NULL;
    pthread_mutex_lock(&m->lock);
    const bool goodbye = (c->phase == PHASE_LOOPING) && !c->gone;
    if (goodbye) {
        c->closing = true;
        m->closing++;
    }
    c->abandoned = true;
    c->gone = true;
    pthread_mutex_unlock(&m->lock);
    if (goodbye) {
        // Queued, and the network thread woken to send it: its loop returns
        // once it is out.
        (void)mosquitto_disconnect(c->mosq);
    }
    return HAL_OK;
}

static hal_status_t mq_publish(port_telemetry_t *port, const char *topic, const uint8_t *payload,
                               size_t len, uint8_t qos, bool retain)
{
    adp_telemetry_mqtt_t *m = impl_of(port);
    if (!port_telemetry_topic_ok(topic) || qos > 2u || len >= PORT_TELEMETRY_PAYLOAD_MAX ||
        (payload == NULL && len > 0u)) {
        return HAL_ERR_INVALID_ARG;
    }
    reap(m);
    client_t *c = m->current;
    if (!usable(m, c)) {
        return HAL_ERR_NOT_READY;
    }
    pthread_mutex_lock(&m->lock);
    const bool full = c->in_flight >= ADP_TELEMETRY_MQTT_OUTBOX;
    if (!full) {
        c->in_flight++;
    }
    pthread_mutex_unlock(&m->lock);
    if (full) {
        return HAL_ERR_NO_MEM;
    }
    // libmosquitto copies the payload before it returns.
    const int rc = mosquitto_publish(c->mosq, NULL, topic, (int)len, (len > 0u) ? payload : NULL,
                                     (int)qos, retain);
    pthread_mutex_lock(&m->lock);
    if (rc != MOSQ_ERR_SUCCESS) {
        if (c->in_flight > 0u) {
            c->in_flight--;
        }
    } else {
        m->stats.published++;
    }
    pthread_mutex_unlock(&m->lock);
    return from_mosq(rc);
}

static hal_status_t mq_subscribe(port_telemetry_t *port, const char *filter, uint8_t qos)
{
    adp_telemetry_mqtt_t *m = impl_of(port);
    if (!port_telemetry_filter_ok(filter) || qos > 2u) {
        return HAL_ERR_INVALID_ARG;
    }
    reap(m);
    client_t *c = m->current;
    if (!usable(m, c)) {
        return HAL_ERR_NOT_READY;
    }
    size_t slot = c->sub_count;
    for (size_t i = 0u; i < c->sub_count; ++i) {
        if (strcmp(c->subs[i], filter) == 0) {
            slot = i;
        }
    }
    if (slot == ADP_TELEMETRY_MQTT_MAX_SUBSCRIPTIONS) {
        return HAL_ERR_NO_MEM;
    }
    const int rc = mosquitto_subscribe(c->mosq, NULL, filter, (int)qos);
    if (rc != MOSQ_ERR_SUCCESS) {
        return from_mosq(rc);
    }
    if (slot == c->sub_count) {
        (void)snprintf(c->subs[slot], sizeof(c->subs[slot]), "%s", filter);
        c->sub_count++;
    }
    return HAL_OK;
}

static bool mq_is_connected(const port_telemetry_t *port)
{
    adp_telemetry_mqtt_t *m = impl_of(port);
    reap(m);
    const bool up = usable(m, m->current);
    if (up) {
        m->last_noted = 0;  // a failure after this one is news again
    }
    return up;
}

static bool mq_poll(port_telemetry_t *port, telemetry_message_t *out)
{
    adp_telemetry_mqtt_t *m = impl_of(port);
    pthread_mutex_lock(&m->lock);
    const bool any = m->in_count > 0u;
    if (any) {
        *out = m->inbox[m->in_head];
        m->in_head = (m->in_head + 1u) % ADP_TELEMETRY_MQTT_INBOX;
        m->in_count--;
    }
    pthread_mutex_unlock(&m->lock);
    return any;
}

// --- the network under it ---------------------------------------------------------------

// The network went: the session goes silent. Its thread is stopped where it
// waits on the socket -- libmosquitto's loop is a cancellation point there,
// and nothing of ours runs then -- and the socket stays open, unread and
// unwritten, as a board's does when Wi-Fi goes: the broker hears nothing more
// and drops it one and a half keepalives after the last packet it heard.
static void sever(adp_telemetry_mqtt_t *m, client_t *c)
{
    pthread_mutex_lock(&m->lock);
    const phase_t p = c->phase;
    c->abandoned = true;
    c->gone = true;
    pthread_mutex_unlock(&m->lock);
    if (p != PHASE_LOOPING) {
        return;  // still connecting: its thread sees `abandoned` and ends by itself
    }
    (void)pthread_cancel(c->thread);
    (void)pthread_join(c->thread, NULL);
    c->joined = true;
    pthread_mutex_lock(&m->lock);
    if (c->closing) {
        c->closing = false;
        m->closing--;
        pthread_cond_broadcast(&m->closed);
    }
    c->phase = PHASE_SEVERED;
    m->stats.severed++;
    pthread_mutex_unlock(&m->lock);
}

void adp_telemetry_mqtt_set_link(adp_telemetry_mqtt_t *m, bool present)
{
    if (present == m->link) {
        return;
    }
    m->link = present;
    if (!present) {
        if (m->current != NULL) {
            client_t *c = m->current;
            m->current = NULL;
            sever(m, c);
        }
        return;
    }
    // The network is back: the silent sockets close now. A broker that has
    // not dropped those sessions yet publishes their wills at once.
    for (size_t i = 0u; i < ADP_TELEMETRY_MQTT_CLIENTS; ++i) {
        client_t *c = &m->clients[i];
        if (phase_of(m, c) == PHASE_SEVERED) {
            release(m, c);
        }
    }
}

void adp_telemetry_mqtt_net_listener(void *client, bool present, uint64_t at_ms)
{
    (void)at_ms;
    adp_telemetry_mqtt_set_link((adp_telemetry_mqtt_t *)client, present);
}

// --- the URL --------------------------------------------------------------------------

static bool host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_';
}

static bool ipv6_char(char c)
{
    return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9') || c == ':' ||
           c == '.';
}

hal_status_t adp_telemetry_mqtt_parse_url(const char *url, adp_telemetry_mqtt_url_t *out, char *err,
                                          size_t err_cap)
{
    static const char scheme[] = "mqtt://";
    static const char tls_scheme[] = "mqtts://";
    memset(out, 0, sizeof(*out));
    if (url == NULL || url[0] == '\0') {
        say(err, err_cap, "no broker: a broker is mqtt://<host>[:<port>]");
        return HAL_ERR_INVALID_ARG;
    }
    if (strncmp(url, tls_scheme, sizeof(tls_scheme) - 1u) == 0) {
        say(err, err_cap,
            "mqtts:// is not supported: the simulator speaks plain MQTT, to a broker on a "
            "network it trusts");
        return HAL_ERR_INVALID_ARG;
    }
    if (strncmp(url, scheme, sizeof(scheme) - 1u) != 0) {
        say(err, err_cap, "a broker is mqtt://<host>[:<port>]");
        return HAL_ERR_INVALID_ARG;
    }
    const char *p = url + sizeof(scheme) - 1u;
    if (strchr(p, '@') != NULL) {
        say(err, err_cap,
            "no user or password in the URL: a password never goes on a command line");
        return HAL_ERR_INVALID_ARG;
    }

    const char *host = p;
    size_t host_len = 0u;
    if (*p == '[') {
        host = ++p;
        while (ipv6_char(*p)) {
            ++p;
        }
        host_len = (size_t)(p - host);
        if (*p != ']' || host_len == 0u) {
            say(err, err_cap, "an IPv6 address goes in brackets: mqtt://[::1]:1883");
            return HAL_ERR_INVALID_ARG;
        }
        ++p;
    } else {
        while (host_char(*p)) {
            ++p;
        }
        host_len = (size_t)(p - host);
    }
    if (host_len == 0u) {
        say(err, err_cap, "no host: a broker is mqtt://<host>[:<port>]");
        return HAL_ERR_INVALID_ARG;
    }
    if (host_len > ADP_TELEMETRY_MQTT_HOST_MAX) {
        say(err, err_cap, "the host name is longer than a DNS name can be");
        return HAL_ERR_INVALID_ARG;
    }

    unsigned long port = ADP_TELEMETRY_MQTT_DEFAULT_PORT;
    if (*p == ':') {
        ++p;
        const char *digits = p;
        port = 0u;
        while (*p >= '0' && *p <= '9' && (size_t)(p - digits) < URL_PORT_DIGITS) {
            port = port * 10u + (unsigned long)(*p - '0');
            ++p;
        }
        if (p == digits || port == 0u || port > URL_PORT_MAX || (*p >= '0' && *p <= '9')) {
            say(err, err_cap, "the port is a number from 1 to 65535");
            return HAL_ERR_INVALID_ARG;
        }
    }
    if (*p != '\0') {
        say(err, err_cap, "nothing may follow the host and the port: mqtt://<host>[:<port>]");
        return HAL_ERR_INVALID_ARG;
    }
    memcpy(out->host, host, host_len);
    out->port = (uint16_t)port;
    return HAL_OK;
}

// --- set-up ---------------------------------------------------------------------------

static bool copy_text(char *dst, size_t cap, const char *src)
{
    const size_t n = strlen(src);
    if (n >= cap) {
        return false;
    }
    memcpy(dst, src, n + 1u);
    return true;
}

adp_telemetry_mqtt_t *adp_telemetry_mqtt_create(const adp_telemetry_mqtt_config_t *cfg, char *err,
                                                size_t err_cap)
{
    if (cfg == NULL || cfg->host == NULL || cfg->host[0] == '\0' || cfg->port == 0u) {
        say(err, err_cap, "no broker to connect to");
        return NULL;
    }
    if (cfg->password != NULL && cfg->username == NULL) {
        say(err, err_cap, "a password without a user name: MQTT sends none (3.1.2.9)");
        return NULL;
    }
    adp_telemetry_mqtt_t *m = (adp_telemetry_mqtt_t *)calloc(1u, sizeof(*m));
    if (m == NULL) {
        say(err, err_cap, "out of memory");
        return NULL;
    }
    if (!copy_text(m->host, sizeof(m->host), cfg->host)) {
        say(err, err_cap, "the host name is longer than a DNS name can be");
        free(m);
        return NULL;
    }
    m->port_number = cfg->port;
    if (cfg->username != NULL) {
        if (cfg->username[0] == '\0' ||
            !copy_text(m->username, sizeof(m->username), cfg->username)) {
            say(err, err_cap, "the user name is empty, or longer than 64 characters");
            free(m);
            return NULL;
        }
        m->has_username = true;
    }
    if (cfg->password != NULL) {
        if (!copy_text(m->password, sizeof(m->password), cfg->password)) {
            say(err, err_cap, "the password is longer than 128 characters");
            free(m);
            return NULL;
        }
        m->has_password = true;
    }
    m->log = cfg->log;
    m->link = true;

    pthread_condattr_t attr;
    bool cond_ok = pthread_condattr_init(&attr) == 0;
    cond_ok = cond_ok && pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0;
    cond_ok = cond_ok && pthread_cond_init(&m->closed, &attr) == 0;
    (void)pthread_condattr_destroy(&attr);
    if (!cond_ok || pthread_mutex_init(&m->lock, NULL) != 0) {
        say(err, err_cap, "cannot create a lock");
        free(m);
        return NULL;
    }
    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
        say(err, err_cap, "libmosquitto did not start");
        (void)pthread_mutex_destroy(&m->lock);
        (void)pthread_cond_destroy(&m->closed);
        free(m);
        return NULL;
    }

    m->port.connect = mq_connect;
    m->port.disconnect = mq_disconnect;
    m->port.publish = mq_publish;
    m->port.subscribe = mq_subscribe;
    m->port.is_connected = mq_is_connected;
    m->port.poll = mq_poll;
    m->port.impl = m;
    return m;
}

void adp_telemetry_mqtt_destroy(adp_telemetry_mqtt_t *m)
{
    if (m == NULL) {
        return;
    }
    // A DISCONNECT disconnect() already asked for goes out first, as it would
    // have without this call: cut short, it would leave a will the session
    // took back.
    pthread_mutex_lock(&m->lock);
    const struct timespec until = deadline_after(ADP_TELEMETRY_MQTT_CLOSE_WAIT_MS);
    while (m->closing > 0u) {
        if (pthread_cond_timedwait(&m->closed, &m->lock, &until) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&m->lock);
    // Then every thread stopped where it waits -- a connect in progress, or
    // the loop -- and every socket closed with no DISCONNECT: the broker
    // publishes the wills.
    for (size_t i = 0u; i < ADP_TELEMETRY_MQTT_CLIENTS; ++i) {
        client_t *c = &m->clients[i];
        pthread_mutex_lock(&m->lock);
        const phase_t p = c->phase;
        c->abandoned = true;
        pthread_cond_broadcast(&m->closed);  // one waiting to connect gives up now
        pthread_mutex_unlock(&m->lock);
        if (p == PHASE_FREE) {
            continue;
        }
        if (!c->joined) {
            if (p == PHASE_CONNECTING || p == PHASE_LOOPING) {
                (void)pthread_cancel(c->thread);
            }
            (void)pthread_join(c->thread, NULL);
            c->joined = true;
        }
        release(m, c);
    }
    (void)mosquitto_lib_cleanup();
    (void)pthread_mutex_destroy(&m->lock);
    (void)pthread_cond_destroy(&m->closed);
    // The password does not outlive the client in memory either.
    memset(m->password, 0, sizeof(m->password));
    free(m);
}

port_telemetry_t *adp_telemetry_mqtt_port(adp_telemetry_mqtt_t *m)
{
    return &m->port;
}

void adp_telemetry_mqtt_set_waker(adp_telemetry_mqtt_t *m, adp_telemetry_mqtt_waker_fn fn,
                                  void *ctx)
{
    m->waker = fn;
    m->waker_ctx = ctx;
}

adp_telemetry_mqtt_stats_t adp_telemetry_mqtt_stats(adp_telemetry_mqtt_t *m)
{
    pthread_mutex_lock(&m->lock);
    adp_telemetry_mqtt_stats_t s = m->stats;
    s.clients_waiting = 0u;
    for (size_t i = 0u; i < ADP_TELEMETRY_MQTT_CLIENTS; ++i) {
        const client_t *c = &m->clients[i];
        if (c->phase != PHASE_FREE && c != m->current) {
            s.clients_waiting++;
        }
    }
    pthread_mutex_unlock(&m->lock);
    return s;
}

const char *adp_telemetry_mqtt_library_version(void)
{
    static char text[24];
    int major = 0;
    int minor = 0;
    int revision = 0;
    (void)mosquitto_lib_version(&major, &minor, &revision);
    (void)snprintf(text, sizeof(text), "%d.%d.%d", major, minor, revision);
    return text;
}
