// SPDX-License-Identifier: Apache-2.0
//
// The GT911 model against Goodix's documents, through raw transfers on the
// fake bus -- no driver involved -- so that the driver's tests
// (test_gt911.c) stand on a model that is itself checked
// (platform/host/i2c_models/gt911_model.h).

#include <stdint.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "hac_test.h"
#include "i2c_models/gt911_model.h"

#define ADDR 0x5Du
#define TO_MS 50u

static adp_clock_virtual_t g_clock;
static adp_i2c_fake_t g_bus;
static gt911_model_t g_model;
static unsigned g_interrupts;

static void on_interrupt(void *ctx)
{
    (void)ctx;
    g_interrupts++;
}

static void setup(void)
{
    adp_clock_virtual_init(&g_clock, 1.0);
    adp_clock_virtual_advance(&g_clock, 1000u);
    adp_i2c_fake_init(&g_bus, &g_clock);
    gt911_model_init(&g_model);
    gt911_model_set_interrupt(&g_model, on_interrupt, NULL);
    g_interrupts = 0u;
    HAC_CHECK(adp_i2c_fake_attach(&g_bus, ADDR, gt911_model_port(&g_model)));
}

static hal_status_t read_at(uint16_t reg, uint8_t *out, size_t n)
{
    const uint8_t w[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFu)};
    port_i2c_t *bus = adp_i2c_fake_port(&g_bus);
    return bus->write_read(bus, ADDR, w, 2u, out, n, TO_MS);
}

static hal_status_t write_at(uint16_t reg, uint8_t value)
{
    const uint8_t w[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFu), value};
    port_i2c_t *bus = adp_i2c_fake_port(&g_bus);
    return bus->write(bus, ADDR, w, 3u, TO_MS);
}

static uint8_t status(void)
{
    uint8_t s = 0xEEu;
    HAC_CHECK_EQ_INT(read_at(0x814Eu, &s, 1u), HAL_OK);
    return s;
}

static void registers_take_a_16_bit_address_high_byte_first(void)
{
    setup();
    uint8_t info[11];
    HAC_CHECK_EQ_INT(read_at(0x8140u, info, sizeof(info)), HAL_OK);
    HAC_CHECK(memcmp(info, "911\0", 4u) == 0);
    HAC_CHECK_EQ_INT(info[4] | (info[5] << 8), GT911_MODEL_DEFAULT_FIRMWARE);
    HAC_CHECK_EQ_INT(info[6] | (info[7] << 8), GT911_MODEL_DEFAULT_X_MAX);
    HAC_CHECK_EQ_INT(info[8] | (info[9] << 8), GT911_MODEL_DEFAULT_Y_MAX);

    // The pointer advances byte by byte: 0x8142 is the third character.
    uint8_t c = 0u;
    HAC_CHECK_EQ_INT(read_at(0x8142u, &c, 1u), HAL_OK);
    HAC_CHECK_EQ_INT(c, '1');
}

static void the_configuration_checksum_is_the_twos_complement_of_the_sum(void)
{
    setup();
    uint8_t config[GT911_MODEL_CONFIG_LEN];
    HAC_CHECK_EQ_INT(read_at(GT911_MODEL_CONFIG_FIRST, config, sizeof(config)), HAL_OK);
    uint8_t sum = 0u;
    for (size_t i = 0u; i <= 184u; ++i) {  // 0x8047..0x80FF, the checksum included
        sum = (uint8_t)(sum + config[i]);
    }
    HAC_CHECK_EQ_INT(sum, 0);
    HAC_CHECK_EQ_INT(config[1] | (config[2] << 8), GT911_MODEL_DEFAULT_X_MAX);
    HAC_CHECK_EQ_INT(config[6] & 0x03, 1);  // falling edge, as the board's
}

