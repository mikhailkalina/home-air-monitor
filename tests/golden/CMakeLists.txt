# SPDX-License-Identifier: Apache-2.0
#
# Byte-for-byte comparisons of a rendered test card against tests/golden/*.bin.
# Run with UPDATE_GOLDEN=1 to regenerate the references after an intentional
# rendering change:
#   UPDATE_GOLDEN=1 ./build/host-debug/tests/test_golden_ui

add_executable(test_golden_ui test_golden_ui.c)
target_link_libraries(test_golden_ui PRIVATE hac_core hac_test_support)
target_compile_definitions(test_golden_ui PRIVATE
    HAC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    HAC_DUMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
hac_apply_warnings(test_golden_ui)
add_test(NAME test_golden_ui COMMAND test_golden_ui)
