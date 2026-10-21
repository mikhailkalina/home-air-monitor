// SPDX-License-Identifier: Apache-2.0

#include "ui/diagnostics_view.h"

#include <string.h>

#include "text/text_append.h"
#include "ui/view_model.h"

// --- text, without stdio ---------------------------------------------------------

// Appends `src` to `dst`, which holds `len` characters, never past `cap`;
// keeps it terminated. Returns the new length.
static size_t append(char *dst, size_t cap, size_t len, const char *src)
{
    if (cap == 0u) {
        return 0u;
    }
    while (*src != '\0' && len + 1u < cap) {
        dst[len++] = *src++;
    }
    dst[len] = '\0';
    return len;
}

// "0" to "999", then "999+".
static void count_text(char *dst, size_t cap, uint32_t n)
{
    const bool more = n > DIAG_COUNT_SHOWN_MAX;
    uint32_t v = more ? DIAG_COUNT_SHOWN_MAX : n;
    char digits[4];
    size_t d = 0u;
    do {
        digits[d++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u && d < sizeof(digits));
    size_t len = 0u;
    dst[0] = '\0';
    while (d > 0u && len + 1u < cap) {
        dst[len++] = digits[--d];
    }
    dst[len] = '\0';
    if (more) {
        (void)append(dst, cap, len, "+");
    }
}

// --- sources -------------------------------------------------------------------------

const char *diag_view_slot_state(const sensor_manager_slot_t *slot)
{
    switch (slot->state) {
        case SENSOR_SLOT_UNINIT:
            return "starting";
        case SENSOR_SLOT_BACKOFF:
            return "restarting";
        case SENSOR_SLOT_IDLE:
        case SENSOR_SLOT_MEASURING:
            break;
    }
    if (slot->failing) {
        return "failing";
    }
    return slot->ever_ready ? "running" : "starting";
}

static void build_slot(diag_slot_view_t *out, const sensor_manager_slot_t *slot)
{
    const char *name = (slot->source->name != NULL) ? slot->source->name : "?";
    (void)append(out->name, sizeof(out->name), 0u, name);
    out->state = diag_view_slot_state(slot);
    out->trouble = slot->failing || slot->state == SENSOR_SLOT_BACKOFF;
    count_text(out->failures, sizeof(out->failures), slot->failures);
    // The first init() is the start; every one after it is a restart.
    count_text(out->restarts, sizeof(out->restarts), (slot->inits > 0u) ? slot->inits - 1u : 0u);
}

// The name of the slot whose source produced `m`, or NULL.
static const char *source_name(const sensor_manager_t *sensors, const measurement_f32_t *m)
{
    if (sensors == NULL || m->status == VAL_UNAVAILABLE || m->source_id == 0u) {
        return NULL;
    }
    for (size_t i = 0u; i < sensors->count; ++i) {
        const sensor_source_t *src = sensors->slots[i].source;
        if (src != NULL && src->id == m->source_id) {
            return (src->name != NULL) ? src->name : "?";
        }
    }
    return NULL;
}

// --- readings ------------------------------------------------------------------------

// The home screen's words for a status (view_model.h): the number's own
// place shows the number there; here the word.
static const char *status_word(value_status_t status)
{
    switch (status) {
        case VAL_OK:
            return "ok";
        case VAL_WARMUP:
            return "warm";
        case VAL_STALE:
            return "stale";
        case VAL_ERROR:
            return "ERR";
        case VAL_UNAVAILABLE:
            break;
    }
    return "n/a";
}

static void build_field(diag_field_view_t *out, const char *label, const measurement_f32_t *m,
                        const sensor_manager_t *sensors)
{
    out->label = label;
    out->status = status_word(m->status);
    out->accuracy = measurement_accuracy_name(m->accuracy);  // "-" when not reported
    const char *src = source_name(sensors, m);
    (void)append(out->source, sizeof(out->source), 0u, (src != NULL) ? src : "");
    out->trouble = m->status == VAL_ERROR || (m->status == VAL_OK && measurement_low_accuracy(m));
}

// --- the rest ----------------------------------------------------------------------

static const char *recorder_word(recorder_state_t state)
{
    switch (state) {
        case RECORDER_WAITING:
            return "waiting";
        case RECORDER_ON:
            return "on";
        case RECORDER_NO_MEDIUM:
            return "no medium";
        case RECORDER_FULL:
            return "full";
        case RECORDER_FAILED:
            break;
    }
    return "failed";
}

// --- the network --------------------------------------------------------------------

static const char *wifi_word(net_state_t state)
{
    switch (state) {
        case NET_UP:
            return "up";
        case NET_CONNECTING:
            return "joining";  // "connecting" does not fit the column
        case NET_FAILED:
            return "failed";
        case NET_DOWN:
            break;
    }
    return "off";
}

// The name as the port gives it, every octet outside printable ASCII -- which
// the fonts do not cover -- as '?'.
static void printable(char *dst, size_t cap, const char *src)
{
    size_t n = 0u;
    for (; src[n] != '\0' && n + 1u < cap; ++n) {
        const unsigned char c = (unsigned char)src[n];
        dst[n] = (c >= (unsigned char)' ' && c <= (unsigned char)'~') ? (char)c : '?';
    }
    dst[n] = '\0';
}

static void address_text(char *dst, size_t cap, const uint8_t ip[PORT_NET_IPV4_LEN])
{
    size_t len = 0u;
    for (size_t i = 0u; i < PORT_NET_IPV4_LEN; ++i) {
        if (i > 0u) {
            len = text_append_char(dst, cap, len, '.');
        }
        len = text_append_u64(dst, cap, len, ip[i], 1u);
    }
}

// "sent 412", "sent 12k", "sent 999k+" (DIAG_SENT_EXACT_MAX).
static void sent_text(char *dst, size_t cap, uint32_t n)
{
    size_t len = text_append_str(dst, cap, 0u, "sent ");
    if (n <= DIAG_SENT_EXACT_MAX) {
        (void)text_append_u64(dst, cap, len, n, 1u);
        return;
    }
    const uint32_t thousands = n / DIAG_SENT_PER_THOUSAND;
    len = text_append_u64(
        dst, cap, len, (thousands > DIAG_SENT_THOUSANDS_MAX) ? DIAG_SENT_THOUSANDS_MAX : thousands,
        1u);
    (void)text_append_str(dst, cap, len, (thousands > DIAG_SENT_THOUSANDS_MAX) ? "k+" : "k");
}

// When the last state message went, in local time.
static void last_text(char *dst, size_t cap, const reporter_status_t *n, const diag_facts_t *f)
{
    const size_t len = text_append_str(dst, cap, 0u, "last ");
    if (!n->published) {
        (void)text_append_str(dst, cap, len, "-");
        return;
    }
    const uint64_t wall = f->reading->wall_time_ms;
    const uint64_t age = f->now_ms - n->last_published_ms;
    if (wall == 0u || f->zone == NULL || f->now_ms < n->last_published_ms || age > wall) {
        (void)text_append_str(dst, cap, len, "--:--");  // no local time to give
        return;
    }
    char when[VM_UPDATED_TEXT_MAX];
    view_model_local_time_text(when, sizeof(when), wall - age, wall, f->zone);
    (void)text_append_str(dst, cap, len, when);
}

static void build_network(diag_view_t *v, const diag_facts_t *f)
{
    v->link = f->reading->link;
    v->link_text = view_model_link_text(v->link);
    v->link_trouble = v->link == LINK_NO_WIFI || v->link == LINK_NO_BROKER;
    const reporter_status_t *n = f->network;
    v->network_present = n != NULL && n->link != LINK_OFF;
    if (!v->network_present) {
        v->wifi_state = "off";
        v->broker_state = "off";
        return;
    }
    if (n->has_net) {
        v->wifi_state = wifi_word(n->net);
        v->wifi_trouble = n->net != NET_UP;
        printable(v->ssid, sizeof(v->ssid), n->net_info.ssid);
        if (n->net == NET_UP) {
            size_t len = text_append_i64(v->rssi, sizeof(v->rssi), 0u, n->rssi_dbm);
            (void)text_append_str(v->rssi, sizeof(v->rssi), len, " dBm");
            if (port_net_ipv4_set(n->net_info.ipv4)) {
                address_text(v->address, sizeof(v->address), n->net_info.ipv4);
            }
        }
    } else {
        v->wifi_state = "-";  // no network to watch: the reporter takes the link as up
    }
    v->broker_state = (n->state == REPORTER_ONLINE) ? "online" : "offline";
    v->broker_trouble = n->state != REPORTER_ONLINE;
    sent_text(v->sent, sizeof(v->sent), n->state_messages);
    char count[DIAG_COUNT_TEXT_MAX];
    count_text(count, sizeof(count), n->refusals);
    const size_t len = text_append_str(v->refused, sizeof(v->refused), 0u, "refused ");
    (void)text_append_str(v->refused, sizeof(v->refused), len, count);
    v->refused_trouble = n->refusals > 0u;
    last_text(v->last, sizeof(v->last), n, f);
}

void diag_view_build(diag_view_t *v, const diag_facts_t *facts)
{
    memset(v, 0, sizeof(*v));
    const air_reading_t *r = facts->reading;
    const sensor_manager_t *sensors = facts->sensors;

    if (sensors != NULL) {
        for (size_t i = 0u; i < sensors->count && i < DIAG_MAX_SLOTS; ++i) {
            build_slot(&v->slots[i], &sensors->slots[i]);
            v->slot_count++;
        }
    }

    build_field(&v->fields[DIAG_FIELD_CO2], "CO2", &r->co2_ppm, sensors);
    build_field(&v->fields[DIAG_FIELD_TEMPERATURE], "Temperature", &r->temperature_c, sensors);
    build_field(&v->fields[DIAG_FIELD_HUMIDITY], "Humidity", &r->humidity_rh, sensors);
    build_field(&v->fields[DIAG_FIELD_PRESSURE], "Pressure", &r->pressure_hpa, sensors);
    build_field(&v->fields[DIAG_FIELD_IAQ], "IAQ", &r->iaq, sensors);
    build_field(&v->fields[DIAG_FIELD_GAS], "Gas", &r->gas_resistance_ohm, sensors);

    v->clock_set = r->wall_time_ms != 0u;
    size_t len = append(v->clock_text, sizeof(v->clock_text), 0u,
                        v->clock_set ? "Clock: set" : "Clock: not set");
    if (v->clock_set && facts->clock_source != NULL && facts->clock_source[0] != '\0') {
        len = append(v->clock_text, sizeof(v->clock_text), len, " (");
        len = append(v->clock_text, sizeof(v->clock_text), len, facts->clock_source);
        (void)append(v->clock_text, sizeof(v->clock_text), len, ")");
    }

    len = append(v->recorder_text, sizeof(v->recorder_text), 0u, "Recording: ");
    if (!facts->recorder.present) {
        (void)append(v->recorder_text, sizeof(v->recorder_text), len, "off");
    } else {
        (void)append(v->recorder_text, sizeof(v->recorder_text), len,
                     recorder_word(facts->recorder.state));
        v->recorder_trouble = facts->recorder.state == RECORDER_NO_MEDIUM ||
                              facts->recorder.state == RECORDER_FULL ||
                              facts->recorder.state == RECORDER_FAILED;
    }

    build_network(v, facts);
}

static bool same_text(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

bool diag_view_equal(const diag_view_t *a, const diag_view_t *b)
{
    if (a->slot_count != b->slot_count || a->clock_set != b->clock_set ||
        a->recorder_trouble != b->recorder_trouble || !same_text(a->clock_text, b->clock_text) ||
        !same_text(a->recorder_text, b->recorder_text)) {
        return false;
    }
    // The network: what redraws it -- the held link, drawn as the title's
    // word -- and nothing that is as of the drawing.
    if (a->network_present != b->network_present || a->link != b->link ||
        a->link_trouble != b->link_trouble || !same_text(a->link_text, b->link_text) ||
        !same_text(a->ssid, b->ssid)) {
        return false;
    }
    for (size_t i = 0u; i < a->slot_count; ++i) {
        const diag_slot_view_t *x = &a->slots[i];
        const diag_slot_view_t *y = &b->slots[i];
        if (x->trouble != y->trouble || !same_text(x->name, y->name) ||
            !same_text(x->state, y->state) || !same_text(x->failures, y->failures) ||
            !same_text(x->restarts, y->restarts)) {
            return false;
        }
    }
    for (size_t i = 0u; i < DIAG_FIELD_COUNT; ++i) {
        const diag_field_view_t *x = &a->fields[i];
        const diag_field_view_t *y = &b->fields[i];
        if (x->trouble != y->trouble || !same_text(x->label, y->label) ||
            !same_text(x->status, y->status) || !same_text(x->accuracy, y->accuracy) ||
            !same_text(x->source, y->source)) {
            return false;
        }
    }
    return true;
}
