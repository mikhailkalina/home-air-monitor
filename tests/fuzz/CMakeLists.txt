# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer target for replay_parse_buffer(). -fsanitize=fuzzer is a
# Clang/LLVM feature, so this is silently skipped on the gcc-only builds this
# repo otherwise uses (the CI "build" and "asan" jobs, and this project's
# default toolchain); it is not wired to ctest; see docs/architecture.md §9.1.
#
# Run for real with Clang, e.g.:
#   cmake --preset host-debug -DCMAKE_C_COMPILER=clang
#   cmake --build build/host-debug --target fuzz_replay_parser
#   ./build/host-debug/tests/fuzz/fuzz_replay_parser -max_total_time=60

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_executable(fuzz_replay_parser fuzz_replay_parser.c)
    target_link_libraries(fuzz_replay_parser PRIVATE hac_platform_host)
    target_compile_options(fuzz_replay_parser PRIVATE -fsanitize=fuzzer,address -g)
    target_link_options(fuzz_replay_parser PRIVATE -fsanitize=fuzzer,address)
else()
    message(STATUS "fuzz_replay_parser skipped: requires Clang (found ${CMAKE_C_COMPILER_ID})")
endif()
