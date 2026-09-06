# SPDX-License-Identifier: Apache-2.0

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)

idf_component_register(
    SRCS
        main.c
        event_loop.c
        bringup_reading.c
    INCLUDE_DIRS .
    REQUIRES hac_core hac_drivers hac_platform hac_contract
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
