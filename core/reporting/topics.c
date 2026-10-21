// SPDX-License-Identifier: Apache-2.0

#include "reporting/topics.h"

#include <string.h>

#include "text/text_append.h"

static bool level_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

bool mqtt_topic_level_valid(const char *level, size_t max)
{
    if (level == NULL || level[0] == '\0') {
        return false;
    }
    size_t n = 0u;
    for (; level[n] != '\0'; ++n) {
        if (n >= max || !level_char(level[n])) {
            return false;
        }
    }
    return true;
}

bool mqtt_topic_prefix_valid(const char *prefix, size_t max)
{
    if (prefix == NULL || prefix[0] == '\0') {
        return false;
    }
    size_t level_len = 0u;
    size_t n = 0u;
    for (; prefix[n] != '\0'; ++n) {
        if (n >= max) {
            return false;
        }
        if (prefix[n] == '/') {
            if (level_len == 0u) {
                return false;  // a leading '/' or "//": an empty level
            }
            level_len = 0u;
        } else if (level_char(prefix[n])) {
            level_len++;
        } else {
            return false;
        }
    }
    return level_len > 0u;  // a trailing '/' leaves an empty last level
}

// "<a>/<b>" into dst; false when it does not fit.
static bool join(char *dst, size_t cap, const char *a, const char *b)
{
    size_t len = text_append_str(dst, cap, 0u, a);
    len = text_append_char(dst, cap, len, '/');
    len = text_append_str(dst, cap, len, b);
    return !text_overflowed(len, cap);
}

hal_status_t mqtt_topics_init(mqtt_topics_t *out, const char *prefix, const char *device_id,
                              const char *discovery_prefix)
{
    memset(out, 0, sizeof(*out));
    if (!mqtt_topic_prefix_valid(prefix, MQTT_PREFIX_MAX) ||
        !mqtt_topic_level_valid(device_id, MQTT_DEVICE_ID_MAX) ||
        !mqtt_topic_prefix_valid(discovery_prefix, MQTT_DISCOVERY_PREFIX_MAX)) {
        return HAL_ERR_INVALID_ARG;
    }
    (void)text_append_str(out->device_id, sizeof(out->device_id), 0u, device_id);
    (void)text_append_str(out->discovery_prefix, sizeof(out->discovery_prefix), 0u,
                          discovery_prefix);
    // The bounds above make every one of these fit (MQTT_TOPIC_MAX).
    const bool built =
        join(out->base, sizeof(out->base), prefix, device_id) &&
        join(out->state, sizeof(out->state), out->base, MQTT_TOPIC_STATE) &&
        join(out->status, sizeof(out->status), out->base, MQTT_TOPIC_STATUS) &&
        join(out->ha_status, sizeof(out->ha_status), discovery_prefix, MQTT_TOPIC_STATUS);
    if (!built) {
        memset(out, 0, sizeof(*out));
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_OK;
}

size_t mqtt_topics_discovery(const mqtt_topics_t *t, const char *component, const char *object_id,
                             char *dst, size_t cap)
{
    if (!mqtt_topic_level_valid(component, MQTT_OBJECT_ID_MAX) ||
        !mqtt_topic_level_valid(object_id, MQTT_OBJECT_ID_MAX) || t->device_id[0] == '\0') {
        if (cap > 0u) {
            dst[0] = '\0';
        }
        return 0u;
    }
    size_t len = text_append_str(dst, cap, 0u, t->discovery_prefix);
    len = text_append_char(dst, cap, len, '/');
    len = text_append_str(dst, cap, len, component);
    len = text_append_char(dst, cap, len, '/');
    len = text_append_str(dst, cap, len, t->device_id);
    len = text_append_char(dst, cap, len, '/');
    len = text_append_str(dst, cap, len, object_id);
    len = text_append_str(dst, cap, len, "/config");
    return text_overflowed(len, cap) ? 0u : len;
}

size_t mqtt_device_id_from_mac(const uint8_t mac[MQTT_DEVICE_ID_MAC_BYTES], char *dst, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    size_t len = text_append_str(dst, cap, 0u, MQTT_DEVICE_ID_MAC_PREFIX);
    for (size_t i = 0u; i < MQTT_DEVICE_ID_MAC_BYTES; ++i) {
        len = text_append_char(dst, cap, len, digits[mac[i] >> 4u]);
        len = text_append_char(dst, cap, len, digits[mac[i] & 0x0Fu]);
    }
    return len;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool hex_digit(char c)
{
    const char l = lower(c);
    return (l >= '0' && l <= '9') || (l >= 'a' && l <= 'f');
}

bool mqtt_device_id_is_from_mac(const char *id)
{
    if (id == NULL || strlen(id) != MQTT_DEVICE_ID_MAC_LEN) {
        return false;
    }
    const size_t prefix_len = sizeof(MQTT_DEVICE_ID_MAC_PREFIX) - 1u;
    for (size_t i = 0u; i < prefix_len; ++i) {
        if (lower(id[i]) != MQTT_DEVICE_ID_MAC_PREFIX[i]) {
            return false;
        }
    }
    for (size_t i = prefix_len; i < MQTT_DEVICE_ID_MAC_LEN; ++i) {
        if (!hex_digit(id[i])) {
            return false;
        }
    }
    return true;
}
