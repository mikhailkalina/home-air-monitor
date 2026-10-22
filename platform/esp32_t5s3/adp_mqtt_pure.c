// SPDX-License-Identifier: Apache-2.0

#include "adp_mqtt_pure.h"

#include <string.h>

// The largest port number, and how many digits it has.
#define URI_PORT_MAX 65535u
#define URI_PORT_DIGITS 5u
#define DECIMAL 10u

// MQTT 3.1.1, 3.2.2.3.
#define CONNACK_ACCEPTED 0
#define CONNACK_BAD_PROTOCOL 1
#define CONNACK_ID_REJECTED 2
#define CONNACK_SERVER_UNAVAILABLE 3
#define CONNACK_BAD_CREDENTIALS 4
#define CONNACK_NOT_AUTHORISED 5

// --- the broker's address --------------------------------------------------------

static bool host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_';
}

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

const char *adp_mqtt_parse_uri(const char *uri, adp_mqtt_broker_t *out)
{
    static const char scheme[] = "mqtt://";
    memset(out, 0, sizeof(*out));
    if (uri == NULL || uri[0] == '\0') {
        return "no broker: a broker is mqtt://<host>[:<port>]";
    }
    if (starts_with(uri, "mqtts://")) {
        return "TLS is not supported: the board speaks plain MQTT, to a broker on its own "
               "network (docs/adr/0033-mqtt-on-the-board.md)";
    }
    if (!starts_with(uri, scheme)) {
        return "a broker is mqtt://<host>[:<port>]";
    }
    const char *p = uri + sizeof(scheme) - 1u;
    if (strchr(p, '@') != NULL) {
        return "no user or password in the URI: they have symbols of their own, "
               "CONFIG_HAC_MQTT_USER and CONFIG_HAC_MQTT_PASSWORD";
    }
    const char *host = p;
    while (host_char(*p)) {
        ++p;
    }
    const size_t host_len = (size_t)(p - host);
    if (host_len == 0u) {
        return "no host: a broker is mqtt://<host>[:<port>]";
    }
    if (host_len > ADP_MQTT_HOST_MAX) {
        return "the broker's host is longer than ADP_MQTT_HOST_MAX characters";
    }
    unsigned long port = ADP_MQTT_DEFAULT_PORT;
    if (*p == ':') {
        ++p;
        const char *digits = p;
        port = 0u;
        while (*p >= '0' && *p <= '9' && (size_t)(p - digits) < URI_PORT_DIGITS) {
            port = port * DECIMAL + (unsigned long)(*p - '0');
            ++p;
        }
        if (p == digits || port == 0u || port > URI_PORT_MAX || (*p >= '0' && *p <= '9')) {
            return "the broker's port is a number from 1 to 65535";
        }
    }
    if (*p != '\0') {
        return "nothing may follow the host and the port: mqtt://<host>[:<port>]";
    }
    memcpy(out->host, host, host_len);
    out->port = (uint16_t)port;
    return NULL;
}

// --- a ring of slots -------------------------------------------------------------------

void adp_mqtt_ring_init(adp_mqtt_ring_t *r, uint16_t capacity)
{
    r->capacity = capacity;
    r->head = 0u;
    r->count = 0u;
    r->reserved = false;
}

int adp_mqtt_ring_reserve(adp_mqtt_ring_t *r)
{
    if (r->reserved || r->count >= r->capacity) {
        return -1;
    }
    r->reserved = true;
    return (int)(((unsigned)r->head + (unsigned)r->count) % (unsigned)r->capacity);
}

void adp_mqtt_ring_commit(adp_mqtt_ring_t *r)
{
    if (r->reserved) {
        r->reserved = false;
        r->count++;
    }
}

void adp_mqtt_ring_cancel(adp_mqtt_ring_t *r)
{
    r->reserved = false;
}

int adp_mqtt_ring_peek(const adp_mqtt_ring_t *r)
{
    return (r->count > 0u) ? (int)r->head : -1;
}

