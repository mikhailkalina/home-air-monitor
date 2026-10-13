// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_clock_contract.c: the same shared body the host
// runner (tests/contract/test_port_clock_contract.c) and the boot-time check
// in apps/firmware_esp32/main/main.c both run, here against adp_clock under
// the ESP-IDF Unity runner. Built into the test app with
//
//   cd apps/firmware_esp32/test_app && idf.py -T hac_platform build
//
// (apps/firmware_esp32/components/hac_platform/test/ registers this file).

#include <stdbool.h>

#include "unity.h"

#include "adp_clock.h"
#include "adp_log.h"
#include "contract_print.h"
#include "port_clock_contract.h"

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

TEST_CASE("adp_clock satisfies the port_clock contract", "[contract][port_clock]")
{
    static adp_log_t log;
    static adp_clock_t clock;
    adp_log_init(&log);
    adp_clock_init(&clock, adp_log_port(&log));

    contract_print_ctx_t ctx = {.port = "port_clock", .implementation = "adp_clock"};
    const unsigned failures = port_clock_contract_run(adp_clock_port(&clock), report, &ctx);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}
