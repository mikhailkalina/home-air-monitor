# SPDX-License-Identifier: Apache-2.0

get_filename_component(HAC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)

idf_component_register(
    SRCS
        main.c
        event_loop.c
        # Compiled into every build, called only under
        # CONFIG_HAC_EPD_CALIBRATION: an unused mode that is never compiled
        # rots unnoticed, and the linker drops it from images that do not
        # call it.
        epd_calibration.c
        duration_stats.c
        # Pure over port_i2c, and tested on the host from this location
        # (tests/unit/test_i2c_scan.c) as duration_stats.c is.
        i2c_scan.c
        # Pure over port_i2c and the PCF8563 driver, and tested on the host
        # from this location (tests/unit/test_wall_clock.c).
        wall_clock.c
        console.c
        # The microSD card's task and the log's tee into it (phase 3f,
        # docs/adr/0024). log_ring.c is pure, and tested on the host from
        # this location (tests/unit/test_log_ring.c).
        card_writer.c
        log_ring.c
        log_tee.c
        # Time from the network (phase 4f, docs/adr/0023's amendment): SNTP,
        # and the RTC measured and set from it in the event loop's steps.
        # Linked only into an image with a network: without one nothing calls
        # net_time_init() or net_time_step(). rtc_sync.c is pure, and tested
        # on the host from this location (tests/unit/test_rtc_sync.c).
        net_time.c
        rtc_sync.c
    INCLUDE_DIRS .
    REQUIRES hac_core hac_drivers hac_platform hac_contract esp_timer console heap log
             # The console's vbat: GPIO14's ADC2 beside Wi-Fi (phase 4e).
             esp_adc
             # net_time.c: esp_netif's SNTP, lwIP's esp_sntp.h, the IP event.
             esp_netif lwip esp_event
)

include(${HAC_ROOT}/cmake/warnings_idf.cmake)
hac_apply_warnings_idf(${COMPONENT_LIB})
