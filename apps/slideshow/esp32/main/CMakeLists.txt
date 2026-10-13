# SPDX-License-Identifier: Apache-2.0

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)

idf_component_register(
    SRCS
        main.c
        ${HAC_ROOT}/apps/slideshow/slideshow.c
    INCLUDE_DIRS . ${HAC_ROOT}/apps/slideshow
    REQUIRES hac_core hac_platform heap
)

# The pictures, embedded at build time by the same helper the host build
# uses. ESP-IDF's Python environment has no Pillow, and needs none: `embed`
# is standard library only (docs/adr/0012-image-pipeline.md). After
# idf_component_register(), which returns early while ESP-IDF only collects
# requirements, so this runs in the real build alone.
include(${HAC_ROOT}/apps/slideshow/slideshow_images.cmake)
idf_build_get_property(python PYTHON)
slideshow_add_images(${COMPONENT_LIB} ${python})

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
