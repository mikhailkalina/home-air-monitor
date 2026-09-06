// SPDX-License-Identifier: Apache-2.0

#include "port_clock_contract.h"

#include <stddef.h>

// A wall clock that claims to be synchronized had better not claim a time
// before this project existed. Not a calendar check, just a sanity floor:
// 2020-01-01T00:00:00Z in milliseconds since the Unix epoch.
#define WALL_MS_SANE_FLOOR 1577836800000ULL

// Kept short: this body runs once at device boot, not just under CTest.
#define CONTRACT_DELAY_MS 15u

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

unsigned port_clock_contract_run(const port_clock_t *clock, contract_report_fn report,
                                 void *report_ctx)
{
    unsigned failures = 0u;

    check(report, report_ctx, &failures, "clock.vtable.now_ms", clock->now_ms != NULL);
    check(report, report_ctx, &failures, "clock.vtable.wall_ms", clock->wall_ms != NULL);
    check(report, report_ctx, &failures, "clock.vtable.delay_ms", clock->delay_ms != NULL);
    if (clock->now_ms == NULL || clock->wall_ms == NULL || clock->delay_ms == NULL) {
        return failures;  // nothing else can be exercised safely
    }

    // now_ms() never goes backwards between two samples taken back to back.
    const uint64_t t0 = clock->now_ms(clock);
    const uint64_t t1 = clock->now_ms(clock);
    check(report, report_ctx, &failures, "clock.now_ms.non_decreasing_at_rest", t1 >= t0);

    // delay_ms(0) is a legal no-op request: it must not move time backwards.
    clock->delay_ms(clock, 0u);
    const uint64_t t2 = clock->now_ms(clock);
    check(report, report_ctx, &failures, "clock.delay_ms(0).does_not_go_backwards", t2 >= t1);

    // delay_ms(N) advances now_ms() by at least N. A virtual clock adds
    // exactly N; a real clock may add more if it is scheduled late, but never
    // less, or the port's "at least N ms have passed" contract is broken.
    const uint64_t before = clock->now_ms(clock);
    clock->delay_ms(clock, CONTRACT_DELAY_MS);
    const uint64_t after = clock->now_ms(clock);
    check(report, report_ctx, &failures, "clock.delay_ms(N).advances_by_at_least_n",
          after >= before + CONTRACT_DELAY_MS);

    // wall_ms() is either exactly 0 (unsynchronized, per the port contract)
    // or a plausible calendar time. It must never silently return garbage
    // that a caller could mistake for a synchronized clock.
    const uint64_t wall = clock->wall_ms(clock);
    check(report, report_ctx, &failures, "clock.wall_ms.zero_or_plausible",
          wall == 0u || wall >= WALL_MS_SANE_FLOOR);

    return failures;
}
