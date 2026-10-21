// SPDX-License-Identifier: Apache-2.0
//
// The PCF8563 driver against the fake bus, whose register file is the chip
// as it is (drivers/pcf8563/pcf8563.h). Every BCD field at its last valid
// value and its first invalid one, the leap day the chip's own rule allows,
// VL, STOP, garbage, an absent chip -- and a time written, then read back;
// and the stop, the time's write and the start that set a clock to the
// millisecond (phase 4f), each the one register write it is meant to be.

#include <string.h>

#include "adp_i2c_fake.h"
#include "hac_test.h"
#include "pcf8563.h"

// The board's address, for realism; nothing depends on it.
#define RTC_ADDR 0x51u

// 2026-10-25T01:00:00Z, a Sunday: VL clear, running.
static const uint8_t k_valid[PCF8563_READ_LEN] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x25u, 0x00u, 0x10u, 0x26u,
};

static pcf8563_reading_t decode(const uint8_t regs[PCF8563_READ_LEN])
{
    pcf8563_reading_t r;
    pcf8563_decode(regs, &r);
    return r;
}

// k_valid with one register replaced.
static pcf8563_reading_t decode_with(size_t reg, uint8_t value)
{
    uint8_t regs[PCF8563_READ_LEN];
    memcpy(regs, k_valid, sizeof(regs));
    regs[reg] = value;
    return decode(regs);
}

static bool time_is(const pcf8563_time_t *t, uint16_t y, uint8_t mo, uint8_t d, uint8_t h,
                    uint8_t mi, uint8_t s)
{
    return t->year == y && t->month == mo && t->day == d && t->hour == h && t->minute == mi &&
           t->second == s;
}

// --- decoding ----------------------------------------------------------------

static void a_valid_time_decodes(void)
{
    const pcf8563_reading_t r = decode(k_valid);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);
    HAC_CHECK(r.decoded);
    HAC_CHECK(time_is(&r.time, 2026u, 10u, 25u, 1u, 0u, 0u));
    HAC_CHECK_EQ_INT(r.time.weekday, 0);
    HAC_CHECK_EQ_INT(r.bus, HAL_OK);
    HAC_CHECK(memcmp(r.regs, k_valid, sizeof(k_valid)) == 0);
}

static void each_field_at_its_last_valid_value_decodes(void)
{
    // 2099-12-31T23:59:59Z, the latest time the chip can hold.
    const uint8_t latest[PCF8563_READ_LEN] = {0x00u, 0x00u, 0x59u, 0x59u, 0x23u,
                                              0x31u, 0x04u, 0x12u, 0x99u};
    pcf8563_reading_t r = decode(latest);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);
    HAC_CHECK(time_is(&r.time, 2099u, 12u, 31u, 23u, 59u, 59u));

    // 2000-01-01T00:00:00Z, the earliest.
    const uint8_t earliest[PCF8563_READ_LEN] = {0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
                                                0x01u, 0x06u, 0x01u, 0x00u};
    r = decode(earliest);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);
    HAC_CHECK(time_is(&r.time, 2000u, 1u, 1u, 0u, 0u, 0u));
}

// One past the last valid value -- the next BCD number -- and a nibble above
// 9, in every field.
static void each_field_past_its_range_or_not_bcd_is_undecodable(void)
{
    const struct {
        size_t reg;
        uint8_t value;
    } cases[] = {
        {2u, 0x60u}, {2u, 0x5Au}, {2u, 0x0Au},  // seconds
        {3u, 0x60u}, {3u, 0x4Fu},               // minutes
        {4u, 0x24u}, {4u, 0x1Au},               // hours
        {5u, 0x00u}, {5u, 0x32u}, {5u, 0x2Bu},  // days; October has 31
        {7u, 0x00u}, {7u, 0x13u}, {7u, 0x0Au},  // months
        {8u, 0x9Au}, {8u, 0xA0u}, {8u, 0xFFu},  // years
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const pcf8563_reading_t r = decode_with(cases[i].reg, cases[i].value);
        HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_UNDECODABLE);
        HAC_CHECK(!r.decoded);
    }
}

