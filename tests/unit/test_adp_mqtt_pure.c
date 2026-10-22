// SPDX-License-Identifier: Apache-2.0
//
// platform/esp32_t5s3/adp_mqtt_pure.c, compiled on the host from where the
// firmware compiles it (see tests/CMakeLists.txt): the MQTT client's
// decisions -- the broker's URI, the rings' indices, a received message put
// together from esp-mqtt's pieces, and the session: which events belong to
// it, what is_connected() says, what the adapter's task does next.

#include "adp_mqtt_pure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hac_test.h"

// --- the broker's URI --------------------------------------------------------------

static adp_mqtt_broker_t g_broker;

static bool parses(const char *uri, const char *host, unsigned port)
{
    const char *why = adp_mqtt_parse_uri(uri, &g_broker);
    return why == NULL && strcmp(g_broker.host, host) == 0 && g_broker.port == port;
}

static bool refused(const char *uri)
{
    const char *why = adp_mqtt_parse_uri(uri, &g_broker);
    return why != NULL && why[0] != '\0' && g_broker.host[0] == '\0' && g_broker.port == 0u;
}

static void a_broker_is_a_host_and_a_port(void)
{
    HAC_CHECK(parses("mqtt://192.168.1.10:1883", "192.168.1.10", 1883u));
    HAC_CHECK(parses("mqtt://broker.lan", "broker.lan", ADP_MQTT_DEFAULT_PORT));
    HAC_CHECK(parses("mqtt://home-assistant_1:1", "home-assistant_1", 1u));
    HAC_CHECK(parses("mqtt://h:65535", "h", 65535u));
}

static void the_host_is_at_most_its_limit(void)
{
    char uri[ADP_MQTT_HOST_MAX + 16u];
    char host[ADP_MQTT_HOST_MAX + 2u];
    memset(host, 'a', ADP_MQTT_HOST_MAX);
    host[ADP_MQTT_HOST_MAX] = '\0';
    memcpy(uri, "mqtt://", 7u);
    memcpy(uri + 7u, host, ADP_MQTT_HOST_MAX + 1u);
    HAC_CHECK(parses(uri, host, ADP_MQTT_DEFAULT_PORT));

    host[ADP_MQTT_HOST_MAX] = 'a';
    host[ADP_MQTT_HOST_MAX + 1u] = '\0';
    memcpy(uri + 7u, host, ADP_MQTT_HOST_MAX + 2u);
    HAC_CHECK(refused(uri));
}

static void anything_but_plain_mqtt_is_refused(void)
{
    HAC_CHECK(refused(NULL));
    HAC_CHECK(refused(""));
    HAC_CHECK(refused("mqtts://broker.lan:8883"));
    HAC_CHECK(refused("ws://broker.lan"));
    HAC_CHECK(refused("tcp://broker.lan"));
    HAC_CHECK(refused("broker.lan:1883"));
    HAC_CHECK(refused("MQTT://broker.lan"));
    HAC_CHECK(refused("mqtt://"));
    HAC_CHECK(refused("mqtt://:1883"));
    HAC_CHECK(refused("mqtt://user:secret@broker.lan"));
    HAC_CHECK(refused("mqtt://broker.lan/path"));
    HAC_CHECK(refused("mqtt://broker.lan:1883/"));
    HAC_CHECK(refused("mqtt://broker lan"));
    HAC_CHECK(refused("mqtt://[::1]:1883"));
}

static void the_port_is_one_to_65535(void)
{
    HAC_CHECK(refused("mqtt://h:"));
    HAC_CHECK(refused("mqtt://h:0"));
    HAC_CHECK(refused("mqtt://h:65536"));
    HAC_CHECK(refused("mqtt://h:100000"));
    HAC_CHECK(refused("mqtt://h:018830"));
    HAC_CHECK(refused("mqtt://h:-1"));
    HAC_CHECK(refused("mqtt://h:1883x"));
    HAC_CHECK(parses("mqtt://h:01883", "h", 1883u));
}

