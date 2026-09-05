# SPDX-License-Identifier: Apache-2.0
#
# Phase 0 has unit tests only. contract/, scenario/, golden/ and fuzz/ arrive
# together with the ports and screens they exercise.

# Shared harness and fakes. The fake clock lives here rather than under
# platform/host/ because it is a test double, not an adapter that ships.
# png_write is a golden-test diagnostic (dumps a mismatch as PNG); it never
# participates in a pass/fail assertion.
add_library(hac_test_support STATIC
    support/hac_test.c
    support/fake_clock.c
    support/png_write.c
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
)

foreach(test IN LISTS HAC_UNIT_TESTS)
    add_executable(${test} unit/${test}.c)
    target_link_libraries(${test} PRIVATE hac_core hac_test_support)
    hac_apply_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

add_subdirectory(golden)
