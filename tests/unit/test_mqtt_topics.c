// SPDX-License-Identifier: Apache-2.0
//
// core/reporting/topics.c: the topics a prefix and a device id make, and the
// ones that are refused -- wildcards, empty levels, the broker's '$' topics,
// anything over the bounds -- with the longest accepted still fitting.

#include "reporting/topics.h"

#include <string.h>

#include "hac_test.h"
#include "text/text_append.h"

// 48, 32 and 32 characters: the longest accepted.
#define PREFIX_48 "abcdefghij/abcdefghij/abcdefghij/abcdefghij/abcd"
#define ID_32 "0123456789abcdef0123456789ABCDEF"
#define DISCOVERY_32 "homeassistant/abcdefghij/abcdefg"

static void the_topics_follow_the_prefix_and_the_device_id(void)
{
    mqtt_topics_t t;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", "hac-a1b2c3d4e5f6", "homeassistant"), HAL_OK);
    HAC_CHECK(strcmp(t.base, "homeair/hac-a1b2c3d4e5f6") == 0);
    HAC_CHECK(strcmp(t.state, "homeair/hac-a1b2c3d4e5f6/state") == 0);
    HAC_CHECK(strcmp(t.status, "homeair/hac-a1b2c3d4e5f6/status") == 0);
    HAC_CHECK(strcmp(t.ha_status, "homeassistant/status") == 0);
    HAC_CHECK(strcmp(t.device_id, "hac-a1b2c3d4e5f6") == 0);

    char topic[MQTT_TOPIC_MAX];
    const size_t n = mqtt_topics_discovery(&t, "sensor", "co2", topic, sizeof(topic));
    HAC_CHECK(strcmp(topic, "homeassistant/sensor/hac-a1b2c3d4e5f6/co2/config") == 0);
    HAC_CHECK_EQ_U64(n, strlen(topic));

    // A prefix of several levels.
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "home/air_2", "t5s3", "ha/disc"), HAL_OK);
    HAC_CHECK(strcmp(t.state, "home/air_2/t5s3/state") == 0);
    HAC_CHECK(strcmp(t.ha_status, "ha/disc/status") == 0);
}

static void the_payloads_and_flags_are_home_assistants_defaults(void)
{
    // Discovery leaves payload_available and payload_not_available out.
    HAC_CHECK(strcmp(MQTT_STATUS_ONLINE, "online") == 0);
    HAC_CHECK(strcmp(MQTT_STATUS_OFFLINE, "offline") == 0);
    HAC_CHECK(strcmp(MQTT_DISCOVERY_PREFIX_DEFAULT, "homeassistant") == 0);
    // ADR 0029: QoS 1 and retained, every one.
    HAC_CHECK_EQ_U64(MQTT_STATE_QOS, 1u);
    HAC_CHECK_EQ_U64(MQTT_STATUS_QOS, 1u);
    HAC_CHECK_EQ_U64(MQTT_DISCOVERY_QOS, 1u);
    HAC_CHECK(MQTT_STATE_RETAIN && MQTT_STATUS_RETAIN && MQTT_DISCOVERY_RETAIN);
}

static void a_level_is_one_or_more_safe_characters(void)
{
    HAC_CHECK(mqtt_topic_level_valid("a", 1u));
    HAC_CHECK(mqtt_topic_level_valid("Az09_-", 6u));
    HAC_CHECK(mqtt_topic_level_valid(ID_32, MQTT_DEVICE_ID_MAX));
    HAC_CHECK(!mqtt_topic_level_valid(ID_32 "0", MQTT_DEVICE_ID_MAX));  // 33: over-long
    HAC_CHECK(!mqtt_topic_level_valid("", MQTT_DEVICE_ID_MAX));
    HAC_CHECK(!mqtt_topic_level_valid(NULL, MQTT_DEVICE_ID_MAX));
    const char *refused[] = {
        "a+b", "a#", "+", "#", "a/b", "$SYS", "a b", "a.b", "a\tb", "\xC3\xA9",
    };
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        HAC_CHECK(!mqtt_topic_level_valid(refused[i], MQTT_DEVICE_ID_MAX));
    }
}

static void a_prefix_has_no_wildcard_and_no_empty_level(void)
{
    HAC_CHECK(mqtt_topic_prefix_valid("homeair", MQTT_PREFIX_MAX));
    HAC_CHECK(mqtt_topic_prefix_valid("home/air/x", MQTT_PREFIX_MAX));
    HAC_CHECK(mqtt_topic_prefix_valid(PREFIX_48, MQTT_PREFIX_MAX));
    HAC_CHECK(!mqtt_topic_prefix_valid(PREFIX_48 "e", MQTT_PREFIX_MAX));  // 49
    const char *refused[] = {
        "",     "/",      "/home",    "home/",    "home//air", "home/+", "home/#",   "+/air",
        "$SYS", "home/$", "home air", "home/a#b", "a/+/b",     "home\n", "home.air",
    };
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        HAC_CHECK(!mqtt_topic_prefix_valid(refused[i], MQTT_PREFIX_MAX));
    }
    HAC_CHECK(!mqtt_topic_prefix_valid(NULL, MQTT_PREFIX_MAX));
}

