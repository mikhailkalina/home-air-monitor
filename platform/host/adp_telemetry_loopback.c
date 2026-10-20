// SPDX-License-Identifier: Apache-2.0

#include "adp_telemetry_loopback.h"

#include <string.h>

// MQTT 3.1.1, 3.1.2.10: one and a half keepalives without a packet.
#define KEEPALIVE_GRACE_NUM 3u
#define KEEPALIVE_GRACE_DEN 2u
#define MS_PER_S 1000u

static uint64_t now_ms(const loopback_broker_t *b)
{
    return b->clock->now_ms(b->clock);
}

static void tap(const loopback_broker_t *b, loopback_event_t event, const char *client_id,
                const loopback_message_t *msg, uint64_t at_ms)
{
    if (b->tap != NULL) {
        b->tap(b->tap_ctx, event, client_id, msg, at_ms);
    }
}

static void wake(const adp_telemetry_loopback_t *c)
{
    if (c->waker != NULL) {
        c->waker(c->waker_ctx);
    }
}

// --- matching ------------------------------------------------------------------

bool loopback_filter_matches(const char *f, const char *t)
{
    if (t[0] == '$' && (f[0] == '+' || f[0] == '#')) {
        return false;
    }
    for (;;) {
        if (f[0] == '#') {
            return true;
        }
        const char *fe = strchr(f, '/');
        const char *te = strchr(t, '/');
        if (fe == NULL) {
            fe = f + strlen(f);
        }
        if (te == NULL) {
            te = t + strlen(t);
        }
        const size_t fl = (size_t)(fe - f);
        const size_t tl = (size_t)(te - t);
        const bool any = (fl == 1u && f[0] == '+');
        if (!any && (fl != tl || memcmp(f, t, fl) != 0)) {
            return false;
        }
        if (*fe == '\0' && *te == '\0') {
            return true;
        }
        if (*fe == '\0') {
            return false;  // the topic goes on, the filter does not
        }
        if (*te == '\0') {
            return strcmp(fe, "/#") == 0;  // "a/#" matches "a" too (4.7.1.2)
        }
        f = fe + 1;
        t = te + 1;
    }
}

// --- delivery ------------------------------------------------------------------------

static void deliver(adp_telemetry_loopback_t *c, const loopback_message_t *m, bool retained)
{
    if (c->in_count >= ADP_TELEMETRY_LOOPBACK_INBOX) {
        c->inbox_dropped++;
        return;
    }
    telemetry_message_t *out = &c->inbox[(c->in_head + c->in_count) % ADP_TELEMETRY_LOOPBACK_INBOX];
    memset(out, 0, sizeof(*out));
    memcpy(out->topic, m->topic, sizeof(out->topic));
    out->truncated = m->len > sizeof(out->payload);
    out->len = out->truncated ? sizeof(out->payload) : m->len;
    memcpy(out->payload, m->payload, out->len);
    out->retained = retained;
    c->in_count++;
    wake(c);
}

static bool subscribed(const adp_telemetry_loopback_t *c, const char *topic)
{
    for (size_t i = 0u; i < c->sub_count; ++i) {
        if (loopback_filter_matches(c->subs[i].filter, topic)) {
            return true;
        }
    }
    return false;
}

static void keep(loopback_broker_t *b, const loopback_message_t *m)
{
    for (size_t i = 0u; i < b->retained_count; ++i) {
        if (strcmp(b->retained[i].topic, m->topic) == 0) {
            if (m->len == 0u) {
                b->retained[i] = b->retained[--b->retained_count];
            } else {
                b->retained[i] = *m;
            }
            return;
        }
    }
    if (m->len == 0u) {
        return;
    }
    if (b->retained_count >= LOOPBACK_BROKER_MAX_RETAINED) {
        b->retained_overflow++;
        return;
    }
    b->retained[b->retained_count++] = *m;
}

// A message published -- by a client, or as a will: kept if retained, and to
// every session with a subscription it matches, once each.
static void take(loopback_broker_t *b, const loopback_message_t *m)
{
    if (m->retain) {
        keep(b, m);
    }
    for (size_t i = 0u; i < b->client_count; ++i) {
        adp_telemetry_loopback_t *c = b->clients[i];
        if (c->connected && c->link && subscribed(c, m->topic)) {
            deliver(c, m, false);
        }
    }
}

