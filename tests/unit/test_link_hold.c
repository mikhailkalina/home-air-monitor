// SPDX-License-Identifier: Apache-2.0
//
// core/app/link_hold.h: what the screen says about the link changes only
// once the link has stayed in a state for the hold, to the millisecond; the
// first state too; no telemetry at once; a change shorter than the hold never
// shows; the changes that do are a hold apart however the link flaps; a
// restarted clock starts the hold over
// (docs/adr/0031-the-network-on-the-screen.md).

#include <stdint.h>
#include <string.h>

#include "app/link_hold.h"
#include "hac_test.h"

#define HOLD LINK_HOLD_DEFAULT_MS

static link_hold_t fresh(void)
{
    link_hold_t h;
    const link_hold_config_t cfg = link_hold_config_default();
    link_hold_init(&h, &cfg);
    return h;
}

static void the_default_hold_is_a_minute(void)
{
    HAC_CHECK_EQ_U64(link_hold_config_default().hold_ms, 60000u);
    const link_hold_t h = fresh();
    HAC_CHECK_EQ_INT(link_hold_shown(&h), LINK_OFF);
}

static void without_telemetry_nothing_is_ever_shown(void)
{
    link_hold_t h = fresh();
    bool changed = true;
    for (uint64_t t = 0u; t <= 10u * HOLD; t += 1000u) {
        HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_OFF, t, &changed), LINK_HOLD_NO_DEADLINE);
        HAC_CHECK(!changed);
    }
    HAC_CHECK_EQ_INT(link_hold_shown(&h), LINK_OFF);
    HAC_CHECK_EQ_INT((int)h.changes, 0);
}

// At start: a place for the word and no word, until the first state has held
// -- not "no Wi-Fi" for the association's seconds.
static void the_first_state_is_held_too(void)
{
    link_hold_t h = fresh();
    bool changed = false;
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_NO_WIFI, 0u, &changed), HOLD);
    HAC_CHECK(changed);
    HAC_CHECK_EQ_INT(link_hold_shown(&h), LINK_UNKNOWN);

    // The association done after 3 s, the session at once: the hold starts
    // over from there.
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_ONLINE, 3000u, &changed), 3000u + HOLD);
    HAC_CHECK(!changed);
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_ONLINE, 3000u + HOLD - 1u, &changed), 3000u + HOLD);
    HAC_CHECK(!changed);
    HAC_CHECK_EQ_INT(link_hold_shown(&h), LINK_UNKNOWN);
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_ONLINE, 3000u + HOLD, &changed),
                     LINK_HOLD_NO_DEADLINE);
    HAC_CHECK(changed);
    HAC_CHECK_EQ_INT(link_hold_shown(&h), LINK_ONLINE);
    HAC_CHECK_EQ_INT((int)h.changes, 2);
}

static link_hold_t online_at(uint64_t t)
{
    link_hold_t h = fresh();
    (void)link_hold_step(&h, LINK_ONLINE, t, NULL);
    (void)link_hold_step(&h, LINK_ONLINE, t + HOLD, NULL);
    HAC_CHECK_EQ_INT(link_hold_shown(&h), LINK_ONLINE);
    return h;
}

// An outage: shown once it has lasted the hold, and the way back the same.
static void an_outage_longer_than_the_hold_is_shown_both_ways(void)
{
    const uint64_t t0 = 100000u;
    link_hold_t h = online_at(t0);
    const uint64_t down = t0 + HOLD + 5000u;
    bool changed = false;
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_NO_WIFI, down, &changed), down + HOLD);
    HAC_CHECK(!changed && link_hold_shown(&h) == LINK_ONLINE);
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_NO_WIFI, down + HOLD, &changed),
                     LINK_HOLD_NO_DEADLINE);
    HAC_CHECK(changed && link_hold_shown(&h) == LINK_NO_WIFI);

    const uint64_t back = down + 10u * HOLD;
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_NO_BROKER, back, &changed), back + HOLD);
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_ONLINE, back + 1000u, &changed),
                     back + 1000u + HOLD);  // the broker's state is a new state
    HAC_CHECK(!changed);
    (void)link_hold_step(&h, LINK_ONLINE, back + 1000u + HOLD, &changed);
    HAC_CHECK(changed && link_hold_shown(&h) == LINK_ONLINE);
}

// Shorter than the hold, by one millisecond: never on the glass.
static void a_change_shorter_than_the_hold_never_shows(void)
{
    const uint64_t t0 = 50000u;
    link_hold_t h = online_at(t0);
    const uint64_t down = t0 + 2u * HOLD;
    bool changed = false;
    (void)link_hold_step(&h, LINK_NO_BROKER, down, &changed);
    (void)link_hold_step(&h, LINK_NO_BROKER, down + HOLD - 1u, &changed);
    HAC_CHECK(!changed);
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_ONLINE, down + HOLD - 1u, &changed),
                     LINK_HOLD_NO_DEADLINE);  // back to what is shown: nothing pending
    HAC_CHECK(!changed && link_hold_shown(&h) == LINK_ONLINE);
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_ONLINE, down + 3u * HOLD, &changed),
                     LINK_HOLD_NO_DEADLINE);
    HAC_CHECK(!changed);
}