static void an_illegal_or_over_long_part_is_refused_and_leaves_nothing(void)
{
    mqtt_topics_t t;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", "a/b", "homeassistant"), HAL_ERR_INVALID_ARG);
    HAC_CHECK(t.state[0] == '\0' && t.base[0] == '\0' && t.device_id[0] == '\0');
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", "id+", "homeassistant"), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", "", "homeassistant"), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", ID_32 "x", "homeassistant"),
                     HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "home#", "id", "homeassistant"), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, PREFIX_48 "e", "id", "homeassistant"),
                     HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", "id", "home//assistant"), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", "id", DISCOVERY_32 "h"), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, NULL, "id", "homeassistant"), HAL_ERR_INVALID_ARG);
    HAC_CHECK(t.state[0] == '\0');
}

static void the_longest_parts_fit_every_topic(void)
{
    mqtt_topics_t t;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, PREFIX_48, ID_32, DISCOVERY_32), HAL_OK);
    HAC_CHECK_EQ_U64(strlen(t.state), 48u + 1u + 32u + 1u + 5u);
    HAC_CHECK_EQ_U64(strlen(t.status), 48u + 1u + 32u + 1u + 6u);

    char topic[MQTT_TOPIC_MAX];
    const char *object_32 = "abcdefghijabcdefghijabcdefghijab";
    const size_t n = mqtt_topics_discovery(&t, "sensor", object_32, topic, sizeof(topic));
    HAC_CHECK_EQ_U64(n, 112u);  // topics.h's longest
    HAC_CHECK_EQ_U64(n, strlen(topic));
}

static void a_discovery_topic_refuses_a_bad_level_or_a_short_buffer(void)
{
    mqtt_topics_t t;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", "id", "homeassistant"), HAL_OK);
    char topic[MQTT_TOPIC_MAX];
    HAC_CHECK_EQ_U64(mqtt_topics_discovery(&t, "sensor", "a/b", topic, sizeof(topic)), 0u);
    HAC_CHECK_EQ_U64(mqtt_topics_discovery(&t, "sensor", "", topic, sizeof(topic)), 0u);
    HAC_CHECK_EQ_U64(mqtt_topics_discovery(&t, "sen+sor", "co2", topic, sizeof(topic)), 0u);
    HAC_CHECK(topic[0] == '\0');

    // "homeassistant/sensor/id/co2/config" is 34 characters.
    HAC_CHECK_EQ_U64(mqtt_topics_discovery(&t, "sensor", "co2", topic, 35u), 34u);
    HAC_CHECK_EQ_U64(mqtt_topics_discovery(&t, "sensor", "co2", topic, 34u), 0u);

    // Topics that were never initialised build nothing.
    mqtt_topics_t empty;
    memset(&empty, 0, sizeof(empty));
    HAC_CHECK_EQ_U64(mqtt_topics_discovery(&empty, "sensor", "co2", topic, sizeof(topic)), 0u);
}

static void the_boards_id_is_its_mac_and_is_recognised(void)
{
    static const uint8_t mac[MQTT_DEVICE_ID_MAC_BYTES] = {0xA1, 0xB2, 0xC3, 0x04, 0xE5, 0xF0};
    char id[MQTT_DEVICE_ID_MAC_LEN + 1u];
    HAC_CHECK_EQ_U64(mqtt_device_id_from_mac(mac, id, sizeof(id)), MQTT_DEVICE_ID_MAC_LEN);
    HAC_CHECK(strcmp(id, "hac-a1b2c304e5f0") == 0);
    HAC_CHECK_EQ_U64(MQTT_DEVICE_ID_MAC_LEN, 16u);
    // A valid level, so it makes topics, and a byte short does not fit.
    mqtt_topics_t t;
    HAC_CHECK_EQ_INT(mqtt_topics_init(&t, "homeair", id, "homeassistant"), HAL_OK);
    HAC_CHECK(text_overflowed(mqtt_device_id_from_mac(mac, id, MQTT_DEVICE_ID_MAC_LEN),
                              MQTT_DEVICE_ID_MAC_LEN));

    HAC_CHECK(mqtt_device_id_is_from_mac("hac-a1b2c304e5f0"));
    HAC_CHECK(mqtt_device_id_is_from_mac("HAC-A1B2C304E5F0"));  // a case away is refused too
    HAC_CHECK(mqtt_device_id_is_from_mac("hac-000000000000"));
    const char *others[] = {
        "simulator",        "hac-a1b2c304e5f",  "hac-a1b2c304e5f00", "hac_a1b2c304e5f0",
        "hacka1b2c304e5f0", "hac-a1b2c304e5g0", "xac-a1b2c304e5f0",  "",
        "kitchen",
    };
    for (size_t i = 0u; i < sizeof(others) / sizeof(others[0]); ++i) {
        HAC_CHECK(!mqtt_device_id_is_from_mac(others[i]));
    }
    HAC_CHECK(!mqtt_device_id_is_from_mac(NULL));
}

int main(void)
{
    HAC_RUN(the_topics_follow_the_prefix_and_the_device_id);
    HAC_RUN(the_payloads_and_flags_are_home_assistants_defaults);
    HAC_RUN(a_level_is_one_or_more_safe_characters);
    HAC_RUN(a_prefix_has_no_wildcard_and_no_empty_level);
    HAC_RUN(an_illegal_or_over_long_part_is_refused_and_leaves_nothing);
    HAC_RUN(the_longest_parts_fit_every_topic);
    HAC_RUN(a_discovery_topic_refuses_a_bad_level_or_a_short_buffer);
    HAC_RUN(the_boards_id_is_its_mac_and_is_recognised);
    return hac_test_summary();
}
