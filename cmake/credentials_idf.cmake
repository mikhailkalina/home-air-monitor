# SPDX-License-Identifier: Apache-2.0
#
# The Wi-Fi credentials overlay of the ESP-IDF projects that link
# hac_platform -- the firmware and the on-target test app (phase 4e,
# docs/adr/0032-wifi-on-the-board.md):
#
#   include(${HAC_ROOT}/cmake/credentials_idf.cmake)
#   hac_credentials_layer(<overlay>)   # before project(): the overlay, last
#   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#   project(...)
#   hac_credentials_check(<overlay>)   # after: did it reach the sdkconfig?
#
# The overlay, apps/firmware_esp32/sdkconfig.credentials, is gitignored; the
# repository has sdkconfig.credentials.example. Without one, nothing changes.
#
# Like every defaults file it reaches only an sdkconfig that does not hold
# the symbols yet (CLAUDE.md): an sdkconfig made before the overlay was
# written, or before it was changed, keeps the old values, the build succeeds
# and the image joins the old network -- or none. hac_credentials_check()
# refuses to configure while the two disagree, naming the symbol and never
# its value.

# Appends `overlay`, when it exists, to SDKCONFIG_DEFAULTS in the caller's
# scope: after whatever the build was given -- `-D SDKCONFIG_DEFAULTS=...`
# (the BSEC and calibration variants), the environment's, or the project's
# own sdkconfig.defaults -- so that it is applied last. A macro, for that
# scope. A change to the overlay re-runs the configuration, where the check
# below sees it.
macro(hac_credentials_layer overlay)
    if(EXISTS "${overlay}")
        if(NOT SDKCONFIG_DEFAULTS)
            if(NOT "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
                set(SDKCONFIG_DEFAULTS "$ENV{SDKCONFIG_DEFAULTS}")
            elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
                set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
            endif()
        endif()
        list(APPEND SDKCONFIG_DEFAULTS "${overlay}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${overlay}")
    endif()
endmacro()

# The line that sets `symbol` in `text` -- everything after the "=" -- into
# `out`, or "<absent>". Read from the whole text rather than as a CMake list,
# which a ';' in a password would split.
function(_hac_credentials_value text symbol out)
    string(REGEX MATCH "(^|\n)${symbol}=[^\r\n]*" line "${text}")
    if(line STREQUAL "")
        set(${out} "<absent>" PARENT_SCOPE)
        return()
    endif()
    string(REGEX REPLACE "^\n?${symbol}=" "" value "${line}")
    string(STRIP "${value}" value)
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# After project(): the sdkconfig this build was configured from holds what
# `overlay` says, or the configuration stops; and an sdkconfig with a network
# that no overlay gives is said out loud, since the image will carry it.
function(hac_credentials_check overlay)
    idf_build_get_property(sdkconfig SDKCONFIG)
    if(NOT EXISTS "${sdkconfig}")
        return()
    endif()
    file(READ "${sdkconfig}" held)
    if(EXISTS "${overlay}")
        file(READ "${overlay}" wanted)
        foreach(symbol CONFIG_HAC_WIFI_SSID CONFIG_HAC_WIFI_PASSWORD)
            _hac_credentials_value("${wanted}" ${symbol} want)
            if(want STREQUAL "<absent>")
                continue()
            endif()
            _hac_credentials_value("${held}" ${symbol} have)
            if(NOT want STREQUAL have)
                message(FATAL_ERROR
                    "${symbol} in ${overlay} differs from ${sdkconfig}: an overlay reaches "
                    "only a fresh sdkconfig, so this image would carry the old value. Delete "
                    "${sdkconfig} and run idf.py reconfigure (docs/adr/0032-wifi-on-the-board.md).")
            endif()
        endforeach()
    else()
        _hac_credentials_value("${held}" CONFIG_HAC_WIFI_SSID have)
        if(NOT have STREQUAL "<absent>" AND NOT have STREQUAL "\"\"")
            message(WARNING
                "${sdkconfig} names a Wi-Fi network that no ${overlay} gives (menuconfig?): the "
                "image carries its credentials in plain text. Never attach it anywhere.")
        endif()
    endif()
endfunction()
