// SPDX-License-Identifier: Apache-2.0
//
// port_log over ESP_LOG. Each write() call renders `msg` plus its
// `log_field_t` array into one fixed-size line (truncated if it would not
// fit) and hands that line to ESP_LOGE/W/I/D as a "%s" argument -- never as
// the format string itself, so a field value or message that happens to
// contain a '%' cannot be mistaken for a format specifier. Going through the
// ESP_LOGx macros, rather than esp_log_write() directly, keeps the usual
// ESP-IDF prefix (level letter, timestamp, tag, colour) and the usual
// compile-time level filtering (CONFIG_LOG_DEFAULT_LEVEL and LOG_LOCAL_LEVEL)
// exactly as every other component gets it.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_LOG_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_LOG_H

#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_log_t port;
} adp_log_t;

void adp_log_init(adp_log_t *l);

const port_log_t *adp_log_port(const adp_log_t *l);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_LOG_H
