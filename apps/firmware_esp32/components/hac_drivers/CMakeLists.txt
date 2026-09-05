# SPDX-License-Identifier: Apache-2.0
#
# Sensor and panel drivers (BME68x, SCD4x, GT911, ED047TC1), which depend only
# on port_i2c.h and port_clock.h and therefore also build on a PC.
#
# Empty until phase 2/3 adds drivers/ and cmake/sources_drivers.cmake; the
# component is registered now so the wrapper layout is fixed from the start.

idf_component_register(REQUIRES hac_core)