// A day its month does not have: the 31st of April, the 29th of February
// outside a leap year. The 29th in 2024 and in 2000 is real.
static void the_day_must_exist_in_its_month(void)
{
    uint8_t regs[PCF8563_READ_LEN];
    memcpy(regs, k_valid, sizeof(regs));

    regs[7] = 0x04u;  // April
    regs[5] = 0x31u;
    HAC_CHECK_EQ_INT(decode(regs).verdict, PCF8563_TIME_UNDECODABLE);
    regs[5] = 0x30u;
    HAC_CHECK_EQ_INT(decode(regs).verdict, PCF8563_TIME_VALID);

    regs[7] = 0x02u;  // February 2026
    regs[5] = 0x29u;
    HAC_CHECK_EQ_INT(decode(regs).verdict, PCF8563_TIME_UNDECODABLE);
    regs[8] = 0x24u;  // 2024
    HAC_CHECK_EQ_INT(decode(regs).verdict, PCF8563_TIME_VALID);
    regs[8] = 0x00u;  // 2000, divisible by 400: the chip's rule and the calendar agree
    HAC_CHECK_EQ_INT(decode(regs).verdict, PCF8563_TIME_VALID);
    regs[5] = 0x30u;
    HAC_CHECK_EQ_INT(decode(regs).verdict, PCF8563_TIME_UNDECODABLE);
}

// Unused bits, the century bit and the weekday do not decide anything.
static void bits_that_carry_no_time_are_ignored(void)
{
    uint8_t regs[PCF8563_READ_LEN];
    memcpy(regs, k_valid, sizeof(regs));
    regs[3] |= 0x80u;  // minutes, bit 7
    regs[4] |= 0xC0u;  // hours, bits 7-6
    regs[5] |= 0xC0u;  // days, bits 7-6
    regs[6] = 0xFFu;   // weekdays: 7 after the mask, a value this driver never writes
    regs[7] |= 0xE0u;  // century bit and months' bits 6-5
    const pcf8563_reading_t r = decode(regs);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);
    HAC_CHECK(time_is(&r.time, 2026u, 10u, 25u, 1u, 0u, 0u));  // 2026, not 1926
    HAC_CHECK_EQ_INT(r.time.weekday, 7);
}

static void vl_set_means_no_time_but_still_decodes(void)
{
    const pcf8563_reading_t r = decode_with(2u, 0x80u);  // VL, 00 seconds
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VL_SET);
    HAC_CHECK(r.decoded);  // what the registers said, for the log
    HAC_CHECK(time_is(&r.time, 2026u, 10u, 25u, 1u, 0u, 0u));
    HAC_CHECK(strcmp(pcf8563_verdict_name(r.verdict), "VL set") == 0);
}

static void a_stopped_clock_has_no_time(void)
{
    const pcf8563_reading_t r = decode_with(0u, 0x20u);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_STOPPED);
    HAC_CHECK(r.decoded);
    // The power-on value of Control_status_1, TESTC set, is still running.
    HAC_CHECK_EQ_INT(decode_with(0u, 0x08u).verdict, PCF8563_TIME_VALID);
}

// Garbage: VL is what is said first when it is set, since it explains the
// rest; without it, the registers do not decode.
static void garbage_is_never_a_time(void)
{
    uint8_t regs[PCF8563_READ_LEN];
    memset(regs, 0xFF, sizeof(regs));
    pcf8563_reading_t r = decode(regs);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VL_SET);
    HAC_CHECK(!r.decoded);

    regs[0] = 0x00u;
    regs[2] = 0x7Fu;
    r = decode(regs);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_UNDECODABLE);
    HAC_CHECK(!r.decoded);

    memset(regs, 0x00, sizeof(regs));  // day 0, month 0
    HAC_CHECK_EQ_INT(decode(regs).verdict, PCF8563_TIME_UNDECODABLE);
}

// --- over the bus --------------------------------------------------------------

