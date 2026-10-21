// SPDX-License-Identifier: Apache-2.0
//
// platform/esp32_t5s3/adp_net_pure.c, compiled on the host from where the
// firmware compiles it (see tests/CMakeLists.txt): the Wi-Fi station's
// decisions, fed what the callers ask and what ESP-IDF's driver reports.

#include "adp_net_pure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hac_test.h"

#define T0 1000u

static adp_net_machine_t g_m;

static void fresh(void)
{
    const adp_net_machine_config_t cfg = adp_net_machine_config_default();
    adp_net_machine_init(&g_m, &cfg);
}

// Asked for, the radio started: an attempt under way at `now`.
static void joining_at(uint64_t now)
{
    fresh();
    HAC_CHECK_EQ_INT(adp_net_machine_up(&g_m, now), ADP_NET_ACT_START);
    HAC_CHECK_EQ_INT(adp_net_machine_started(&g_m, true, now), ADP_NET_ACT_CONNECT);
}

// ... and up.
static void up_at(uint64_t now)
{
    joining_at(now);
    adp_net_machine_associated(&g_m, now);
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, now), ADP_NET_ACT_NONE);
}

static void the_defaults_are_the_limits_header(void)
{
    const adp_net_machine_config_t cfg = adp_net_machine_config_default();
    HAC_CHECK_EQ_U64(cfg.backoff_first_ms, ADP_NET_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_U64(cfg.backoff_max_ms, ADP_NET_BACKOFF_MAX_MS);
    HAC_CHECK_EQ_U64(cfg.attempt_timeout_ms, ADP_NET_ATTEMPT_TIMEOUT_MS);
    HAC_CHECK(ADP_NET_BACKOFF_FIRST_MS <= ADP_NET_BACKOFF_MAX_MS);
}

static void it_starts_down_with_nothing_to_do(void)
{
    fresh();
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_DOWN);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), ADP_NET_NO_DEADLINE);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, UINT64_MAX - 1u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_DOWN);
    HAC_CHECK_EQ_INT(g_m.stats.last_reason, ADP_NET_REASON_NONE);
}

static void up_starts_the_radio_and_then_an_attempt(void)
{
    fresh();
    HAC_CHECK_EQ_INT(adp_net_machine_up(&g_m, T0), ADP_NET_ACT_START);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), T0 + ADP_NET_ATTEMPT_TIMEOUT_MS);
    HAC_CHECK_EQ_INT(adp_net_machine_up(&g_m, T0 + 1u), ADP_NET_ACT_NONE);  // already asked

    HAC_CHECK_EQ_INT(adp_net_machine_started(&g_m, true, T0 + 50u), ADP_NET_ACT_CONNECT);
    HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_JOINING);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), T0 + 50u + ADP_NET_ATTEMPT_TIMEOUT_MS);
    HAC_CHECK_EQ_U64(g_m.stats.starts, 1u);
    HAC_CHECK_EQ_U64(g_m.stats.attempts, 1u);
}

static void an_address_is_up_and_waits_for_nothing(void)
{
    joining_at(T0);
    adp_net_machine_associated(&g_m, T0 + 1000u);
    HAC_CHECK(g_m.associated);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);  // associated is not up
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, T0 + 1500u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_UP);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), ADP_NET_NO_DEADLINE);
    HAC_CHECK_EQ_U64(g_m.stats.connections, 1u);
    HAC_CHECK_EQ_U64(g_m.stats.up_since_ms, T0 + 1500u);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, T0 + 3600000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_UP);

    // A lease renewed: still up, not a second connection.
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, T0 + 7200000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_UP);
    HAC_CHECK_EQ_U64(g_m.stats.connections, 1u);
}

