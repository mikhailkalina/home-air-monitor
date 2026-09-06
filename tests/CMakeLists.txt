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

add_subdirectory(golden)
add_subdirectory(contract)
add_subdirectory(fuzz)
