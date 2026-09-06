// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_log_contract.c (see port_clock_contract.h for why the
// contract body is shared, unmodified, with the ESP32 firmware). This file
// only adapts contract_report_fn to HAC_CHECK and supplies the
// implementation to run it against: adp_log_console, the host's port_log.

#include "port_log_contract.h"

#include "adp_log_console.h"
#include "hac_test.h"

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    (void)ctx;
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static void adp_log_console_satisfies_the_port_log_contract(void)
{
    adp_log_console_t console;
    adp_log_console_init(&console);

    const unsigned failures =
        port_log_contract_run(adp_log_console_port(&console), report_to_hac_test, NULL);
    HAC_CHECK_EQ_INT((int)failures, 0);
}

int main(void)
{
    HAC_RUN(adp_log_console_satisfies_the_port_log_contract);
    return hac_test_summary();
}
