# SPDX-License-Identifier: Apache-2.0
#
# THE source list for drivers/. It is included by both the host build (root
# CMakeLists.txt) and the ESP-IDF build
# (apps/firmware_esp32/components/hac_drivers/CMakeLists.txt), exactly as
# cmake/sources_core.cmake feeds both builds of core/, so a driver compiled
# and tested on the host is the one the firmware links.
#
# Never copy these paths into a component CMakeLists.txt.
#
# Two lists, because two sets of rules apply:
#
#   * HAC_DRIVERS_SOURCES -- this project's code, under the full warning set
#     like everything else first-party.
#   * HAC_DRIVERS_VENDOR_SOURCES -- third-party code vendored unmodified under
#     drivers/*/vendor/ (see THIRD_PARTY_NOTICES.md). It is compiled with
#     HAC_DRIVERS_VENDOR_COMPILE_OPTIONS on top of the full set, and its
#     headers are included as SYSTEM, so its style cannot fail this project's
#     build and this project's flags never tempt anyone to edit it.
#
# The includer must define HAC_ROOT as the absolute path of the repository root.

if(NOT DEFINED HAC_ROOT)
    message(FATAL_ERROR "HAC_ROOT must be set before including sources_drivers.cmake")
endif()

set(HAC_DRIVERS_SOURCES
    ${HAC_ROOT}/drivers/bme68x/bme68x_hal.c
    ${HAC_ROOT}/drivers/bme68x/bme68x_source.c
    ${HAC_ROOT}/drivers/scd4x/scd4x_hal.c
    ${HAC_ROOT}/drivers/scd4x/scd4x_source.c
)

# Drivers resolve each other's headers as "scd4x/scd4x_source.h".
set(HAC_DRIVERS_INCLUDES
    ${HAC_ROOT}/drivers
    ${HAC_ROOT}/drivers/bme68x
    ${HAC_ROOT}/drivers/scd4x
)

# Sensirion embedded-i2c-scd4x 1.2.0 (drivers/scd4x/vendor/README.md). Its
# sensirion_i2c_hal.c, a template of TODOs, is not vendored:
# drivers/scd4x/scd4x_hal.c implements that header.
#
# Bosch BME68x Sensor API v4.4.8 (drivers/bme68x/vendor/README.md). Bosch
# ships no HAL: drivers/bme68x/bme68x_hal.c supplies its callbacks.
set(HAC_DRIVERS_VENDOR_SOURCES
    ${HAC_ROOT}/drivers/bme68x/vendor/bme68x.c
    ${HAC_ROOT}/drivers/scd4x/vendor/scd4x_i2c.c
    ${HAC_ROOT}/drivers/scd4x/vendor/sensirion_common.c
    ${HAC_ROOT}/drivers/scd4x/vendor/sensirion_i2c.c
)

set(HAC_DRIVERS_VENDOR_INCLUDES
    ${HAC_ROOT}/drivers/bme68x/vendor
    ${HAC_ROOT}/drivers/scd4x/vendor
)

# Relaxed exactly as far as the vendored code needs and no further, so a
# vendor update that brings a new kind of warning still stops the build and
# gets looked at. Sensirion 1.2.0 trips three. scd4x_i2c.h declares its
# argument-less functions as "f()" rather than "f(void)", which is not a
# prototype: -Wstrict-prototypes at the declaration, and
# -Wmissing-prototypes at the definition, which then has none before it (the
# second is a host-only flag; on ESP-IDF the option is merely redundant).
# And its framing code narrows integers implicitly (-Wconversion). Bosch
# v4.4.8 trips only the last: its bit-field macros (BME68X_SET_BITS and
# friends) narrow int to uint8_t and convert signed to unsigned.
set(HAC_DRIVERS_VENDOR_COMPILE_OPTIONS
    -Wno-conversion
    -Wno-strict-prototypes
    -Wno-missing-prototypes
)
