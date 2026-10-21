// SPDX-License-Identifier: Apache-2.0
//
// The PCF8563 model (platform/host/i2c_models/pcf8563_model.h) against the
// datasheet, Rev. 11.1, over the fake bus and its virtual clock: the power-up
// state, a tick a second with the calendar's carries, STOP holding the
// counters and its release ticking 0.507813-0.507935 s later, a write to a
// running clock leaving its phase alone, the crystal's ppm; the alarm setting
// AF only on the increment into its minute, and INT following AF and AIE,
// told with the tick's moment. Raw transfers
// only: the driver under test elsewhere is not used here, so the model is
// held to the datasheet and not to the driver's reading of it.

#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "hac_test.h"
#include "i2c_models/pcf8563_model.h"

#define RTC_ADDR 0x51u
#define TIMEOUT_MS 20u

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    pcf8563_model_t chip;
} rig_t;

static void rig_init(rig_t *r, uint32_t phase_us)
{
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_i2c_fake_init(&r->bus, &r->clock);
    pcf8563_model_init(&r->chip, 0u, phase_us);
    (void)adp_i2c_fake_attach(&r->bus, RTC_ADDR, pcf8563_model_port(&r->chip));
}

static void advance(rig_t *r, uint64_t ms)
{
    adp_clock_virtual_advance(&r->clock, ms);
}

// Registers `from` onwards, `n` of them, as the bus reads them now.
static void read_regs(rig_t *r, uint8_t from, uint8_t *out, size_t n)
{
    port_i2c_t *port = adp_i2c_fake_port(&r->bus);
    HAC_CHECK_EQ_INT(port->write_read(port, RTC_ADDR, &from, 1u, out, n, TIMEOUT_MS), HAL_OK);
}

static uint8_t read_reg(rig_t *r, uint8_t reg)
{
    uint8_t v = 0u;
    read_regs(r, reg, &v, 1u);
    return v;
}

static void write_regs(rig_t *r, const uint8_t *bytes, size_t n)
{
    port_i2c_t *port = adp_i2c_fake_port(&r->bus);
    HAC_CHECK_EQ_INT(port->write(port, RTC_ADDR, bytes, n, TIMEOUT_MS), HAL_OK);
}

// Sets 02h-08h, VL clear: seconds .. years in BCD as given.
static void set_time(rig_t *r, uint8_t s, uint8_t mi, uint8_t h, uint8_t d, uint8_t wd, uint8_t mo,
                     uint8_t y)
{
    const uint8_t w[8] = {0x02u, s, mi, h, d, wd, mo, y};
    write_regs(r, w, sizeof(w));
}

static bool time_is(rig_t *r, uint8_t s, uint8_t mi, uint8_t h, uint8_t d, uint8_t wd, uint8_t mo,
                    uint8_t y)
{
    uint8_t g[7];
    read_regs(r, 0x02u, g, sizeof(g));
    return g[0] == s && g[1] == mi && g[2] == h && g[3] == d && g[4] == wd && g[5] == mo &&
           g[6] == y;
}

// Table 27 and Table 8: Control_status_1 08h, Control_status_2 00h, VL set.
static void power_up_is_the_datasheets(void)
{
    rig_t r;
    rig_init(&r, 300000u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x00u), 0x08);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u), 0x00);
    HAC_CHECK((read_reg(&r, 0x02u) & 0x80u) != 0u);
    HAC_CHECK(pcf8563_model_running(&r.chip));  // TESTC does not stop it
}

// One tick a second, at the phase the power-up gave it.
static void it_ticks_once_a_second(void)
{
    rig_t r;
    rig_init(&r, 300000u);
    HAC_CHECK(time_is(&r, 0x80u, 0x00u, 0x00u, 0x01u, 0x06u, 0x01u, 0x00u));
    advance(&r, 299u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x80);
    advance(&r, 1u);  // 300 ms: the first tick
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x81);
    advance(&r, 999u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x81);
    advance(&r, 1u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x82);
    advance(&r, 8000u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x90);  // BCD: ten seconds
    HAC_CHECK_EQ_U64(r.chip.stats.ticks, 10u);
}

