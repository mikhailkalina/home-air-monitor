// SPDX-License-Identifier: Apache-2.0
//
// The host port_log implementation: one line per call, to stdout for
// LOG_LEVEL_INFO/DEBUG and stderr for LOG_LEVEL_WARN/ERROR, with fields
// rendered as "key=value" pairs after the message.
//
// This is the second port_log implementation CLAUDE.md requires before the
// port itself is trusted ("a port is created when there are two real
// implementations" -- docs/architecture.md 12): platform/esp32_t5s3/adp_log.c
// over ESP_LOG is the first. Nothing in the simulator depends on this yet;
// it exists so tests/contract/test_port_log_contract.c has a second adapter
// to run the shared contract against, and so a future host consumer of
// port_log has somewhere to plug in.

#ifndef HAC_PLATFORM_HOST_ADP_LOG_CONSOLE_H
#define HAC_PLATFORM_HOST_ADP_LOG_CONSOLE_H

#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_log_t port;
} adp_log_console_t;

void adp_log_console_init(adp_log_console_t *l);

const port_log_t *adp_log_console_port(const adp_log_console_t *l);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_LOG_CONSOLE_H
