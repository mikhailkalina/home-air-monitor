# SPDX-License-Identifier: Apache-2.0
#
# scenario/ arrives together with app_core (see docs/architecture.md §10).

# Shared harness and fakes. The fake clock lives here rather than under
# platform/host/ because it is a test double, not an adapter that ships.
add_library(hac_test_support STATIC
    support/hac_test.c
    support/fake_clock.c
)
target_include_directories(hac_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(hac_test_support PUBLIC hac_core)  # for ports/ and core/ headers
hac_apply_warnings(hac_test_support)

# One test file is one executable and one CTest entry.
set(HAC_UNIT_TESTS
    test_hal_status
    test_fake_clock
    test_measurement
    test_air_quality
    test_civil_time
    test_framebuffer
    test_gfx
    test_update_policy
    test_link_hold
    test_tap
    test_screen_controller
    test_sensor_manager
    test_view_model
    test_diagnostics_view
    test_screen_home_layout
    test_screen_diagnostics_layout
    test_adp_clock_virtual
    test_replay_source
    test_fault_inject
    test_epd_model
    test_text_append
    test_json_writer
    test_mqtt_topics
    test_mqtt_payload
    test_ha_discovery
    test_publish_policy
)

foreach(test IN LISTS HAC_UNIT_TESTS)
    add_executable(${test} unit/${test}.c)
    target_link_libraries(${test} PRIVATE hac_core hac_test_support)
    hac_apply_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The platform/host tests additionally need adp_clock_virtual and
# replay/fault_inject; test_replay_source also writes scratch CSV files
# under its own binary directory for the duration of one check.
target_link_libraries(test_adp_clock_virtual PRIVATE hac_platform_host)
target_link_libraries(test_replay_source PRIVATE hac_platform_host)
target_link_libraries(test_fault_inject PRIVATE hac_platform_host)
target_link_libraries(test_epd_model PRIVATE hac_platform_host)

# The reporter (docs/adr/0030-telemetry-ports-and-the-reporter.md) on a fake
# clock against the broker model and the simulated network, and the broker
# model's own behaviour past the port_telemetry contract.
foreach(test IN ITEMS test_reporter test_telemetry_loopback)
    add_executable(${test} unit/${test}.c)
    target_link_libraries(${test} PRIVATE hac_platform_host hac_test_support)
    hac_apply_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The simulator's scripted taps (--taps), from platform/host.
add_executable(test_adp_input_script unit/test_adp_input_script.c)
target_link_libraries(test_adp_input_script PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_adp_input_script)
add_test(NAME test_adp_input_script COMMAND test_adp_input_script)
target_link_libraries(test_sensor_manager PRIVATE hac_platform_host)
target_compile_definitions(test_sensor_manager PRIVATE HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(test_replay_source PRIVATE
    HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    HAC_SCENARIOS_DIR="${HAC_ROOT}/scenarios"
)

# The one pure piece of the firmware's panel calibration mode, compiled from
# where the firmware compiles it: apps/firmware_esp32/main/ is otherwise built
# only by idf.py, so this is the only way its arithmetic gets a host test.
add_executable(test_duration_stats
    unit/test_duration_stats.c
    ${HAC_ROOT}/apps/firmware_esp32/main/duration_stats.c
)
target_include_directories(test_duration_stats PRIVATE ${HAC_ROOT}/apps/firmware_esp32/main)
target_link_libraries(test_duration_stats PRIVATE hac_test_support)
hac_apply_warnings(test_duration_stats)
add_test(NAME test_duration_stats COMMAND test_duration_stats)

# The debug log's ring between the logging tasks and the card's writer
# (docs/adr/0024), from where the firmware compiles it.
add_executable(test_log_ring
    unit/test_log_ring.c
    ${HAC_ROOT}/apps/firmware_esp32/main/log_ring.c
)
target_include_directories(test_log_ring PRIVATE ${HAC_ROOT}/apps/firmware_esp32/main)
target_link_libraries(test_log_ring PRIVATE hac_test_support)
hac_apply_warnings(test_log_ring)
add_test(NAME test_log_ring COMMAND test_log_ring)

# The firmware's I2C bus scan, against the host's fake bus.
add_executable(test_i2c_scan
    unit/test_i2c_scan.c
    ${HAC_ROOT}/apps/firmware_esp32/main/i2c_scan.c
)
target_include_directories(test_i2c_scan PRIVATE ${HAC_ROOT}/apps/firmware_esp32/main)
target_link_libraries(test_i2c_scan PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_i2c_scan)
add_test(NAME test_i2c_scan COMMAND test_i2c_scan)

# The firmware's wall clock -- which RTC readings set the system time, and
# time_set's write -- against the fake bus, from where the firmware compiles it.
add_executable(test_wall_clock
    unit/test_wall_clock.c
    ${HAC_ROOT}/apps/firmware_esp32/main/wall_clock.c
)
target_include_directories(test_wall_clock PRIVATE ${HAC_ROOT}/apps/firmware_esp32/main)
target_link_libraries(test_wall_clock PRIVATE hac_drivers hac_platform_host hac_test_support)
hac_apply_warnings(test_wall_clock)
add_test(NAME test_wall_clock COMMAND test_wall_clock)

# Phase 4f's RTC against the network's time: the measurement by the RTC's
# alarm, the aligned write and the drift, in the steps the event loop takes,
# against the PCF8563 model and its INT, from where the firmware compiles it.
add_executable(test_rtc_sync
    unit/test_rtc_sync.c
    ${HAC_ROOT}/apps/firmware_esp32/main/rtc_sync.c
    ${HAC_ROOT}/apps/firmware_esp32/main/wall_clock.c
)
target_include_directories(test_rtc_sync PRIVATE ${HAC_ROOT}/apps/firmware_esp32/main)
target_link_libraries(test_rtc_sync PRIVATE hac_drivers hac_platform_host hac_test_support)
hac_apply_warnings(test_rtc_sync)
add_test(NAME test_rtc_sync COMMAND test_rtc_sync)

# The SCD41 command-level model against the datasheet, and the SCD4x driver --
# Sensirion's code behind sensor_source_t -- against the model, over the fake
# bus (docs/adr/0015-scd4x-driver.md).
add_executable(test_scd41_model unit/test_scd41_model.c)
target_link_libraries(test_scd41_model PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_scd41_model)
add_test(NAME test_scd41_model COMMAND test_scd41_model)

add_executable(test_scd4x_source unit/test_scd4x_source.c)
target_link_libraries(test_scd4x_source PRIVATE hac_drivers hac_platform_host hac_test_support)
hac_apply_warnings(test_scd4x_source)
add_test(NAME test_scd4x_source COMMAND test_scd4x_source)

# The BME680 register-level model against the datasheet and a published set
# of test vectors, and the BME68x driver -- Bosch's code behind
# sensor_source_t -- against the model, over the fake bus
# (docs/adr/0016-bme68x-driver.md).
add_executable(test_bme680_model unit/test_bme680_model.c)
target_link_libraries(test_bme680_model PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_bme680_model)
add_test(NAME test_bme680_model COMMAND test_bme680_model)

add_executable(test_bme68x_source unit/test_bme68x_source.c)
target_link_libraries(test_bme68x_source PRIVATE hac_drivers hac_platform_host hac_test_support)
hac_apply_warnings(test_bme68x_source)
add_test(NAME test_bme68x_source COMMAND test_bme68x_source)

# The GT911 touch controller (docs/adr/0025-touch-input-gt911.md): its model
# against Goodix's documents; the driver against the model, which refuses and
# counts any write into the vendor's configuration; the snapshots-to-events
# function and the coordinate mapping, pure; and port_input over the driver.
add_executable(test_gt911_model unit/test_gt911_model.c)
target_link_libraries(test_gt911_model PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_gt911_model)
add_test(NAME test_gt911_model COMMAND test_gt911_model)

foreach(test IN ITEMS test_gt911 test_gt911_touch test_gt911_input)
    add_executable(${test} unit/${test}.c)
    target_link_libraries(${test} PRIVATE hac_drivers hac_platform_host hac_test_support)
    hac_apply_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The PCF8563 real-time clock driver against the fake bus's register file,
# which is the chip as it is (drivers/pcf8563/pcf8563.h); and the model of
# the chip that keeps time and raises its alarm on INT (phase 4f), against the
# datasheet, which test_rtc_sync measures and sets to the millisecond.
add_executable(test_pcf8563_model unit/test_pcf8563_model.c)
target_link_libraries(test_pcf8563_model PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_pcf8563_model)
add_test(NAME test_pcf8563_model COMMAND test_pcf8563_model)

add_executable(test_pcf8563 unit/test_pcf8563.c)
target_link_libraries(test_pcf8563 PRIVATE hac_drivers hac_platform_host hac_test_support)
hac_apply_warnings(test_pcf8563)
add_test(NAME test_pcf8563 COMMAND test_pcf8563)

# How BSEC's accuracy and gas statuses become value_status_t
# (docs/adr/0020-bsec-iaq.md). Pure and free of Bosch's headers, so it runs
# in every build, CI's included.
add_executable(test_bme68x_bsec_status unit/test_bme68x_bsec_status.c)
target_link_libraries(test_bme68x_bsec_status PRIVATE hac_drivers hac_test_support)
hac_apply_warnings(test_bme68x_bsec_status)
add_test(NAME test_bme68x_bsec_status COMMAND test_bme68x_bsec_status)

# The BME68x under BSEC -- Bosch's library deciding when and how it measures
# -- against the same BME680 model, over the same fake bus. Only with the
# library (HAC_BSEC, cmake/bsec.cmake), which is never in CI.
if(HAC_BSEC)
    add_executable(test_bme68x_bsec_source unit/test_bme68x_bsec_source.c)
    target_link_libraries(test_bme68x_bsec_source PRIVATE
        hac_drivers_bsec hac_platform_host hac_test_support)
    target_compile_definitions(test_bme68x_bsec_source PRIVATE
        HAC_BSEC_TEST_CONFIG_NAME="${HAC_BSEC_CONFIG_NAME}")
    hac_apply_warnings(test_bme68x_bsec_source)
    add_test(NAME test_bme68x_bsec_source COMMAND test_bme68x_bsec_source)
endif()

# The recordings (docs/adr/0024): the row format against the replay parser
# that reads it back, the file names and the bound over adp_files_dir, the
# recorder itself over the same directory, the round trip -- scenarios
# recorded and the recordings replayed through the same sensor manager -- and
# the debug log's files.
foreach(test IN ITEMS test_record_format test_record_files test_recorder
                      test_recording_round_trip test_log_file)
    add_executable(${test} unit/${test}.c)
    target_link_libraries(${test} PRIVATE hac_platform_host hac_test_support)
    target_compile_definitions(${test} PRIVATE HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    hac_apply_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The same through the simulator's command line: --record, then --recording,
# deciding the same redraws (tests/cli/simulator_record_replay.cmake).
add_test(NAME simulator_record_replay
    COMMAND ${CMAKE_COMMAND} -DSIMULATOR=$<TARGET_FILE:simulator>
            -DSCENARIOS=${HAC_ROOT}/scenarios
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/simulator_record_replay
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/simulator_record_replay.cmake)

# Taps through both screens from the simulator's command line
# (tests/cli/simulator_taps.cmake, docs/adr/0025-touch-input-gt911.md).
add_test(NAME simulator_taps
    COMMAND ${CMAKE_COMMAND} -DSIMULATOR=$<TARGET_FILE:simulator>
            -DSCENARIOS=${HAC_ROOT}/scenarios
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/simulator_taps.cmake)

# IAQ trembling on a band's boundary does not redraw, a real change does
# (tests/cli/simulator_iaq_flapping.cmake, docs/adr/0026-iaq-redraw-delta.md).
add_test(NAME simulator_iaq_flapping
    COMMAND ${CMAKE_COMMAND} -DSIMULATOR=$<TARGET_FILE:simulator>
            -DSCENARIOS=${HAC_ROOT}/scenarios
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/simulator_iaq_flapping.cmake)

# The reporter through the simulator's command line: a day's state messages,
# a network outage that changes nothing on the card and reaches the panel only
# as the status bar's link, a minute late, the next session announcing itself
# again (tests/cli/simulator_telemetry.cmake,
# docs/adr/0030-telemetry-ports-and-the-reporter.md,
# docs/adr/0031-the-network-on-the-screen.md).
add_test(NAME simulator_telemetry
    COMMAND ${CMAKE_COMMAND} -DSIMULATOR=$<TARGET_FILE:simulator>
            -DSCENARIOS=${HAC_ROOT}/scenarios
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/simulator_telemetry
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/simulator_telemetry.cmake)

# A link that flaps every few seconds redraws nothing, one slower than the
# hold a change a minute at most (tests/cli/simulator_link_flapping.cmake,
# docs/adr/0031-the-network-on-the-screen.md).
add_test(NAME simulator_link_flapping
    COMMAND ${CMAKE_COMMAND} -DSIMULATOR=$<TARGET_FILE:simulator>
            -DSCENARIOS=${HAC_ROOT}/scenarios
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/simulator_link_flapping.cmake)

# The simulator's two sensor modes on one scenario: replay sources, and both
# drivers over the fake bus measuring platform/host/scenario_world.c -- each
# under core/app/sensor_manager.c, which this exercises over real drivers.
add_executable(test_scenario_world unit/test_scenario_world.c)
target_link_libraries(test_scenario_world PRIVATE hac_drivers hac_platform_host hac_test_support)
target_compile_definitions(test_scenario_world PRIVATE
    HAC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
hac_apply_warnings(test_scenario_world)
add_test(NAME test_scenario_world COMMAND test_scenario_world)

# The pure half of the ESP32 I2C adapter -- deadline-to-driver-wait
# arithmetic and failure classification -- compiled from where the firmware
# compiles it, like duration_stats.c above. adp_i2c.c itself needs ESP-IDF
# and is exercised on the device by the port_i2c contract.
add_executable(test_adp_i2c_pure
    unit/test_adp_i2c_pure.c
    ${HAC_ROOT}/platform/esp32_t5s3/adp_i2c_pure.c
)
target_include_directories(test_adp_i2c_pure PRIVATE ${HAC_ROOT}/platform/esp32_t5s3)
target_link_libraries(test_adp_i2c_pure PRIVATE hac_test_support)
hac_apply_warnings(test_adp_i2c_pure)
add_test(NAME test_adp_i2c_pure COMMAND test_adp_i2c_pure)

# The Wi-Fi station's decisions -- the backoff, the timeouts, which events
# belong to the attempt under way -- compiled from where the firmware compiles
# them, as adp_i2c_pure.c is (docs/adr/0032-wifi-on-the-board.md). adp_net.c
# itself needs ESP-IDF and is held to the port_net contract in the test app.
add_executable(test_adp_net_pure
    unit/test_adp_net_pure.c
    ${HAC_ROOT}/platform/esp32_t5s3/adp_net_pure.c
)
target_include_directories(test_adp_net_pure PRIVATE
    ${HAC_ROOT}/platform/esp32_t5s3
    ${HAC_ROOT}/ports
)
target_link_libraries(test_adp_net_pure PRIVATE hac_test_support)
hac_apply_warnings(test_adp_net_pure)
add_test(NAME test_adp_net_pure COMMAND test_adp_net_pure)

# The MQTT client's decisions -- the broker's URI, the queues' indices, a
# message put together from pieces, which session an esp-mqtt event belongs
# to and what the adapter's task does next -- compiled from where the firmware
# compiles them (docs/adr/0033-mqtt-on-the-board.md). adp_mqtt.c itself needs
# ESP-IDF and is held to the port_telemetry contract in the test app.
add_executable(test_adp_mqtt_pure
    unit/test_adp_mqtt_pure.c
    ${HAC_ROOT}/platform/esp32_t5s3/adp_mqtt_pure.c
)
target_include_directories(test_adp_mqtt_pure PRIVATE
    ${HAC_ROOT}/platform/esp32_t5s3
    ${HAC_ROOT}/ports
)
target_link_libraries(test_adp_mqtt_pure PRIVATE hac_test_support)
hac_apply_warnings(test_adp_mqtt_pure)
add_test(NAME test_adp_mqtt_pure COMMAND test_adp_mqtt_pure)

# The device's panel limits against the simulator's. Only board_config.h's
# directory is added -- it is plain data -- not anything that builds for the
# ESP32.
add_executable(test_panel_limits_agree unit/test_panel_limits_agree.c)
target_include_directories(test_panel_limits_agree PRIVATE ${HAC_ROOT}/platform/esp32_t5s3)
target_link_libraries(test_panel_limits_agree PRIVATE hac_platform_host hac_test_support)
hac_apply_warnings(test_panel_limits_agree)
add_test(NAME test_panel_limits_agree COMMAND test_panel_limits_agree)

# The slideshow test app's schedule and layout, compiled from where its two
# compositions compile it, like duration_stats.c above.
add_executable(test_slideshow
    unit/test_slideshow.c
    ${HAC_ROOT}/apps/slideshow/slideshow.c
)
target_include_directories(test_slideshow PRIVATE ${HAC_ROOT}/apps/slideshow)
target_link_libraries(test_slideshow PRIVATE hac_core hac_test_support)
hac_apply_warnings(test_slideshow)
add_test(NAME test_slideshow COMMAND test_slideshow)

# The picture pipeline end to end (docs/adr/0012-image-pipeline.md): PNG
# fixtures embedded by apps/tools/image_gen/image_gen.py through the same
# cmake/image_gen.cmake the slideshow uses, then blitted. Needs a Python 3
# interpreter -- the standard library only -- and is left out without one,
# as the slideshow is.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    include(${HAC_ROOT}/cmake/image_gen.cmake)
    set(HAC_IMAGE_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/unit/fixtures/image_gen)

    add_executable(test_image_embed unit/test_image_embed.c)
    target_link_libraries(test_image_embed PRIVATE hac_core hac_test_support)
    hac_embed_images(test_image_embed
        NAME test_images
        PYTHON ${Python3_EXECUTABLE}
        IMAGES
            ${HAC_IMAGE_FIXTURES}/gray16.png
            ${HAC_IMAGE_FIXTURES}/gray4.png
            ${HAC_IMAGE_FIXTURES}/gray8.png
            ${HAC_IMAGE_FIXTURES}/palette4.png
            ${HAC_IMAGE_FIXTURES}/rgb.png
    )
    hac_apply_warnings(test_image_embed)
    add_test(NAME test_image_embed COMMAND test_image_embed)

    # embed must refuse what is not panel-ready rather than guess. Each case
    # passes only on its own refusal message, so a crash -- or a refusal for
    # the wrong reason -- fails it.
    set(image_gen ${HAC_ROOT}/apps/tools/image_gen/image_gen.py)
    set(reject_dir ${CMAKE_CURRENT_BINARY_DIR}/image_gen_rejects)
    foreach(case IN ITEMS
            "off_level|pixel [(]3, 1[)] is grey 100, not one of the sixteen levels"
            "colour|pixel [(]2, 2[)] is a colour, not a grey"
            "transparent|pixel [(]0, 0[)] is not fully opaque")
        string(REPLACE "|" ";" parts "${case}")
        list(GET parts 0 name)
        list(GET parts 1 message)
        add_test(NAME image_gen_rejects_${name}
            COMMAND ${Python3_EXECUTABLE} ${image_gen} embed --name rejected
                    --out-dir ${reject_dir} ${HAC_IMAGE_FIXTURES}/reject_${name}.png)
        set_tests_properties(image_gen_rejects_${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "not panel-ready: ${message}")
    endforeach()

    # prepare reads how light each level is from platform/host/epd_emulation.h,
    # the simulator's own table (docs/adr/0013-grey-level-lightness.md); this
    # proves it still finds and accepts it.
    add_test(NAME image_gen_reads_the_level_table
        COMMAND ${Python3_EXECUTABLE} ${image_gen} levels)
    set_tests_properties(image_gen_reads_the_level_table PROPERTIES
        PASS_REGULAR_EXPRESSION "level 15: lightness 1[.]000")

    # The committed demo pictures are what the generator makes now. A change
    # to the dithering or a test pattern that is not regenerated fails here
    # instead of leaving the repository's pictures quietly out of step.
    add_test(NAME image_gen_demo_pictures_are_current
        COMMAND ${Python3_EXECUTABLE} ${image_gen} demo --verify
                --out-dir ${HAC_ROOT}/apps/slideshow/images)

    # Every message core/reporting/ builds, parsed by Python's own JSON
    # parser, with the cross-checks no C test can make: the templates name
    # only keys the state message has, "~" expands to the device's topics,
    # escaped text comes back as it went in
    # (docs/adr/0029-mqtt-topics-payload-and-discovery.md).
    add_executable(mqtt_messages_dump unit/mqtt_messages_dump.c)
    target_link_libraries(mqtt_messages_dump PRIVATE hac_core)
    hac_apply_warnings(mqtt_messages_dump)
    add_test(NAME mqtt_messages_parse
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/unit/mqtt_messages_check.py
                $<TARGET_FILE:mqtt_messages_dump>)
    set_tests_properties(mqtt_messages_parse PROPERTIES
        PASS_REGULAR_EXPRESSION "[0-9]+ messages parsed, 0 failures")

    # ...and round, when asked to, instead.
    add_test(NAME image_gen_quantize_accepts_off_level
        COMMAND ${Python3_EXECUTABLE} ${image_gen} embed --name quantized --quantize
                --out-dir ${reject_dir} ${HAC_IMAGE_FIXTURES}/reject_off_level.png)
    set_tests_properties(image_gen_quantize_accepts_off_level PROPERTIES
        PASS_REGULAR_EXPRESSION "embedded 1 image")
else()
    message(WARNING "Python 3 not found: the image pipeline tests and the MQTT messages' "
                    "parse check are not built.")
endif()

# The mosquitto broker, for the tests that need a real one: the port_telemetry
# contract against libmosquitto (tests/contract/) and the simulator in front of
# a broker that restarts (tests/cli/simulator_broker.py). Each starts a private
# one on a free port of 127.0.0.1. Without it, both say they were not exercised.
find_program(HAC_MOSQUITTO_BROKER mosquitto PATHS /usr/sbin /usr/local/sbin)
find_program(HAC_MOSQUITTO_PASSWD mosquitto_passwd)

# The simulator in front of that broker: it appears, Home Assistant's birth
# brings the configs again, a broker restart and the run's end recover and
# leave the will, a broker with a password logs in and the password is printed
# nowhere (tests/cli/simulator_broker.py). Skipped -- "not exercised" -- without
# a broker or without libmosquitto.
if(Python3_Interpreter_FOUND)
    set(broker_args "")
    if(TARGET hac_platform_host_mqtt)
        list(APPEND broker_args --with-mqtt)
    endif()
    if(HAC_MOSQUITTO_BROKER)
        list(APPEND broker_args --mosquitto ${HAC_MOSQUITTO_BROKER})
    endif()
    if(HAC_MOSQUITTO_PASSWD)
        list(APPEND broker_args --mosquitto-passwd ${HAC_MOSQUITTO_PASSWD})
    endif()
    add_test(NAME simulator_broker
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli/simulator_broker.py
                --simulator $<TARGET_FILE:simulator> --scenarios ${HAC_ROOT}/scenarios
                --work ${CMAKE_CURRENT_BINARY_DIR}/simulator_broker ${broker_args})
    set_tests_properties(simulator_broker PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()

add_subdirectory(golden)
add_subdirectory(contract)
add_subdirectory(fuzz)
