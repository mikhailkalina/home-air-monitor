# SPDX-License-Identifier: Apache-2.0
#
# Where Bosch's BSEC2 library, its headers and the one configuration this
# project uses are found -- for both builds that can link it: the host's
# (root CMakeLists.txt, option HAC_BSEC) and the ESP-IDF one
# (apps/firmware_esp32/components/hac_drivers/, CONFIG_HAC_BSEC). One file,
# so the board and the host tests link the same configuration
# (docs/adr/0020-bsec-iaq.md).
#
# None of it is in this repository, and none of it may be: the library is
# under Bosch's licence agreement, and the headers, the configuration files
# and the examples are marked "Confidential" with "It is not allowed to
# deliver the source code of the Software to any third party without
# permission of Bosch Sensortec" (THIRD_PARTY_NOTICES.md, "Notes on BSEC /
# BSEC2"). The archive is downloaded after accepting that agreement and
# unpacked, as it comes, into third_party/bsec/, which is gitignored; point
# HAC_BSEC_DIR elsewhere to keep it outside the tree.
#
# The includer must define HAC_ROOT. This file sets:
#
#   HAC_BSEC_VERSION           the release this project was written and tested
#                              against; the source logs the one it links
#   HAC_BSEC_CONFIG_NAME       the configuration, below
#   HAC_BSEC_INCLUDES          Bosch's headers and the configuration's
#   HAC_BSEC_CONFIG_SOURCES    the configuration: Bosch's bsec_iaq.c, compiled
#                              as it comes, so nothing of it is copied here
#   HAC_BSEC_LIB_ESP32S3       libalgobsec.a for the board
#   HAC_BSEC_LIB_LINUX_X86_64  libalgobsec.a for a 64-bit x86 Linux host
#
# and hac_bsec_require(<path>...), which stops the configure step with what
# to download when a file is missing -- rather than a link error later.

if(NOT DEFINED HAC_ROOT)
    message(FATAL_ERROR "HAC_ROOT must be set before including bsec.cmake")
endif()

set(HAC_BSEC_DIR "${HAC_ROOT}/third_party/bsec" CACHE PATH
    "Bosch's BSEC2 archive, unpacked as downloaded (gitignored; see THIRD_PARTY_NOTICES.md)")

set(HAC_BSEC_VERSION "2.6.1.0")

# CHOICE: the configuration, one of the sixteen the IAQ solution ships for a
# BME680 and a BME688. Each part of its name is a decision:
#
#   bme680  the part on this board: variant_id 0x00, read back on 2026-10-16
#           (docs/hardware/board_notes.md, "Which sensor variants are fitted")
#   33v     its supply. The module runs from the switched 3V3 rail; what its
#           own regulator hands the chip has not been measured -- an ESTIMATE,
#           which matters only to BSEC's self-heating compensation
#           (board_notes.md, open item "BSEC on the board")
#   3s      low-power mode, a measurement every 3 s: the rate
#           bme68x_bsec_source.c subscribes at. The two must agree, which is
#           why both are named only here and there (BSEC warns
#           BSEC_W_SU_SAMPLERATEMISMATCH otherwise, and init() fails on it)
#   4d      four days of history for the IAQ's background calibration rather
#           than 28. BSEC's state restarts at every boot until phase 5 keeps
#           it (board_notes.md, open item "BSEC's state restarts at every
#           boot"), so a 28-day window would never fill; revisit with that
set(HAC_BSEC_CONFIG_NAME "bme680_iaq_33v_3s_4d")

set(HAC_BSEC_ALGO_DIR "${HAC_BSEC_DIR}/algo/bsec_IAQ")
set(HAC_BSEC_CONFIG_DIR "${HAC_BSEC_ALGO_DIR}/config/bme680/${HAC_BSEC_CONFIG_NAME}")

set(HAC_BSEC_INCLUDES
    ${HAC_BSEC_ALGO_DIR}/inc
    ${HAC_BSEC_CONFIG_DIR}
)
set(HAC_BSEC_CONFIG_SOURCES ${HAC_BSEC_CONFIG_DIR}/bsec_iaq.c)

set(HAC_BSEC_LIB_ESP32S3 "${HAC_BSEC_ALGO_DIR}/bin/esp/esp32_s3/libalgobsec.a")
set(HAC_BSEC_LIB_LINUX_X86_64 "${HAC_BSEC_ALGO_DIR}/bin/gcc/Linux/m64/libalgobsec.a")

function(hac_bsec_require)
    foreach(path IN LISTS ARGN)
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR
                "BSEC is switched on, but ${path} is missing.\n"
                "Download BSEC2 ${HAC_BSEC_VERSION} from Bosch Sensortec, accept its licence "
                "agreement, and unpack the archive as it comes into ${HAC_BSEC_DIR} "
                "(THIRD_PARTY_NOTICES.md, \"Notes on BSEC / BSEC2\"). Or build without it: "
                "HAC_BSEC=OFF on the host, CONFIG_HAC_BSEC=n in the firmware.")
        endif()
    endforeach()
endfunction()
