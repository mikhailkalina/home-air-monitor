// SPDX-License-Identifier: Apache-2.0

#include "adp_net_sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_PER_S 1000.0

static void start_association(adp_net_sim_t *n, uint64_t at_ms)
{
    n->associating = true;
    n->associated_at_ms = at_ms + n->connect_ms;
}

static void apply_link(adp_net_sim_t *n, bool present, uint64_t at_ms)
{
    if (present == n->link) {
        return;
    }
    n->link = present;
    if (!present) {
        n->links_lost++;
        n->associating = false;
    } else if (n->wanted) {
        start_association(n, at_ms);  // the manager finds it again by itself
    }
    if (n->listener != NULL) {
        n->listener(n->listener_ctx, present, at_ms);
    }
}

// Everything the script says has happened by `now`, in order, each at its own
// time; then an association that has had its time is done.
static void catch_up(adp_net_sim_t *n)
{
    const uint64_t now = n->clock->now_ms(n->clock);
    while (n->next < n->count && n->events[n->next].at_ms <= now) {
        const adp_net_sim_event_t *ev = &n->events[n->next++];
        apply_link(n, ev->present, ev->at_ms);
    }
    if (n->associating && now >= n->associated_at_ms) {
        n->associating = false;
    }
}

static net_state_t sim_state(const port_net_t *port)
{
    adp_net_sim_t *n = (adp_net_sim_t *)port->impl;
    catch_up(n);
    if (!n->wanted) {
        return NET_DOWN;
    }
    if (!n->link || n->associating) {
        return NET_CONNECTING;
    }
    return NET_UP;
}

static int8_t sim_rssi(const port_net_t *port)
{
    const adp_net_sim_t *n = (const adp_net_sim_t *)port->impl;
    return (sim_state(port) == NET_UP) ? n->rssi_dbm : (int8_t)PORT_NET_RSSI_NONE;
}

static hal_status_t sim_up(port_net_t *port)
{
    adp_net_sim_t *n = (adp_net_sim_t *)port->impl;
    catch_up(n);
    if (n->wanted) {
        return HAL_OK;
    }
    n->wanted = true;
    n->ups++;
    if (n->link) {
        start_association(n, n->clock->now_ms(n->clock));
    }
    return HAL_OK;
}

static hal_status_t sim_down(port_net_t *port)
{
    adp_net_sim_t *n = (adp_net_sim_t *)port->impl;
    catch_up(n);
    if (n->wanted) {
        n->downs++;
    }
    n->wanted = false;
    n->associating = false;
    return HAL_OK;
}

void adp_net_sim_init(adp_net_sim_t *n, const port_clock_t *clock)
{
    memset(n, 0, sizeof(*n));
    n->clock = clock;
    n->rssi_dbm = (int8_t)ADP_NET_SIM_DEFAULT_RSSI_DBM;
    n->connect_ms = ADP_NET_SIM_CONNECT_MS;
    n->link = true;
    n->port.up = sim_up;
    n->port.down = sim_down;
    n->port.state = sim_state;
    n->port.rssi_dbm = sim_rssi;
    n->port.impl = n;
}

port_net_t *adp_net_sim_port(adp_net_sim_t *n)
{
    return &n->port;
}

void adp_net_sim_set_listener(adp_net_sim_t *n, adp_net_sim_listener_fn fn, void *ctx)
{
    n->listener = fn;
    n->listener_ctx = ctx;
}

void adp_net_sim_set_link(adp_net_sim_t *n, bool present)
{
    catch_up(n);
    apply_link(n, present, n->clock->now_ms(n->clock));
}

uint64_t adp_net_sim_next_ms(const adp_net_sim_t *n)
{
    uint64_t next = (n->next < n->count) ? n->events[n->next].at_ms : ADP_NET_SIM_NO_EVENT;
    if (n->associating && n->associated_at_ms < next) {
        next = n->associated_at_ms;
    }
    return next;
}

// One "<t_s>:down" or "<t_s>:up" from `p` up to `end`.
static bool parse_item(const char *p, const char *end, adp_net_sim_event_t *out, char *err,
                       size_t err_cap)
{
    char item[48];
    const size_t len = (size_t)(end - p);
    if (len == 0u || len >= sizeof(item)) {
        snprintf(err, err_cap, "a change is <seconds>:down or <seconds>:up, not \"%.*s\"", (int)len,
                 p);
        return false;
    }
    memcpy(item, p, len);
    item[len] = '\0';

    char *rest = NULL;
    const double t_s = strtod(item, &rest);
    if (rest == item || *rest != ':' || !isfinite(t_s) || t_s < 0.0) {
        snprintf(err, err_cap, "a change is <seconds>:down or <seconds>:up, not \"%s\"", item);
        return false;
    }
    const char *word = rest + 1;
    if (strcmp(word, "down") == 0) {
        out->present = false;
    } else if (strcmp(word, "up") == 0) {
        out->present = true;
    } else {
        snprintf(err, err_cap, "a change is <seconds>:down or <seconds>:up, not \"%s\"", item);
        return false;
    }
    out->at_ms = (uint64_t)llround(t_s * MS_PER_S);
    return true;
}

hal_status_t adp_net_sim_add_script(adp_net_sim_t *n, const char *text, char *err, size_t err_cap)
{
    adp_net_sim_event_t items[ADP_NET_SIM_MAX_EVENTS];
    size_t count = 0u;
    uint64_t last = (n->count > 0u) ? n->events[n->count - 1u].at_ms : 0u;

    const char *p = text;
    while (*p != '\0') {
        const char *end = strchr(p, ';');
        if (end == NULL) {
            end = p + strlen(p);
        }
        if (n->count + count >= ADP_NET_SIM_MAX_EVENTS) {
            snprintf(err, err_cap, "at most %u changes", (unsigned)ADP_NET_SIM_MAX_EVENTS);
            return HAL_ERR_INVALID_ARG;
        }
        if (!parse_item(p, end, &items[count], err, err_cap)) {
            return HAL_ERR_INVALID_ARG;
        }
        if ((count > 0u || n->count > 0u) && items[count].at_ms < last) {
            snprintf(err, err_cap, "the changes must come in time order");
            return HAL_ERR_INVALID_ARG;
        }
        last = items[count].at_ms;
        ++count;
        p = (*end == ';') ? end + 1 : end;
    }
    if (count == 0u) {
        snprintf(err, err_cap, "no changes in \"%s\"", text);
        return HAL_ERR_INVALID_ARG;
    }
    memcpy(&n->events[n->count], items, count * sizeof(items[0]));
    n->count += count;
    return HAL_OK;
}
