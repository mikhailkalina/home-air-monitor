# SPDX-License-Identifier: Apache-2.0
#
# The slideshow's main component, compiled from where it lives: the same
# main.c, the same Kconfig menu, different pictures.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
set(SLIDESHOW_MAIN ${HAC_ROOT}/apps/slideshow/esp32/main)

idf_component_register(
    SRCS
        ${SLIDESHOW_MAIN}/main.c
        ${HAC_ROOT}/apps/slideshow/slideshow.c
    INCLUDE_DIRS ${HAC_ROOT}/apps/slideshow
    REQUIRES hac_core hac_platform heap
    KCONFIG_PROJBUILD ${SLIDESHOW_MAIN}/Kconfig.projbuild
)

include(${HAC_ROOT}/apps/slideshow/slideshow_images.cmake)

# idf.py -DPHOTO_FRAME_IMAGES_DIR=$HOME/photo-frame build
set(PHOTO_FRAME_IMAGES_DIR "" CACHE STRING
    "Directory of panel-ready PNGs for the photo frame; empty = apps/photo_frame/images")

slideshow_images_in(photo_frame_images "${PHOTO_FRAME_IMAGES_DIR}" apps/photo_frame/images)
if(NOT photo_frame_images)
    message(FATAL_ERROR
        "photo_frame: no photographs to show in apps/photo_frame/images (or "
        "PHOTO_FRAME_IMAGES_DIR). Prepare them with apps/tools/image_gen/image_gen.py "
        "prepare (docs/adr/0012-image-pipeline.md, \"The photo frame\").")
endif()

idf_build_get_property(python PYTHON)
slideshow_embed_images(${COMPONENT_LIB} ${python} ${photo_frame_images})

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
