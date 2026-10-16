# SPDX-License-Identifier: Apache-2.0
#
# Contract suites: one shared body per port, run against every implementation
# of it (see docs/architecture.md §5.3 and §9.2).
#
# The contract bodies themselves (port_clock_contract.c, port_log_contract.c,
# port_i2c_contract.c, port_sensor_contract.c, port_files_contract.c) are NOT
# compiled directly by this file. They come from
# cmake/sources_contract.cmake, the same list
# apps/firmware_esp32/components/hac_contract/CMakeLists.txt includes, so the
# object code exercised here is exactly what the device runs -- at boot for
# port_clock and port_log (docs/adr/0005-contract-suites-run-at-boot.md), in
# the test app for the rest (docs/adr/0009, 0011, 0024). This CMakeLists.txt
# supplies only the host runners that adapt the shared bodies to HAC_CHECK.
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

# port_i2c: the shared body against the register-level fake bus, plus the
# deliberately broken wrappers that prove the body catches what it claims to.
# The same body runs on the device in the test app (ADR 0011), never at boot.
add_executable(test_port_i2c_contract test_port_i2c_contract.c)
target_link_libraries(test_port_i2c_contract PRIVATE
    hac_contract_bodies hac_platform_host hac_test_support)
hac_apply_warnings(test_port_i2c_contract)
add_test(NAME test_port_i2c_contract COMMAND test_port_i2c_contract)

# port_sensor: the shared body against replay_source, and against
# scd4x_source and bme68x_source over the fake bus and their device models,
# plus broken wrappers that prove the body catches what it claims to.
add_executable(test_port_sensor_contract test_port_sensor_contract.c)
target_link_libraries(test_port_sensor_contract PRIVATE
    hac_contract_bodies hac_drivers hac_platform_host hac_test_support)
target_compile_definitions(test_port_sensor_contract PRIVATE
    HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
# With Bosch's BSEC2 library (HAC_BSEC), the BME68x as BSEC measures it too.
if(HAC_BSEC)
    target_link_libraries(test_port_sensor_contract PRIVATE hac_drivers_bsec)
endif()
hac_apply_warnings(test_port_sensor_contract)
add_test(NAME test_port_sensor_contract COMMAND test_port_sensor_contract)

# port_files: the shared body against adp_files_dir in a scratch directory,
# plus broken wrappers that prove the body catches what it claims to. The
# same body runs on the board's card in the test app (docs/adr/0024).
add_executable(test_port_files_contract test_port_files_contract.c)
target_link_libraries(test_port_files_contract PRIVATE
    hac_contract_bodies hac_platform_host hac_test_support)
target_compile_definitions(test_port_files_contract PRIVATE
    HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
hac_apply_warnings(test_port_files_contract)
add_test(NAME test_port_files_contract COMMAND test_port_files_contract)