static void a_radio_that_will_not_start_is_given_up_until_up_again(void)
{
    fresh();
    (void)adp_net_machine_up(&g_m, T0);
    HAC_CHECK_EQ_INT(adp_net_machine_started(&g_m, false, T0 + 5u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_FAILED);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), ADP_NET_NO_DEADLINE);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, T0 + 3600000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_FAILED);
    HAC_CHECK(!g_m.radio_on);

    // up() again tries again (port_net.h).
    HAC_CHECK_EQ_INT(adp_net_machine_up(&g_m, T0 + 3600001u), ADP_NET_ACT_START);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);
}

static void a_start_event_that_never_comes_fails_and_stops_the_radio(void)
{
    fresh();
    (void)adp_net_machine_up(&g_m, T0);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, T0 + ADP_NET_ATTEMPT_TIMEOUT_MS - 1u),
                     ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, T0 + ADP_NET_ATTEMPT_TIMEOUT_MS), ADP_NET_ACT_STOP);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_FAILED);
    HAC_CHECK_EQ_INT(adp_net_machine_down(&g_m, T0 + ADP_NET_ATTEMPT_TIMEOUT_MS + 1u),
                     ADP_NET_ACT_NONE);  // already stopped
}

static void a_lost_link_is_sought_again_at_once(void)
{
    up_at(T0);
    const uint64_t lost = T0 + 60000u;
    HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_BEACON_TIMEOUT, lost),
                     ADP_NET_ACT_CONNECT);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);  // never down while asked
    HAC_CHECK_EQ_U64(g_m.stats.losses, 1u);
    HAC_CHECK_EQ_U64(g_m.stats.attempts_failed, 0u);
    HAC_CHECK_EQ_U64(g_m.stats.attempts, 2u);
    HAC_CHECK_EQ_INT(g_m.stats.last_reason, ADP_NET_REASON_BEACON_TIMEOUT);
    HAC_CHECK_EQ_U64(g_m.stats.sought_since_ms, lost);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), lost + ADP_NET_ATTEMPT_TIMEOUT_MS);

    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, lost + 3000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_UP);
    HAC_CHECK_EQ_U64(g_m.stats.connections, 2u);
}

// Every failed attempt waits the backoff, 1 s doubling to 30 s, and the next
// attempt begins exactly when it is over.
static void failed_attempts_back_off_doubling_to_the_ceiling(void)
{
    joining_at(T0);
    const uint32_t expected[] = {1000u, 2000u, 4000u, 8000u, 16000u, 30000u, 30000u, 30000u};
    uint64_t now = T0;
    for (size_t i = 0u; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        now += 2500u;  // a scan that found nothing
        HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, now),
                         ADP_NET_ACT_NONE);
        HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);
        HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_WAITING);
        HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), now + expected[i]);
        HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, now + expected[i] - 1u), ADP_NET_ACT_NONE);
        now += expected[i];
        HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, now), ADP_NET_ACT_CONNECT);
        HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_JOINING);
    }
    HAC_CHECK_EQ_U64(g_m.stats.failures_in_a_row, 8u);
    HAC_CHECK_EQ_U64(g_m.stats.attempts_failed, 8u);
    HAC_CHECK_EQ_U64(g_m.stats.attempts, 9u);
    HAC_CHECK_EQ_U64(g_m.stats.refusals_in_a_row, 0u);

    // Up at last: the next failure waits the first step again.
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, now + 2000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_U64(g_m.stats.failures_in_a_row, 0u);
    HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_BEACON_TIMEOUT, now + 3000u),
                     ADP_NET_ACT_CONNECT);
    HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, now + 5000u),
                     ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), now + 5000u + ADP_NET_BACKOFF_FIRST_MS);
}

static void the_backoff_never_overflows_past_its_ceiling(void)
{
    const adp_net_machine_config_t cfg = {
        .backoff_first_ms = 0x80000001u,
        .backoff_max_ms = UINT32_MAX,
        .attempt_timeout_ms = 1000u,
    };
    adp_net_machine_init(&g_m, &cfg);
    (void)adp_net_machine_up(&g_m, 0u);
    (void)adp_net_machine_started(&g_m, true, 0u);
    (void)adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, 10u);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), 10u + 0x80000001u);
    HAC_CHECK_EQ_U64(g_m.backoff_ms, UINT32_MAX);
    (void)adp_net_machine_tick(&g_m, 10u + 0x80000001u);
    (void)adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, 0x90000000u);
    HAC_CHECK_EQ_U64(g_m.backoff_ms, UINT32_MAX);
}

