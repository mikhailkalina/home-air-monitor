// SPDX-License-Identifier: Apache-2.0

#include "port_i2c_contract.h"

#include <stddef.h>

// The deadline the suite gives an ordinary transfer. Generous against the
// ~0.3 ms a two-byte register read takes at 100 kHz, so a pass never depends
// on scheduling luck.
#define CONTRACT_I2C_TIMEOUT_MS 50u

// What "returns within its deadline" allows beyond `to_ms`. ports/port_i2c.h
// permits the platform's scheduling granularity on top of the deadline; on
// the board that is up to two 10 ms ticks (adp_i2c.c rounds every driver wait
// up by one tick, and the wait can begin anywhere inside the current one --
// docs/adr/0006), plus the follow-up probe adp_i2c.c uses to classify a
// failure. Fifty milliseconds covers that with room, and is still far below
// anything a hang would produce.
#define CONTRACT_I2C_SLACK_MS 50u

// Shorter than one scheduler tick on the board (10 ms at 100 Hz). A healthy
// transfer must still complete: ports/port_i2c.h forbids giving up early.
#define CONTRACT_I2C_SHORT_TIMEOUT_MS 1u

// A transfer that cannot complete is given this long. Long enough that the
// "returns within its deadline" check is about the deadline rather than the
// slack, short enough to keep the run quick.
#define CONTRACT_I2C_STALL_TIMEOUT_MS 100u

// The first address above the 7-bit range.
#define CONTRACT_I2C_ADDR_TOO_LARGE (PORT_I2C_ADDR_MAX + 1u)

// Two registers: enough to make write_read() a multi-byte read without
// reaching past what any register-file device has.
#define CONTRACT_I2C_READ_LEN 2u

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

static bool within(const port_i2c_contract_env_t *env, uint64_t start_ms, uint32_t to_ms)
{
    const uint64_t elapsed = env->clock->now_ms(env->clock) - start_ms;
    return elapsed <= (uint64_t)to_ms + CONTRACT_I2C_SLACK_MS;
}

static bool not_found_or_io(hal_status_t s)
{
    return s == HAL_ERR_NOT_FOUND || s == HAL_ERR_IO;
}

static hal_status_t read_present_register(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                                          uint32_t to_ms)
{
    uint8_t buf[CONTRACT_I2C_READ_LEN] = {0};
    return bus->write_read(bus, env->present_addr, &env->present_reg, 1u, buf, sizeof buf, to_ms);
}

static void check_arguments(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                            contract_report_fn report, void *ctx, unsigned *failures)
{
    uint8_t buf[1] = {0};

    check(report, ctx, failures, "i2c.addr_above_7_bits.invalid_arg",
          bus->write(bus, (uint8_t)CONTRACT_I2C_ADDR_TOO_LARGE, &env->present_reg, 1u,
                     CONTRACT_I2C_TIMEOUT_MS) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "i2c.to_ms_zero.invalid_arg",
          bus->write_read(bus, env->present_addr, &env->present_reg, 1u, buf, sizeof buf, 0u) ==
              HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "i2c.null_buffer.invalid_arg",
          bus->read(bus, env->present_addr, NULL, 1u, CONTRACT_I2C_TIMEOUT_MS) ==
              HAL_ERR_INVALID_ARG);
}