static void a_refusal_says_why(void)
{
    const char *tls = adp_mqtt_parse_uri("mqtts://h", &g_broker);
    HAC_CHECK(tls != NULL && strstr(tls, "TLS") != NULL);
    const char *user = adp_mqtt_parse_uri("mqtt://u:p@h", &g_broker);
    HAC_CHECK(user != NULL && strstr(user, "CONFIG_HAC_MQTT_PASSWORD") != NULL);
    // The password is never repeated back.
    HAC_CHECK(user != NULL && strstr(user, "p@h") == NULL);
}

// --- the ring --------------------------------------------------------------------------

static void a_ring_hands_over_in_order(void)
{
    adp_mqtt_ring_t r;
    adp_mqtt_ring_init(&r, 3u);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), -1);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), 0);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), -1);  // reserved is not committed
    adp_mqtt_ring_commit(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), 0);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), 1);
    adp_mqtt_ring_commit(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), 2);
    adp_mqtt_ring_commit(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), -1);  // full
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), 0);
    adp_mqtt_ring_release(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), 1);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), 0);  // wrapped
    adp_mqtt_ring_commit(&r);
    adp_mqtt_ring_release(&r);
    adp_mqtt_ring_release(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), 0);
    adp_mqtt_ring_release(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), -1);
    adp_mqtt_ring_release(&r);  // nothing to release: nothing moves
    HAC_CHECK_EQ_INT(r.count, 0);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), 1);
}

static void a_reserved_slot_stays_put_while_the_consumer_moves(void)
{
    adp_mqtt_ring_t r;
    adp_mqtt_ring_init(&r, 4u);
    for (int i = 0; i < 3; ++i) {
        (void)adp_mqtt_ring_reserve(&r);
        adp_mqtt_ring_commit(&r);
    }
    const int slot = adp_mqtt_ring_reserve(&r);
    HAC_CHECK_EQ_INT(slot, 3);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), -1);  // one reservation at a time
    adp_mqtt_ring_release(&r);
    adp_mqtt_ring_release(&r);
    adp_mqtt_ring_release(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), -1);  // still not committed
    adp_mqtt_ring_commit(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), slot);
    adp_mqtt_ring_release(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), 0);
    adp_mqtt_ring_cancel(&r);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), -1);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), 0);  // the cancelled slot again
    // Committing twice commits once.
    adp_mqtt_ring_commit(&r);
    adp_mqtt_ring_commit(&r);
    HAC_CHECK_EQ_INT(r.count, 1);
}

static void a_ring_of_nothing_takes_nothing(void)
{
    adp_mqtt_ring_t r;
    adp_mqtt_ring_init(&r, 0u);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_reserve(&r), -1);
    HAC_CHECK_EQ_INT(adp_mqtt_ring_peek(&r), -1);
    adp_mqtt_ring_release(&r);
    HAC_CHECK_EQ_INT(r.count, 0);
}

// --- a message in pieces --------------------------------------------------------------

static adp_mqtt_assembly_t g_a;
static uint8_t g_data[PORT_TELEMETRY_RX_PAYLOAD_MAX * 2u];

static adp_mqtt_piece_t piece(const char *topic, size_t offset, size_t len, size_t total)
{
    adp_mqtt_piece_t p;
    memset(&p, 0, sizeof(p));
    p.topic = topic;
    p.topic_len = (topic != NULL) ? strlen(topic) : 0u;
    p.data = (len > 0u) ? g_data + offset : NULL;
    p.data_len = len;
    p.offset = offset;
    p.total = total;
    return p;
}

static void fill_data(void)
{
    for (size_t i = 0u; i < sizeof(g_data); ++i) {
        g_data[i] = (uint8_t)(i * 7u + 1u);
    }
}

static void a_message_in_one_piece(void)
{
    adp_mqtt_assembly_reset(&g_a);
    memcpy(g_data, "online", 6u);
    adp_mqtt_piece_t p = piece("homeassistant/status", 0u, 6u, 6u);
    p.retain = true;
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK(strcmp(g_a.msg.topic, "homeassistant/status") == 0);
    HAC_CHECK_EQ_U64(g_a.msg.len, 6u);
    HAC_CHECK(memcmp(g_a.msg.payload, "online", 6u) == 0);
    HAC_CHECK(!g_a.msg.truncated);
    HAC_CHECK(g_a.msg.retained);
    HAC_CHECK(!g_a.under_way);
}

