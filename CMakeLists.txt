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
# The simulator's way to a real MQTT broker, and so to a Home Assistant: over
# libmosquitto, when it is found (platform/host/CMakeLists.txt). Host only:
# nothing of it reaches a firmware image.
option(SIM_MQTT "Build the simulator's MQTT client over libmosquitto, when it is found" ON)
# Off by default and in CI: Bosch's BSEC2 library is downloaded separately and
# never committed (THIRD_PARTY_NOTICES.md). With it, the BME68x's BSEC source
# and its tests build against the x86-64 Linux library Bosch ships
# (docs/adr/0020-bsec-iaq.md); the simulator keeps replaying IAQ from CSV.
option(HAC_BSEC "Build the BME68x's BSEC2 source and its tests (needs third_party/bsec/)" OFF)
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

# The BME68x under BSEC, beside hac_drivers rather than in it, so that a build
# without the library is the build it has always been. Bosch's configuration
# (bsec_iaq.c) is compiled where it lies, under the vendored code's relaxed
# options, and Bosch's headers are SYSTEM includes, as vendored headers are.
if(HAC_BSEC)
    include(cmake/bsec.cmake)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        message(FATAL_ERROR "HAC_BSEC needs a 64-bit x86 Linux host: the only desktop "
                            "library Bosch ships that this build links is bin/gcc/Linux/m64.")
    endif()
    hac_bsec_require(${HAC_BSEC_LIB_LINUX_X86_64} ${HAC_BSEC_CONFIG_SOURCES})
    add_library(hac_drivers_bsec STATIC ${HAC_DRIVERS_BSEC_SOURCES} ${HAC_BSEC_CONFIG_SOURCES})
    target_include_directories(hac_drivers_bsec SYSTEM PRIVATE ${HAC_BSEC_INCLUDES})
    target_compile_definitions(hac_drivers_bsec PRIVATE
        HAC_BSEC_CONFIG_NAME="${HAC_BSEC_CONFIG_NAME}")
    target_link_libraries(hac_drivers_bsec PUBLIC hac_drivers ${HAC_BSEC_LIB_LINUX_X86_64})
    # Users of the source (the tests) learn that it is there.
    target_compile_definitions(hac_drivers_bsec INTERFACE HAC_BSEC=1)
    hac_apply_warnings(hac_drivers_bsec)
    set_source_files_properties(${HAC_BSEC_CONFIG_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${HAC_DRIVERS_VENDOR_COMPILE_OPTIONS}")
endif()

add_subdirectory(platform/host)
add_subdirectory(apps/simulator)
add_subdirectory(apps/slideshow/host)
add_subdirectory(apps/photo_frame/host)

enable_testing()
add_subdirectory(tests)
