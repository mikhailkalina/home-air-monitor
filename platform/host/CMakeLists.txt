# SPDX-License-Identifier: Apache-2.0
#
# The host adapters.
#
# hac_platform_host needs nothing beyond libc: the virtual clock, CSV scenario
# replay, the e-paper model, the headless PNG display, the fake I2C bus with
# its device models, and the simulated network with an MQTT broker model.
# This is what CI builds, and what the ci-headless preset must be able to
# build on a machine with no SDL2 installed at all.
#
# hac_platform_host_mqtt adds port_telemetry over libmosquitto, to a real
# broker (the simulator's --broker), and hac_platform_host_gui the SDL window
# and mouse-to-touch input. Each is a separate target so that neither library
# is a link dependency of anything else, and each is only created when its
# library is actually present.

add_library(hac_platform_host STATIC
    adp_clock_virtual.c
    adp_display_png.c
    adp_files_dir.c
    adp_i2c_fake.c
    adp_input_script.c
    adp_log_console.c
    adp_net_sim.c
    adp_telemetry_loopback.c
    epd_model.c
    host_time.c
    i2c_models/bme680_model.c
    i2c_models/gt911_model.c
    i2c_models/regfile_model.c
    i2c_models/scd41_model.c
    png_write.c
    scenario_world.c
    replay/replay_source.c
    replay/fault_inject.c
)
target_include_directories(hac_platform_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(hac_platform_host PUBLIC hac_core)
hac_apply_warnings(hac_platform_host)

# libmosquitto, 2.0 or later (THIRD_PARTY_NOTICES.md): its header and its
# library, the version read from the header -- no pkg-config, whose file
# names absolute paths, so that a copy unpacked anywhere is found through
# CMAKE_PREFIX_PATH as well. Without it the simulator has no --broker, and
# the port_telemetry contract's case against a real broker says it was not
# exercised (docs/adr/0030-telemetry-ports-and-the-reporter.md, amendment).
if(SIM_MQTT)
    find_path(HAC_MOSQUITTO_INCLUDE_DIR mosquitto.h)
    find_library(HAC_MOSQUITTO_LIBRARY mosquitto)
    set(HAC_MOSQUITTO_VERSION "")
    if(HAC_MOSQUITTO_INCLUDE_DIR)
        file(STRINGS ${HAC_MOSQUITTO_INCLUDE_DIR}/mosquitto.h version_lines
             REGEX "^#define LIBMOSQUITTO_(MAJOR|MINOR|REVISION) [0-9]+")
        foreach(part MAJOR MINOR REVISION)
            string(REGEX REPLACE ".*LIBMOSQUITTO_${part} ([0-9]+).*" "\\1" mosquitto_${part}
                   "${version_lines}")
        endforeach()
        set(HAC_MOSQUITTO_VERSION
            "${mosquitto_MAJOR}.${mosquitto_MINOR}.${mosquitto_REVISION}")
    endif()
    find_package(Threads)
    if(HAC_MOSQUITTO_INCLUDE_DIR AND HAC_MOSQUITTO_LIBRARY AND Threads_FOUND
       AND HAC_MOSQUITTO_VERSION VERSION_GREATER_EQUAL 2.0)
        add_library(hac_platform_host_mqtt STATIC adp_telemetry_mqtt.c)
        target_include_directories(hac_platform_host_mqtt SYSTEM PRIVATE
            ${HAC_MOSQUITTO_INCLUDE_DIR})
        target_link_libraries(hac_platform_host_mqtt PUBLIC
            hac_platform_host ${HAC_MOSQUITTO_LIBRARY} Threads::Threads)
        # PUBLIC, so that the simulator and the contract runner compile their
        # broker branch only when this target is part of the build.
        target_compile_definitions(hac_platform_host_mqtt PUBLIC HAC_SIM_MQTT=1)
        hac_apply_warnings(hac_platform_host_mqtt)
        message(STATUS "libmosquitto ${HAC_MOSQUITTO_VERSION}: the simulator has --broker")
    else()
        set(found "")
        if(HAC_MOSQUITTO_VERSION)
            set(found " (found ${HAC_MOSQUITTO_VERSION})")
        endif()
        message(STATUS "libmosquitto 2.x not found${found}: the simulator has no --broker. "
                       "Install libmosquitto-dev, or configure with -DSIM_MQTT=OFF to stop "
                       "looking.")
    endif()
endif()

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
