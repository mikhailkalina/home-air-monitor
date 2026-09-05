# SPDX-License-Identifier: Apache-2.0
#
# Contract suites: one file per port, run against every implementation of it.
# port_sensor is the first (see docs/architecture.md §5.3 and §9.2).

add_executable(test_port_sensor_contract test_port_sensor_contract.c)
target_link_libraries(test_port_sensor_contract PRIVATE hac_platform_host hac_test_support)
target_compile_definitions(test_port_sensor_contract PRIVATE
    HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
hac_apply_warnings(test_port_sensor_contract)
add_test(NAME test_port_sensor_contract COMMAND test_port_sensor_contract)
