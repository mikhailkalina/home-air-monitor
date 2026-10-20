// SPDX-License-Identifier: Apache-2.0
//
// The GT911 driver (drivers/gt911/gt911.h) against the GT911 model on the
// fake bus: finding the part at either address, waking it first, naming it,
// reading its configuration, reading and clearing reports, sleep and wake on
// the datasheet's timing -- and, after every one of those, that nothing was
// written into the vendor's configuration (the model refuses and counts any
// such write: platform/host/i2c_models/gt911_model.h).

#include <stdint.h>
#include <string.h>

#include "gt911.h"
#include "gt911_rig.h"
#include "hac_test.h"

static gt911_rig_t g_rig;
static gt911_t g_dev;

static void setup_at(uint8_t address)
{
    gt911_rig_init(&g_rig, address);
    const gt911_config_t cfg = gt911_rig_device_config(&g_rig);
    HAC_CHECK_EQ_INT(gt911_setup(&g_dev, &cfg), HAL_OK);
}

static void the_configuration_was_never_written(void)
{
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.config_writes, 0);
}

static void init_wakes_finds_and_names_the_controller(void)
{
    setup_at(GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);

    const gt911_info_t *i = gt911_info(&g_dev);
    HAC_CHECK_EQ_INT(i->address, GT911_RIG_ADDR);
    HAC_CHECK(strcmp(i->product_id, "911") == 0);
    HAC_CHECK_EQ_INT(i->firmware_version, GT911_MODEL_DEFAULT_FIRMWARE);
    HAC_CHECK_EQ_INT(i->x_max, GT911_MODEL_DEFAULT_X_MAX);
    HAC_CHECK_EQ_INT(i->y_max, GT911_MODEL_DEFAULT_Y_MAX);
    HAC_CHECK_EQ_INT(i->x_resolution, GT911_MODEL_DEFAULT_X_MAX);
    HAC_CHECK_EQ_INT(i->touch_number, GT911_MODEL_DEFAULT_TOUCH_NUMBER);
    HAC_CHECK_EQ_INT(i->config_version, GT911_MODEL_DEFAULT_CONFIG_VERSION);
    HAC_CHECK_EQ_INT(i->trigger, GT911_TRIGGER_FALLING);  // the board's configuration
    HAC_CHECK(i->config_checksum_ok);

    // The wake pulse came first, within the datasheet's 2-5 ms, and on an
    // awake controller it did nothing.
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.pulses_while_awake, 1);
    HAC_CHECK(g_rig.model.stats.last_pulse_us >= GT911_MODEL_WAKE_PULSE_MIN_US);
    HAC_CHECK(g_rig.model.stats.last_pulse_us <= GT911_MODEL_WAKE_PULSE_MAX_US);
    // A report left over from before is cleared.
    HAC_CHECK(g_rig.model.stats.status_clears >= 1u);
    the_configuration_was_never_written();
}

static void init_wakes_a_controller_a_previous_firmware_put_to_sleep(void)
{
    setup_at(GT911_RIG_ADDR);
    // Asleep, as an ESP32 reset leaves it: INT was held low, the command
    // sent, and then the host went away.
    gt911_model_int_drive_low(&g_rig.model, g_rig.clock.now_ms);
    g_rig.model.asleep = true;
    g_rig.model.slept_at_ms = 0u;
    gt911_model_int_release(&g_rig.model, g_rig.clock.now_ms);

    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.wakes, 1);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.long_wake_pulses, 0);
    HAC_CHECK(strcmp(gt911_info(&g_dev)->product_id, "911") == 0);
    the_configuration_was_never_written();
}

static void a_controller_slow_to_answer_after_waking_is_asked_again(void)
{
    setup_at(GT911_RIG_ADDR);
    g_rig.model.asleep = true;
    g_rig.model.answer_delay_ms = 300u;  // longer than the settle estimate
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    HAC_CHECK(g_dev.answer_attempts > 1u);
    HAC_CHECK_EQ_INT(gt911_info(&g_dev)->address, GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT((int)g_rig.bus.refused_data_transfers, 0);
}

// What overflowed app_main's stack on the board (2026-10-20): the controller
// re-latched at 0x14 after a power glitch, and a second of reads at 0x5D,
// each refused, each logged three times by ESP-IDF's I2C driver. Found by
// address-only probes, it costs no refused data transfer at all.
static void init_finds_the_other_address_without_a_refused_transfer(void)
{
    setup_at(GT911_RIG_ALT_ADDR);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    HAC_CHECK_EQ_INT(gt911_info(&g_dev)->address, GT911_RIG_ALT_ADDR);
    HAC_CHECK_EQ_INT((int)g_dev.answer_attempts, 1);
    HAC_CHECK_EQ_INT((int)g_rig.bus.refused_data_transfers, 0);
}

static void init_reports_nothing_at_either_address_after_the_answer_timeout(void)
{
    setup_at(0u);
    const uint64_t start = g_rig.clock.now_ms;
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_ERR_NOT_FOUND);
    // Both addresses probed in turn for the whole timeout -- probes only.
    HAC_CHECK(g_rig.clock.now_ms - start >= GT911_ANSWER_TIMEOUT_MS);
    HAC_CHECK(g_rig.clock.now_ms - start < 2u * GT911_ANSWER_TIMEOUT_MS);
    HAC_CHECK_EQ_INT((int)g_rig.bus.refused_data_transfers, 0);
    gt911_frame_t frame;
    HAC_CHECK_EQ_INT(gt911_read_frame(&g_dev, &frame), HAL_ERR_NOT_READY);
}

