# SPDX-License-Identifier: Apache-2.0
#
# slideshow_add_images(<target> <python>)
#
# The pictures the slideshow shows: every *.png directly inside
# SLIDESHOW_IMAGES_DIR, in file-name order, embedded into <target> through
# cmake/image_gen.cmake. Shared by apps/slideshow/host/CMakeLists.txt and
# apps/slideshow/esp32/main/CMakeLists.txt, so the window and the panel show
# the same set from the same option:
#
#   cmake --preset host-debug -DSLIDESHOW_IMAGES_DIR=$HOME/slideshow
#   idf.py -DSLIDESHOW_IMAGES_DIR=$HOME/slideshow build
#
# Empty means apps/slideshow/images/, the synthetic test set. A relative path
# is taken from the repository root. The PNGs must be panel-ready -- see
# apps/tools/image_gen/image_gen.py prepare -- or the build stops and says
# which pixel is not. Keep pictures you have no right to publish outside the
# repository: it is public, and git history is permanent.
#
# The includer must define HAC_ROOT.

include(${HAC_ROOT}/cmake/image_gen.cmake)

# STRING rather than PATH: CMake would resolve a relative PATH against the
# build directory, which differs between the host build and idf.py.
set(SLIDESHOW_IMAGES_DIR "" CACHE STRING
    "Directory of panel-ready PNGs for the slideshow; empty = apps/slideshow/images")

function(slideshow_add_images target python)
    set(dir "${SLIDESHOW_IMAGES_DIR}")
    if(dir STREQUAL "")
        set(dir "${HAC_ROOT}/apps/slideshow/images")
    elseif(NOT IS_ABSOLUTE "${dir}")
        set(dir "${HAC_ROOT}/${dir}")
    endif()

    file(GLOB images CONFIGURE_DEPENDS "${dir}/*.png")
    list(SORT images)
    list(LENGTH images count)
    if(count EQUAL 0)
        message(FATAL_ERROR
            "slideshow: no *.png in ${dir}. Make some with "
            "apps/tools/image_gen/image_gen.py prepare (or demo), or point "
            "SLIDESHOW_IMAGES_DIR at a directory that has them.")
    endif()
    message(STATUS "slideshow: ${count} image(s) from ${dir}")

    hac_embed_images(${target} NAME slideshow_images PYTHON ${python} IMAGES ${images})
endfunction()
