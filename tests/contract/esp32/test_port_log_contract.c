// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_log_contract.c against adp_log. See
// tests/contract/esp32/test_port_clock_contract.c for how it is built.

#include <stdbool.h>

#include "unity.h"

#include "adp_log.h"
#include "contract_print.h"
#include "port_log_contract.h"

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

TEST_CASE("adp_log satisfies the port_log contract", "[contract][port_log]")
{
    static adp_log_t log;
    adp_log_init(&log);

    contract_print_ctx_t ctx = {.port = "port_log", .implementation = "adp_log"};
    const unsigned failures = port_log_contract_run(adp_log_port(&log), report, &ctx);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}
