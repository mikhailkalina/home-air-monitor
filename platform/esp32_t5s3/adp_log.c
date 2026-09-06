// SPDX-License-Identifier: Apache-2.0

#include "adp_log.h"

#include <stdio.h>

#include "esp_log.h"

// Long enough for every call site in this phase (a flush log line: a
// rectangle, a refresh mode name, a dirty-rect flag) with headroom; longer
// lines are truncated by snprintf rather than overflowing.
#define ADP_LOG_LINE_MAX 256u

static void append_field(char *buf, size_t cap, size_t *len, const log_field_t *f)
{
    if (*len >= cap) {
        return;
    }

    const char *key = (f->key != NULL) ? f->key : "?";
    int n = 0;

    switch (f->kind) {
        case LOG_FIELD_I64:
            n = snprintf(buf + *len, cap - *len, " %s=%lld", key, (long long)f->v.i64);
            break;
        case LOG_FIELD_U64:
            n = snprintf(buf + *len, cap - *len, " %s=%llu", key, (unsigned long long)f->v.u64);
            break;
        case LOG_FIELD_F32:
            n = snprintf(buf + *len, cap - *len, " %s=%.3f", key, (double)f->v.f32);
            break;
        case LOG_FIELD_BOOL:
            n = snprintf(buf + *len, cap - *len, " %s=%s", key, f->v.b ? "true" : "false");
            break;
        case LOG_FIELD_STR:
            n = snprintf(buf + *len, cap - *len, " %s=%s", key,
                         (f->v.str != NULL) ? f->v.str : "(null)");
            break;
        default:
            n = snprintf(buf + *len, cap - *len, " %s=?", key);
            break;
    }

    if (n > 0) {
        // snprintf can report more than it actually wrote when the buffer is
        // full; clamp so *len never exceeds cap and the next call's
        // remaining-space computation cannot underflow.
        const size_t written = (size_t)n;
        *len += (written < cap - *len) ? written : (cap - *len);
    }
}

static void log_write(const port_log_t *port, log_level_t level, const char *tag, const char *msg,
                      const log_field_t *fields, size_t field_count)
{
    (void)port;

    char line[ADP_LOG_LINE_MAX];
    size_t len = 0u;

    const int n = snprintf(line, sizeof(line), "%s", (msg != NULL) ? msg : "");
    if (n > 0) {
        const size_t written = (size_t)n;
        len = (written < sizeof(line)) ? written : sizeof(line) - 1u;
    }

    if (fields != NULL) {
        for (size_t i = 0u; i < field_count; ++i) {
            append_field(line, sizeof(line), &len, &fields[i]);
        }
    }

    const char *use_tag = (tag != NULL) ? tag : "?";

    switch (level) {
        case LOG_LEVEL_ERROR:
            ESP_LOGE(use_tag, "%s", line);
            break;
        case LOG_LEVEL_WARN:
            ESP_LOGW(use_tag, "%s", line);
            break;
        case LOG_LEVEL_INFO:
            ESP_LOGI(use_tag, "%s", line);
            break;
        case LOG_LEVEL_DEBUG:
        default:
            ESP_LOGD(use_tag, "%s", line);
            break;
    }
}

void adp_log_init(adp_log_t *l)
{
    l->port.write = log_write;
    l->port.impl = l;
}

const port_log_t *adp_log_port(const adp_log_t *l)
{
    return &l->port;
}
