// SPDX-License-Identifier: Apache-2.0
//
// The Wi-Fi station the test app's network cases share: the port_net
// contract's (test_port_net_contract.c) and the port_telemetry contract's
// (test_port_telemetry_contract.c), which needs a link under its broker.
// ESP-IDF's Wi-Fi driver is one, and adp_net takes it once
// (platform/esp32_t5s3/adp_net.h): the first case to ask sets it up, with
// the firmware's task parameters, and every later one gets the same.

#ifndef HAC_TESTS_CONTRACT_ESP32_TEST_STATION_H
#define HAC_TESTS_CONTRACT_ESP32_TEST_STATION_H

#include <stdbool.h>
#include <stdint.h>

#include "adp_net.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_log.h"

typedef struct {
    hal_status_t status;  // adp_net_init()'s answer
    adp_net_t *net;
    const port_clock_t *clock;
    const port_log_t *log;
} test_station_t;

// Set up on the first call. Only for an image with credentials
// (CONFIG_HAC_WIFI_SSID): the caller checks first, and IGNOREs without one.
const test_station_t *test_station(void);

// down() first, and the wait for the station to be quiet; then up(), and the
// wait for the link -- both within `within_ms` in all. True once NET_UP.
bool test_station_up(const test_station_t *st, uint32_t within_ms);

#endif  // HAC_TESTS_CONTRACT_ESP32_TEST_STATION_H
