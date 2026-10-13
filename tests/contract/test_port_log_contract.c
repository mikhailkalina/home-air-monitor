// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_log_contract.c (see port_clock_contract.h for why the
// contract body is shared, unmodified, with the ESP32 firmware). This file
// only adapts contract_report_fn to HAC_CHECK and supplies the
// implementation to run it against: adp_log_console, the host's port_log.
// Checks are also printed through contract_print.h, as the on-target runner
// prints them.

#include "port_log_contract.h"

#include "adp_log_console.h"
#include "contract_print.h"
#include "hac_test.h"

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static void adp_log_console_satisfies_the_port_log_contract(void)
{
    adp_log_console_t console;
    adp_log_console_init(&console);

    contract_print_ctx_t ctx = {.port = "port_log", .implementation = "adp_log_console"};
    const unsigned failures =
        port_log_contract_run(adp_log_console_port(&console), report_to_hac_test, &ctx);
    HAC_CHECK_EQ_INT((int)failures, 0);
}

int main(void)
{
    HAC_RUN(adp_log_console_satisfies_the_port_log_contract);
    return hac_test_summary();
}