void adp_mqtt_ring_release(adp_mqtt_ring_t *r)
{
    if (r->count == 0u) {
        return;
    }
    r->head = (uint16_t)(((unsigned)r->head + 1u) % (unsigned)r->capacity);
    r->count--;
}

// --- a message in pieces ------------------------------------------------------------

void adp_mqtt_assembly_reset(adp_mqtt_assembly_t *a)
{
    memset(a, 0, sizeof(*a));
}

adp_mqtt_piece_result_t adp_mqtt_assemble(adp_mqtt_assembly_t *a, const adp_mqtt_piece_t *p)
{
    if (p->data_len > 0u && p->data == NULL) {
        a->under_way = false;
        return (p->offset == 0u) ? ADP_MQTT_PIECE_REFUSED : ADP_MQTT_PIECE_STRAY;
    }
    if (p->offset == 0u) {
        // A new message: one still under way never got its end.
        a->under_way = false;
        if (p->topic == NULL || p->topic_len == 0u || p->topic_len >= PORT_TELEMETRY_TOPIC_MAX ||
            memchr(p->topic, '\0', p->topic_len) != NULL) {
            return ADP_MQTT_PIECE_REFUSED;
        }
        memset(&a->msg, 0, sizeof(a->msg));
        memcpy(a->msg.topic, p->topic, p->topic_len);
        a->msg.retained = p->retain;
        a->total = p->total;
        a->next_offset = 0u;
        a->under_way = true;
    } else if (!a->under_way || p->offset != a->next_offset) {
        a->under_way = false;
        return ADP_MQTT_PIECE_STRAY;
    }

    const size_t room = sizeof(a->msg.payload) - a->msg.len;
    const size_t n = (p->data_len < room) ? p->data_len : room;
    if (n > 0u) {
        memcpy(a->msg.payload + a->msg.len, p->data, n);
        a->msg.len += n;
    }
    if (n < p->data_len) {
        a->msg.truncated = true;
    }
    a->next_offset = p->offset + p->data_len;
    if (a->next_offset < a->total) {
        return ADP_MQTT_PIECE_MORE;
    }
    a->under_way = false;
    if (a->total > sizeof(a->msg.payload)) {
        a->msg.truncated = true;
    }
    return ADP_MQTT_PIECE_DONE;
}

// --- the session -----------------------------------------------------------------

void adp_mqtt_session_init(adp_mqtt_session_t *s)
{
    memset(s, 0, sizeof(*s));
}

hal_status_t adp_mqtt_session_connect(adp_mqtt_session_t *s, bool net_up)
{
    if (s->wanted && !s->over) {
        return HAL_ERR_BUSY;
    }
    if (!net_up) {
        return HAL_ERR_NOT_READY;
    }
    s->gen++;
    if (s->gen == 0u) {
        s->gen = 1u;  // 0 is no session
    }
    s->wanted = true;
    s->accepted = false;
    s->over = false;
    s->end = ADP_MQTT_END_NONE;
    return HAL_OK;
}

void adp_mqtt_session_disconnect(adp_mqtt_session_t *s)
{
    s->wanted = false;
}

bool adp_mqtt_session_usable(adp_mqtt_session_t *s, bool net_up)
{
    if (!s->wanted || !s->accepted || s->over) {
        return false;
    }
    if (!net_up) {
        s->over = true;
        s->end = ADP_MQTT_END_NETWORK;
        return false;
    }
    return true;
}

bool adp_mqtt_session_holds(const adp_mqtt_session_t *s, uint32_t gen)
{
    return gen != 0u && gen == s->gen && s->wanted && !s->over;
}

bool adp_mqtt_session_accepted(adp_mqtt_session_t *s, uint32_t gen)
{
    if (!adp_mqtt_session_holds(s, gen) || s->accepted) {
        return false;
    }
    s->accepted = true;
    return true;
}

bool adp_mqtt_session_ended(adp_mqtt_session_t *s, uint32_t gen, bool refused)
{
    if (!adp_mqtt_session_holds(s, gen)) {
        return false;
    }
    s->over = true;
    if (s->accepted) {
        s->end = ADP_MQTT_END_LOST;
    } else {
        s->end = refused ? ADP_MQTT_END_REFUSED : ADP_MQTT_END_NO_CONNECTION;
    }
    return true;
}

