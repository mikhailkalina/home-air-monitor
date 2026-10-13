# SPDX-License-Identifier: Apache-2.0

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)

idf_component_register(
    SRCS test_app_main.c
    PRIV_REQUIRES unity
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
