# SPDX-License-Identifier: Apache-2.0
#
# Host adapters that need no SDL and no display: the virtual clock and CSV
# scenario replay. adp_display_sdl / sim_gui arrive in a later phase.

add_library(hac_platform_host STATIC
    adp_clock_virtual.c
    replay/replay_source.c
    replay/fault_inject.c
)
target_include_directories(hac_platform_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(hac_platform_host PUBLIC hac_core)
hac_apply_warnings(hac_platform_host)
