// SPDX-License-Identifier: Apache-2.0

#include "adp_net_pure.h"

#include <stddef.h>
#include <string.h>

adp_net_machine_config_t adp_net_machine_config_default(void)
{
    const adp_net_machine_config_t cfg = {
        .backoff_first_ms = ADP_NET_BACKOFF_FIRST_MS,
        .backoff_max_ms = ADP_NET_BACKOFF_MAX_MS,
        .attempt_timeout_ms = ADP_NET_ATTEMPT_TIMEOUT_MS,
    };
    return cfg;
}

void adp_net_machine_init(adp_net_machine_t *m, const adp_net_machine_config_t *cfg)
{
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    m->phase = ADP_NET_PHASE_IDLE;
    m->backoff_ms = cfg->backoff_first_ms;
    m->stats.last_reason = ADP_NET_REASON_NONE;
}

static adp_net_action_t begin_attempt(adp_net_machine_t *m, uint64_t now_ms)
{
    m->phase = ADP_NET_PHASE_JOINING;
    m->associated = false;
    m->deadline_ms = now_ms + m->cfg.attempt_timeout_ms;
    m->stats.attempts++;
    return ADP_NET_ACT_CONNECT;
}

// An attempt ended without an address: wait the backoff, which doubles for
// the next one.
static void attempt_failed(adp_net_machine_t *m, uint16_t reason, uint64_t now_ms)
{
    m->stats.attempts_failed++;
    m->stats.failures_in_a_row++;
    if (adp_net_reason_is_refusal(reason)) {
        m->stats.refusals_in_a_row++;
    }
    m->stats.last_reason = reason;
    m->phase = ADP_NET_PHASE_WAITING;
    m->associated = false;
    m->deadline_ms = now_ms + m->backoff_ms;
    const uint32_t doubled =
        (m->backoff_ms > m->cfg.backoff_max_ms / 2u) ? m->cfg.backoff_max_ms : m->backoff_ms * 2u;
    m->backoff_ms = (doubled < m->cfg.backoff_max_ms) ? doubled : m->cfg.backoff_max_ms;
}

adp_net_action_t adp_net_machine_up(adp_net_machine_t *m, uint64_t now_ms)
{
    if (m->phase != ADP_NET_PHASE_IDLE && m->phase != ADP_NET_PHASE_FAILED) {
        return ADP_NET_ACT_NONE;  // already asked for
    }
    m->phase = ADP_NET_PHASE_STARTING;
    m->radio_on = true;
    m->deadline_ms = now_ms + m->cfg.attempt_timeout_ms;
    m->backoff_ms = m->cfg.backoff_first_ms;
    m->stats.failures_in_a_row = 0u;
    m->stats.refusals_in_a_row = 0u;
    m->stats.sought_since_ms = now_ms;
    return ADP_NET_ACT_START;
}

adp_net_action_t adp_net_machine_down(adp_net_machine_t *m, uint64_t now_ms)
{
    (void)now_ms;
    m->phase = ADP_NET_PHASE_IDLE;
    m->associated = false;
    if (!m->radio_on) {
        return ADP_NET_ACT_NONE;
    }
    m->radio_on = false;
    return ADP_NET_ACT_STOP;
}

adp_net_action_t adp_net_machine_started(adp_net_machine_t *m, bool ok, uint64_t now_ms)
{
    if (m->phase != ADP_NET_PHASE_STARTING) {
        return ADP_NET_ACT_NONE;  // a start event from before a down()
    }
    if (!ok) {
        m->phase = ADP_NET_PHASE_FAILED;
        m->radio_on = false;
        return ADP_NET_ACT_NONE;
    }
    m->stats.starts++;
    return begin_attempt(m, now_ms);
}

adp_net_action_t adp_net_machine_connect_error(adp_net_machine_t *m, uint64_t now_ms)
{
    if (m->phase == ADP_NET_PHASE_JOINING) {
        attempt_failed(m, ADP_NET_REASON_CONNECT_ERROR, now_ms);
    }
    return ADP_NET_ACT_NONE;
}

void adp_net_machine_associated(adp_net_machine_t *m, uint64_t now_ms)
{
    (void)now_ms;
    if (m->phase == ADP_NET_PHASE_JOINING) {
        m->associated = true;
    }
}

adp_net_action_t adp_net_machine_got_address(adp_net_machine_t *m, uint64_t now_ms)
{
    if (m->phase != ADP_NET_PHASE_JOINING) {
        return ADP_NET_ACT_NONE;  // UP: a lease renewed; anything else: stale
    }
    m->phase = ADP_NET_PHASE_UP;
    m->deadline_ms = ADP_NET_NO_DEADLINE;
    m->backoff_ms = m->cfg.backoff_first_ms;
    m->stats.connections++;
    m->stats.failures_in_a_row = 0u;
    m->stats.refusals_in_a_row = 0u;
    m->stats.up_since_ms = now_ms;
    return ADP_NET_ACT_NONE;
}

adp_net_action_t adp_net_machine_lost_address(adp_net_machine_t *m, uint64_t now_ms)
{
    if (m->phase != ADP_NET_PHASE_UP) {
        return ADP_NET_ACT_NONE;
    }
    // Still associated, no address: an attempt that has associated, given
    // the timeout to get one back before it is given up and begun again.
    m->stats.losses++;
    m->stats.sought_since_ms = now_ms;
    m->phase = ADP_NET_PHASE_JOINING;
    m->associated = true;
    m->deadline_ms = now_ms + m->cfg.attempt_timeout_ms;
    return ADP_NET_ACT_NONE;
}

