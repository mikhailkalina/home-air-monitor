// SPDX-License-Identifier: Apache-2.0
//
// apps/firmware_esp32/main/wall_clock.c against the host's fake bus,
// compiled from where the firmware compiles it (see tests/CMakeLists.txt):
// which RTC readings set the system time at boot, the reason the console
// gives for each that does not, and time_set's write; since phase 4f the
// years an SNTP answer is believed in and the conversion to the chip's time
// (the RTC against the network is tests/unit/test_rtc_sync.c's).

#include "wall_clock.h"

#include <string.h>

#include "adp_i2c_fake.h"
#include "hac_test.h"

#define RTC_ADDR 0x51u

// 2026-01-01T00:00:00Z, the board's floor (ADP_CLOCK_WALL_EARLIEST_MS).
#define EARLIEST_MS 1767225600000u

// 2026-10-25T01:00:00Z.
#define SOME_TIME_MS 1792890000000u

typedef struct {
    adp_i2c_fake_t bus;
    adp_i2c_fake_device_t *chip;
} rig_t;

static void rig_init(rig_t *r, const uint8_t regs[PCF8563_READ_LEN])
{
    adp_i2c_fake_init(&r->bus, NULL);
    r->chip = adp_i2c_fake_add_device(&r->bus, RTC_ADDR);
    if (r->chip != NULL && regs != NULL) {
        memcpy(r->chip->regs, regs, PCF8563_READ_LEN);
    }
}

static wall_clock_rtc_t read_rtc(rig_t *r)
{
    wall_clock_rtc_t out;
    wall_clock_read_rtc(adp_i2c_fake_port(&r->bus), RTC_ADDR, EARLIEST_MS, &out);
    return out;
}

static void a_valid_time_sets_the_clock(void)
{
    const uint8_t regs[PCF8563_READ_LEN] = {0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
                                            0x25u, 0x00u, 0x10u, 0x26u};
    rig_t r;
    rig_init(&r, regs);
    const wall_clock_rtc_t rtc = read_rtc(&r);
    HAC_CHECK(rtc.usable);
    HAC_CHECK_EQ_U64(rtc.unix_ms, SOME_TIME_MS);
    HAC_CHECK(strcmp(wall_clock_rtc_reason(&rtc), "rtc: valid") == 0);
}

// Every way the RTC gives no time, and the reason the console prints for it.
static void each_unusable_reading_says_why(void)
{
    struct {
        uint8_t regs[PCF8563_READ_LEN];
        const char *reason;
        bool decoded;
    } cases[] = {
        {{0x00u, 0x00u, 0x80u, 0x00u, 0x01u, 0x25u, 0x00u, 0x10u, 0x26u}, "rtc: VL set", true},
        {{0x20u, 0x00u, 0x00u, 0x00u, 0x01u, 0x25u, 0x00u, 0x10u, 0x26u}, "rtc: stopped", true},
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x32u, 0x00u, 0x10u, 0x26u},
         "rtc: undecodable",
         false},
        // 2025-12-31T23:59:59Z: valid on the chip, before anything this
        // firmware could have set -- and 2000-01-01, a chip's typical
        // default.
        {{0x00u, 0x00u, 0x59u, 0x59u, 0x23u, 0x31u, 0x03u, 0x12u, 0x25u},
         "rtc: implausible year",
         true},
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x06u, 0x01u, 0x00u},
         "rtc: implausible year",
         true},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        rig_t r;
        rig_init(&r, cases[i].regs);
        const wall_clock_rtc_t rtc = read_rtc(&r);
        HAC_CHECK(!rtc.usable);
        HAC_CHECK(rtc.decoded == cases[i].decoded);
        HAC_CHECK(strcmp(wall_clock_rtc_reason(&rtc), cases[i].reason) == 0);
    }

    // The first second of 2026 is believable.
    const uint8_t first[PCF8563_READ_LEN] = {0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
                                             0x01u, 0x04u, 0x01u, 0x26u};
    rig_t r;
    rig_init(&r, first);
    const wall_clock_rtc_t rtc = read_rtc(&r);
    HAC_CHECK(rtc.usable);
    HAC_CHECK_EQ_U64(rtc.unix_ms, EARLIEST_MS);
}

static void an_rtc_not_answering_says_so(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    wall_clock_rtc_t rtc;
    wall_clock_read_rtc(adp_i2c_fake_port(&bus), RTC_ADDR, EARLIEST_MS, &rtc);
    HAC_CHECK(!rtc.usable);
    HAC_CHECK(!rtc.decoded);
    HAC_CHECK(strcmp(wall_clock_rtc_reason(&rtc), "rtc: not answering") == 0);
}

