# SPDX-License-Identifier: Apache-2.0
#
# drivers/: sensor drivers over port_i2c and port_clock, wrapped as
# sensor_source_t (docs/adr/0014-sensor-sources-live-in-drivers.md), with the
# vendored code they build on. They build and are tested on a PC too.
#
# The source lists are NOT repeated here: they come from the same
# cmake/sources_drivers.cmake the host build uses. Vendored sources get the
# relaxed options that file defines, after the project's warning set, and
# their headers are SYSTEM includes -- as on the host.
#
# REQUIRES hac_core: drivers include ports/ and, through port_sensor.h,
# core/domain/measurement.h -- nothing from ESP-IDF, which is what
# scripts/check_sources.sh checks.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
include(${HAC_ROOT}/cmake/sources_drivers.cmake)

idf_component_register(
    SRCS ${HAC_DRIVERS_SOURCES} ${HAC_DRIVERS_VENDOR_SOURCES}
    INCLUDE_DIRS ${HAC_DRIVERS_INCLUDES}
    REQUIRES hac_core
)

target_include_directories(${COMPONENT_LIB} SYSTEM PUBLIC ${HAC_DRIVERS_VENDOR_INCLUDES})

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
set_source_files_properties(${HAC_DRIVERS_VENDOR_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${HAC_DRIVERS_VENDOR_COMPILE_OPTIONS}")
