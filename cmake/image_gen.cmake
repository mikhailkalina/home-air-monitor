# SPDX-License-Identifier: Apache-2.0
#
# hac_embed_images(<target> NAME <name> PYTHON <interpreter> IMAGES <png>...)
#
# Turns panel-ready PNGs into C at build time with
# apps/tools/image_gen/image_gen.py embed, and compiles the result into
# <target>: <name>.generated.c joins its sources and the directory holding
# <name>.generated.h its include path. The generated files live in the build
# tree only -- a picture is a few hundred kilobytes of hex as C, and the PNG
# it came from is what the repository keeps (docs/adr/0012-image-pipeline.md).
#
# Included by the host build and by ESP-IDF component CMakeLists alike, so
# both builds embed a picture the same way, as sources_core.cmake keeps them
# compiling the same core. `embed` needs nothing beyond the Python standard
# library, which is what lets ESP-IDF's own Python environment run it.
#
# The includer must define HAC_ROOT as the absolute path of the repository root.

if(NOT DEFINED HAC_ROOT)
    message(FATAL_ERROR "HAC_ROOT must be set before including image_gen.cmake")
endif()

function(hac_embed_images target)
    cmake_parse_arguments(ARG "" "NAME;PYTHON" "IMAGES" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_PYTHON OR NOT ARG_IMAGES)
        message(FATAL_ERROR "hac_embed_images(${target}) needs NAME, PYTHON and IMAGES")
    endif()

    set(script ${HAC_ROOT}/apps/tools/image_gen/image_gen.py)
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/${ARG_NAME})
    set(out_c ${out_dir}/${ARG_NAME}.generated.c)
    set(out_h ${out_dir}/${ARG_NAME}.generated.h)
    list(LENGTH ARG_IMAGES count)

    add_custom_command(
        OUTPUT ${out_c} ${out_h}
        COMMAND ${ARG_PYTHON} ${script} embed --name ${ARG_NAME} --out-dir ${out_dir}
                ${ARG_IMAGES}
        DEPENDS ${script} ${ARG_IMAGES}
        COMMENT "Embedding ${count} image(s) as ${ARG_NAME}"
        VERBATIM
    )

    # The header is listed too, so every source of <target> that includes it
    # waits for the generator rather than racing it.
    target_sources(${target} PRIVATE ${out_c} ${out_h})
    target_include_directories(${target} PRIVATE ${out_dir})
endfunction()
