# SPDX-License-Identifier: Apache-2.0
#
# The host adapters.
#
# hac_platform_host needs nothing beyond libc: the virtual clock, CSV scenario
# replay, the e-paper model, and the headless PNG display. This is what CI
# builds, and what the ci-headless preset must be able to build on a machine
# with no SDL2 installed at all.
#
# hac_platform_host_gui adds the SDL window and mouse-to-touch input. It is a
# separate target so that SDL2 is not a link dependency of anything headless,
# and it is only created when SDL2 is actually present.

add_library(hac_platform_host STATIC
    adp_clock_virtual.c
    adp_display_png.c
    adp_log_console.c
    epd_model.c
    host_time.c
    png_write.c
    replay/replay_source.c
    replay/fault_inject.c
)
target_include_directories(hac_platform_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(hac_platform_host PUBLIC hac_core)
hac_apply_warnings(hac_platform_host)

if(NOT SIM_GUI)
    message(STATUS "SIM_GUI=OFF: SDL2 is not looked for, and the simulator is headless only")
    return()
endif()

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(WARNING
        "SIM_GUI=ON but SDL2 was not found. Building the simulator headless only; "
        "run it with --headless. Configure with -DSIM_GUI=OFF to silence this.")
    return()
endif()

add_library(hac_platform_host_gui STATIC
    adp_display_sdl.c
    adp_input_sdl.c
)
target_link_libraries(hac_platform_host_gui PUBLIC hac_platform_host)

# HAC_SIM_GUI is PUBLIC so that apps/simulator/main.c compiles the windowed
# branch only when this target is actually part of the build.
target_compile_definitions(hac_platform_host_gui PUBLIC HAC_SIM_GUI=1)

if(TARGET SDL2::SDL2)
    target_link_libraries(hac_platform_host_gui PUBLIC SDL2::SDL2)
else()
    # An SDL2 package that exports variables rather than imported targets.
    # SYSTEM: the project builds with -Werror, and third-party headers must not
    # be able to fail the build.
    target_include_directories(hac_platform_host_gui SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(hac_platform_host_gui PUBLIC ${SDL2_LIBRARIES})
endif()

hac_apply_warnings(hac_platform_host_gui)