// Whatever the link does, the shown state changes at most once per hold.
// A link that flips every `period_ms`, stepped every second: below the hold
// it never shows a change at all; above it every change shows, a hold late.
static unsigned changes_over_an_hour(uint64_t period_ms)
{
    link_hold_t h = online_at(0u);
    const unsigned before = (unsigned)h.changes;
    uint64_t last_change = 0u;
    uint64_t closest = UINT64_MAX;
    for (uint64_t t = HOLD; t <= HOLD + 3600000u; t += 1000u) {
        const bool up = ((t - HOLD) / period_ms) % 2u == 0u;
        bool changed = false;
        (void)link_hold_step(&h, up ? LINK_ONLINE : LINK_NO_WIFI, t, &changed);
        if (changed) {
            if (last_change != 0u && t - last_change < closest) {
                closest = t - last_change;
            }
            last_change = t;
        }
    }
    HAC_CHECK(closest >= HOLD);
    return (unsigned)h.changes - before;
}

static void a_flapping_link_costs_at_most_one_change_per_hold(void)
{
    HAC_CHECK_EQ_INT((int)changes_over_an_hour(4000u), 0);
    HAC_CHECK_EQ_INT((int)changes_over_an_hour(30000u), 0);
    HAC_CHECK_EQ_INT((int)changes_over_an_hour(HOLD - 1000u), 0);
    // Just past the hold: every flip shows -- an hour of 61 s states, 59 of
    // them -- still never two within a hold of each other.
    const unsigned slow = changes_over_an_hour(HOLD + 1000u);
    HAC_CHECK(slow >= 58u && slow <= 3600u / 60u);
}

static void a_restarted_clock_starts_the_hold_over(void)
{
    link_hold_t h = online_at(1000000u);
    bool changed = false;
    (void)link_hold_step(&h, LINK_NO_WIFI, 2000000u, &changed);
    // Deep sleep is a cold boot: the monotonic clock is back near 0.
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_NO_WIFI, 500u, &changed), 500u + HOLD);
    HAC_CHECK(!changed);
    (void)link_hold_step(&h, LINK_NO_WIFI, 500u + HOLD, &changed);
    HAC_CHECK(changed && link_hold_shown(&h) == LINK_NO_WIFI);
}

static void a_hold_of_zero_shows_every_state_at_once(void)
{
    link_hold_t h;
    const link_hold_config_t cfg = {.hold_ms = 0u};
    link_hold_init(&h, &cfg);
    bool changed = false;
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_NO_WIFI, 10u, &changed), LINK_HOLD_NO_DEADLINE);
    HAC_CHECK(changed && link_hold_shown(&h) == LINK_NO_WIFI);
    (void)link_hold_step(&h, LINK_ONLINE, 11u, &changed);
    HAC_CHECK(changed && link_hold_shown(&h) == LINK_ONLINE);
}

// Telemetry going away is not a link state: shown at once.
static void telemetry_gone_shows_nothing_at_once(void)
{
    link_hold_t h = online_at(0u);
    bool changed = false;
    HAC_CHECK_EQ_U64(link_hold_step(&h, LINK_OFF, 2u * HOLD, &changed), LINK_HOLD_NO_DEADLINE);
    HAC_CHECK(changed && link_hold_shown(&h) == LINK_OFF);
}

static void the_names_say_each_state(void)
{
    HAC_CHECK(strcmp(link_status_name(LINK_OFF), "off") == 0);
    HAC_CHECK(strcmp(link_status_name(LINK_UNKNOWN), "unknown") == 0);
    HAC_CHECK(strcmp(link_status_name(LINK_NO_WIFI), "no-wifi") == 0);
    HAC_CHECK(strcmp(link_status_name(LINK_NO_BROKER), "no-broker") == 0);
    HAC_CHECK(strcmp(link_status_name(LINK_ONLINE), "online") == 0);
    HAC_CHECK(strcmp(link_status_name((link_status_t)99), "?") == 0);
}

int main(void)
{
    HAC_RUN(the_default_hold_is_a_minute);
    HAC_RUN(without_telemetry_nothing_is_ever_shown);
    HAC_RUN(the_first_state_is_held_too);
    HAC_RUN(an_outage_longer_than_the_hold_is_shown_both_ways);
    HAC_RUN(a_change_shorter_than_the_hold_never_shows);
    HAC_RUN(a_flapping_link_costs_at_most_one_change_per_hold);
    HAC_RUN(a_restarted_clock_starts_the_hold_over);
    HAC_RUN(a_hold_of_zero_shows_every_state_at_once);
    HAC_RUN(telemetry_gone_shows_nothing_at_once);
    HAC_RUN(the_names_say_each_state);
    return hac_test_summary();
}
