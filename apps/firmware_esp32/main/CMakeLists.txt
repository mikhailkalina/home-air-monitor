# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES hac_core hac_drivers hac_platform
)
