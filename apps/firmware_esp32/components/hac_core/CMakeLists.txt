# SPDX-License-Identifier: Apache-2.0
#
# The platform-independent core, compiled for the ESP32-S3.
#
# The source list is NOT repeated here: it comes from the same
# cmake/sources_core.cmake the host build uses, so a file added to the core
# cannot reach one build and miss the other.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
include(${HAC_ROOT}/cmake/sources_core.cmake)

idf_component_register(
    SRCS ${HAC_CORE_SOURCES}
    INCLUDE_DIRS ${HAC_CORE_INCLUDES}
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
