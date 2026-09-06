# SPDX-License-Identifier: Apache-2.0
#
# platform/esp32_t5s3/: the adapters implementing the ports on this board.
#
# Sources are listed here, not in a shared cmake/ file: unlike core/ and the
# contract bodies, there is exactly one ESP32 platform, so there is no second
# build that could drift from this list.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)

idf_component_register(
    SRCS
        ${HAC_ROOT}/platform/esp32_t5s3/adp_clock.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_log.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_display_null.c
    INCLUDE_DIRS
        ${HAC_ROOT}/platform/esp32_t5s3
    REQUIRES
        hac_core
        esp_timer
        esp_psram
        esp_rom
        heap
        freertos
        log
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
