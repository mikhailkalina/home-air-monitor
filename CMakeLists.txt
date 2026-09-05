# SPDX-License-Identifier: Apache-2.0
#
# Host build: the platform-independent core plus its tests.
#
# The ESP-IDF build lives in apps/firmware_esp32 and is driven by idf.py; it
# reuses cmake/sources_core.cmake rather than repeating the file list.
#
# Phase 1b adds platform/host/ (virtual clock, CSV scenario replay). drivers/
# and apps/simulator/ are added by their own phases (see docs/architecture.md §10).

cmake_minimum_required(VERSION 3.20)

# C only for now. CXX is enabled in phase 1, together with the C++17 simulator
# GUI under platform/host/sim_gui/.
project(home_air_monitor C)

set(HAC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Consumed from phase 1 onwards; declared here so the presets that set it do
# not trip CMake's "manually-specified variables were not used" warning.
option(SIM_GUI "Build the SDL2 simulator GUI" ON)
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

add_subdirectory(platform/host)

enable_testing()
add_subdirectory(tests)
