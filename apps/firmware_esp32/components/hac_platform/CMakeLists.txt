# SPDX-License-Identifier: Apache-2.0
#
# platform/esp32_t5s3/: the adapters implementing the ports on this board.
#
# Sources are listed here, not in a shared cmake/ file: unlike core/ and the
# contract bodies, there is exactly one ESP32 platform, so there is no second
# build that could drift from this list.

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)

idf_component_register(
    SRCS
        ${HAC_ROOT}/platform/esp32_t5s3/adp_clock.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_log.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_display_null.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_display.c
        ${HAC_ROOT}/platform/esp32_t5s3/epd_white_path.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_i2c.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_i2c_pure.c
        # The GT911 behind port_input: its interrupt and its task. The
        # driver and the port_input over it are hac_drivers' (ADR 0025).
        ${HAC_ROOT}/platform/esp32_t5s3/adp_input.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_sd.c
        # The Wi-Fi station behind port_net, and its decisions, which the
        # host tests from this location (phase 4e, docs/adr/0032).
        ${HAC_ROOT}/platform/esp32_t5s3/adp_net.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_net_pure.c
        # The MQTT client behind port_telemetry, over esp-mqtt, and its
        # decisions, which the host tests from this location (phase 4g,
        # docs/adr/0033). Linked only into an image with a broker: without
        # one nothing calls adp_mqtt_init().
        ${HAC_ROOT}/platform/esp32_t5s3/adp_mqtt.c
        ${HAC_ROOT}/platform/esp32_t5s3/adp_mqtt_pure.c
        ${HAC_ROOT}/platform/esp32_t5s3/board_power.c
        # The RTC's INT on IO9, its falling edges stamped in an interrupt
        # (phase 4f, docs/adr/0023's amendment), and the GPIO ISR service it
        # shares with the touch adapter, installed once.
        ${HAC_ROOT}/platform/esp32_t5s3/board_rtc_int.c
        ${HAC_ROOT}/platform/esp32_t5s3/board_gpio_isr.c
    INCLUDE_DIRS
        ${HAC_ROOT}/platform/esp32_t5s3
    REQUIRES
        hac_core
        hac_drivers
        epdiy
        esp_timer
        esp_driver_i2c
        esp_driver_gpio
        # The microSD card (adp_sd.c): ESP-IDF's FATFS, which links ChaN's
        # FatFs (THIRD_PARTY_NOTICES.md), the SD protocol and SD over SPI.
        fatfs
        sdmmc
        esp_driver_sdspi
        esp_driver_spi
        vfs
        # The Wi-Fi station (adp_net.c): the driver, lwIP behind esp_netif,
        # the default event loop, and NVS for the radio's calibration data.
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        # The MQTT client (adp_mqtt.c): esp-mqtt, bundled with ESP-IDF
        # (THIRD_PARTY_NOTICES.md).
        mqtt
        esp_psram
        esp_hw_support
        esp_rom
        heap
        freertos
        log
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
