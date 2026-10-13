// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_i2c_contract.c against adp_i2c_fake, the bus the
// simulator's fake-i2c mode and phase 3's driver tests run on. The on-target
// runner (tests/contract/esp32/test_port_i2c_contract.c) runs the same body
// against adp_i2c and the board's RTC; both print one
// "contract port_i2c <implementation> <check> PASS|FAIL" line per check, so
// the two results diff line for line once the implementation column is
// dropped.

#include "port_i2c_contract.h"

#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "contract_print.h"
#include "hac_test.h"

// Arbitrary, but kept apart from each other. The fake knows nothing about
// the board; these are not the board's addresses.
#define FAKE_PRESENT_ADDR 0x51u
#define FAKE_ABSENT_ADDR 0x08u
#define FAKE_PRESENT_REG 0x00u

// A slave the bus-clear procedure frees part-way through its nine clocks,
// so the fake's recover() has some clocking to do.
#define FAKE_HANG_PULSES_TO_RELEASE 4u

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static bool fake_hang(void *ctx)
{
    adp_i2c_fake_hang((adp_i2c_fake_t *)ctx, FAKE_HANG_PULSES_TO_RELEASE);
    return true;
}

static bool fake_stall(void *ctx)
{
    adp_i2c_fake_hold_scl((adp_i2c_fake_t *)ctx, true);
    return true;
}

static void fake_end_stall(void *ctx)
{
    adp_i2c_fake_hold_scl((adp_i2c_fake_t *)ctx, false);
}

static bool fake_sda_held_low(void *ctx)
{
    return adp_i2c_fake_sda_held_low((const adp_i2c_fake_t *)ctx);
}

static void adp_i2c_fake_satisfies_the_port_i2c_contract(void)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);

    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, &clock);
    HAC_CHECK(adp_i2c_fake_add_device(&bus, FAKE_PRESENT_ADDR) != NULL);

    const port_i2c_contract_env_t env = {
        .clock = adp_clock_virtual_port(&clock),
        .present_addr = FAKE_PRESENT_ADDR,
        .present_reg = FAKE_PRESENT_REG,
        .absent_addr = FAKE_ABSENT_ADDR,
        .hang_bus = fake_hang,
        .stall_bus = fake_stall,
        .end_stall = fake_end_stall,
        .sda_held_low = fake_sda_held_low,
        .ctx = &bus,
    };

    contract_print_ctx_t ctx = {.port = "port_i2c", .implementation = "adp_i2c_fake"};
    const unsigned failures =
        port_i2c_contract_run(adp_i2c_fake_port(&bus), &env, report_to_hac_test, &ctx);
    HAC_CHECK_EQ_INT((int)failures, 0);

    // The suite must leave the bus as it found it: no fault still injected.
    HAC_CHECK(!bus.sda_stuck);
    HAC_CHECK(!bus.scl_held);
}

// --- the suite has teeth ------------------------------------------------
//
// A contract that has only ever passed is not known to work. Each case below
// wraps the fake in a port that breaks exactly one rule of ports/port_i2c.h --
// each one a mistake an adapter over a real driver could plausibly make --
// and asserts that the named check, and only checks, fail.

typedef enum {
    MUTANT_RECOVER_DOES_NOTHING,     // returns HAL_OK, never clocks SCL
    MUTANT_TIMEOUT_OVERRUNS,         // a stalled transfer blocks a second past its deadline
    MUTANT_ZERO_LENGTH_REJECTED,     // the raw ESP-IDF driver's own behaviour
    MUTANT_SUB_TICK_DEADLINE_FAILS,  // pdMS_TO_TICKS() truncating a short deadline to 0
    MUTANT_HUNG_BUS_BLAMES_DEVICE,   // a stuck SDA reported as an absent device
} mutant_kind_t;

typedef struct {
    port_i2c_t port;
    port_i2c_t *inner;
    adp_i2c_fake_t *fake;
    adp_clock_virtual_t *clock;
    mutant_kind_t kind;
} mutant_t;

// Simulated scheduler tick for MUTANT_SUB_TICK_DEADLINE_FAILS: the board's.
#define MUTANT_TICK_MS 10u
#define MUTANT_OVERRUN_MS 1000u

static hal_status_t mutant_gate(mutant_t *m, uint32_t to_ms)
{
    if (m->kind == MUTANT_SUB_TICK_DEADLINE_FAILS && to_ms > 0u && to_ms < MUTANT_TICK_MS) {
        return HAL_ERR_TIMEOUT;
    }
    return HAL_OK;
}

static hal_status_t mutant_after(mutant_t *m, hal_status_t s)
{
    if (m->kind == MUTANT_TIMEOUT_OVERRUNS && s == HAL_ERR_TIMEOUT && m->fake->scl_held) {
        adp_clock_virtual_advance(m->clock, MUTANT_OVERRUN_MS);
    }
    if (m->kind == MUTANT_HUNG_BUS_BLAMES_DEVICE && s == HAL_ERR_TIMEOUT && m->fake->sda_stuck) {
        return HAL_ERR_NOT_FOUND;
    }
    return s;
}

static hal_status_t mutant_write(port_i2c_t *port, uint8_t addr, const uint8_t *d, size_t n,
                                 uint32_t to_ms)
{
    mutant_t *m = (mutant_t *)port->impl;
    if (m->kind == MUTANT_ZERO_LENGTH_REJECTED && n == 0u) {
        return HAL_ERR_INVALID_ARG;
    }
    const hal_status_t gate = mutant_gate(m, to_ms);
    if (gate != HAL_OK) {
        return gate;
    }
    return mutant_after(m, m->inner->write(m->inner, addr, d, n, to_ms));
}

