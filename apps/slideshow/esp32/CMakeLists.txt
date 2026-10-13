# SPDX-License-Identifier: Apache-2.0
#
# The slideshow test app on the board: an ESP-IDF project of its own that
# links the firmware's components -- hac_core, hac_platform and, through
# hac_platform's manifest, EPDiy -- rather than copies of them, as the
# on-target test app does (apps/firmware_esp32/test_app/).
#
#   . $HOME/esp/esp-idf/export.sh
#   cd apps/slideshow/esp32
#   idf.py build                                      # the synthetic test set
#   idf.py -DSLIDESHOW_IMAGES_DIR=$HOME/slideshow build   # your own pictures
#
# Configured from the firmware's sdkconfig.defaults first and this
# directory's second, so the panel is driven with the firmware's EPDiy render
# method, PSRAM and partition table: the flash command differs from the
# firmware's only in the image it writes.

cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware_esp32/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../firmware_esp32/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hac_slideshow)
