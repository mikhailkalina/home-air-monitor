# SPDX-License-Identifier: Apache-2.0

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)

idf_component_register(
    SRCS
        main.c
        event_loop.c
        bringup_reading.c
        # Compiled into every build, called only under
        # CONFIG_HAC_EPD_CALIBRATION: an unused mode that is never compiled
        # rots unnoticed, and the linker drops it from images that do not
        # call it.
        epd_calibration.c
        duration_stats.c
    INCLUDE_DIRS .
    REQUIRES hac_core hac_drivers hac_platform hac_contract esp_timer
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
