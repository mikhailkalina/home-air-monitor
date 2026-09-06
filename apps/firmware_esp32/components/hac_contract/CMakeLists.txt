# SPDX-License-Identifier: Apache-2.0
#
# The shared contract bodies (tests/contract/port_clock_contract.c,
# port_log_contract.c), compiled for the ESP32-S3.
#
# The source list is NOT repeated here: it comes from the same
# cmake/sources_contract.cmake tests/contract/CMakeLists.txt uses on the
# host, so the object code app_main() runs at boot is built from exactly the
# files CTest already exercises -- see docs/adr/0005 for why running them at
# boot, rather than through idf.py -T, is this phase's answer, and
# docs/hardware/board_notes.md for why the proper test app is deferred to
# phase 2d.
#
# No REQUIRES beyond hac_core: the bodies touch only ports/*.h (port_clock.h,
# port_log.h) and tests/contract's own headers -- no FreeRTOS, no ESP_LOG,
# nothing IDF-specific. That is deliberate (see
# tests/contract/port_clock_contract.h) and this component is where it is
# checked: if a body ever needed an IDF header, it would stop compiling on
# the host and this list would have to grow.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
include(${HAC_ROOT}/cmake/sources_contract.cmake)

idf_component_register(
    SRCS ${HAC_CONTRACT_SOURCES}
    INCLUDE_DIRS ${HAC_CONTRACT_INCLUDES}
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