// An attempt that goes nowhere -- DHCP that never answers -- is given up at
// its timeout, and the driver's own disconnection that follows is not counted
// a second time.
static void an_attempt_without_an_address_is_given_up_at_its_timeout(void)
{
    joining_at(T0);
    adp_net_machine_associated(&g_m, T0 + 2000u);
    const uint64_t deadline = T0 + ADP_NET_ATTEMPT_TIMEOUT_MS;
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, deadline - 1u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, deadline), ADP_NET_ACT_DISCONNECT);
    HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_WAITING);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);
    HAC_CHECK_EQ_U64(g_m.stats.timeouts, 1u);
    HAC_CHECK_EQ_U64(g_m.stats.attempts_failed, 1u);
    HAC_CHECK_EQ_INT(g_m.stats.last_reason, ADP_NET_REASON_ATTEMPT_TIMEOUT);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), deadline + ADP_NET_BACKOFF_FIRST_MS);

    HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_ASSOC_LEAVE, deadline + 5u),
                     ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_U64(g_m.stats.attempts_failed, 1u);
    HAC_CHECK_EQ_INT(g_m.stats.last_reason, ADP_NET_REASON_ATTEMPT_TIMEOUT);
    // ... nor an address that arrives after it was given up.
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, deadline + 6u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_WAITING);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, deadline + ADP_NET_BACKOFF_FIRST_MS),
                     ADP_NET_ACT_CONNECT);
}

static void a_connect_the_driver_refuses_is_a_failed_attempt(void)
{
    joining_at(T0);
    HAC_CHECK_EQ_INT(adp_net_machine_connect_error(&g_m, T0 + 1u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_WAITING);
    HAC_CHECK_EQ_INT(g_m.stats.last_reason, ADP_NET_REASON_CONNECT_ERROR);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), T0 + 1u + ADP_NET_BACKOFF_FIRST_MS);
    // Outside an attempt it means nothing.
    HAC_CHECK_EQ_INT(adp_net_machine_connect_error(&g_m, T0 + 2u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_U64(g_m.stats.attempts_failed, 1u);
}

// A wrong password is refused for as long as it is wrong, and the machine
// keeps asking: never NET_FAILED, which would leave the device offline after
// a power cut that brought the access point up slower than the device.
static void refused_credentials_are_counted_and_never_given_up(void)
{
    joining_at(T0);
    uint64_t now = T0;
    for (unsigned i = 0u; i < 100u; ++i) {
        now += 3000u;
        const uint16_t reason =
            (i % 2u == 0u) ? ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT : ADP_NET_REASON_AUTH_FAIL;
        (void)adp_net_machine_disconnected(&g_m, reason, now);
        HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);
        now = adp_net_machine_next_ms(&g_m);
        HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, now), ADP_NET_ACT_CONNECT);
    }
    HAC_CHECK_EQ_U64(g_m.stats.refusals_in_a_row, 100u);
    HAC_CHECK_EQ_U64(g_m.stats.failures_in_a_row, 100u);

    // Something else in between: still a failure, not a refusal.
    now += 3000u;
    (void)adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, now);
    HAC_CHECK_EQ_U64(g_m.stats.refusals_in_a_row, 100u);
    HAC_CHECK_EQ_U64(g_m.stats.failures_in_a_row, 101u);

    // Accepted at last.
    (void)adp_net_machine_tick(&g_m, adp_net_machine_next_ms(&g_m));
    (void)adp_net_machine_got_address(&g_m, adp_net_machine_next_ms(&g_m));
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_UP);
    HAC_CHECK_EQ_U64(g_m.stats.refusals_in_a_row, 0u);
}

