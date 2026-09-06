# SPDX-License-Identifier: Apache-2.0
#
# THE source list for core/. It is included by both the host build
# (root CMakeLists.txt) and the ESP-IDF build
# (apps/firmware_esp32/components/hac_core/CMakeLists.txt), so a file added
# here reaches both and the two builds cannot drift apart.
#
# Never copy these paths into a component CMakeLists.txt.
#
# The includer must define HAC_ROOT as the absolute path of the repository root.

if(NOT DEFINED HAC_ROOT)
    message(FATAL_ERROR "HAC_ROOT must be set before including sources_core.cmake")
endif()

set(HAC_CORE_SOURCES
    ${HAC_ROOT}/core/domain/measurement.c
    ${HAC_ROOT}/core/app/update_policy.c
    ${HAC_ROOT}/core/ui/framebuffer.c
    ${HAC_ROOT}/core/ui/gfx.c
    ${HAC_ROOT}/core/ui/view_model.c
    ${HAC_ROOT}/core/ui/screen_home.c
    ${HAC_ROOT}/core/ui/fonts/font_dejavu_sans_16.generated.c
    ${HAC_ROOT}/core/ui/fonts/font_dejavu_sans_32.generated.c
    ${HAC_ROOT}/core/ui/fonts/font_dejavu_sans_64.generated.c
)

# core/ resolves headers as "domain/measurement.h", ports/ as "port_clock.h".
set(HAC_CORE_INCLUDES
    ${HAC_ROOT}/core
    ${HAC_ROOT}/ports
)
