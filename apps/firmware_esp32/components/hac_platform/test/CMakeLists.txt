# SPDX-License-Identifier: Apache-2.0
#
# hac_platform's on-target tests, built into apps/firmware_esp32/test_app/
# by `idf.py -T hac_platform build` and never into the firmware: ESP-IDF adds
# a component's test/ directory only when -T selects it.
#
# The runners live beside the host runners in tests/contract/, not here --
# everything under components/ is a thin wrapper -- and the contract bodies
# they call come from hac_contract, compiled from the same
# cmake/sources_contract.cmake list the host build uses.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../.. ABSOLUTE)

idf_component_register(
    SRCS
        ${HAC_ROOT}/tests/contract/esp32/test_port_clock_contract.c
        ${HAC_ROOT}/tests/contract/esp32/test_port_log_contract.c
        ${HAC_ROOT}/tests/contract/esp32/test_port_i2c_contract.c
        ${HAC_ROOT}/tests/contract/esp32/test_port_sensor_contract.c
        ${HAC_ROOT}/tests/contract/esp32/test_port_files_contract.c
        ${HAC_ROOT}/tests/contract/esp32/test_port_input_contract.c
        ${HAC_ROOT}/tests/contract/esp32/test_port_net_contract.c
    PRIV_REQUIRES unity hac_platform hac_contract hac_drivers
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