static void an_empty_message_is_whole_at_once(void)
{
    adp_mqtt_assembly_reset(&g_a);
    adp_mqtt_piece_t p = piece("t", 0u, 0u, 0u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK_EQ_U64(g_a.msg.len, 0u);
    HAC_CHECK(!g_a.msg.truncated);
    HAC_CHECK(!g_a.msg.retained);
}

static void pieces_are_put_together(void)
{
    fill_data();
    adp_mqtt_assembly_reset(&g_a);
    const size_t total = 200u;
    adp_mqtt_piece_t p = piece("a/b", 0u, 80u, total);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_MORE);
    p = piece(NULL, 80u, 80u, total);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_MORE);
    p = piece(NULL, 160u, 40u, total);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK_EQ_U64(g_a.msg.len, total);
    HAC_CHECK(memcmp(g_a.msg.payload, g_data, total) == 0);
    HAC_CHECK(!g_a.msg.truncated);
    HAC_CHECK(strcmp(g_a.msg.topic, "a/b") == 0);
}

static void a_payload_is_kept_to_the_port_s_limit(void)
{
    fill_data();
    const size_t max = PORT_TELEMETRY_RX_PAYLOAD_MAX;

    adp_mqtt_assembly_reset(&g_a);
    adp_mqtt_piece_t p = piece("t", 0u, max, max);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK_EQ_U64(g_a.msg.len, max);
    HAC_CHECK(!g_a.msg.truncated);

    adp_mqtt_assembly_reset(&g_a);
    p = piece("t", 0u, max + 1u, max + 1u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK_EQ_U64(g_a.msg.len, max);
    HAC_CHECK(g_a.msg.truncated);
    HAC_CHECK(memcmp(g_a.msg.payload, g_data, max) == 0);

    // Cut in pieces: the second straddles the limit, the third is all past it.
    adp_mqtt_assembly_reset(&g_a);
    const size_t total = max + 100u;
    p = piece("t", 0u, max - 10u, total);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_MORE);
    p = piece(NULL, max - 10u, 60u, total);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_MORE);
    HAC_CHECK(g_a.msg.truncated);
    p = piece(NULL, max + 50u, 50u, total);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK_EQ_U64(g_a.msg.len, max);
    HAC_CHECK(memcmp(g_a.msg.payload, g_data, max) == 0);
}

static void a_piece_out_of_place_is_dropped(void)
{
    adp_mqtt_assembly_reset(&g_a);
    adp_mqtt_piece_t p = piece(NULL, 10u, 10u, 30u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_STRAY);

    p = piece("t", 0u, 10u, 30u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_MORE);
    p = piece(NULL, 15u, 10u, 30u);  // a gap
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_STRAY);
    p = piece(NULL, 20u, 10u, 30u);  // and what follows it
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_STRAY);

    // A new message abandons one never finished.
    p = piece("first", 0u, 10u, 30u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_MORE);
    p = piece("second", 0u, 3u, 3u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK(strcmp(g_a.msg.topic, "second") == 0);
    HAC_CHECK_EQ_U64(g_a.msg.len, 3u);

    // Data said to be there and not there.
    p = piece("t", 0u, 0u, 5u);
    p.data_len = 5u;
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_REFUSED);
}

