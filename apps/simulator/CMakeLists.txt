# SPDX-License-Identifier: Apache-2.0
#
# Composition only: the simulator is an application that wires adapters to the
# core, exactly as apps/firmware_esp32 does for the board. It links the
# drivers for --sensors fake-i2c, where the board's own SCD4x and BME68x code
# reads device models on the fake bus.
#
# hac_platform_host_gui exists only when SIM_GUI is ON and SDL2 was found; it
# carries the HAC_SIM_GUI definition, so main.c falls back to the headless
# display when it is absent. hac_platform_host_mqtt, the same for
# libmosquitto and --broker.

add_executable(simulator main.c)
target_link_libraries(simulator PRIVATE hac_core hac_drivers hac_platform_host)

if(TARGET hac_platform_host_gui)
    target_link_libraries(simulator PRIVATE hac_platform_host_gui)
endif()

# hac_platform_host_mqtt exists only when libmosquitto was found; it carries
# HAC_SIM_MQTT, so main.c offers --broker only then.
if(TARGET hac_platform_host_mqtt)
    target_link_libraries(simulator PRIVATE hac_platform_host_mqtt)
endif()

# board_config.h only, for the addresses the board's sensors answer at -- it
# is plain data, as tests/unit/test_panel_limits_agree.c includes it too.
# Nothing else under platform/esp32_t5s3/ builds for the host.
target_include_directories(simulator PRIVATE ${HAC_ROOT}/platform/esp32_t5s3)

hac_apply_warnings(simulator)
