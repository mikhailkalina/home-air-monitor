# SPDX-License-Identifier: Apache-2.0
#
# The photo frame on a PC: the slideshow's composition
# (apps/slideshow/host/main.c) built with photographs instead of test
# pictures, and without captions. Nothing here is code of its own -- the
# frame differs from the slideshow only in what it shows.
#
# The photographs are private, so they are never in the repository: they live
# in the gitignored apps/photo_frame/images/, or wherever
# PHOTO_FRAME_IMAGES_DIR points. With none there -- a fresh clone, CI -- the
# frame is simply not built.
#
#   cmake --preset host-debug -DPHOTO_FRAME_IMAGES_DIR=$HOME/photo-frame

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    return()  # apps/slideshow/host has already said why
endif()

include(${HAC_ROOT}/apps/slideshow/slideshow_images.cmake)

set(PHOTO_FRAME_IMAGES_DIR "" CACHE STRING
    "Directory of panel-ready PNGs for the photo frame; empty = apps/photo_frame/images")

slideshow_images_in(photo_frame_images "${PHOTO_FRAME_IMAGES_DIR}" apps/photo_frame/images)
if(NOT photo_frame_images)
    message(STATUS "photo_frame: no photographs prepared, so it is not built")
    return()
endif()

add_executable(photo_frame
    ${HAC_ROOT}/apps/slideshow/host/main.c
    ${HAC_ROOT}/apps/slideshow/slideshow.c
)
target_include_directories(photo_frame PRIVATE ${HAC_ROOT}/apps/slideshow)
target_compile_definitions(photo_frame PRIVATE SLIDESHOW_DEFAULT_CAPTION=0)
target_link_libraries(photo_frame PRIVATE hac_core hac_platform_host)

if(TARGET hac_platform_host_gui)
    target_link_libraries(photo_frame PRIVATE hac_platform_host_gui)
endif()

slideshow_embed_images(photo_frame ${Python3_EXECUTABLE} ${photo_frame_images})
hac_apply_warnings(photo_frame)
