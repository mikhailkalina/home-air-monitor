# SPDX-License-Identifier: Apache-2.0
#
# The slideshow test app on a PC: composition only, like apps/simulator.
# The pictures are embedded at build time (../slideshow_images.cmake), which
# takes a Python 3 interpreter -- standard library only. Without one the app
# is left out of the build rather than failing it, as the simulator's window
# is left out without SDL2.
#
#   cmake --preset host-debug -DSLIDESHOW_IMAGES_DIR=$HOME/pictures

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING "Python 3 not found: the slideshow test app is not built.")
    return()
endif()

include(${HAC_ROOT}/apps/slideshow/slideshow_images.cmake)

# STRING rather than PATH: CMake would resolve a relative PATH against the
# build directory, and this one is meant from the repository root.
set(SLIDESHOW_IMAGES_DIR "" CACHE STRING
    "Directory of panel-ready PNGs for the slideshow; empty = apps/slideshow/images")

slideshow_images_in(slideshow_images "${SLIDESHOW_IMAGES_DIR}" apps/slideshow/images)
if(NOT slideshow_images)
    message(FATAL_ERROR
        "slideshow: no *.png to show. Make some with apps/tools/image_gen/image_gen.py "
        "prepare (or demo), or point SLIDESHOW_IMAGES_DIR at a directory that has them.")
endif()

add_executable(slideshow
    main.c
    ${HAC_ROOT}/apps/slideshow/slideshow.c
)
target_include_directories(slideshow PRIVATE ${HAC_ROOT}/apps/slideshow)
target_link_libraries(slideshow PRIVATE hac_core hac_platform_host)

if(TARGET hac_platform_host_gui)
    target_link_libraries(slideshow PRIVATE hac_platform_host_gui)
endif()

slideshow_embed_images(slideshow ${Python3_EXECUTABLE} ${slideshow_images})
hac_apply_warnings(slideshow)
