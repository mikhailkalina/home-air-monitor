# SPDX-License-Identifier: Apache-2.0
#
# Host build: the platform-independent core plus its tests.
#
# The ESP-IDF build lives in apps/firmware_esp32 and is driven by idf.py; it
# reuses cmake/sources_core.cmake rather than repeating the file list.
#
# drivers/ arrived in phase 3a (docs/adr/0014-sensor-sources-live-in-drivers.md)
# and builds from cmake/sources_drivers.cmake, the same list the firmware's
# hac_drivers component uses.

cmake_minimum_required(VERSION 3.20)

# C only. CXX is enabled when the C++17 control panel under
# platform/host/sim_gui/ arrives; the SDL display and input adapters are C11
# like the rest of platform/.
project(home_air_monitor C)

set(HAC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# With SIM_GUI=OFF, platform/host does not even look for SDL2, so the
# ci-headless preset configures and builds on a machine that has none.
option(SIM_GUI "Build the SDL2 simulator window" ON)
set(HAC_SANITIZERS "" CACHE STRING "Sanitizers to enable, e.g. address,undefined")

include(cmake/sources_core.cmake)
include(cmake/warnings.cmake)

add_library(hac_core STATIC ${HAC_CORE_SOURCES})
target_include_directories(hac_core PUBLIC ${HAC_CORE_INCLUDES})
hac_apply_warnings(hac_core)

# core/ uses <math.h>, which is a separate library on most Unix toolchains.
find_library(HAC_LIBM m)
if(HAC_LIBM)
    target_link_libraries(hac_core PUBLIC ${HAC_LIBM})
endif()

# Drivers over port_i2c, with the vendored code they wrap. One library, as in
# the ESP-IDF component: the vendor's code and the HAL glue call each other.
include(cmake/sources_drivers.cmake)
add_library(hac_drivers STATIC ${HAC_DRIVERS_SOURCES} ${HAC_DRIVERS_VENDOR_SOURCES})
target_include_directories(hac_drivers PUBLIC ${HAC_DRIVERS_INCLUDES})
target_include_directories(hac_drivers SYSTEM PUBLIC ${HAC_DRIVERS_VENDOR_INCLUDES})
target_link_libraries(hac_drivers PUBLIC hac_core)
hac_apply_warnings(hac_drivers)
# Source-file options come after the target's on the command line, so these
# win over the -W flags hac_apply_warnings() just set.
set_source_files_properties(${HAC_DRIVERS_VENDOR_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${HAC_DRIVERS_VENDOR_COMPILE_OPTIONS}")

add_subdirectory(platform/host)
add_subdirectory(apps/simulator)
add_subdirectory(apps/slideshow/host)
add_subdirectory(apps/photo_frame/host)

enable_testing()
add_subdirectory(tests)