static void publish_will(loopback_broker_t *b, loopback_will_t *w, uint64_t at_ms)
{
    w->pending = false;
    b->wills_published++;
    tap(b, LOOPBACK_EVENT_WILL, w->client_id, &w->msg, at_ms);
    take(b, &w->msg);
}

void loopback_broker_service(loopback_broker_t *b)
{
    const uint64_t now = now_ms(b);
    // In the order they fell due.
    for (;;) {
        loopback_will_t *first = NULL;
        for (size_t i = 0u; i < LOOPBACK_BROKER_MAX_CLIENTS; ++i) {
            loopback_will_t *w = &b->wills[i];
            if (w->pending && w->due_ms <= now && (first == NULL || w->due_ms < first->due_ms)) {
                first = w;
            }
        }
        if (first == NULL) {
            return;
        }
        publish_will(b, first, first->due_ms);
    }
}

uint64_t loopback_broker_next_ms(const loopback_broker_t *b)
{
    uint64_t next = LOOPBACK_BROKER_NO_DEADLINE;
    for (size_t i = 0u; i < LOOPBACK_BROKER_MAX_CLIENTS; ++i) {
        if (b->wills[i].pending && b->wills[i].due_ms < next) {
            next = b->wills[i].due_ms;
        }
    }
    return next;
}

// The client's queue to the broker, while it can take it.
static void pump(adp_telemetry_loopback_t *c)
{
    loopback_broker_t *b = c->broker;
    while (c->connected && c->link && !b->stalled && c->out_count > 0u) {
        const loopback_message_t *m = &c->outbox[c->out_head];
        b->messages++;
        tap(b, LOOPBACK_EVENT_MESSAGE, c->client_id, m, now_ms(b));
        take(b, m);
        c->out_head = (c->out_head + 1u) % ADP_TELEMETRY_LOOPBACK_OUTBOX;
        c->out_count--;
    }
}

// The session is over: what waited is lost, the subscriptions with it.
static void end_session(adp_telemetry_loopback_t *c)
{
    c->connected = false;
    c->attempting = false;
    c->outbox_lost += (uint32_t)c->out_count;
    c->out_count = 0u;
    c->out_head = 0u;
    c->sub_count = 0u;
}

// The session ended without disconnect(): its will falls due at `due_ms`.
static void will_pending(loopback_broker_t *b, const adp_telemetry_loopback_t *c, uint64_t due_ms,
                         bool now)
{
    if (!c->has_will) {
        return;
    }
    loopback_will_t *slot = NULL;
    for (size_t i = 0u; i < LOOPBACK_BROKER_MAX_CLIENTS && slot == NULL; ++i) {
        if (!b->wills[i].pending) {
            slot = &b->wills[i];
        }
    }
    if (slot == NULL) {
        return;  // one per client at most: cannot happen
    }
    memcpy(slot->client_id, c->client_id, sizeof(slot->client_id));
    slot->msg = c->will;
    slot->due_ms = due_ms;
    slot->pending = true;
    if (now) {
        publish_will(b, slot, due_ms);
    }
}

// --- the port --------------------------------------------------------------------------