static void down_stops_the_radio_from_any_phase_and_later_events_are_ignored(void)
{
    // From each phase the radio is on in.
    const adp_net_phase_t phases[] = {ADP_NET_PHASE_STARTING, ADP_NET_PHASE_JOINING,
                                      ADP_NET_PHASE_UP, ADP_NET_PHASE_WAITING};
    for (size_t i = 0u; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        joining_at(T0);
        if (phases[i] == ADP_NET_PHASE_STARTING) {
            fresh();
            (void)adp_net_machine_up(&g_m, T0);
        } else if (phases[i] == ADP_NET_PHASE_UP) {
            (void)adp_net_machine_got_address(&g_m, T0 + 1u);
        } else if (phases[i] == ADP_NET_PHASE_WAITING) {
            (void)adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, T0 + 1u);
        }
        HAC_CHECK_EQ_INT(g_m.phase, phases[i]);
        HAC_CHECK_EQ_INT(adp_net_machine_down(&g_m, T0 + 10u), ADP_NET_ACT_STOP);
        HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_DOWN);
        HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), ADP_NET_NO_DEADLINE);
        HAC_CHECK_EQ_INT(adp_net_machine_down(&g_m, T0 + 11u), ADP_NET_ACT_NONE);  // again

        // What the driver posts as it stops, and anything older, changes
        // nothing.
        HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_ASSOC_LEAVE, T0 + 12u),
                         ADP_NET_ACT_NONE);
        HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, T0 + 13u), ADP_NET_ACT_NONE);
        HAC_CHECK_EQ_INT(adp_net_machine_lost_address(&g_m, T0 + 14u), ADP_NET_ACT_NONE);
        HAC_CHECK_EQ_INT(adp_net_machine_started(&g_m, true, T0 + 15u), ADP_NET_ACT_NONE);
        adp_net_machine_associated(&g_m, T0 + 16u);
        HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, T0 + 10000000u), ADP_NET_ACT_NONE);
        HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_DOWN);
    }
    // A losses count is a link lost, not one taken down.
    HAC_CHECK_EQ_U64(g_m.stats.losses, 0u);
}

static void down_from_failed_or_idle_has_no_radio_to_stop(void)
{
    fresh();
    HAC_CHECK_EQ_INT(adp_net_machine_down(&g_m, T0), ADP_NET_ACT_NONE);
    (void)adp_net_machine_up(&g_m, T0);
    (void)adp_net_machine_started(&g_m, false, T0 + 1u);
    HAC_CHECK_EQ_INT(adp_net_machine_down(&g_m, T0 + 2u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_DOWN);
}

// down() then up() before the driver's events have arrived: the first
// start's event belongs to the radio already started again, so the attempt
// begins; the second start's event, later, is ignored.
static void up_again_after_down_begins_one_attempt(void)
{
    up_at(T0);
    HAC_CHECK_EQ_INT(adp_net_machine_down(&g_m, T0 + 100u), ADP_NET_ACT_STOP);
    HAC_CHECK_EQ_INT(adp_net_machine_up(&g_m, T0 + 101u), ADP_NET_ACT_START);
    HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_ASSOC_LEAVE, T0 + 102u),
                     ADP_NET_ACT_NONE);  // the stop's own, while starting
    HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_STARTING);
    HAC_CHECK_EQ_INT(adp_net_machine_started(&g_m, true, T0 + 103u), ADP_NET_ACT_CONNECT);
    HAC_CHECK_EQ_INT(adp_net_machine_started(&g_m, true, T0 + 104u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_U64(g_m.stats.attempts, 2u);
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, T0 + 2000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_UP);
    HAC_CHECK_EQ_U64(g_m.stats.losses, 0u);
}

