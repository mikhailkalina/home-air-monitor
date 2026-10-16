// SPDX-License-Identifier: Apache-2.0

#include "bme68x_bsec_status.h"

value_status_t bme68x_bsec_iaq_status(uint8_t accuracy)
{
    switch (accuracy) {
        case BME68X_BSEC_ACCURACY_UNRELIABLE:
            return VAL_WARMUP;
        case BME68X_BSEC_ACCURACY_LOW:
        case BME68X_BSEC_ACCURACY_MEDIUM:
        case BME68X_BSEC_ACCURACY_HIGH:
            return VAL_OK;
        default:
            return VAL_ERROR;
    }
}

value_accuracy_t bme68x_bsec_iaq_accuracy(uint8_t accuracy)
{
    switch (accuracy) {
        case BME68X_BSEC_ACCURACY_UNRELIABLE:
            return ACC_UNRELIABLE;
        case BME68X_BSEC_ACCURACY_LOW:
            return ACC_LOW;
        case BME68X_BSEC_ACCURACY_MEDIUM:
            return ACC_MEDIUM;
        case BME68X_BSEC_ACCURACY_HIGH:
            return ACC_HIGH;
        default:
            return ACC_NOT_REPORTED;
    }
}

value_status_t bme68x_bsec_gas_status(bool gas_valid, bool stabilized, bool run_in_done)
{
    if (!gas_valid) {
        return VAL_ERROR;
    }
    return (stabilized && run_in_done) ? VAL_OK : VAL_WARMUP;
}

const char *bme68x_bsec_accuracy_name(uint8_t accuracy)
{
    switch (accuracy) {
        case BME68X_BSEC_ACCURACY_UNRELIABLE:
            return "unreliable";
        case BME68X_BSEC_ACCURACY_LOW:
            return "low";
        case BME68X_BSEC_ACCURACY_MEDIUM:
            return "medium";
        case BME68X_BSEC_ACCURACY_HIGH:
            return "high";
        default:
            return "?";
    }
}