adp_net_action_t adp_net_machine_disconnected(adp_net_machine_t *m, uint16_t reason,
                                              uint64_t now_ms)
{
    if (m->phase == ADP_NET_PHASE_UP) {
        m->stats.losses++;
        m->stats.last_reason = reason;
        m->stats.sought_since_ms = now_ms;
        return begin_attempt(m, now_ms);  // at once: it was there a moment ago
    }
    if (m->phase == ADP_NET_PHASE_JOINING) {
        attempt_failed(m, reason, now_ms);
    }
    return ADP_NET_ACT_NONE;
}

adp_net_action_t adp_net_machine_tick(adp_net_machine_t *m, uint64_t now_ms)
{
    if (now_ms < m->deadline_ms) {
        return ADP_NET_ACT_NONE;
    }
    switch (m->phase) {
        case ADP_NET_PHASE_WAITING:
            return begin_attempt(m, now_ms);
        case ADP_NET_PHASE_JOINING:
            // The driver's disconnection, if it posts one, finds the machine
            // waiting and is ignored.
            m->stats.timeouts++;
            attempt_failed(m, ADP_NET_REASON_ATTEMPT_TIMEOUT, now_ms);
            return ADP_NET_ACT_DISCONNECT;
        case ADP_NET_PHASE_STARTING:
            // esp_wifi_start() returned, and its start event never came.
            m->phase = ADP_NET_PHASE_FAILED;
            m->radio_on = false;
            return ADP_NET_ACT_STOP;
        case ADP_NET_PHASE_IDLE:
        case ADP_NET_PHASE_UP:
        case ADP_NET_PHASE_FAILED:
            break;
    }
    return ADP_NET_ACT_NONE;
}

uint64_t adp_net_machine_next_ms(const adp_net_machine_t *m)
{
    switch (m->phase) {
        case ADP_NET_PHASE_STARTING:
        case ADP_NET_PHASE_JOINING:
        case ADP_NET_PHASE_WAITING:
            return m->deadline_ms;
        case ADP_NET_PHASE_IDLE:
        case ADP_NET_PHASE_UP:
        case ADP_NET_PHASE_FAILED:
            break;
    }
    return ADP_NET_NO_DEADLINE;
}

net_state_t adp_net_machine_state(const adp_net_machine_t *m)
{
    switch (m->phase) {
        case ADP_NET_PHASE_IDLE:
            return NET_DOWN;
        case ADP_NET_PHASE_UP:
            return NET_UP;
        case ADP_NET_PHASE_FAILED:
            return NET_FAILED;
        case ADP_NET_PHASE_STARTING:
        case ADP_NET_PHASE_JOINING:
        case ADP_NET_PHASE_WAITING:
            break;
    }
    return NET_CONNECTING;
}

const char *adp_net_phase_name(adp_net_phase_t phase)
{
    switch (phase) {
        case ADP_NET_PHASE_IDLE:
            return "idle";
        case ADP_NET_PHASE_STARTING:
            return "starting";
        case ADP_NET_PHASE_JOINING:
            return "joining";
        case ADP_NET_PHASE_UP:
            return "up";
        case ADP_NET_PHASE_WAITING:
            return "waiting";
        case ADP_NET_PHASE_FAILED:
            return "failed";
    }
    return "?";
}

typedef struct {
    uint16_t reason;
    const char *name;
} reason_name_t;

static const reason_name_t REASON_NAMES[] = {
    {ADP_NET_REASON_NONE, "none"},
    {ADP_NET_REASON_UNSPECIFIED, "unspecified"},
    {ADP_NET_REASON_AUTH_EXPIRE, "auth_expire"},
    {ADP_NET_REASON_AUTH_LEAVE, "auth_leave"},
    {ADP_NET_REASON_DISASSOC_DUE_TO_INACTIVITY, "inactivity"},
    {ADP_NET_REASON_ASSOC_TOOMANY, "assoc_toomany"},
    {ADP_NET_REASON_ASSOC_LEAVE, "assoc_leave"},
    {ADP_NET_REASON_MIC_FAILURE, "mic_failure"},
    {ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT, "4way_handshake_timeout"},
    {ADP_NET_REASON_802_1X_AUTH_FAILED, "802_1x_auth_failed"},
    {ADP_NET_REASON_BEACON_TIMEOUT, "beacon_timeout"},
    {ADP_NET_REASON_NO_AP_FOUND, "no_ap_found"},
    {ADP_NET_REASON_AUTH_FAIL, "auth_fail"},
    {ADP_NET_REASON_ASSOC_FAIL, "assoc_fail"},
    {ADP_NET_REASON_HANDSHAKE_TIMEOUT, "handshake_timeout"},
    {ADP_NET_REASON_CONNECTION_FAIL, "connection_fail"},
    {ADP_NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY, "no_ap_with_compatible_security"},
    {ADP_NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD, "no_ap_in_authmode_threshold"},
    {ADP_NET_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD, "no_ap_in_rssi_threshold"},
    {ADP_NET_REASON_ATTEMPT_TIMEOUT, "attempt_timeout"},
    {ADP_NET_REASON_CONNECT_ERROR, "connect_error"},
};

const char *adp_net_reason_name(uint16_t reason)
{
    for (size_t i = 0u; i < sizeof(REASON_NAMES) / sizeof(REASON_NAMES[0]); ++i) {
        if (REASON_NAMES[i].reason == reason) {
            return REASON_NAMES[i].name;
        }
    }
    return "other";
}

bool adp_net_reason_is_refusal(uint16_t reason)
{
    switch (reason) {
        case ADP_NET_REASON_MIC_FAILURE:
        case ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case ADP_NET_REASON_802_1X_AUTH_FAILED:
        case ADP_NET_REASON_AUTH_FAIL:
        case ADP_NET_REASON_HANDSHAKE_TIMEOUT:
            return true;
        default:
            return false;
    }
}
