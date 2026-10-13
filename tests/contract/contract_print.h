// SPDX-License-Identifier: Apache-2.0
//
// One line per contract check, printed identically by every runner -- the
// host CTest binaries and the on-target Unity app
// (apps/firmware_esp32/test_app/) -- so "the device produces the same result
// as the host" is something diff can check rather than something read by eye:
//
//   contract <port> <implementation> <check> PASS|FAIL
//
// Runner-side only. The contract bodies themselves stay free of stdio (see
// port_clock_contract.h); they report through contract_report_fn, and a
// runner forwards to contract_print_check() from its own callback.

#ifndef HAC_TESTS_CONTRACT_PRINT_H
#define HAC_TESTS_CONTRACT_PRINT_H

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *port;            // "port_clock"
    const char *implementation;  // "adp_clock", "fake_clock", ...
} contract_print_ctx_t;

static inline void contract_print_check(const contract_print_ctx_t *ctx, const char *check, bool ok)
{
    printf("contract %s %s %s %s\n", ctx->port, ctx->implementation, check, ok ? "PASS" : "FAIL");
}

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_PRINT_H