static void a_topic_the_port_cannot_hold_is_refused(void)
{
    char topic[PORT_TELEMETRY_TOPIC_MAX + 1u];
    memset(topic, 'x', sizeof(topic));
    topic[PORT_TELEMETRY_TOPIC_MAX - 1u] = '\0';  // 127 characters: fits
    adp_mqtt_assembly_reset(&g_a);
    adp_mqtt_piece_t p = piece(topic, 0u, 0u, 0u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_DONE);
    HAC_CHECK_EQ_U64(strlen(g_a.msg.topic), PORT_TELEMETRY_TOPIC_MAX - 1u);

    topic[PORT_TELEMETRY_TOPIC_MAX - 1u] = 'x';
    topic[PORT_TELEMETRY_TOPIC_MAX] = '\0';  // 128: does not
    p = piece(topic, 0u, 0u, 0u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_REFUSED);

    p = piece(NULL, 0u, 0u, 0u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_REFUSED);

    static const char with_nul[] = {'a', '\0', 'b'};
    p = piece("t", 0u, 0u, 0u);
    p.topic = with_nul;
    p.topic_len = sizeof(with_nul);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_REFUSED);

    // What followed a refused first piece is no message's.
    p = piece(topic, 0u, 5u, 10u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_REFUSED);
    p = piece(NULL, 5u, 5u, 10u);
    HAC_CHECK_EQ_INT(adp_mqtt_assemble(&g_a, &p), ADP_MQTT_PIECE_STRAY);
}

// --- the session -----------------------------------------------------------------

static adp_mqtt_session_t g_s;

static uint32_t connected(void)
{
    adp_mqtt_session_init(&g_s);
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_OK);
    HAC_CHECK(adp_mqtt_session_accepted(&g_s, g_s.gen));
    return g_s.gen;
}

static void no_session_before_connect(void)
{
    adp_mqtt_session_init(&g_s);
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, true));
    HAC_CHECK(!adp_mqtt_session_holds(&g_s, 0u));
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_IDLE);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, 0u, 0u), ADP_MQTT_PLAN_IDLE);
    // An event before any session -- generation 0, or one never made.
    HAC_CHECK(!adp_mqtt_session_accepted(&g_s, 0u));
    HAC_CHECK(!adp_mqtt_session_accepted(&g_s, 1u));
    HAC_CHECK(!adp_mqtt_session_ended(&g_s, 1u, false));
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_IDLE);
}

static void connect_needs_the_network_and_no_session(void)
{
    adp_mqtt_session_init(&g_s);
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, false), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_U64(g_s.gen, 0u);
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_OK);
    HAC_CHECK_EQ_U64(g_s.gen, 1u);
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_CONNECTING);
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_ERR_BUSY);  // connecting
    HAC_CHECK(adp_mqtt_session_accepted(&g_s, 1u));
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_ERR_BUSY);  // connected
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, false), HAL_ERR_BUSY);
    HAC_CHECK_EQ_U64(g_s.gen, 1u);
}

static void an_accepted_session_is_usable(void)
{
    const uint32_t gen = connected();
    HAC_CHECK(adp_mqtt_session_usable(&g_s, true));
    HAC_CHECK(adp_mqtt_session_holds(&g_s, gen));
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_CONNECTED);
    HAC_CHECK(!adp_mqtt_session_accepted(&g_s, gen));  // not news twice
}

static void the_network_going_ends_it_for_good(void)
{
    const uint32_t gen = connected();
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, false));
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_NETWORK);
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, true));  // back: still over
    HAC_CHECK(!adp_mqtt_session_holds(&g_s, gen));
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_OVER);
    // esp-mqtt's own word of it, later, changes nothing.
    HAC_CHECK(!adp_mqtt_session_ended(&g_s, gen, false));
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_NETWORK);
    // And a session over takes a new connect(), with the network.
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, false), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_OK);
    HAC_CHECK_EQ_U64(g_s.gen, gen + 1u);
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_NONE);
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_CONNECTING);
}

static void the_network_does_not_end_an_attempt(void)
{
    adp_mqtt_session_init(&g_s);
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_OK);
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, false));
    HAC_CHECK(!g_s.over);  // the reporter gives an attempt up; this does not
    HAC_CHECK(adp_mqtt_session_accepted(&g_s, g_s.gen));
}