// The address lost while associated: an attempt with the timeout to get it
// back.
static void a_lost_address_is_sought_again(void)
{
    up_at(T0);
    HAC_CHECK_EQ_INT(adp_net_machine_lost_address(&g_m, T0 + 50000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_CONNECTING);
    HAC_CHECK_EQ_U64(g_m.stats.losses, 1u);
    HAC_CHECK_EQ_U64(adp_net_machine_next_ms(&g_m), T0 + 50000u + ADP_NET_ATTEMPT_TIMEOUT_MS);
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, T0 + 51000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_UP);

    // Or not: given up at the timeout, and begun again after the backoff.
    HAC_CHECK_EQ_INT(adp_net_machine_lost_address(&g_m, T0 + 60000u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_tick(&g_m, T0 + 60000u + ADP_NET_ATTEMPT_TIMEOUT_MS),
                     ADP_NET_ACT_DISCONNECT);
    HAC_CHECK_EQ_U64(g_m.stats.losses, 2u);
}

static void events_outside_their_phase_change_nothing(void)
{
    fresh();
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, T0), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, T0),
                     ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_started(&g_m, true, T0), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_state(&g_m), NET_DOWN);
    HAC_CHECK_EQ_U64(g_m.stats.attempts_failed, 0u);

    // Starting: a disconnection or an address is not this attempt's.
    (void)adp_net_machine_up(&g_m, T0);
    HAC_CHECK_EQ_INT(adp_net_machine_got_address(&g_m, T0 + 1u), ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(adp_net_machine_disconnected(&g_m, ADP_NET_REASON_NO_AP_FOUND, T0 + 2u),
                     ADP_NET_ACT_NONE);
    HAC_CHECK_EQ_INT(g_m.phase, ADP_NET_PHASE_STARTING);
    HAC_CHECK_EQ_U64(g_m.stats.attempts_failed, 0u);
}

static void reasons_have_names_and_refusals_are_the_credentials(void)
{
    HAC_CHECK(strcmp(adp_net_reason_name(ADP_NET_REASON_NO_AP_FOUND), "no_ap_found") == 0);
    HAC_CHECK(strcmp(adp_net_reason_name(ADP_NET_REASON_AUTH_FAIL), "auth_fail") == 0);
    HAC_CHECK(strcmp(adp_net_reason_name(ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT),
                     "4way_handshake_timeout") == 0);
    HAC_CHECK(strcmp(adp_net_reason_name(ADP_NET_REASON_ATTEMPT_TIMEOUT), "attempt_timeout") == 0);
    HAC_CHECK(strcmp(adp_net_reason_name(ADP_NET_REASON_CONNECT_ERROR), "connect_error") == 0);
    HAC_CHECK(strcmp(adp_net_reason_name(ADP_NET_REASON_NONE), "none") == 0);
    HAC_CHECK(strcmp(adp_net_reason_name(99u), "other") == 0);
    HAC_CHECK(strcmp(adp_net_reason_name(255u), "other") == 0);

    // Every reason this adapter names has a name of its own.
    const uint16_t named[] = {
        ADP_NET_REASON_NONE,
        ADP_NET_REASON_UNSPECIFIED,
        ADP_NET_REASON_AUTH_EXPIRE,
        ADP_NET_REASON_AUTH_LEAVE,
        ADP_NET_REASON_DISASSOC_DUE_TO_INACTIVITY,
        ADP_NET_REASON_ASSOC_TOOMANY,
        ADP_NET_REASON_ASSOC_LEAVE,
        ADP_NET_REASON_MIC_FAILURE,
        ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT,
        ADP_NET_REASON_802_1X_AUTH_FAILED,
        ADP_NET_REASON_BEACON_TIMEOUT,
        ADP_NET_REASON_NO_AP_FOUND,
        ADP_NET_REASON_AUTH_FAIL,
        ADP_NET_REASON_ASSOC_FAIL,
        ADP_NET_REASON_HANDSHAKE_TIMEOUT,
        ADP_NET_REASON_CONNECTION_FAIL,
        ADP_NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY,
        ADP_NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD,
        ADP_NET_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD,
        ADP_NET_REASON_ATTEMPT_TIMEOUT,
        ADP_NET_REASON_CONNECT_ERROR,
    };
    const size_t n = sizeof(named) / sizeof(named[0]);
    for (size_t i = 0u; i < n; ++i) {
        HAC_CHECK(strcmp(adp_net_reason_name(named[i]), "other") != 0);
        for (size_t j = i + 1u; j < n; ++j) {
            HAC_CHECK(strcmp(adp_net_reason_name(named[i]), adp_net_reason_name(named[j])) != 0);
        }
    }

    HAC_CHECK(adp_net_reason_is_refusal(ADP_NET_REASON_AUTH_FAIL));
    HAC_CHECK(adp_net_reason_is_refusal(ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT));
    HAC_CHECK(adp_net_reason_is_refusal(ADP_NET_REASON_HANDSHAKE_TIMEOUT));
    HAC_CHECK(adp_net_reason_is_refusal(ADP_NET_REASON_MIC_FAILURE));
    HAC_CHECK(adp_net_reason_is_refusal(ADP_NET_REASON_802_1X_AUTH_FAILED));
    HAC_CHECK(!adp_net_reason_is_refusal(ADP_NET_REASON_NO_AP_FOUND));
    HAC_CHECK(!adp_net_reason_is_refusal(ADP_NET_REASON_BEACON_TIMEOUT));
    HAC_CHECK(!adp_net_reason_is_refusal(ADP_NET_REASON_ASSOC_LEAVE));
    HAC_CHECK(!adp_net_reason_is_refusal(ADP_NET_REASON_ATTEMPT_TIMEOUT));
    HAC_CHECK(!adp_net_reason_is_refusal(ADP_NET_REASON_NONE));
}

