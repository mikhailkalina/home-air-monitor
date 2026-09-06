# SPDX-License-Identifier: Apache-2.0
#
# Composition only: the simulator is an application that wires adapters to the
# core, exactly as apps/firmware_esp32 does for the board.
#
# hac_platform_host_gui exists only when SIM_GUI is ON and SDL2 was found; it
# carries the HAC_SIM_GUI definition, so main.c falls back to the headless
# display when it is absent.

add_executable(simulator main.c)
target_link_libraries(simulator PRIVATE hac_core hac_platform_host)

if(TARGET hac_platform_host_gui)
    target_link_libraries(simulator PRIVATE hac_platform_host_gui)
endif()

hac_apply_warnings(simulator)