// The carries, each from its last value: a minute, an hour, a day into the
// next month, a common and a leap February, a year, and the century.
static void the_calendar_carries(void)
{
    rig_t r;
    rig_init(&r, 0u);
    advance(&r, 1u);  // past the power-up tick at 0

    // 2026-12-31 23:59:59, a Thursday (4): into 2027-01-01, a Friday.
    set_time(&r, 0x59u, 0x59u, 0x23u, 0x31u, 0x04u, 0x12u, 0x26u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x00u, 0x00u, 0x01u, 0x05u, 0x01u, 0x27u));

    // 2027-02-28 23:59:59: no 29th in 2027.
    set_time(&r, 0x59u, 0x59u, 0x23u, 0x28u, 0x00u, 0x02u, 0x27u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x03u, 0x27u));

    // 2028-02-28 23:59:59: a 29th, and then March.
    set_time(&r, 0x59u, 0x59u, 0x23u, 0x28u, 0x01u, 0x02u, 0x28u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x00u, 0x00u, 0x29u, 0x02u, 0x02u, 0x28u));
    set_time(&r, 0x59u, 0x59u, 0x23u, 0x29u, 0x02u, 0x02u, 0x28u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x00u, 0x00u, 0x01u, 0x03u, 0x03u, 0x28u));

    // 2026-04-30: April has 30 days.
    set_time(&r, 0x59u, 0x59u, 0x23u, 0x30u, 0x04u, 0x04u, 0x26u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x00u, 0x00u, 0x01u, 0x05u, 0x05u, 0x26u));

    // 2099-12-31 23:59:59: the years overflow and the century bit toggles
    // (8.4.7, note [1]); 00 is a leap year to the chip (8.4.4).
    set_time(&r, 0x59u, 0x59u, 0x23u, 0x31u, 0x04u, 0x12u, 0x99u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x00u, 0x00u, 0x01u, 0x05u, 0x81u, 0x00u));

    // A minute and an hour alone.
    set_time(&r, 0x59u, 0x09u, 0x13u, 0x15u, 0x03u, 0x06u, 0x26u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x10u, 0x13u, 0x15u, 0x03u, 0x06u, 0x26u));
    set_time(&r, 0x59u, 0x59u, 0x09u, 0x15u, 0x03u, 0x06u, 0x26u);
    advance(&r, 1000u);
    HAC_CHECK(time_is(&r, 0x00u, 0x00u, 0x10u, 0x15u, 0x03u, 0x06u, 0x26u));
}

// VL stays set through ticks, and goes only when 02h is written without it.
static void vl_stays_until_written(void)
{
    rig_t r;
    rig_init(&r, 500000u);
    advance(&r, 5000u);
    HAC_CHECK((read_reg(&r, 0x02u) & 0x80u) != 0u);
    const uint8_t w[2] = {0x02u, 0x30u};
    write_regs(&r, w, sizeof(w));
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x30);
    advance(&r, 1000u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x31);
}

// STOP holds the counters; released, the first tick comes 0.507813 s to
// 0.507935 s later and one a second after it (8.10, Table 26).
static void stop_holds_and_its_release_ticks_half_a_second_later(void)
{
    rig_t r;
    rig_init(&r, 100000u);
    advance(&r, 50u);
    const uint8_t stop[2] = {0x00u, 0x20u};
    write_regs(&r, stop, sizeof(stop));
    HAC_CHECK(!pcf8563_model_running(&r.chip));
    set_time(&r, 0x00u, 0x00u, 0x12u, 0x25u, 0x00u, 0x10u, 0x26u);
    advance(&r, 10000u);  // ten seconds stopped: nothing counted
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x00);
    HAC_CHECK_EQ_U64(r.chip.stats.stops, 1u);

    const uint8_t start[2] = {0x00u, 0x00u};
    write_regs(&r, start, sizeof(start));
    HAC_CHECK_EQ_U64(r.chip.stats.starts, 1u);
    advance(&r, 507u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x00);  // 507 ms: not yet
    advance(&r, 1u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x01);  // 508 ms: the first tick
    advance(&r, 999u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x01);
    advance(&r, 1u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x02);

    // Writing 0 to a running clock is no release: the phase stays.
    write_regs(&r, start, sizeof(start));
    advance(&r, 999u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x02);
    advance(&r, 1u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x03);
    HAC_CHECK_EQ_U64(r.chip.stats.starts, 1u);
}

