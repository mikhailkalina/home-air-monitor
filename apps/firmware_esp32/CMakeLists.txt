# SPDX-License-Identifier: Apache-2.0
#
# The ESP-IDF application. Build with: idf.py build
#
# Everything under components/ is a thin wrapper: it registers sources that
# live in core/, drivers/ and platform/ at the repository root. No business
# logic lives inside this directory.

cmake_minimum_required(VERSION 3.16)

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)

# The Wi-Fi credentials (phase 4e, docs/adr/0032-wifi-on-the-board.md): the
# gitignored sdkconfig.credentials beside this file, layered last over the
# defaults of every build of this project -- the BSEC and calibration
# variants' too -- and checked against the sdkconfig once it is configured.
include(${HAC_ROOT}/cmake/credentials_idf.cmake)
set(HAC_CREDENTIALS_OVERLAY "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
hac_credentials_layer("${HAC_CREDENTIALS_OVERLAY}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(home_air_monitor)

hac_credentials_check("${HAC_CREDENTIALS_OVERLAY}")
