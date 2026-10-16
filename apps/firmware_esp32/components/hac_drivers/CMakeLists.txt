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
# With CONFIG_HAC_BSEC (Kconfig beside this file, docs/adr/0020-bsec-iaq.md)
# the BME68x's BSEC source joins them, with Bosch's headers, configuration and
# ESP32-S3 library from cmake/bsec.cmake -- the same configuration the host's
# HAC_BSEC build links. Without it, nothing of BSEC is looked for.
#
# REQUIRES hac_core: drivers include ports/ and, through port_sensor.h,
# core/domain/measurement.h -- nothing from ESP-IDF, which is what
# scripts/check_sources.sh checks.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
include(${HAC_ROOT}/cmake/sources_drivers.cmake)

set(hac_drivers_srcs ${HAC_DRIVERS_SOURCES} ${HAC_DRIVERS_VENDOR_SOURCES})
if(CONFIG_HAC_BSEC)
    include(${HAC_ROOT}/cmake/bsec.cmake)
    hac_bsec_require(${HAC_BSEC_LIB_ESP32S3} ${HAC_BSEC_CONFIG_SOURCES})
    list(APPEND hac_drivers_srcs ${HAC_DRIVERS_BSEC_SOURCES} ${HAC_BSEC_CONFIG_SOURCES})
endif()

idf_component_register(
    SRCS ${hac_drivers_srcs}
    INCLUDE_DIRS ${HAC_DRIVERS_INCLUDES}
    REQUIRES hac_core
)

target_include_directories(${COMPONENT_LIB} SYSTEM PUBLIC ${HAC_DRIVERS_VENDOR_INCLUDES})

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
set_source_files_properties(${HAC_DRIVERS_VENDOR_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${HAC_DRIVERS_VENDOR_COMPILE_OPTIONS}")

if(CONFIG_HAC_BSEC)
    target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE ${HAC_BSEC_INCLUDES})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        HAC_BSEC_CONFIG_NAME="${HAC_BSEC_CONFIG_NAME}")
    set_source_files_properties(${HAC_BSEC_CONFIG_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${HAC_DRIVERS_VENDOR_COMPILE_OPTIONS}")
    add_prebuilt_library(hac_algobsec "${HAC_BSEC_LIB_ESP32S3}")
    target_link_libraries(${COMPONENT_LIB} PRIVATE hac_algobsec)
endif()