// Writing the time of a running clock leaves its prescaler alone: the next
// tick comes where the old phase put it, not a second after the write.
static void a_time_written_keeps_the_phase(void)
{
    rig_t r;
    rig_init(&r, 700000u);  // ticks at x.700
    advance(&r, 1200u);     // 1.2 s: one tick behind it
    set_time(&r, 0x00u, 0x00u, 0x12u, 0x25u, 0x00u, 0x10u, 0x26u);
    advance(&r, 499u);  // 1.699 s
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x00);
    advance(&r, 1u);  // 1.700 s: the old phase's tick
    HAC_CHECK_EQ_INT(read_reg(&r, 0x02u), 0x01);
    HAC_CHECK_EQ_U64(r.chip.stats.time_writes, 1u);
}

// A crystal `ppm` fast gains that many microseconds a second: after an hour
// at +20 ppm, the 3600th tick comes 72 ms early; at -50 ppm, 180 ms late.
static void the_crystal_runs_fast_or_slow(void)
{
    rig_t r;
    rig_init(&r, 0u);
    r.chip.ppm = 20;
    advance(&r, 1u);  // the tick at 0
    set_time(&r, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x26u);
    // Ticks at 1 s - 20 us, 2 s - 40 us, ...: the 3600th at 3600 s - 72 ms.
    advance(&r, 3600000u - 72u - 2u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x03u), 0x59);  // 00:59:59 still
    advance(&r, 2u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x04u), 0x01);  // 01:00:00
    HAC_CHECK_EQ_INT(read_reg(&r, 0x03u), 0x00);

    rig_t s;
    rig_init(&s, 0u);
    s.chip.ppm = -50;
    advance(&s, 1u);
    set_time(&s, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x26u);
    advance(&s, 3600000u + 180u - 2u);
    HAC_CHECK_EQ_INT(read_reg(&s, 0x04u), 0x00);
    advance(&s, 2u);
    HAC_CHECK_EQ_INT(read_reg(&s, 0x04u), 0x01);
}

// The address increments after every byte, and wraps from 0Fh to 00h
// (ASSUMED in the model's header); the alarm and timer registers store what
// is written.
static void the_pointer_increments_and_wraps(void)
{
    rig_t r;
    rig_init(&r, 0u);
    const uint8_t w[4] = {0x0Eu, 0x83u, 0x42u, 0x08u};  // 0Eh, 0Fh, then 00h
    write_regs(&r, w, sizeof(w));
    uint8_t g[3];
    read_regs(&r, 0x0Eu, g, sizeof(g));
    HAC_CHECK_EQ_INT(g[0], 0x83);
    HAC_CHECK_EQ_INT(g[1], 0x42);
    HAC_CHECK_EQ_INT(g[2], 0x08);
}

typedef struct {
    unsigned falls;
    uint64_t at_us;
} int_seen_t;

static void on_int(void *ctx, uint64_t at_us)
{
    int_seen_t *seen = (int_seen_t *)ctx;
    seen->falls++;
    seen->at_us = at_us;
}

// The minute alarm alone (8.6.5): AF on the increment into its minute, at
// that tick, and INT low with it while AIE is set -- told with the tick's
// moment, however late the bus looks.
static void the_alarm_falls_on_the_tick_into_its_minute(void)
{
    rig_t r;
    rig_init(&r, 250000u);  // ticks at x.250
    int_seen_t seen = {0u, 0u};
    pcf8563_model_on_int(&r.chip, on_int, &seen);
    advance(&r, 1u);
    set_time(&r, 0x50u, 0x07u, 0x12u, 0x25u, 0x00u, 0x10u, 0x26u);  // 12:07:50
    const uint8_t alarm[5] = {0x09u, 0x08u, 0x80u, 0x80u, 0x80u};   // minute 08 only
    write_regs(&r, alarm, sizeof(alarm));
    const uint8_t aie[2] = {0x01u, 0x02u};
    write_regs(&r, aie, sizeof(aie));
    advance(&r, 8000u);  // 12:07:58
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u), 0x02);
    HAC_CHECK_EQ_U64(seen.falls, 0u);
    advance(&r, 30000u);                          // the bus looks 28 s after the tick
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u), 0x0A);  // AF and AIE
    HAC_CHECK(r.chip.int_low);
    HAC_CHECK_EQ_U64(seen.falls, 1u);
    // The tenth tick after the write at 1 ms took 12:07:59 into 12:08:00, at
    // 9.250 s on the bus's clock.
    HAC_CHECK_EQ_U64(seen.at_us, 9250000u);

    // AF stays until cleared, and INT with it; clearing lets INT go.
    advance(&r, 60000u);
    HAC_CHECK(r.chip.int_low);
    write_regs(&r, aie, sizeof(aie));  // AF written 0
    HAC_CHECK(!r.chip.int_low);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u), 0x02);
    HAC_CHECK_EQ_U64(r.chip.stats.alarms, 1u);
}