static void why_a_session_ended(void)
{
    adp_mqtt_session_init(&g_s);
    (void)adp_mqtt_session_connect(&g_s, true);
    HAC_CHECK(adp_mqtt_session_ended(&g_s, g_s.gen, true));
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_REFUSED);
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, true));

    (void)adp_mqtt_session_connect(&g_s, true);
    HAC_CHECK(adp_mqtt_session_ended(&g_s, g_s.gen, false));
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_NO_CONNECTION);
    HAC_CHECK(!adp_mqtt_session_ended(&g_s, g_s.gen, false));  // once

    const uint32_t gen = connected();
    HAC_CHECK(adp_mqtt_session_ended(&g_s, gen, true));  // after accepted: lost, whatever
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_LOST);
    HAC_CHECK(!adp_mqtt_session_accepted(&g_s, gen));  // an end is not undone
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, true));

    (void)adp_mqtt_session_connect(&g_s, true);
    adp_mqtt_session_not_started(&g_s, g_s.gen);
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_NOT_STARTED);
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_OVER);
}

static void an_old_client_s_events_change_nothing(void)
{
    const uint32_t old = connected();
    adp_mqtt_session_disconnect(&g_s);
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, true));
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_IDLE);
    HAC_CHECK(!adp_mqtt_session_ended(&g_s, old, false));  // let go of: not news
    HAC_CHECK_EQ_INT(g_s.end, ADP_MQTT_END_NONE);

    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_OK);
    const uint32_t now = g_s.gen;
    HAC_CHECK(!adp_mqtt_session_accepted(&g_s, old));
    HAC_CHECK(!adp_mqtt_session_ended(&g_s, old, false));
    HAC_CHECK(!adp_mqtt_session_holds(&g_s, old));
    HAC_CHECK_EQ_INT(adp_mqtt_session_state(&g_s), ADP_MQTT_STATE_CONNECTING);
    HAC_CHECK(adp_mqtt_session_accepted(&g_s, now));
    HAC_CHECK(adp_mqtt_session_usable(&g_s, true));
}

static void disconnect_while_connecting(void)
{
    adp_mqtt_session_init(&g_s);
    (void)adp_mqtt_session_connect(&g_s, true);
    const uint32_t gen = g_s.gen;
    adp_mqtt_session_disconnect(&g_s);
    HAC_CHECK(!adp_mqtt_session_accepted(&g_s, gen));  // the broker's late yes
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, true));
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_OK);
    HAC_CHECK_EQ_U64(g_s.gen, gen + 1u);
}

static void the_generation_skips_zero_when_it_wraps(void)
{
    adp_mqtt_session_init(&g_s);
    g_s.gen = UINT32_MAX;
    HAC_CHECK_EQ_INT(adp_mqtt_session_connect(&g_s, true), HAL_OK);
    HAC_CHECK_EQ_U64(g_s.gen, 1u);
}

static void what_the_task_does_next(void)
{
    adp_mqtt_session_init(&g_s);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, 0u, 0u), ADP_MQTT_PLAN_IDLE);

    (void)adp_mqtt_session_connect(&g_s, true);
    const uint32_t gen = g_s.gen;
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, 0u, 0u), ADP_MQTT_PLAN_START);
    // Started, its client waiting for the broker.
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, gen, gen), ADP_MQTT_PLAN_IDLE);
    (void)adp_mqtt_session_accepted(&g_s, gen);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, gen, gen), ADP_MQTT_PLAN_FORWARD);
    // Dropped: its client stopped, and none started again for it.
    (void)adp_mqtt_session_ended(&g_s, gen, false);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, gen, gen), ADP_MQTT_PLAN_STOP);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, 0u, gen), ADP_MQTT_PLAN_IDLE);
    // A client that would not start is not tried again for the same session.
    (void)adp_mqtt_session_connect(&g_s, true);
    const uint32_t next = g_s.gen;
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, 0u, gen), ADP_MQTT_PLAN_START);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, 0u, next), ADP_MQTT_PLAN_IDLE);
    // A client of an older session -- the caller let go, then asked anew --
    // is stopped before the new one starts.
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, gen, gen), ADP_MQTT_PLAN_STOP);
    // disconnect(): its client stopped.
    (void)adp_mqtt_session_accepted(&g_s, next);
    adp_mqtt_session_disconnect(&g_s);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, next, next), ADP_MQTT_PLAN_STOP);
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, 0u, next), ADP_MQTT_PLAN_IDLE);
    // The network gone: its client stopped too.
    const uint32_t third = connected();
    HAC_CHECK(!adp_mqtt_session_usable(&g_s, false));
    HAC_CHECK_EQ_INT(adp_mqtt_session_plan(&g_s, third, third), ADP_MQTT_PLAN_STOP);
}

