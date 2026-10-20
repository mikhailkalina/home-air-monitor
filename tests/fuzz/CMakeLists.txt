# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer target for replay_parse_buffer(). -fsanitize=fuzzer is a
# Clang/LLVM feature, so this is silently skipped on the gcc-only builds this
# repo otherwise uses (the CI "build" and "asan" jobs, and this project's
# default toolchain); it is not wired to ctest; see docs/architecture.md §9.1.
#
# Run for real with Clang, seeded from corpus/ (recordings as the device
# writes them) and the bundled scenarios, e.g.:
#   cmake --preset host-debug -DCMAKE_C_COMPILER=clang
#   cmake --build build/host-debug --target fuzz_replay_parser
#   mkdir -p build/fuzz-corpus
#   ./build/host-debug/tests/fuzz/fuzz_replay_parser -max_total_time=60 \
#       build/fuzz-corpus tests/fuzz/corpus scenarios

# The parser and what it calls are compiled into the target itself, not
# linked from hac_platform_host and hac_core: -fsanitize=fuzzer instruments
# only what it compiles, and against the libraries the fuzzer saw nothing but
# this harness -- 4 coverage points, mutating blind (found in phase 3f). The
# list is the parser's own closure, so it is not a second copy of
# cmake/sources_core.cmake: a file the parser does not reach does not belong
# here.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_executable(fuzz_replay_parser
        fuzz_replay_parser.c
        ${HAC_ROOT}/platform/host/replay/replay_source.c
        ${HAC_ROOT}/core/domain/civil_time.c
        ${HAC_ROOT}/core/domain/measurement.c
        ${HAC_ROOT}/core/recording/record_format.c
        ${HAC_ROOT}/core/text/text_append.c
    )
    target_include_directories(fuzz_replay_parser PRIVATE
        ${HAC_ROOT}/platform/host ${HAC_CORE_INCLUDES})
    if(HAC_LIBM)
        target_link_libraries(fuzz_replay_parser PRIVATE ${HAC_LIBM})
    endif()
    target_compile_options(fuzz_replay_parser PRIVATE -fsanitize=fuzzer,address,undefined -g)
    target_link_options(fuzz_replay_parser PRIVATE -fsanitize=fuzzer,address,undefined)
else()
    message(STATUS "fuzz_replay_parser skipped: requires Clang (found ${CMAKE_C_COMPILER_ID})")
endif()