// A write that makes the alarm match sets nothing: only an increment into
// the minute does -- an hour later.
static void a_write_into_the_alarms_minute_sets_nothing(void)
{
    rig_t r;
    rig_init(&r, 0u);
    advance(&r, 1u);
    set_time(&r, 0x10u, 0x08u, 0x12u, 0x25u, 0x00u, 0x10u, 0x26u);  // 12:08:10
    const uint8_t alarm[5] = {0x09u, 0x08u, 0x80u, 0x80u, 0x80u};
    write_regs(&r, alarm, sizeof(alarm));
    const uint8_t aie[2] = {0x01u, 0x02u};
    write_regs(&r, aie, sizeof(aie));
    advance(&r, 120000u);  // into 12:10
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u) & 0x08u, 0);
    HAC_CHECK_EQ_U64(r.chip.stats.alarms, 0u);
    advance(&r, 3600000u);  // through 13:08:00
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u) & 0x08u, 0x08);
    HAC_CHECK_EQ_U64(r.chip.stats.alarms, 1u);
}

// Without AIE the flag sets and INT stays high; with every alarm off
// nothing sets; writing AF 1 leaves it; bits 7-5 of 01h read 0.
static void aie_gates_int_and_af_clears_only_by_a_zero(void)
{
    rig_t r;
    rig_init(&r, 0u);
    int_seen_t seen = {0u, 0u};
    pcf8563_model_on_int(&r.chip, on_int, &seen);
    advance(&r, 1u);
    set_time(&r, 0x59u, 0x07u, 0x12u, 0x25u, 0x00u, 0x10u, 0x26u);
    const uint8_t alarm[5] = {0x09u, 0x08u, 0x80u, 0x80u, 0x80u};
    write_regs(&r, alarm, sizeof(alarm));
    advance(&r, 1000u);
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u), 0x08);
    HAC_CHECK(!r.chip.int_low);
    HAC_CHECK_EQ_U64(seen.falls, 0u);

    const uint8_t keep[2] = {0x01u, 0xE8u};  // AF written 1, bits 7-5 written 1
    write_regs(&r, keep, sizeof(keep));
    HAC_CHECK_EQ_INT(read_reg(&r, 0x01u), 0x08);
    const uint8_t aie[2] = {0x01u, 0x0Au};  // AIE on, AF left: INT falls now
    write_regs(&r, aie, sizeof(aie));
    HAC_CHECK(r.chip.int_low);
    HAC_CHECK_EQ_U64(seen.falls, 1u);

    rig_t q;
    rig_init(&q, 0u);
    advance(&q, 3600000u);  // power-up's alarms are all off: a whole hour, nothing
    HAC_CHECK_EQ_U64(q.chip.stats.alarms, 0u);
}

int main(void)
{
    HAC_RUN(power_up_is_the_datasheets);
    HAC_RUN(it_ticks_once_a_second);
    HAC_RUN(the_calendar_carries);
    HAC_RUN(vl_stays_until_written);
    HAC_RUN(stop_holds_and_its_release_ticks_half_a_second_later);
    HAC_RUN(a_time_written_keeps_the_phase);
    HAC_RUN(the_crystal_runs_fast_or_slow);
    HAC_RUN(the_pointer_increments_and_wraps);
    HAC_RUN(the_alarm_falls_on_the_tick_into_its_minute);
    HAC_RUN(a_write_into_the_alarms_minute_sets_nothing);
    HAC_RUN(aie_gates_int_and_af_clears_only_by_a_zero);
    return hac_test_summary();
}
