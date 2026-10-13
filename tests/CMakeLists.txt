# SPDX-License-Identifier: Apache-2.0
#
# scenario/ arrives together with app_core (see docs/architecture.md §10).

# Shared harness and fakes. The fake clock lives here rather than under
# platform/host/ because it is a test double, not an adapter that ships.
add_library(hac_test_support STATIC
    support/hac_test.c
    support/fake_clock.c
)
target_include_directories(hac_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(hac_test_support PUBLIC hac_core)  # for ports/ and core/ headers
hac_apply_warnings(hac_test_support)

# One test file is one executable and one CTest entry.
set(HAC_UNIT_TESTS
    test_hal_status
    test_fake_clock
    test_measurement
    test_framebuffer
    test_gfx
    test_update_policy
    test_view_model
    test_adp_clock_virtual
    test_replay_source
    test_fault_inject
    test_epd_model
)

foreach(test IN LISTS HAC_UNIT_TESTS)
    add_executable(${test} unit/${test}.c)
    target_link_libraries(${test} PRIVATE hac_core hac_test_support)
    hac_apply_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The platform/host tests additionally need adp_clock_virtual and
# replay/fault_inject; test_replay_source also writes scratch CSV files
# under its own binary directory for the duration of one check.
target_link_libraries(test_adp_clock_virtual PRIVATE hac_platform_host)
target_link_libraries(test_replay_source PRIVATE hac_platform_host)
target_link_libraries(test_fault_inject PRIVATE hac_platform_host)
target_link_libraries(test_epd_model PRIVATE hac_platform_host)
target_compile_definitions(test_replay_source PRIVATE
    HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    HAC_SCENARIOS_DIR="${HAC_ROOT}/scenarios"
)

# The one pure piece of the firmware's panel calibration mode, compiled from
# where the firmware compiles it: apps/firmware_esp32/main/ is otherwise built
# only by idf.py, so this is the only way its arithmetic gets a host test.
add_executable(test_duration_stats
    unit/test_duration_stats.c
    ${HAC_ROOT}/apps/firmware_esp32/main/duration_stats.c
)
target_include_directories(test_duration_stats PRIVATE ${HAC_ROOT}/apps/firmware_esp32/main)
target_link_libraries(test_duration_stats PRIVATE hac_test_support)
hac_apply_warnings(test_duration_stats)
add_test(NAME test_duration_stats COMMAND test_duration_stats)

# The firmware's I2C bus scan, against the host's fake bus.
add_executable(test_i2c_scan
    unit/test_i2c_scan.c
    ${HAC_ROOT}/apps/firmware_esp32/main/i2c_scan.c
)
target_include_directories(test_i2c_scan PRIVATE ${HAC_ROOT}/apps/firmware_esp32/main)
target_link_libraries(test_i2c_scan PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_i2c_scan)
add_test(NAME test_i2c_scan COMMAND test_i2c_scan)

# The pure half of the ESP32 I2C adapter -- deadline-to-driver-wait
# arithmetic and failure classification -- compiled from where the firmware
# compiles it, like duration_stats.c above. adp_i2c.c itself needs ESP-IDF
# and is exercised on the device by the port_i2c contract.
add_executable(test_adp_i2c_pure
    unit/test_adp_i2c_pure.c
    ${HAC_ROOT}/platform/esp32_t5s3/adp_i2c_pure.c
)
target_include_directories(test_adp_i2c_pure PRIVATE ${HAC_ROOT}/platform/esp32_t5s3)
target_link_libraries(test_adp_i2c_pure PRIVATE hac_test_support)
hac_apply_warnings(test_adp_i2c_pure)
add_test(NAME test_adp_i2c_pure COMMAND test_adp_i2c_pure)

# The device's panel limits against the simulator's. Only board_config.h's
# directory is added -- it is plain data -- not anything that builds for the
# ESP32.
add_executable(test_panel_limits_agree unit/test_panel_limits_agree.c)
target_include_directories(test_panel_limits_agree PRIVATE ${HAC_ROOT}/platform/esp32_t5s3)
target_link_libraries(test_panel_limits_agree PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_panel_limits_agree)
add_test(NAME test_panel_limits_agree COMMAND test_panel_limits_agree)

add_subdirectory(golden)
add_subdirectory(contract)
add_subdirectory(fuzz)