static void check_present_device(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                                 contract_report_fn report, void *ctx, unsigned *failures)
{
    uint8_t buf[1] = {0};

    check(report, ctx, failures, "i2c.write_read.present_device_ok",
          read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
    // One byte only: on the board this sets the RTC's register pointer and
    // changes nothing else (see port_i2c_contract.h).
    check(report, ctx, failures, "i2c.write.register_pointer_only_ok",
          bus->write(bus, env->present_addr, &env->present_reg, 1u, CONTRACT_I2C_TIMEOUT_MS) ==
              HAL_OK);
    check(report, ctx, failures, "i2c.read.present_device_ok",
          bus->read(bus, env->present_addr, buf, sizeof buf, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
}

static void check_zero_length(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                              contract_report_fn report, void *ctx, unsigned *failures)
{
    uint8_t buf[1] = {0};

    check(report, ctx, failures, "i2c.write(n=0).present_address_acks",
          bus->write(bus, env->present_addr, NULL, 0u, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
    check(report, ctx, failures, "i2c.write(n=0).absent_address_not_found",
          bus->write(bus, env->absent_addr, NULL, 0u, CONTRACT_I2C_TIMEOUT_MS) ==
              HAL_ERR_NOT_FOUND);
    check(report, ctx, failures, "i2c.read(n=0).invalid_arg",
          bus->read(bus, env->present_addr, buf, 0u, CONTRACT_I2C_TIMEOUT_MS) ==
              HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "i2c.write_read(wn=0).invalid_arg",
          bus->write_read(bus, env->present_addr, &env->present_reg, 0u, buf, sizeof buf,
                          CONTRACT_I2C_TIMEOUT_MS) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "i2c.write_read(rn=0).invalid_arg",
          bus->write_read(bus, env->present_addr, &env->present_reg, 1u, buf, 0u,
                          CONTRACT_I2C_TIMEOUT_MS) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "i2c.zero_length.bus_still_usable",
          read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
}

static void check_absent_address(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                                 contract_report_fn report, void *ctx, unsigned *failures)
{
    uint8_t buf[CONTRACT_I2C_READ_LEN] = {0};

    uint64_t start = env->clock->now_ms(env->clock);
    const hal_status_t r =
        bus->read(bus, env->absent_addr, buf, sizeof buf, CONTRACT_I2C_TIMEOUT_MS);
    check(report, ctx, failures, "i2c.read.absent_address_not_found_or_io", not_found_or_io(r));
    check(report, ctx, failures, "i2c.read.absent_address_returns_within_deadline",
          within(env, start, CONTRACT_I2C_TIMEOUT_MS));

    start = env->clock->now_ms(env->clock);
    const hal_status_t w =
        bus->write(bus, env->absent_addr, &env->present_reg, 1u, CONTRACT_I2C_TIMEOUT_MS);
    check(report, ctx, failures, "i2c.write.absent_address_not_found_or_io", not_found_or_io(w));
    check(report, ctx, failures, "i2c.write.absent_address_returns_within_deadline",
          within(env, start, CONTRACT_I2C_TIMEOUT_MS));

    start = env->clock->now_ms(env->clock);
    const hal_status_t wr = bus->write_read(bus, env->absent_addr, &env->present_reg, 1u, buf,
                                            sizeof buf, CONTRACT_I2C_TIMEOUT_MS);
    check(report, ctx, failures, "i2c.write_read.absent_address_not_found_or_io",
          not_found_or_io(wr));
    check(report, ctx, failures, "i2c.write_read.absent_address_returns_within_deadline",
          within(env, start, CONTRACT_I2C_TIMEOUT_MS));

    check(report, ctx, failures, "i2c.absent_address.bus_still_usable",
          read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
}

static void check_timeout(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                          contract_report_fn report, void *ctx, unsigned *failures)
{
    check(report, ctx, failures, "i2c.timeout.short_deadline_still_completes",
          read_present_register(bus, env, CONTRACT_I2C_SHORT_TIMEOUT_MS) == HAL_OK);

    const bool stalled = env->stall_bus(env->ctx);
    check(report, ctx, failures, "i2c.timeout.stall_injected", stalled);
    if (stalled) {
        const uint64_t start = env->clock->now_ms(env->clock);
        const hal_status_t s = read_present_register(bus, env, CONTRACT_I2C_STALL_TIMEOUT_MS);
        check(report, ctx, failures, "i2c.timeout.stalled_transfer_reports_timeout",
              s == HAL_ERR_TIMEOUT);
        check(report, ctx, failures, "i2c.timeout.stalled_transfer_returns_within_deadline",
              within(env, start, CONTRACT_I2C_STALL_TIMEOUT_MS));
        env->end_stall(env->ctx);
    }

    // No recover() in between: a slave that stretches too long and then lets
    // go must not leave the bus broken for the next caller.
    check(report, ctx, failures, "i2c.timeout.bus_usable_after_stall_ends",
          read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
}

static void check_recovery(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                           contract_report_fn report, void *ctx, unsigned *failures)
{
    check(report, ctx, failures, "i2c.recover.healthy_bus_ok", bus->recover(bus) == HAL_OK);
    check(report, ctx, failures, "i2c.recover.healthy_bus_still_usable",
          read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);

    // Round one isolates recover(): nothing touches the bus between the hang
    // and the recovery, so a released SDA afterwards is recover()'s doing and
    // not a side effect of some failed transfer's own clocking.
    const bool hung = env->hang_bus(env->ctx) && env->sda_held_low(env->ctx);
    check(report, ctx, failures, "i2c.hang.injected_sda_held_low", hung);
    if (hung) {
        check(report, ctx, failures, "i2c.recover.after_hang_ok", bus->recover(bus) == HAL_OK);
        check(report, ctx, failures, "i2c.recover.after_hang_sda_released",
              !env->sda_held_low(env->ctx));
        check(report, ctx, failures, "i2c.recover.after_hang_bus_usable",
              read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
    }

    // Round two: what a caller sees if it transfers on a hung bus before
    // anyone recovers it -- promptly, never a hang, and never an answer that
    // blames the device instead of the bus. TIMEOUT or IO, depending on where
    // in the transfer the controller notices the stuck line (ports/port_i2c.h);
    // or HAL_OK, if the implementation cleared the bus by itself and the
    // transfer really completed -- which is what adp_i2c does on the board,
    // through the driver's own clear-bus (ADR 0011). NOT_FOUND would send a
    // caller looking for an absent device when recover() is what it needs.
    const bool hung_again = env->hang_bus(env->ctx) && env->sda_held_low(env->ctx);
    check(report, ctx, failures, "i2c.hang.reinjected_sda_held_low", hung_again);
    if (hung_again) {
        const uint64_t start = env->clock->now_ms(env->clock);
        const hal_status_t s = read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS);
        check(report, ctx, failures, "i2c.hang.transfer_reports_timeout_io_or_completes",
              s == HAL_ERR_TIMEOUT || s == HAL_ERR_IO || s == HAL_OK);
        check(report, ctx, failures, "i2c.hang.transfer_returns_within_deadline",
              within(env, start, CONTRACT_I2C_TIMEOUT_MS));
        check(report, ctx, failures, "i2c.recover.after_failed_transfer_ok",
              bus->recover(bus) == HAL_OK);
        check(report, ctx, failures, "i2c.recover.after_failed_transfer_bus_usable",
              read_present_register(bus, env, CONTRACT_I2C_TIMEOUT_MS) == HAL_OK);
    }
}

unsigned port_i2c_contract_run(port_i2c_t *bus, const port_i2c_contract_env_t *env,
                               contract_report_fn report, void *report_ctx)
{
    unsigned failures = 0u;

    check(report, report_ctx, &failures, "i2c.vtable.write", bus->write != NULL);
    check(report, report_ctx, &failures, "i2c.vtable.read", bus->read != NULL);
    check(report, report_ctx, &failures, "i2c.vtable.write_read", bus->write_read != NULL);
    check(report, report_ctx, &failures, "i2c.vtable.recover", bus->recover != NULL);
    if (bus->write == NULL || bus->read == NULL || bus->write_read == NULL ||
        bus->recover == NULL) {
        return failures;  // nothing else can be exercised safely
    }

    check_arguments(bus, env, report, report_ctx, &failures);
    check_present_device(bus, env, report, report_ctx, &failures);
    check_zero_length(bus, env, report, report_ctx, &failures);
    check_absent_address(bus, env, report, report_ctx, &failures);
    check_timeout(bus, env, report, report_ctx, &failures);
    check_recovery(bus, env, report, report_ctx, &failures);

    return failures;
}