static void an_absent_chip_is_not_answering(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    pcf8563_reading_t r;
    pcf8563_read(adp_i2c_fake_port(&bus), RTC_ADDR, &r);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_NOT_ANSWERING);
    HAC_CHECK_EQ_INT(r.bus, HAL_ERR_NOT_FOUND);
    HAC_CHECK(!r.decoded);
    HAC_CHECK(strcmp(pcf8563_verdict_name(r.verdict), "not answering") == 0);
}

static void a_read_takes_registers_zero_to_eight_in_one_transfer(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    adp_i2c_fake_device_t *chip = adp_i2c_fake_add_device(&bus, RTC_ADDR);
    HAC_CHECK(chip != NULL);
    if (chip == NULL) {
        return;
    }
    memcpy(chip->regs, k_valid, sizeof(k_valid));
    chip->regs[9] = 0xEEu;  // an alarm register, never read

    pcf8563_reading_t r;
    pcf8563_read(adp_i2c_fake_port(&bus), RTC_ADDR, &r);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);
    HAC_CHECK(time_is(&r.time, 2026u, 10u, 25u, 1u, 0u, 0u));
    HAC_CHECK_EQ_U64(bus.transfers, 1u);
}

static void a_time_written_reads_back_and_clears_vl_and_stop(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    adp_i2c_fake_device_t *chip = adp_i2c_fake_add_device(&bus, RTC_ADDR);
    HAC_CHECK(chip != NULL);
    if (chip == NULL) {
        return;
    }
    // A chip whose cell ran out, and which some earlier firmware stopped.
    memset(chip->regs, 0xFF, sizeof(chip->regs));
    chip->regs[0] = 0x28u;  // STOP and TESTC
    chip->regs[1] = 0x12u;  // Control_status_2: not this driver's to touch

    const pcf8563_time_t t = {2026u, 10u, 25u, 1u, 0u, 0u, 0u};
    HAC_CHECK_EQ_INT(pcf8563_set(adp_i2c_fake_port(&bus), RTC_ADDR, &t), HAL_OK);

    const uint8_t expected[PCF8563_READ_LEN] = {0x00u, 0x12u, 0x00u, 0x00u, 0x01u,
                                                0x25u, 0x00u, 0x10u, 0x26u};
    HAC_CHECK(memcmp(chip->regs, expected, sizeof(expected)) == 0);

    pcf8563_reading_t r;
    pcf8563_read(adp_i2c_fake_port(&bus), RTC_ADDR, &r);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);
    HAC_CHECK(time_is(&r.time, 2026u, 10u, 25u, 1u, 0u, 0u));

    // The two ends of the range, and a leap day, round trip too.
    const pcf8563_time_t others[] = {
        {2099u, 12u, 31u, 23u, 59u, 59u, 4u},
        {2000u, 1u, 1u, 0u, 0u, 0u, 6u},
        {2028u, 2u, 29u, 12u, 34u, 56u, 2u},
    };
    for (size_t i = 0u; i < sizeof(others) / sizeof(others[0]); ++i) {
        const pcf8563_time_t *o = &others[i];
        HAC_CHECK_EQ_INT(pcf8563_set(adp_i2c_fake_port(&bus), RTC_ADDR, o), HAL_OK);
        pcf8563_read(adp_i2c_fake_port(&bus), RTC_ADDR, &r);
        HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);
        HAC_CHECK(time_is(&r.time, o->year, o->month, o->day, o->hour, o->minute, o->second));
        HAC_CHECK_EQ_INT(r.time.weekday, o->weekday);
    }
}

