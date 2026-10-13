// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_clock_contract.c (see that file for why the contract
// body itself is shared, unmodified, with the ESP32 firmware). This file
// only adapts contract_report_fn to HAC_CHECK and supplies the
// implementations to run it against: fake_clock (the test double) and
// adp_clock_virtual (the adapter the simulator ships).
//
// Every check is also printed through contract_print.h, in the format the
// on-target runner (tests/contract/esp32/) prints, so the two results can be
// compared line for line.

#include "port_clock_contract.h"

#include <stddef.h>

#include "adp_clock_virtual.h"
#include "contract_print.h"
#include "fake_clock.h"
#include "hac_test.h"

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static void fake_clock_satisfies_the_port_clock_contract(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);

    contract_print_ctx_t ctx = {.port = "port_clock", .implementation = "fake_clock"};
    const unsigned failures =
        port_clock_contract_run(fake_clock_port(&fc), report_to_hac_test, &ctx);
    HAC_CHECK_EQ_INT((int)failures, 0);
}

static void adp_clock_virtual_satisfies_the_port_clock_contract(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    contract_print_ctx_t ctx = {.port = "port_clock", .implementation = "adp_clock_virtual"};
    const unsigned failures =
        port_clock_contract_run(adp_clock_virtual_port(&vc), report_to_hac_test, &ctx);
    HAC_CHECK_EQ_INT((int)failures, 0);
}

int main(void)
{
    HAC_RUN(fake_clock_satisfies_the_port_clock_contract);
    HAC_RUN(adp_clock_virtual_satisfies_the_port_clock_contract);
    return hac_test_summary();
}
