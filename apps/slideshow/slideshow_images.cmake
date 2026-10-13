# SPDX-License-Identifier: Apache-2.0
#
# Where a slideshow's pictures come from, shared by every composition that
# runs apps/slideshow/slideshow.c: the slideshow test app (apps/slideshow/)
# and the photo frame (apps/photo_frame/), each on the host and on the board.
#
#   slideshow_images_in(<out_var> <dir> <default_dir>)
#       Every *.png directly inside <dir> -- or <default_dir> when <dir> is
#       empty -- in file-name order. A relative path is taken from the
#       repository root. Empty when there are none, or the directory does
#       not exist; what that means is the caller's to decide.
#
#   slideshow_embed_images(<target> <python> <png>...)
#       Embeds them into <target> as slideshow_images_all[] and
#       slideshow_images_names[] (slideshow_images.generated.h), through
#       cmake/image_gen.cmake.
#
# The PNGs must be panel-ready -- see apps/tools/image_gen/image_gen.py
# prepare -- or the build stops and says which pixel is not. Keep pictures
# you have no right to publish outside git: the repository is public, and
# its history is permanent.
#
# The includer must define HAC_ROOT.

include(${HAC_ROOT}/cmake/image_gen.cmake)

function(slideshow_images_in out_var dir default_dir)
    if("${dir}" STREQUAL "")
        set(dir "${default_dir}")
    endif()
    if(NOT IS_ABSOLUTE "${dir}")
        set(dir "${HAC_ROOT}/${dir}")
    endif()

    file(GLOB images CONFIGURE_DEPENDS "${dir}/*.png")
    list(SORT images)
    list(LENGTH images count)
    message(STATUS "${out_var}: ${count} image(s) in ${dir}")
    set(${out_var} "${images}" PARENT_SCOPE)
endfunction()

function(slideshow_embed_images target python)
    hac_embed_images(${target} NAME slideshow_images PYTHON ${python} IMAGES ${ARGN})
endfunction()