static void a_part_that_is_not_a_gt911_is_refused(void)
{
    setup_at(GT911_RIG_ADDR);
    memcpy(g_rig.model.product_id, "928", 4u);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_ERR_UNSUPPORTED);
    HAC_CHECK(strcmp(gt911_info(&g_dev)->product_id, "928") == 0);
}

static void a_bad_checksum_is_reported_not_repaired(void)
{
    setup_at(GT911_RIG_ADDR);
    g_rig.model.config[184] = (uint8_t)(g_rig.model.config[184] + 1u);
    g_rig.model.config[6] = 0x00u;  // a rising edge, in a configuration whose checksum is wrong
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    HAC_CHECK(!gt911_info(&g_dev)->config_checksum_ok);
    HAC_CHECK_EQ_INT(gt911_info(&g_dev)->trigger, GT911_TRIGGER_RISING);
    the_configuration_was_never_written();
}

static void a_report_is_read_and_cleared(void)
{
    setup_at(GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    const uint32_t clears = g_rig.model.stats.status_clears;

    gt911_frame_t frame;
    HAC_CHECK_EQ_INT(gt911_read_frame(&g_dev, &frame), HAL_OK);
    HAC_CHECK(!frame.ready);  // nothing touched: a read that finds nothing
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.status_clears, (int)clears);

    const gt911_model_point_t two[2] = {gt911_rig_point(4u, 10u, 900u),
                                        gt911_rig_point(7u, 530u, 5u)};
    gt911_model_touch(&g_rig.model, two, 2u);
    HAC_CHECK_EQ_INT(gt911_read_frame(&g_dev, &frame), HAL_OK);
    HAC_CHECK(frame.ready);
    HAC_CHECK_EQ_INT(frame.count, 2);
    HAC_CHECK_EQ_INT(frame.status, 0x82);
    HAC_CHECK_EQ_INT(frame.points[0].track_id, 4);
    HAC_CHECK_EQ_INT(frame.points[0].x, 10);
    HAC_CHECK_EQ_INT(frame.points[0].y, 900);
    HAC_CHECK_EQ_INT(frame.points[0].size, 20);
    HAC_CHECK_EQ_INT(frame.points[1].track_id, 7);
    HAC_CHECK_EQ_INT(frame.points[1].x, 530);
    HAC_CHECK_EQ_INT(frame.points[1].y, 5);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.status_clears, (int)clears + 1);
    HAC_CHECK(!g_rig.model.buffer_ready);

    // The lift: a report with no points, cleared too.
    gt911_model_touch(&g_rig.model, NULL, 0u);
    HAC_CHECK_EQ_INT(gt911_read_frame(&g_dev, &frame), HAL_OK);
    HAC_CHECK(frame.ready);
    HAC_CHECK_EQ_INT(frame.count, 0);
    HAC_CHECK(!g_rig.model.buffer_ready);
    the_configuration_was_never_written();
}