static void a_write_into_the_configuration_is_refused_and_counted(void)
{
    setup();
    const uint8_t before = g_model.config[0];
    HAC_CHECK_EQ_INT(write_at(0x8047u, 0x00u), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(write_at(0x8100u, 0x01u), HAL_ERR_IO);  // Config_Fresh, the last byte
    HAC_CHECK_EQ_INT((int)g_model.stats.config_writes, 2);
    HAC_CHECK_EQ_INT(g_model.config[0], before);
    // Either side of the area is not the area.
    HAC_CHECK_EQ_INT(write_at(0x8046u, 0x00u), HAL_OK);
    HAC_CHECK_EQ_INT(write_at(0x8101u, 0x00u), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_model.stats.config_writes, 2);
}

static void a_report_is_held_until_the_host_clears_it(void)
{
    setup();
    HAC_CHECK_EQ_INT(status(), 0x00);

    gt911_model_point_t p = {.track_id = 3u, .x = 100u, .y = 700u, .size = 25u};
    gt911_model_touch(&g_model, &p, 1u);
    HAC_CHECK_EQ_INT((int)g_interrupts, 1);
    HAC_CHECK_EQ_INT(status(), 0x81);

    uint8_t rec[8];
    HAC_CHECK_EQ_INT(read_at(0x814Fu, rec, sizeof(rec)), HAL_OK);
    HAC_CHECK_EQ_INT(rec[0], 3);
    HAC_CHECK_EQ_INT(rec[1] | (rec[2] << 8), 100);
    HAC_CHECK_EQ_INT(rec[3] | (rec[4] << 8), 700);
    HAC_CHECK_EQ_INT(rec[5] | (rec[6] << 8), 25);

    // Moved before the host cleared: the report waits, and the newest points
    // come when the host clears.
    p.x = 120u;
    gt911_model_touch(&g_model, &p, 1u);
    HAC_CHECK_EQ_INT((int)g_interrupts, 1);
    HAC_CHECK_EQ_INT(read_at(0x8150u, rec, 2u), HAL_OK);
    HAC_CHECK_EQ_INT(rec[0] | (rec[1] << 8), 100);

    HAC_CHECK_EQ_INT(write_at(0x814Eu, 0x00u), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_interrupts, 2);
    HAC_CHECK_EQ_INT(status(), 0x81);
    HAC_CHECK_EQ_INT(read_at(0x8150u, rec, 2u), HAL_OK);
    HAC_CHECK_EQ_INT(rec[0] | (rec[1] << 8), 120);

    HAC_CHECK_EQ_INT(write_at(0x814Eu, 0x00u), HAL_OK);
    HAC_CHECK_EQ_INT(status(), 0x00);
}

static void the_last_lift_is_a_report_with_no_points(void)
{
    setup();
    const gt911_model_point_t p = {.track_id = 0u, .x = 10u, .y = 10u, .size = 5u};
    gt911_model_touch(&g_model, &p, 1u);
    HAC_CHECK_EQ_INT(write_at(0x814Eu, 0x00u), HAL_OK);
    gt911_model_touch(&g_model, NULL, 0u);
    HAC_CHECK_EQ_INT(status(), 0x80);
    HAC_CHECK_EQ_INT((int)g_interrupts, 2);
}

static void a_held_finger_is_reported_every_scan_and_reports_can_be_lost(void)
{
    setup();
    const gt911_model_point_t p = {.track_id = 0u, .x = 10u, .y = 10u, .size = 5u};
    gt911_model_touch(&g_model, &p, 1u);
    HAC_CHECK_EQ_INT(write_at(0x814Eu, 0x00u), HAL_OK);
    gt911_model_scan(&g_model);
    HAC_CHECK_EQ_INT((int)g_interrupts, 2);
    HAC_CHECK_EQ_INT(write_at(0x814Eu, 0x00u), HAL_OK);

    gt911_model_drop_reports(&g_model, 1u);
    gt911_model_touch(&g_model, NULL, 0u);
    HAC_CHECK_EQ_INT((int)g_interrupts, 2);
    HAC_CHECK_EQ_INT(status(), 0x00);
    HAC_CHECK_EQ_INT((int)g_model.stats.reports_dropped, 1);

    gt911_model_scan(&g_model);  // nothing touched: nothing to report
    HAC_CHECK_EQ_INT((int)g_interrupts, 2);

    gt911_model_spurious_interrupt(&g_model);
    HAC_CHECK_EQ_INT((int)g_interrupts, 3);
    HAC_CHECK_EQ_INT(status(), 0x00);
}

static void sleep_needs_int_low_and_a_long_enough_pulse_after_58_ms(void)
{
    setup();
    // INT not held low: the command is ignored.
    HAC_CHECK_EQ_INT(write_at(0x8040u, 0x05u), HAL_OK);
    HAC_CHECK(!g_model.asleep);
    HAC_CHECK_EQ_INT((int)g_model.stats.ignored_sleep_commands, 1);

    gt911_model_int_drive_low(&g_model, g_clock.now_ms);
    HAC_CHECK_EQ_INT(write_at(0x8040u, 0x05u), HAL_OK);
    HAC_CHECK(g_model.asleep);
    uint8_t s = 0u;
    HAC_CHECK_EQ_INT(read_at(0x814Eu, &s, 1u), HAL_ERR_NOT_FOUND);

    // Asleep, a touch reports nothing.
    const gt911_model_point_t p = {.track_id = 0u, .x = 10u, .y = 10u, .size = 5u};
    gt911_model_touch(&g_model, &p, 1u);
    HAC_CHECK_EQ_INT((int)g_interrupts, 0);

    adp_clock_virtual_advance(&g_clock, 30u);
    gt911_model_int_pulse_high(&g_model, 3000u, g_clock.now_ms);
    HAC_CHECK(g_model.asleep);
    HAC_CHECK_EQ_INT((int)g_model.stats.early_wake_pulses, 1);

    adp_clock_virtual_advance(&g_clock, 40u);
    gt911_model_int_pulse_high(&g_model, 1000u, g_clock.now_ms);
    HAC_CHECK(g_model.asleep);
    HAC_CHECK_EQ_INT((int)g_model.stats.short_wake_pulses, 1);

    gt911_model_int_pulse_high(&g_model, 3000u, g_clock.now_ms);
    HAC_CHECK(!g_model.asleep);
    HAC_CHECK_EQ_INT((int)g_model.stats.wakes, 1);
    adp_clock_virtual_advance(&g_clock, 3u);
    HAC_CHECK_EQ_INT(read_at(0x814Eu, &s, 1u), HAL_OK);

    // Awake, a pulse is nothing but counted.
    gt911_model_int_pulse_high(&g_model, 3000u, g_clock.now_ms);
    HAC_CHECK_EQ_INT((int)g_model.stats.pulses_while_awake, 1);
}

static void the_answer_delay_knob_refuses_the_address_after_a_wake(void)
{
    setup();
    g_model.answer_delay_ms = 100u;
    gt911_model_int_drive_low(&g_model, g_clock.now_ms);
    HAC_CHECK_EQ_INT(write_at(0x8040u, 0x05u), HAL_OK);
    adp_clock_virtual_advance(&g_clock, 100u);
    gt911_model_int_pulse_high(&g_model, 3000u, g_clock.now_ms);
    uint8_t s = 0u;
    adp_clock_virtual_advance(&g_clock, 50u);
    HAC_CHECK_EQ_INT(read_at(0x814Eu, &s, 1u), HAL_ERR_NOT_FOUND);
    adp_clock_virtual_advance(&g_clock, 60u);
    HAC_CHECK_EQ_INT(read_at(0x814Eu, &s, 1u), HAL_OK);
}

int main(void)
{
    HAC_RUN(registers_take_a_16_bit_address_high_byte_first);
    HAC_RUN(the_configuration_checksum_is_the_twos_complement_of_the_sum);
    HAC_RUN(a_write_into_the_configuration_is_refused_and_counted);
    HAC_RUN(a_report_is_held_until_the_host_clears_it);
    HAC_RUN(the_last_lift_is_a_report_with_no_points);
    HAC_RUN(a_held_finger_is_reported_every_scan_and_reports_can_be_lost);
    HAC_RUN(sleep_needs_int_low_and_a_long_enough_pulse_after_58_ms);
    HAC_RUN(the_answer_delay_knob_refuses_the_address_after_a_wake);
    return hac_test_summary();
}
