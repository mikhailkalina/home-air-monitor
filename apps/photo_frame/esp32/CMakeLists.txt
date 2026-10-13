# SPDX-License-Identifier: Apache-2.0
#
# The photo frame on the board: an ESP-IDF project of its own, so it has its
# own build directory, sdkconfig and image, but no code of its own. Its main
# component compiles the slideshow's (apps/slideshow/esp32/main/main.c) with
# the photographs from the gitignored apps/photo_frame/images/ and the
# caption off (sdkconfig.defaults).
#
#   . $HOME/esp/esp-idf/export.sh
#   cd apps/photo_frame/esp32
#   idf.py build                                         # apps/photo_frame/images
#   idf.py -DPHOTO_FRAME_IMAGES_DIR=$HOME/photo-frame build
#
# Configured from the firmware's sdkconfig.defaults first and this
# directory's second, like the slideshow and the on-target test app.

cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware_esp32/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../firmware_esp32/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hac_photo_frame)