static void phases_have_names(void)
{
    HAC_CHECK(strcmp(adp_net_phase_name(ADP_NET_PHASE_IDLE), "idle") == 0);
    HAC_CHECK(strcmp(adp_net_phase_name(ADP_NET_PHASE_STARTING), "starting") == 0);
    HAC_CHECK(strcmp(adp_net_phase_name(ADP_NET_PHASE_JOINING), "joining") == 0);
    HAC_CHECK(strcmp(adp_net_phase_name(ADP_NET_PHASE_UP), "up") == 0);
    HAC_CHECK(strcmp(adp_net_phase_name(ADP_NET_PHASE_WAITING), "waiting") == 0);
    HAC_CHECK(strcmp(adp_net_phase_name(ADP_NET_PHASE_FAILED), "failed") == 0);
    HAC_CHECK(strcmp(adp_net_phase_name((adp_net_phase_t)99), "?") == 0);
}

int main(void)
{
    HAC_RUN(the_defaults_are_the_limits_header);
    HAC_RUN(it_starts_down_with_nothing_to_do);
    HAC_RUN(up_starts_the_radio_and_then_an_attempt);
    HAC_RUN(an_address_is_up_and_waits_for_nothing);
    HAC_RUN(a_radio_that_will_not_start_is_given_up_until_up_again);
    HAC_RUN(a_start_event_that_never_comes_fails_and_stops_the_radio);
    HAC_RUN(a_lost_link_is_sought_again_at_once);
    HAC_RUN(failed_attempts_back_off_doubling_to_the_ceiling);
    HAC_RUN(the_backoff_never_overflows_past_its_ceiling);
    HAC_RUN(an_attempt_without_an_address_is_given_up_at_its_timeout);
    HAC_RUN(a_connect_the_driver_refuses_is_a_failed_attempt);
    HAC_RUN(refused_credentials_are_counted_and_never_given_up);
    HAC_RUN(down_stops_the_radio_from_any_phase_and_later_events_are_ignored);
    HAC_RUN(down_from_failed_or_idle_has_no_radio_to_stop);
    HAC_RUN(up_again_after_down_begins_one_attempt);
    HAC_RUN(a_lost_address_is_sought_again);
    HAC_RUN(events_outside_their_phase_change_nothing);
    HAC_RUN(reasons_have_names_and_refusals_are_the_credentials);
    HAC_RUN(phases_have_names);
    return hac_test_summary();
}