static adp_telemetry_loopback_t *impl_of(const port_telemetry_t *port)
{
    return (adp_telemetry_loopback_t *)port->impl;
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

// A session already holding the id ends as if its network had failed, and a
// will still pending for the id -- a session dropped and not yet noticed --
// is published now: the broker learns at once that it is gone.
static void take_over(loopback_broker_t *b, const adp_telemetry_loopback_t *newcomer)
{
    const uint64_t now = now_ms(b);
    for (size_t i = 0u; i < b->client_count; ++i) {
        adp_telemetry_loopback_t *old = b->clients[i];
        if (old != newcomer && old->connected && strcmp(old->client_id, newcomer->client_id) == 0) {
            end_session(old);
            tap(b, LOOPBACK_EVENT_TAKEN_OVER, old->client_id, NULL, now);
            will_pending(b, old, now, true);
            wake(old);
        }
    }
    for (size_t i = 0u; i < LOOPBACK_BROKER_MAX_CLIENTS; ++i) {
        loopback_will_t *w = &b->wills[i];
        if (w->pending && strcmp(w->client_id, newcomer->client_id) == 0) {
            publish_will(b, w, now);
        }
    }
}

static hal_status_t lb_connect(port_telemetry_t *port, const telemetry_session_t *s)
{
    adp_telemetry_loopback_t *c = impl_of(port);
    loopback_broker_t *b = c->broker;
    if (!session_ok(s)) {
        return HAL_ERR_INVALID_ARG;
    }
    loopback_broker_service(b);
    if (c->connected || c->attempting) {
        return HAL_ERR_BUSY;
    }
    if (!c->link) {
        return HAL_ERR_NOT_READY;
    }
    if (b->refusal != LOOPBACK_ACCEPT && b->refuse_attempts > 0u) {
        if (b->refuse_attempts != LOOPBACK_BROKER_REFUSE_ALWAYS) {
            b->refuse_attempts--;
        }
        b->refused++;
        tap(b, LOOPBACK_EVENT_REFUSED, s->client_id, NULL, now_ms(b));
        if (b->refusal == LOOPBACK_SILENT) {
            c->attempting = true;
            return HAL_OK;
        }
        return HAL_ERR_IO;
    }

    memset(c->client_id, 0, sizeof(c->client_id));
    memcpy(c->client_id, s->client_id, strlen(s->client_id));
    take_over(b, c);
    c->keepalive_s = s->keepalive_s;
    c->has_will = (s->will_topic != NULL);
    memset(&c->will, 0, sizeof(c->will));
    if (c->has_will) {
        memcpy(c->will.topic, s->will_topic, strlen(s->will_topic));
        if (s->will_len > 0u) {
            memcpy(c->will.payload, s->will_payload, s->will_len);
        }
        c->will.len = s->will_len;
        c->will.qos = s->will_qos;
        c->will.retain = s->will_retain;
    }
    c->sub_count = 0u;
    c->out_head = 0u;
    c->out_count = 0u;
    c->connected = true;
    b->sessions++;
    tap(b, LOOPBACK_EVENT_CONNECTED, c->client_id, NULL, now_ms(b));
    return HAL_OK;
}

static hal_status_t lb_disconnect(port_telemetry_t *port)
{
    adp_telemetry_loopback_t *c = impl_of(port);
    loopback_broker_service(c->broker);
    if (c->connected) {
        // Only a connected client with its network can say goodbye: one whose
        // network went is no longer connected here.
        tap(c->broker, LOOPBACK_EVENT_DISCONNECTED, c->client_id, NULL, now_ms(c->broker));
    }
    end_session(c);
    return HAL_OK;
}

static hal_status_t lb_publish(port_telemetry_t *port, const char *topic, const uint8_t *payload,
                               size_t len, uint8_t qos, bool retain)
{
    adp_telemetry_loopback_t *c = impl_of(port);
    if (!port_telemetry_topic_ok(topic) || qos > 2u || len >= PORT_TELEMETRY_PAYLOAD_MAX ||
        (payload == NULL && len > 0u)) {
        return HAL_ERR_INVALID_ARG;
    }
    loopback_broker_service(c->broker);
    if (!c->connected) {
        return HAL_ERR_NOT_READY;
    }
    if (c->out_count >= ADP_TELEMETRY_LOOPBACK_OUTBOX) {
        return HAL_ERR_NO_MEM;
    }
    loopback_message_t *m =
        &c->outbox[(c->out_head + c->out_count) % ADP_TELEMETRY_LOOPBACK_OUTBOX];
    memset(m->topic, 0, sizeof(m->topic));
    memcpy(m->topic, topic, strlen(topic));
    if (len > 0u) {
        memcpy(m->payload, payload, len);
    }
    m->len = len;
    m->qos = qos;
    m->retain = retain;
    c->out_count++;
    c->published++;
    pump(c);
    return HAL_OK;
}

static hal_status_t lb_subscribe(port_telemetry_t *port, const char *filter, uint8_t qos)
{
    adp_telemetry_loopback_t *c = impl_of(port);
    if (!port_telemetry_filter_ok(filter) || qos > 2u) {
        return HAL_ERR_INVALID_ARG;
    }
    loopback_broker_service(c->broker);
    if (!c->connected) {
        return HAL_ERR_NOT_READY;
    }
    size_t slot = c->sub_count;
    for (size_t i = 0u; i < c->sub_count; ++i) {
        if (strcmp(c->subs[i].filter, filter) == 0) {
            slot = i;
        }
    }
    if (slot == ADP_TELEMETRY_LOOPBACK_MAX_SUBSCRIPTIONS) {
        return HAL_ERR_NO_MEM;
    }
    memset(c->subs[slot].filter, 0, sizeof(c->subs[slot].filter));
    memcpy(c->subs[slot].filter, filter, strlen(filter));
    c->subs[slot].qos = qos;
    if (slot == c->sub_count) {
        c->sub_count++;
    }
    const loopback_broker_t *b = c->broker;
    for (size_t i = 0u; i < b->retained_count; ++i) {
        if (loopback_filter_matches(filter, b->retained[i].topic)) {
            deliver(c, &b->retained[i], true);
        }
    }
    return HAL_OK;
}

static bool lb_is_connected(const port_telemetry_t *port)
{
    adp_telemetry_loopback_t *c = impl_of(port);
    loopback_broker_service(c->broker);
    return c->connected;
}

static bool lb_poll(port_telemetry_t *port, telemetry_message_t *out)
{
    adp_telemetry_loopback_t *c = impl_of(port);
    loopback_broker_service(c->broker);
    if (c->in_count == 0u) {
        return false;
    }
    *out = c->inbox[c->in_head];
    c->in_head = (c->in_head + 1u) % ADP_TELEMETRY_LOOPBACK_INBOX;
    c->in_count--;
    return true;
}

// --- set-up ------------------------------------------------------------------------------

void loopback_broker_init(loopback_broker_t *b, const port_clock_t *clock)
{
    memset(b, 0, sizeof(*b));
    b->clock = clock;
}

void loopback_broker_set_tap(loopback_broker_t *b, loopback_tap_fn fn, void *ctx)
{
    b->tap = fn;
    b->tap_ctx = ctx;
}

void loopback_broker_refuse(loopback_broker_t *b, loopback_refusal_t how, uint32_t attempts)
{
    b->refusal = (attempts == 0u) ? LOOPBACK_ACCEPT : how;
    b->refuse_attempts = (how == LOOPBACK_ACCEPT) ? 0u : attempts;
}

void loopback_broker_stall(loopback_broker_t *b, bool stalled)
{
    b->stalled = stalled;
    if (!stalled) {
        for (size_t i = 0u; i < b->client_count; ++i) {
            pump(b->clients[i]);
        }
    }
}

const loopback_message_t *loopback_broker_retained(const loopback_broker_t *b, const char *topic)
{
    for (size_t i = 0u; i < b->retained_count; ++i) {
        if (strcmp(b->retained[i].topic, topic) == 0) {
            return &b->retained[i];
        }
    }
    return NULL;
}

hal_status_t adp_telemetry_loopback_init(adp_telemetry_loopback_t *c, loopback_broker_t *broker)
{
    memset(c, 0, sizeof(*c));
    if (broker->client_count >= LOOPBACK_BROKER_MAX_CLIENTS) {
        return HAL_ERR_NO_MEM;
    }
    broker->clients[broker->client_count++] = c;
    c->broker = broker;
    c->link = true;
    c->port.connect = lb_connect;
    c->port.disconnect = lb_disconnect;
    c->port.publish = lb_publish;
    c->port.subscribe = lb_subscribe;
    c->port.is_connected = lb_is_connected;
    c->port.poll = lb_poll;
    c->port.impl = c;
    return HAL_OK;
}

port_telemetry_t *adp_telemetry_loopback_port(adp_telemetry_loopback_t *c)
{
    return &c->port;
}

void adp_telemetry_loopback_set_waker(adp_telemetry_loopback_t *c, loopback_waker_fn fn, void *ctx)
{
    c->waker = fn;
    c->waker_ctx = ctx;
}

void adp_telemetry_loopback_set_link(adp_telemetry_loopback_t *c, bool present, uint64_t at_ms)
{
    loopback_broker_t *b = c->broker;
    if (present == c->link) {
        return;
    }
    c->link = present;
    if (present) {
        return;
    }
    c->attempting = false;
    if (!c->connected) {
        return;
    }
    end_session(c);
    tap(b, LOOPBACK_EVENT_DROPPED, c->client_id, NULL, at_ms);
    // A keepalive of 0 is never dropped for silence: the will waits for a
    // connection that takes the id over.
    if (c->keepalive_s > 0u) {
        const uint64_t grace_ms =
            (uint64_t)c->keepalive_s * MS_PER_S * KEEPALIVE_GRACE_NUM / KEEPALIVE_GRACE_DEN;
        will_pending(b, c, at_ms + grace_ms, false);
    } else {
        will_pending(b, c, LOOPBACK_BROKER_NO_DEADLINE, false);
    }
    wake(c);
}

void adp_telemetry_loopback_net_listener(void *client, bool present, uint64_t at_ms)
{
    adp_telemetry_loopback_set_link((adp_telemetry_loopback_t *)client, present, at_ms);
}
