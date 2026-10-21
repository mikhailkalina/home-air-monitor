// SPDX-License-Identifier: Apache-2.0
//
// A private mosquitto broker for one test: started on a free port of
// 127.0.0.1 with a configuration of its own -- anonymous, nothing kept on
// disk -- so that a test never meets another's sessions, or a broker the
// machine already runs, and stopped when the test is done
// (docs/adr/0030-telemetry-ports-and-the-reporter.md, amendment "The
// simulator on a real broker"). POSIX processes and sockets: host tests only.

#ifndef HAC_TESTS_SUPPORT_MQTT_TEST_BROKER_H
#define HAC_TESTS_SUPPORT_MQTT_TEST_BROKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How long a broker may take to accept its first connection.
#define MQTT_TEST_BROKER_START_MS 5000u

#define MQTT_TEST_BROKER_PATH_MAX 512u

typedef struct {
    long pid;  // 0: not running
    uint16_t port;
    char log_path[MQTT_TEST_BROKER_PATH_MAX];
} mqtt_test_broker_t;

// Starts `binary`, the mosquitto executable, writing its configuration and
// its log into `work_dir`. False, with a sentence in `why`, when it cannot
// start, or does not accept a connection within MQTT_TEST_BROKER_START_MS.
bool mqtt_test_broker_start(mqtt_test_broker_t *b, const char *binary, const char *work_dir,
                            char *why, size_t why_cap);

// SIGTERM, then SIGKILL if it lingers; waits for it. Fine on one not running.
void mqtt_test_broker_stop(mqtt_test_broker_t *b);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_SUPPORT_MQTT_TEST_BROKER_H