static void a_status_no_gt911_reports_is_refused_and_cleared(void)
{
    setup_at(GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    g_rig.model.buffer_ready = true;
    g_rig.model.buffer_count = 9u;
    gt911_frame_t frame;
    HAC_CHECK_EQ_INT(gt911_read_frame(&g_dev, &frame), HAL_ERR_IO);
    HAC_CHECK(!g_rig.model.buffer_ready);
}

static void sleep_holds_int_low_and_wake_waits_out_58_ms(void)
{
    setup_at(GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);

    HAC_CHECK_EQ_INT(gt911_sleep(&g_dev), HAL_OK);
    HAC_CHECK(g_rig.model.asleep);  // INT was low first, or the model would have ignored it
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.ignored_sleep_commands, 0);
    HAC_CHECK_EQ_INT(g_rig.model.int_level, GT911_MODEL_INT_HOST_LOW);
    gt911_frame_t frame;
    HAC_CHECK_EQ_INT(gt911_read_frame(&g_dev, &frame), HAL_ERR_NOT_READY);

    // Woken at once: the driver waits out the datasheet's interval first.
    const uint64_t slept = g_rig.clock.now_ms;
    HAC_CHECK_EQ_INT(gt911_wake(&g_dev), HAL_OK);
    HAC_CHECK(!g_rig.model.asleep);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.early_wake_pulses, 0);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.wakes, 1);
    HAC_CHECK(g_rig.clock.now_ms - slept > GT911_MODEL_SLEEP_TO_WAKE_MS);
    HAC_CHECK_EQ_INT(g_rig.model.int_level, GT911_MODEL_INT_RELEASED);

    const gt911_model_point_t p = gt911_rig_point(0u, 100u, 100u);
    gt911_model_touch(&g_rig.model, &p, 1u);
    HAC_CHECK_EQ_INT(gt911_read_frame(&g_dev, &frame), HAL_OK);
    HAC_CHECK(frame.ready);

    // Twice round, and still nothing in the configuration.
    HAC_CHECK_EQ_INT(gt911_sleep(&g_dev), HAL_OK);
    HAC_CHECK_EQ_INT(gt911_wake(&g_dev), HAL_OK);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    the_configuration_was_never_written();
}

static void without_an_int_line_there_is_no_sleep(void)
{
    gt911_rig_init(&g_rig, GT911_RIG_ADDR);
    gt911_config_t cfg = gt911_rig_device_config(&g_rig);
    cfg.int_line = NULL;
    HAC_CHECK_EQ_INT(gt911_setup(&g_dev, &cfg), HAL_OK);
    HAC_CHECK_EQ_INT(gt911_init(&g_dev), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.pulses_while_awake, 0);
    HAC_CHECK_EQ_INT(gt911_sleep(&g_dev), HAL_ERR_UNSUPPORTED);
    HAC_CHECK_EQ_INT(gt911_wake(&g_dev), HAL_ERR_UNSUPPORTED);
    HAC_CHECK(!g_rig.model.asleep);
}

static void setup_refuses_a_config_without_bus_clock_or_address(void)
{
    gt911_rig_init(&g_rig, GT911_RIG_ADDR);
    gt911_config_t cfg = gt911_rig_device_config(&g_rig);
    cfg.address = 0u;
    HAC_CHECK_EQ_INT(gt911_setup(&g_dev, &cfg), HAL_ERR_INVALID_ARG);
    cfg = gt911_rig_device_config(&g_rig);
    cfg.bus = NULL;
    HAC_CHECK_EQ_INT(gt911_setup(&g_dev, &cfg), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(gt911_setup(&g_dev, NULL), HAL_ERR_INVALID_ARG);
}

static void the_checksum_rule(void)
{
    uint8_t config[GT911_CONFIG_LEN];
    memset(config, 0, sizeof(config));
    HAC_CHECK_EQ_INT(gt911_config_checksum(config), 0);
    config[0] = 0x41u;
    config[1] = 0x1Cu;
    config[183] = 0x01u;
    config[184] = 0xFFu;  // the checksum itself is not summed
    HAC_CHECK_EQ_INT(gt911_config_checksum(config), (uint8_t)(0u - 0x41u - 0x1Cu - 0x01u));
    HAC_CHECK(strcmp(gt911_trigger_name(GT911_TRIGGER_HIGH_LEVEL), "high") == 0);
}

int main(void)
{
    HAC_RUN(init_wakes_finds_and_names_the_controller);
    HAC_RUN(init_wakes_a_controller_a_previous_firmware_put_to_sleep);
    HAC_RUN(a_controller_slow_to_answer_after_waking_is_asked_again);
    HAC_RUN(init_finds_the_other_address_without_a_refused_transfer);
    HAC_RUN(init_reports_nothing_at_either_address_after_the_answer_timeout);
    HAC_RUN(a_part_that_is_not_a_gt911_is_refused);
    HAC_RUN(a_bad_checksum_is_reported_not_repaired);
    HAC_RUN(a_report_is_read_and_cleared);
    HAC_RUN(a_status_no_gt911_reports_is_refused_and_cleared);
    HAC_RUN(sleep_holds_int_low_and_wake_waits_out_58_ms);
    HAC_RUN(without_an_int_line_there_is_no_sleep);
    HAC_RUN(setup_refuses_a_config_without_bus_clock_or_address);
    HAC_RUN(the_checksum_rule);
    return hac_test_summary();
}
