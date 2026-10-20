// SPDX-License-Identifier: Apache-2.0
//
// The broker model's own behaviour, past what the port_telemetry contract
// asks of any implementation (platform/host/adp_telemetry_loopback.h):
// matching against MQTT 3.1.1's examples, a session taken over by its id,
// the will's timing, both refusals, and what happens when a client's inbox
// or the broker's store is full.

#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_telemetry_loopback.h"
#include "hac_test.h"

static adp_clock_virtual_t g_clock;
static loopback_broker_t g_broker;
static adp_telemetry_loopback_t g_a;
static adp_telemetry_loopback_t g_b;

static unsigned g_events[LOOPBACK_EVENT_WILL + 1];
static uint64_t g_will_at;

static void tap(void *ctx, loopback_event_t event, const char *client, const loopback_message_t *m,
                uint64_t at_ms)
{
    (void)ctx;
    (void)client;
    (void)m;
    g_events[event]++;
    if (event == LOOPBACK_EVENT_WILL) {
        g_will_at = at_ms;
    }
}

static void fresh(void)
{
    adp_clock_virtual_init(&g_clock, 1.0);
    loopback_broker_init(&g_broker, adp_clock_virtual_port(&g_clock));
    loopback_broker_set_tap(&g_broker, tap, NULL);
    memset(g_events, 0, sizeof(g_events));
    g_will_at = 0u;
    HAC_CHECK_EQ_INT(adp_telemetry_loopback_init(&g_a, &g_broker), HAL_OK);
    HAC_CHECK_EQ_INT(adp_telemetry_loopback_init(&g_b, &g_broker), HAL_OK);
}

static hal_status_t connect_as(adp_telemetry_loopback_t *c, const char *id, uint16_t keepalive_s)
{
    static const uint8_t gone[] = "gone";
    const telemetry_session_t s = {
        .client_id = id,
        .keepalive_s = keepalive_s,
        .will_topic = "t/will",
        .will_payload = gone,
        .will_len = 4u,
        .will_qos = 1u,
        .will_retain = true,
    };
    return c->port.connect(&c->port, &s);
}

static void matching_follows_mqtt(void)
{
    static const struct {
        const char *filter;
        const char *topic;
        bool match;
    } cases[] = {
        // MQTT 3.1.1, 4.7.1.2 and 4.7.1.3.
        {"sport/tennis/player1/#", "sport/tennis/player1", true},
        {"sport/tennis/player1/#", "sport/tennis/player1/ranking", true},
        {"sport/tennis/player1/#", "sport/tennis/player1/score/wimbledon", true},
        {"sport/#", "sport", true},
        {"#", "sport/tennis", true},
        {"sport/tennis/+", "sport/tennis/player1", true},
        {"sport/tennis/+", "sport/tennis/player1/ranking", false},
        {"sport/+", "sport", false},
        {"sport/+", "sport/", true},
        {"+/+", "/finance", true},
        {"/+", "/finance", true},
        {"+", "/finance", false},
        // 4.7.2: '$' topics out of a leading wildcard's reach.
        {"#", "$SYS/broker", false},
        {"+/broker", "$SYS/broker", false},
        {"$SYS/#", "$SYS/broker", true},
        // Exact, and the empty level.
        {"homeassistant/status", "homeassistant/status", true},
        {"homeassistant/status", "homeassistant/statu", false},
        {"homeassistant/status", "homeassistant/status/x", false},
        {"a/+/b", "a//b", true},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        hac_test_check(loopback_filter_matches(cases[i].filter, cases[i].topic) == cases[i].match,
                       cases[i].filter, cases[i].topic, __LINE__);
    }
}

static void a_second_connection_with_the_id_takes_the_session_over(void)
{
    fresh();
    HAC_CHECK_EQ_INT(connect_as(&g_a, "same", 60u), HAL_OK);
    HAC_CHECK_EQ_INT(connect_as(&g_b, "same", 60u), HAL_OK);
    HAC_CHECK(!g_a.port.is_connected(&g_a.port));
    HAC_CHECK(g_b.port.is_connected(&g_b.port));
    HAC_CHECK_EQ_INT((int)g_events[LOOPBACK_EVENT_TAKEN_OVER], 1);
    HAC_CHECK_EQ_INT((int)g_events[LOOPBACK_EVENT_WILL], 1);  // the old session's, at once
    const loopback_message_t *will = loopback_broker_retained(&g_broker, "t/will");
    HAC_CHECK(will != NULL && will->len == 4u);
}

static void a_will_falls_due_one_and_a_half_keepalives_after_the_drop(void)
{
    fresh();
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 20u), HAL_OK);
    adp_clock_virtual_advance(&g_clock, 1000u);
    adp_telemetry_loopback_set_link(&g_a, false, g_clock.now_ms);
    HAC_CHECK_EQ_U64(loopback_broker_next_ms(&g_broker), 1000u + 30000u);
    adp_clock_virtual_advance(&g_clock, 29999u);
    loopback_broker_service(&g_broker);
    HAC_CHECK_EQ_INT((int)g_events[LOOPBACK_EVENT_WILL], 0);
    adp_clock_virtual_advance(&g_clock, 1u);
    loopback_broker_service(&g_broker);
    HAC_CHECK_EQ_INT((int)g_events[LOOPBACK_EVENT_WILL], 1);
    HAC_CHECK_EQ_U64(g_will_at, 31000u);
    HAC_CHECK_EQ_U64(loopback_broker_next_ms(&g_broker), LOOPBACK_BROKER_NO_DEADLINE);
}

