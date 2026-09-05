# SPDX-License-Identifier: Apache-2.0
#
# platform/esp32_t5s3/: the adapters implementing the ports on this board.
#
# Empty until phase 2 brings up the board; registered now so that main/ can
# depend on a stable component name.

idf_component_register(REQUIRES hac_core)