void adp_mqtt_session_not_started(adp_mqtt_session_t *s, uint32_t gen)
{
    if (adp_mqtt_session_holds(s, gen)) {
        s->over = true;
        s->end = ADP_MQTT_END_NOT_STARTED;
    }
}

adp_mqtt_state_t adp_mqtt_session_state(const adp_mqtt_session_t *s)
{
    if (!s->wanted) {
        return ADP_MQTT_STATE_IDLE;
    }
    if (s->over) {
        return ADP_MQTT_STATE_OVER;
    }
    return s->accepted ? ADP_MQTT_STATE_CONNECTED : ADP_MQTT_STATE_CONNECTING;
}

adp_mqtt_plan_t adp_mqtt_session_plan(const adp_mqtt_session_t *s, uint32_t client_gen,
                                      uint32_t started_gen)
{
    if (client_gen != 0u) {
        if (client_gen != s->gen || !s->wanted || s->over) {
            return ADP_MQTT_PLAN_STOP;
        }
        return s->accepted ? ADP_MQTT_PLAN_FORWARD : ADP_MQTT_PLAN_IDLE;
    }
    if (s->gen != 0u && s->wanted && !s->over && started_gen != s->gen) {
        return ADP_MQTT_PLAN_START;
    }
    return ADP_MQTT_PLAN_IDLE;
}

// --- a session's subscriptions ----------------------------------------------------

void adp_mqtt_subs_init(adp_mqtt_subs_t *subs)
{
    memset(subs, 0, sizeof(*subs));
}

bool adp_mqtt_subs_take(adp_mqtt_subs_t *subs, uint32_t gen, const char *filter)
{
    if (subs->gen != gen) {
        subs->gen = gen;
        subs->count = 0u;
    }
    for (size_t i = 0u; i < subs->count; ++i) {
        if (strcmp(subs->filters[i], filter) == 0) {
            return true;
        }
    }
    const size_t n = strlen(filter);
    if (subs->count >= ADP_MQTT_MAX_SUBSCRIPTIONS || n >= PORT_TELEMETRY_TOPIC_MAX) {
        return false;
    }
    memcpy(subs->filters[subs->count], filter, n + 1u);
    subs->count++;
    return true;
}

// --- names ---------------------------------------------------------------------------

const char *adp_mqtt_end_name(adp_mqtt_end_t end)
{
    switch (end) {
        case ADP_MQTT_END_NONE:
            return "none";
        case ADP_MQTT_END_NO_CONNECTION:
            return "no connection";
        case ADP_MQTT_END_REFUSED:
            return "refused";
        case ADP_MQTT_END_LOST:
            return "lost";
        case ADP_MQTT_END_NETWORK:
            return "network";
        case ADP_MQTT_END_NOT_STARTED:
            return "not started";
    }
    return "?";
}

const char *adp_mqtt_state_name(adp_mqtt_state_t state)
{
    switch (state) {
        case ADP_MQTT_STATE_IDLE:
            return "idle";
        case ADP_MQTT_STATE_CONNECTING:
            return "connecting";
        case ADP_MQTT_STATE_CONNECTED:
            return "connected";
        case ADP_MQTT_STATE_OVER:
            return "over";
    }
    return "?";
}

const char *adp_mqtt_connack_name(int code)
{
    switch (code) {
        case CONNACK_ACCEPTED:
            return "accepted";
        case CONNACK_BAD_PROTOCOL:
            return "bad protocol version";
        case CONNACK_ID_REJECTED:
            return "client id rejected";
        case CONNACK_SERVER_UNAVAILABLE:
            return "server unavailable";
        case CONNACK_BAD_CREDENTIALS:
            return "bad user name or password";
        case CONNACK_NOT_AUTHORISED:
            return "not authorised";
        default:
            return "unknown";
    }
}