static void without_a_keepalive_the_will_waits_for_a_takeover(void)
{
    fresh();
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 0u), HAL_OK);
    adp_telemetry_loopback_set_link(&g_a, false, g_clock.now_ms);
    adp_clock_virtual_advance(&g_clock, 86400000u);
    loopback_broker_service(&g_broker);
    HAC_CHECK_EQ_INT((int)g_events[LOOPBACK_EVENT_WILL], 0);
    HAC_CHECK_EQ_INT(connect_as(&g_b, "a", 0u), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_events[LOOPBACK_EVENT_WILL], 1);
}

static void without_a_network_there_is_no_attempt(void)
{
    fresh();
    adp_telemetry_loopback_set_link(&g_a, false, 0u);
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_ERR_NOT_READY);
    adp_telemetry_loopback_set_link(&g_a, true, 0u);
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_OK);
}

static void refusals_are_counted_down(void)
{
    fresh();
    loopback_broker_refuse(&g_broker, LOOPBACK_REFUSE, 2u);
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_broker.refused, 2);

    // Unanswered: the attempt is under way, and never ends by itself.
    (void)g_a.port.disconnect(&g_a.port);
    loopback_broker_refuse(&g_broker, LOOPBACK_SILENT, 1u);
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_OK);
    adp_clock_virtual_advance(&g_clock, 60000u);
    HAC_CHECK(!g_a.port.is_connected(&g_a.port));
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_ERR_BUSY);
    HAC_CHECK_EQ_INT(g_a.port.disconnect(&g_a.port), HAL_OK);
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_OK);
    HAC_CHECK(g_a.port.is_connected(&g_a.port));
}

static void a_long_message_is_cut_and_marked(void)
{
    fresh();
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_OK);
    HAC_CHECK_EQ_INT(g_a.port.subscribe(&g_a.port, "t/#", 1u), HAL_OK);
    uint8_t big[PORT_TELEMETRY_RX_PAYLOAD_MAX + 44u];
    memset(big, 'x', sizeof(big));
    HAC_CHECK_EQ_INT(g_a.port.publish(&g_a.port, "t/big", big, sizeof(big), 1u, false), HAL_OK);
    telemetry_message_t m;
    HAC_CHECK(g_a.port.poll(&g_a.port, &m));
    HAC_CHECK(m.truncated && m.len == PORT_TELEMETRY_RX_PAYLOAD_MAX);
}

static void a_full_inbox_drops_and_counts_the_newest(void)
{
    fresh();
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_OK);
    HAC_CHECK_EQ_INT(g_a.port.subscribe(&g_a.port, "t/#", 1u), HAL_OK);
    for (unsigned i = 0u; i < ADP_TELEMETRY_LOOPBACK_INBOX + 4u; ++i) {
        const uint8_t n = (uint8_t)i;
        HAC_CHECK_EQ_INT(g_a.port.publish(&g_a.port, "t/n", &n, 1u, 1u, false), HAL_OK);
    }
    HAC_CHECK_EQ_INT((int)g_a.inbox_dropped, 4);
    telemetry_message_t m;
    HAC_CHECK(g_a.port.poll(&g_a.port, &m) && m.payload[0] == 0u);
}

static void a_full_store_counts_what_it_could_not_keep(void)
{
    fresh();
    HAC_CHECK_EQ_INT(connect_as(&g_a, "a", 60u), HAL_OK);
    char topic[16];
    for (unsigned i = 0u; i < LOOPBACK_BROKER_MAX_RETAINED + 2u; ++i) {
        topic[0] = 'k';
        topic[1] = '/';
        topic[2] = (char)('a' + i / 26u);
        topic[3] = (char)('a' + i % 26u);
        topic[4] = '\0';
        const uint8_t v = 1u;
        HAC_CHECK_EQ_INT(g_a.port.publish(&g_a.port, topic, &v, 1u, 1u, true), HAL_OK);
    }
    HAC_CHECK_EQ_INT((int)g_broker.retained_count, LOOPBACK_BROKER_MAX_RETAINED);
    HAC_CHECK_EQ_INT((int)g_broker.retained_overflow, 2);
}

static void the_broker_holds_a_bounded_number_of_clients(void)
{
    fresh();
    static adp_telemetry_loopback_t more[LOOPBACK_BROKER_MAX_CLIENTS];
    unsigned ok = 0u;
    for (unsigned i = 0u; i < LOOPBACK_BROKER_MAX_CLIENTS; ++i) {
        ok += (adp_telemetry_loopback_init(&more[i], &g_broker) == HAL_OK) ? 1u : 0u;
    }
    HAC_CHECK_EQ_INT((int)ok, LOOPBACK_BROKER_MAX_CLIENTS - 2);  // two taken by fresh()
}

int main(void)
{
    HAC_RUN(matching_follows_mqtt);
    HAC_RUN(a_second_connection_with_the_id_takes_the_session_over);
    HAC_RUN(a_will_falls_due_one_and_a_half_keepalives_after_the_drop);
    HAC_RUN(without_a_keepalive_the_will_waits_for_a_takeover);
    HAC_RUN(without_a_network_there_is_no_attempt);
    HAC_RUN(refusals_are_counted_down);
    HAC_RUN(a_long_message_is_cut_and_marked);
    HAC_RUN(a_full_inbox_drops_and_counts_the_newest);
    HAC_RUN(a_full_store_counts_what_it_could_not_keep);
    HAC_RUN(the_broker_holds_a_bounded_number_of_clients);
    return hac_test_summary();
}
