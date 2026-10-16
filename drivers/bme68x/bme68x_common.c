// SPDX-License-Identifier: Apache-2.0

#include "bme68x_common.h"

#include <string.h>

#include "bme68x_limits.h"

void bme68x_format_id(uint8_t id, char out[BME68X_ID_TEXT_LEN])
{
    static const char HEX[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    out[2] = HEX[(id >> 4) & 0xFu];
    out[3] = HEX[id & 0xFu];
    out[4] = '\0';
}

const char *bme68x_variant_name(uint8_t variant_id)
{
    switch (variant_id) {
        case BME68X_VARIANT_GAS_LOW:
            return "BME680";
        case BME68X_VARIANT_GAS_HIGH:
            return "BME688";
        default:
            return "unknown";
    }
}

hal_status_t bme68x_vendor_status(const struct bme68x_dev *dev, int8_t rslt)
{
    switch (rslt) {
        case BME68X_OK:
            return HAL_OK;
        case BME68X_E_COM_FAIL:
            return bme68x_hal_status(dev);
        case BME68X_E_DEV_NOT_FOUND:
            return HAL_ERR_UNSUPPORTED;  // it answered, with the wrong chip id
        case BME68X_W_NO_NEW_DATA:
            return HAL_ERR_NOT_READY;
        case BME68X_E_NULL_PTR:
        case BME68X_E_INVALID_LENGTH:
            return HAL_ERR_INVALID_ARG;
        default:
            return HAL_ERR_IO;
    }
}

const char *bme68x_status_name(hal_status_t status)
{
    switch (status) {
        case HAL_OK:
            return "ok";
        case HAL_ERR_TIMEOUT:
            return "timeout";
        case HAL_ERR_IO:
            return "io";
        case HAL_ERR_NOT_FOUND:
            return "not_found";
        case HAL_ERR_INVALID_ARG:
            return "invalid_arg";
        case HAL_ERR_NO_MEM:
            return "no_mem";
        case HAL_ERR_UNSUPPORTED:
            return "unsupported";
        case HAL_ERR_BUSY:
            return "busy";
        case HAL_ERR_NOT_READY:
            return "not_ready";
        default:
            return "?";
    }
}

hal_status_t bme68x_bring_up(struct bme68x_dev *dev, bme68x_hal_t *hal, const char **failed_step)
{
    // The supply may have risen only just now (bme68x_limits.h).
    hal->clock->delay_ms(hal->clock, HAC_BME68X_POWER_UP_MS);

    memset(dev, 0, sizeof *dev);
    bme68x_hal_bind(dev, hal);
    dev->amb_temp = HAC_BME68X_HEATER_AMBIENT_C;

    // Soft reset, chip id, variant id, calibration. The reset puts a sensor
    // a software reset left configured, or measuring, back to its power-on
    // state (bme68x_source.h, "Resets").
    hal_status_t status = bme68x_vendor_status(dev, bme68x_init(dev));
    if (status != HAL_OK) {
        *failed_step = "init";
        return status;
    }

    // Asleep, as a reset leaves it, before anything that goes through
    // Bosch's unbounded wait for sleep mode.
    status = bme68x_check_asleep(dev);
    if (status == HAL_ERR_BUSY) {
        status = HAL_ERR_IO;  // the reset did not take
    }
    if (status != HAL_OK) {
        *failed_step = "reset";
    }
    return status;
}

hal_status_t bme68x_check_asleep(struct bme68x_dev *dev)
{
    uint8_t mode = BME68X_SLEEP_MODE;
    const hal_status_t status = bme68x_vendor_status(dev, bme68x_get_op_mode(&mode, dev));
    if (status != HAL_OK) {
        return status;
    }
    return (mode == BME68X_SLEEP_MODE) ? HAL_OK : HAL_ERR_BUSY;
}

void bme68x_log_calibration(const port_log_t *log, const char *tag,
                            const struct bme68x_calib_data *c)
{
    const log_field_t t_heat[] = {
        log_u64("par_t1", c->par_t1),
        log_i64("par_t2", c->par_t2),
        log_i64("par_t3", c->par_t3),
        log_u64("res_heat_range", c->res_heat_range),
        log_i64("res_heat_val", c->res_heat_val),
        log_i64("range_switching_error", c->range_sw_err),
    };
    const log_field_t p_low[] = {
        log_u64("par_p1", c->par_p1), log_i64("par_p2", c->par_p2), log_i64("par_p3", c->par_p3),
        log_i64("par_p4", c->par_p4), log_i64("par_p5", c->par_p5),
    };
    const log_field_t p_high[] = {
        log_i64("par_p6", c->par_p6), log_i64("par_p7", c->par_p7),   log_i64("par_p8", c->par_p8),
        log_i64("par_p9", c->par_p9), log_u64("par_p10", c->par_p10),
    };
    const log_field_t h_low[] = {
        log_u64("par_h1", c->par_h1), log_u64("par_h2", c->par_h2), log_i64("par_h3", c->par_h3),
        log_i64("par_h4", c->par_h4), log_i64("par_h5", c->par_h5),
    };
    const log_field_t h_gas[] = {
        log_u64("par_h6", c->par_h6),  log_i64("par_h7", c->par_h7),  log_i64("par_g1", c->par_gh1),
        log_i64("par_g2", c->par_gh2), log_i64("par_g3", c->par_gh3),
    };
    port_log_write(log, LOG_LEVEL_INFO, tag, "calibration (1/5)", t_heat,
                   sizeof t_heat / sizeof t_heat[0]);
    port_log_write(log, LOG_LEVEL_INFO, tag, "calibration (2/5)", p_low,
                   sizeof p_low / sizeof p_low[0]);
    port_log_write(log, LOG_LEVEL_INFO, tag, "calibration (3/5)", p_high,
                   sizeof p_high / sizeof p_high[0]);
    port_log_write(log, LOG_LEVEL_INFO, tag, "calibration (4/5)", h_low,
                   sizeof h_low / sizeof h_low[0]);
    port_log_write(log, LOG_LEVEL_INFO, tag, "calibration (5/5)", h_gas,
                   sizeof h_gas / sizeof h_gas[0]);
}