// time_set: the time written reads back usable, VL cleared, to the second.
static void a_time_written_reads_back(void)
{
    const uint8_t lost[PCF8563_READ_LEN] = {0x08u, 0x00u, 0x80u, 0x00u, 0x00u,
                                            0x01u, 0x06u, 0x01u, 0x00u};
    rig_t r;
    rig_init(&r, lost);
    HAC_CHECK(!read_rtc(&r).usable);

    HAC_CHECK_EQ_INT(wall_clock_write_rtc(adp_i2c_fake_port(&r.bus), RTC_ADDR, SOME_TIME_MS + 999u),
                     HAL_OK);
    const wall_clock_rtc_t rtc = read_rtc(&r);
    HAC_CHECK(rtc.usable);
    HAC_CHECK_EQ_U64(rtc.unix_ms, SOME_TIME_MS);  // the milliseconds are dropped
    HAC_CHECK_EQ_INT(r.chip->regs[6], 0);         // a Sunday
}

static void a_time_outside_the_chips_century_is_not_written(void)
{
    rig_t r;
    rig_init(&r, NULL);
    // 2100-01-01T00:00:00Z.
    HAC_CHECK_EQ_INT(wall_clock_write_rtc(adp_i2c_fake_port(&r.bus), RTC_ADDR, 4102444800000u),
                     HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(wall_clock_write_rtc(adp_i2c_fake_port(&r.bus), RTC_ADDR, 0u),
                     HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_U64(r.bus.transfers, 0u);
}

static void sources_have_names(void)
{
    HAC_CHECK(strcmp(wall_clock_source_name(WALL_CLOCK_SOURCE_NONE), "none") == 0);
    HAC_CHECK(strcmp(wall_clock_source_name(WALL_CLOCK_SOURCE_RTC), "rtc") == 0);
    HAC_CHECK(strcmp(wall_clock_source_name(WALL_CLOCK_SOURCE_CONSOLE), "console") == 0);
    HAC_CHECK(strcmp(wall_clock_source_name(WALL_CLOCK_SOURCE_KEPT),
                     "kept across a software reset") == 0);
    HAC_CHECK(strcmp(wall_clock_source_name(WALL_CLOCK_SOURCE_SNTP), "sntp") == 0);
}

// --- Phase 4f ------------------------------------------------------------------

// The chip's time of an instant: to the second, and nothing outside its
// century.
static void a_time_becomes_the_chips(void)
{
    pcf8563_time_t t;
    memset(&t, 0xAA, sizeof(t));
    HAC_CHECK(wall_clock_chip_time(SOME_TIME_MS + 999u, &t));
    HAC_CHECK(t.year == 2026u && t.month == 10u && t.day == 25u && t.hour == 1u && t.minute == 0u &&
              t.second == 0u);
    HAC_CHECK_EQ_INT(t.weekday, 0);  // a Sunday
    pcf8563_time_t untouched;
    memset(&untouched, 0x55, sizeof(untouched));
    HAC_CHECK(!wall_clock_chip_time(WALL_CLOCK_LATEST_MS, &untouched));
    HAC_CHECK_EQ_INT(untouched.year, 0x5555);
    HAC_CHECK(!wall_clock_chip_time(0u, &untouched));
}

static void believable_times_are_the_firmwares_years(void)
{
    HAC_CHECK(wall_clock_believable_ms(EARLIEST_MS, EARLIEST_MS));
    HAC_CHECK(!wall_clock_believable_ms(EARLIEST_MS - 1u, EARLIEST_MS));
    HAC_CHECK(wall_clock_believable_ms(WALL_CLOCK_LATEST_MS - 1u, EARLIEST_MS));
    HAC_CHECK(!wall_clock_believable_ms(WALL_CLOCK_LATEST_MS, EARLIEST_MS));
    HAC_CHECK(!wall_clock_believable_ms(0u, EARLIEST_MS));
    HAC_CHECK(wall_clock_believable_ms(SOME_TIME_MS, EARLIEST_MS));
}

int main(void)
{
    HAC_RUN(a_valid_time_sets_the_clock);
    HAC_RUN(each_unusable_reading_says_why);
    HAC_RUN(an_rtc_not_answering_says_so);
    HAC_RUN(a_time_written_reads_back);
    HAC_RUN(a_time_outside_the_chips_century_is_not_written);
    HAC_RUN(sources_have_names);
    HAC_RUN(a_time_becomes_the_chips);
    HAC_RUN(believable_times_are_the_firmwares_years);
    return hac_test_summary();
}