static hal_status_t mutant_read(port_i2c_t *port, uint8_t addr, uint8_t *d, size_t n,
                                uint32_t to_ms)
{
    mutant_t *m = (mutant_t *)port->impl;
    const hal_status_t gate = mutant_gate(m, to_ms);
    if (gate != HAL_OK) {
        return gate;
    }
    return mutant_after(m, m->inner->read(m->inner, addr, d, n, to_ms));
}

static hal_status_t mutant_write_read(port_i2c_t *port, uint8_t addr, const uint8_t *w, size_t wn,
                                      uint8_t *r, size_t rn, uint32_t to_ms)
{
    mutant_t *m = (mutant_t *)port->impl;
    const hal_status_t gate = mutant_gate(m, to_ms);
    if (gate != HAL_OK) {
        return gate;
    }
    return mutant_after(m, m->inner->write_read(m->inner, addr, w, wn, r, rn, to_ms));
}

static hal_status_t mutant_recover(port_i2c_t *port)
{
    mutant_t *m = (mutant_t *)port->impl;
    if (m->kind == MUTANT_RECOVER_DOES_NOTHING) {
        return HAL_OK;
    }
    return m->inner->recover(m->inner);
}

// Collects which checks failed, by name.
#define MAX_RECORDED_FAILURES 16u

typedef struct {
    const char *failed[MAX_RECORDED_FAILURES];
    unsigned count;
} failure_log_t;

static void record_failures(void *ctx, const char *check, bool ok)
{
    failure_log_t *log = (failure_log_t *)ctx;
    if (!ok && log->count < MAX_RECORDED_FAILURES) {
        log->failed[log->count++] = check;
    }
}

static bool failed(const failure_log_t *log, const char *check)
{
    for (unsigned i = 0; i < log->count; i++) {
        if (strcmp(log->failed[i], check) == 0) {
            return true;
        }
    }
    return false;
}

static failure_log_t run_against_mutant(mutant_kind_t kind)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);

    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, &clock);
    (void)adp_i2c_fake_add_device(&bus, FAKE_PRESENT_ADDR);

    mutant_t m = {
        .port =
            {
                .write = mutant_write,
                .read = mutant_read,
                .write_read = mutant_write_read,
                .recover = mutant_recover,
            },
        .inner = adp_i2c_fake_port(&bus),
        .fake = &bus,
        .clock = &clock,
        .kind = kind,
    };
    m.port.impl = &m;

    const port_i2c_contract_env_t env = {
        .clock = adp_clock_virtual_port(&clock),
        .present_addr = FAKE_PRESENT_ADDR,
        .present_reg = FAKE_PRESENT_REG,
        .absent_addr = FAKE_ABSENT_ADDR,
        .hang_bus = fake_hang,
        .stall_bus = fake_stall,
        .end_stall = fake_end_stall,
        .sda_held_low = fake_sda_held_low,
        .ctx = &bus,
    };

    failure_log_t log = {.count = 0u};
    (void)port_i2c_contract_run(&m.port, &env, record_failures, &log);
    return log;
}

static void the_contract_catches_a_recover_that_does_nothing(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_RECOVER_DOES_NOTHING);
    HAC_CHECK(failed(&log, "i2c.recover.after_hang_sda_released"));
    HAC_CHECK(failed(&log, "i2c.recover.after_hang_bus_usable"));
}

static void the_contract_catches_a_timeout_that_overruns_its_deadline(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_TIMEOUT_OVERRUNS);
    HAC_CHECK(failed(&log, "i2c.timeout.stalled_transfer_returns_within_deadline"));
    HAC_CHECK_EQ_INT((int)log.count, 1);
}

static void the_contract_catches_a_rejected_zero_length_write(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_ZERO_LENGTH_REJECTED);
    HAC_CHECK(failed(&log, "i2c.write(n=0).present_address_acks"));
    HAC_CHECK(failed(&log, "i2c.write(n=0).absent_address_not_found"));
    HAC_CHECK_EQ_INT((int)log.count, 2);
}

static void the_contract_catches_a_sub_tick_deadline_that_fails_early(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_SUB_TICK_DEADLINE_FAILS);
    HAC_CHECK(failed(&log, "i2c.timeout.short_deadline_still_completes"));
    HAC_CHECK_EQ_INT((int)log.count, 1);
}

static void the_contract_catches_a_hung_bus_blamed_on_the_device(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_HUNG_BUS_BLAMES_DEVICE);
    HAC_CHECK(failed(&log, "i2c.hang.transfer_reports_timeout_io_or_completes"));
    HAC_CHECK_EQ_INT((int)log.count, 1);
}

int main(void)
{
    HAC_RUN(adp_i2c_fake_satisfies_the_port_i2c_contract);
    HAC_RUN(the_contract_catches_a_recover_that_does_nothing);
    HAC_RUN(the_contract_catches_a_timeout_that_overruns_its_deadline);
    HAC_RUN(the_contract_catches_a_rejected_zero_length_write);
    HAC_RUN(the_contract_catches_a_sub_tick_deadline_that_fails_early);
    HAC_RUN(the_contract_catches_a_hung_bus_blamed_on_the_device);
    return hac_test_summary();
}