static void a_time_out_of_range_is_not_written(void)
{
    const pcf8563_time_t bad[] = {
        {1999u, 12u, 31u, 23u, 59u, 59u, 5u},  // before the chip's century
        {2100u, 1u, 1u, 0u, 0u, 0u, 5u},       // after it
        {2026u, 2u, 29u, 0u, 0u, 0u, 0u},      // not a leap year
        {2026u, 13u, 1u, 0u, 0u, 0u, 0u},     {2026u, 1u, 0u, 0u, 0u, 0u, 0u},
        {2026u, 1u, 1u, 24u, 0u, 0u, 0u},     {2026u, 1u, 1u, 0u, 60u, 0u, 0u},
        {2026u, 1u, 1u, 0u, 0u, 60u, 0u},     {2026u, 1u, 1u, 0u, 0u, 0u, 7u},  // no eighth weekday
    };
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    (void)adp_i2c_fake_add_device(&bus, RTC_ADDR);
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        uint8_t out[PCF8563_TIME_LEN] = {0xAAu};
        HAC_CHECK(!pcf8563_encode(&bad[i], out));
        HAC_CHECK_EQ_INT(out[0], 0xAA);  // untouched
        HAC_CHECK_EQ_INT(pcf8563_set(adp_i2c_fake_port(&bus), RTC_ADDR, &bad[i]),
                         HAL_ERR_INVALID_ARG);
    }
    HAC_CHECK_EQ_U64(bus.transfers, 0u);  // nothing reached the bus
}

static void a_set_on_a_broken_bus_says_so(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    const pcf8563_time_t t = {2026u, 10u, 25u, 1u, 0u, 0u, 0u};
    HAC_CHECK_EQ_INT(pcf8563_set(adp_i2c_fake_port(&bus), RTC_ADDR, &t), HAL_ERR_NOT_FOUND);

    (void)adp_i2c_fake_add_device(&bus, RTC_ADDR);
    adp_i2c_fake_hang(&bus, 1u);
    HAC_CHECK_EQ_INT(pcf8563_set(adp_i2c_fake_port(&bus), RTC_ADDR, &t), HAL_ERR_TIMEOUT);
    pcf8563_reading_t r;
    pcf8563_read(adp_i2c_fake_port(&bus), RTC_ADDR, &r);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_NOT_ANSWERING);
    HAC_CHECK_EQ_INT(r.bus, HAL_ERR_TIMEOUT);
}

// pcf8563_stop(), pcf8563_write_time(), pcf8563_start(): each one transfer,
// touching only its own registers -- Control_status_2 and the alarms never.
static void stop_write_and_start_are_one_transfer_each(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    adp_i2c_fake_device_t *chip = adp_i2c_fake_add_device(&bus, RTC_ADDR);
    HAC_CHECK(chip != NULL);
    if (chip == NULL) {
        return;
    }
    memset(chip->regs, 0xFF, sizeof(chip->regs));
    chip->regs[0] = 0x08u;  // TESTC, as at power-up
    chip->regs[1] = 0x12u;
    port_i2c_t *port = adp_i2c_fake_port(&bus);

    HAC_CHECK_EQ_INT(pcf8563_stop(port, RTC_ADDR), HAL_OK);
    HAC_CHECK_EQ_INT(chip->regs[0], 0x20);  // STOP alone
    HAC_CHECK_EQ_U64(bus.transfers, 1u);

    const pcf8563_time_t t = {2026u, 10u, 25u, 1u, 0u, 0u, 0u};
    HAC_CHECK_EQ_INT(pcf8563_write_time(port, RTC_ADDR, &t), HAL_OK);
    HAC_CHECK_EQ_U64(bus.transfers, 2u);
    const uint8_t stopped[PCF8563_READ_LEN] = {0x20u, 0x12u, 0x00u, 0x00u, 0x01u,
                                               0x25u, 0x00u, 0x10u, 0x26u};
    HAC_CHECK(memcmp(chip->regs, stopped, sizeof(stopped)) == 0);  // still stopped
    HAC_CHECK_EQ_INT(chip->regs[9], 0xFF);                         // the alarms untouched

    pcf8563_reading_t r;
    pcf8563_read(port, RTC_ADDR, &r);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_STOPPED);
    HAC_CHECK(time_is(&r.time, 2026u, 10u, 25u, 1u, 0u, 0u));

    HAC_CHECK_EQ_INT(pcf8563_start(port, RTC_ADDR), HAL_OK);
    HAC_CHECK_EQ_INT(chip->regs[0], 0x00);
    HAC_CHECK_EQ_INT(chip->regs[1], 0x12);
    pcf8563_read(port, RTC_ADDR, &r);
    HAC_CHECK_EQ_INT(r.verdict, PCF8563_TIME_VALID);

    // A time out of range is refused before the bus.
    const uint32_t before = bus.transfers;
    const pcf8563_time_t bad = {2100u, 1u, 1u, 0u, 0u, 0u, 5u};
    HAC_CHECK_EQ_INT(pcf8563_write_time(port, RTC_ADDR, &bad), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_U64(bus.transfers, before);
}

