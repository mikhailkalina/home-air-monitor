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

# port_clock and port_log: the contract bodies themselves
# (port_clock_contract.c, port_log_contract.c) are NOT compiled directly by
# this file. They come from cmake/sources_contract.cmake, the same list
# apps/firmware_esp32/components/hac_contract/CMakeLists.txt includes, so the
# object code exercised here is exactly what runs at device boot -- see
# docs/adr/0005-contract-suites-run-at-boot.md. This CMakeLists.txt supplies
# only the host runners that adapt the shared bodies to HAC_CHECK.
include(${HAC_ROOT}/cmake/sources_contract.cmake)

add_library(hac_contract_bodies STATIC ${HAC_CONTRACT_SOURCES})
target_include_directories(hac_contract_bodies PUBLIC ${HAC_CONTRACT_INCLUDES})
hac_apply_warnings(hac_contract_bodies)

add_executable(test_port_clock_contract test_port_clock_contract.c)
target_link_libraries(test_port_clock_contract PRIVATE
    hac_contract_bodies hac_platform_host hac_test_support)
hac_apply_warnings(test_port_clock_contract)
add_test(NAME test_port_clock_contract COMMAND test_port_clock_contract)

add_executable(test_port_log_contract test_port_log_contract.c)
target_link_libraries(test_port_log_contract PRIVATE
    hac_contract_bodies hac_platform_host hac_test_support)
hac_apply_warnings(test_port_log_contract)
add_test(NAME test_port_log_contract COMMAND test_port_log_contract)
