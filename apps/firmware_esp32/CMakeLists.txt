# SPDX-License-Identifier: Apache-2.0
#
# The ESP-IDF application. Build with: idf.py build
#
# Everything under components/ is a thin wrapper: it registers sources that
# live in core/, drivers/ and platform/ at the repository root. No business
# logic lives inside this directory.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(home_air_monitor)