// The alarm armed for a minute: 09h-0Ch the minute alone, then
// Control_status_2 to AIE, AF and TF cleared; taken back: Control_status_2 to
// 0, the minute alarm off. The time registers untouched either way.
static void the_alarm_is_armed_and_taken_back(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    adp_i2c_fake_device_t *chip = adp_i2c_fake_add_device(&bus, RTC_ADDR);
    HAC_CHECK(chip != NULL);
    if (chip == NULL) {
        return;
    }
    memcpy(chip->regs, k_valid, sizeof(k_valid));
    chip->regs[1] = 0x0Du;  // AF, TF and TIE set by someone
    port_i2c_t *port = adp_i2c_fake_port(&bus);

    HAC_CHECK_EQ_INT(pcf8563_alarm_at_minute(port, RTC_ADDR, 59u), HAL_OK);
    HAC_CHECK_EQ_U64(bus.transfers, 2u);
    HAC_CHECK_EQ_INT(chip->regs[9], 0x59);
    HAC_CHECK_EQ_INT(chip->regs[10], 0x80);
    HAC_CHECK_EQ_INT(chip->regs[11], 0x80);
    HAC_CHECK_EQ_INT(chip->regs[12], 0x80);
    HAC_CHECK_EQ_INT(chip->regs[1], 0x02);
    HAC_CHECK(memcmp(&chip->regs[2], &k_valid[2], PCF8563_TIME_LEN) == 0);

    HAC_CHECK_EQ_INT(pcf8563_alarm_off(port, RTC_ADDR), HAL_OK);
    HAC_CHECK_EQ_INT(chip->regs[1], 0x00);
    HAC_CHECK_EQ_INT(chip->regs[9] & 0x80, 0x80);
    HAC_CHECK(memcmp(&chip->regs[2], &k_valid[2], PCF8563_TIME_LEN) == 0);

    const uint32_t before = bus.transfers;
    HAC_CHECK_EQ_INT(pcf8563_alarm_at_minute(port, RTC_ADDR, 60u), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_U64(bus.transfers, before);
}

// The datasheet's figure for the first tick after STOP, as the driver keeps
// it: within 0.507813 s to 0.507935 s (Rev. 11.1, 8.10).
static void the_first_tick_after_a_start_is_the_datasheets(void)
{
    HAC_CHECK(PCF8563_FIRST_TICK_AFTER_START_US >= 507813u);
    HAC_CHECK(PCF8563_FIRST_TICK_AFTER_START_US <= 507935u);
}

int main(void)
{
    HAC_RUN(a_valid_time_decodes);
    HAC_RUN(each_field_at_its_last_valid_value_decodes);
    HAC_RUN(each_field_past_its_range_or_not_bcd_is_undecodable);
    HAC_RUN(the_day_must_exist_in_its_month);
    HAC_RUN(bits_that_carry_no_time_are_ignored);
    HAC_RUN(vl_set_means_no_time_but_still_decodes);
    HAC_RUN(a_stopped_clock_has_no_time);
    HAC_RUN(garbage_is_never_a_time);
    HAC_RUN(an_absent_chip_is_not_answering);
    HAC_RUN(a_read_takes_registers_zero_to_eight_in_one_transfer);
    HAC_RUN(a_time_written_reads_back_and_clears_vl_and_stop);
    HAC_RUN(a_time_out_of_range_is_not_written);
    HAC_RUN(a_set_on_a_broken_bus_says_so);
    HAC_RUN(stop_write_and_start_are_one_transfer_each);
    HAC_RUN(the_first_tick_after_a_start_is_the_datasheets);
    HAC_RUN(the_alarm_is_armed_and_taken_back);
    return hac_test_summary();
}
