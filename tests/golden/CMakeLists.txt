# SPDX-License-Identifier: Apache-2.0
#
# Byte-for-byte comparisons of a rendered framebuffer against
# tests/golden/*.bin. Run with UPDATE_GOLDEN=1 to regenerate the references
# after an intentional rendering change:
#   UPDATE_GOLDEN=1 ./build/host-debug/tests/golden/test_golden_screen_home
#
# golden_check.c needs png_write for its mismatch dumps, which lives in
# hac_platform_host.

set(HAC_GOLDEN_TESTS
    test_golden_ui
    test_golden_screen_home
)

foreach(test IN LISTS HAC_GOLDEN_TESTS)
    add_executable(${test} ${test}.c golden_check.c)
    target_link_libraries(${test} PRIVATE hac_core hac_platform_host hac_test_support)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${test} PRIVATE
        HAC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        HAC_DUMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    )
    hac_apply_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()
