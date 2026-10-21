# SPDX-License-Identifier: Apache-2.0
#
# The on-target test app of docs/architecture.md 9.2: a second ESP-IDF
# project that links the firmware's own components and runs the Unity test
# cases found in their test/ subdirectories. Which components' tests go in is
# chosen at build time with -T (see idf_ext.py beside this file):
#
#   . $HOME/esp/esp-idf/export.sh
#   cd apps/firmware_esp32/test_app
#   idf.py -T hac_platform build
#
# The image runs every linked test case once at boot and prints Unity's
# summary; there is no interactive menu, because this project flashes from a
# different machine than it builds on (CLAUDE.md, "Environment") and the log
# is all that comes back.
#
# It is configured from the firmware's own sdkconfig.defaults first and this
# directory's second, so the adapters are tested under the firmware's tick
# rate, console and partition table rather than under defaults of their own
# -- the delay_ms() contract in particular is a statement about one tick
# rate (docs/adr/0006).

cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
)

# With -D HAC_BSEC=ON, the firmware's BSEC overlay as well: the BME68x's BSEC
# source, and its port_sensor contract case, are built in (phase 3d,
# docs/adr/0020-bsec-iaq.md). Needs Bosch's library in third_party/bsec/.
# In a build directory of its own, as the firmware's BSEC image is:
#
#   idf.py -T hac_platform -B build-bsec -D SDKCONFIG=build-bsec/sdkconfig \
#       -D HAC_BSEC=ON build
if(HAC_BSEC)
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.bsec")
endif()

# The firmware's Wi-Fi credentials overlay, when there is one (phase 4e,
# docs/adr/0032-wifi-on-the-board.md): the port_net contract joins the same
# network the firmware does, and is IGNORED without one.
get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
include(${HAC_ROOT}/cmake/credentials_idf.cmake)
set(HAC_CREDENTIALS_OVERLAY "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.credentials")
hac_credentials_layer("${HAC_CREDENTIALS_OVERLAY}")

# The firmware's components, not copies of them: hac_core, hac_contract,
# hac_drivers, hac_platform and, through hac_platform's manifest, EPDiy.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hac_test_app)

hac_credentials_check("${HAC_CREDENTIALS_OVERLAY}")