// --- subscriptions ------------------------------------------------------------------

static void subscriptions_are_counted_per_session(void)
{
    static adp_mqtt_subs_t subs;
    adp_mqtt_subs_init(&subs);
    char filter[16];
    for (unsigned i = 0u; i < ADP_MQTT_MAX_SUBSCRIPTIONS; ++i) {
        filter[0] = 'f';
        filter[1] = (char)('a' + i);
        filter[2] = '\0';
        HAC_CHECK(adp_mqtt_subs_take(&subs, 1u, filter));
    }
    HAC_CHECK(!adp_mqtt_subs_take(&subs, 1u, "one more"));
    HAC_CHECK(adp_mqtt_subs_take(&subs, 1u, "fa"));  // the same filter again replaces it
    HAC_CHECK_EQ_U64(subs.count, ADP_MQTT_MAX_SUBSCRIPTIONS);
    HAC_CHECK(adp_mqtt_subs_take(&subs, 2u, "one more"));  // a new session has none
    HAC_CHECK_EQ_U64(subs.count, 1u);
}

// --- names ---------------------------------------------------------------------------

static void every_name_is_a_word(void)
{
    for (int e = (int)ADP_MQTT_END_NONE; e <= (int)ADP_MQTT_END_NOT_STARTED; ++e) {
        HAC_CHECK(strcmp(adp_mqtt_end_name((adp_mqtt_end_t)e), "?") != 0);
    }
    for (int s = (int)ADP_MQTT_STATE_IDLE; s <= (int)ADP_MQTT_STATE_OVER; ++s) {
        HAC_CHECK(strcmp(adp_mqtt_state_name((adp_mqtt_state_t)s), "?") != 0);
    }
    HAC_CHECK(strcmp(adp_mqtt_connack_name(0), "accepted") == 0);
    HAC_CHECK(strcmp(adp_mqtt_connack_name(4), "bad user name or password") == 0);
    HAC_CHECK(strcmp(adp_mqtt_connack_name(5), "not authorised") == 0);
    HAC_CHECK(strcmp(adp_mqtt_connack_name(6), "unknown") == 0);
    HAC_CHECK(strcmp(adp_mqtt_connack_name(-1), "unknown") == 0);
}

int main(void)
{
    HAC_RUN(a_broker_is_a_host_and_a_port);
    HAC_RUN(the_host_is_at_most_its_limit);
    HAC_RUN(anything_but_plain_mqtt_is_refused);
    HAC_RUN(the_port_is_one_to_65535);
    HAC_RUN(a_refusal_says_why);
    HAC_RUN(a_ring_hands_over_in_order);
    HAC_RUN(a_reserved_slot_stays_put_while_the_consumer_moves);
    HAC_RUN(a_ring_of_nothing_takes_nothing);
    HAC_RUN(a_message_in_one_piece);
    HAC_RUN(an_empty_message_is_whole_at_once);
    HAC_RUN(pieces_are_put_together);
    HAC_RUN(a_payload_is_kept_to_the_port_s_limit);
    HAC_RUN(a_piece_out_of_place_is_dropped);
    HAC_RUN(a_topic_the_port_cannot_hold_is_refused);
    HAC_RUN(no_session_before_connect);
    HAC_RUN(connect_needs_the_network_and_no_session);
    HAC_RUN(an_accepted_session_is_usable);
    HAC_RUN(the_network_going_ends_it_for_good);
    HAC_RUN(the_network_does_not_end_an_attempt);
    HAC_RUN(why_a_session_ended);
    HAC_RUN(an_old_client_s_events_change_nothing);
    HAC_RUN(disconnect_while_connecting);
    HAC_RUN(the_generation_skips_zero_when_it_wraps);
    HAC_RUN(what_the_task_does_next);
    HAC_RUN(subscriptions_are_counted_per_session);
    HAC_RUN(every_name_is_a_word);
    return hac_test_summary();
}
