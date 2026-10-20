# SPDX-License-Identifier: Apache-2.0
#
# IAQ trembling on the Excellent/Good boundary does not redraw the panel, and
# a real change still reaches it within a sensor cycle
# (docs/adr/0026-iaq-redraw-delta.md). scenarios/iaq_flapping_at_50.csv
# is BSEC's index at rest crossing 50 about once a minute -- for the first
# 600 s never by more than the delta, then twice past it -- with something in
# the room at 900 s and a breath on the sensor at 1500 s; nothing else moves,
# so every redraw after the first frame is IAQ's. With ADR 0021's rule -- a
# redraw at every change of band -- one loop of it made 63 such redraws, 15 in
# the first 600 s; with the delta of 5, 49, none there: the rest follow the
# two events, a redraw each time the number has moved by more than 5.
#
#   cmake -DSIMULATOR=<path> -DSCENARIOS=<dir> -P simulator_iaq_flapping.cmake

foreach(var SIMULATOR SCENARIOS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} must be defined")
    endif()
endforeach()

# One loop of the scenario.
execute_process(
    COMMAND ${SIMULATOR} --headless --duration 2400 --wall-time 2026-10-18T12:00:00Z
            --scenario-env ${SCENARIOS}/iaq_flapping_at_50.csv
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the run failed (${result}):\n${out}")
endif()

string(REGEX MATCHALL "\\[policy\\] [a-z]+ +reason=[a-z0-9-]+ +partials=[0-9]+ now_ms=[0-9]+"
       decisions "${out}")
list(LENGTH decisions n)
if(n LESS 2)
    message(FATAL_ERROR "expected the first frame and IAQ's redraws, found ${n}:\n${out}")
endif()

set(iaq_times "")
foreach(d IN LISTS decisions)
    string(REGEX REPLACE ".*reason=([a-z0-9-]+).*" "\\1" reason "${d}")
    string(REGEX REPLACE ".*now_ms=([0-9]+)" "\\1" now_ms "${d}")
    if(reason STREQUAL "iaq")
        list(APPEND iaq_times ${now_ms})
    elseif(NOT reason STREQUAL "first-frame")
        message(FATAL_ERROR "a redraw for something other than IAQ: ${d}\n${out}")
    endif()
endforeach()

# The trembling within the delta, the first 600 s, redraws nothing.
list(GET iaq_times 0 first)
if(first LESS 600000)
    message(FATAL_ERROR "IAQ trembling on 50 redrew at ${first} ms: ${iaq_times}")
endif()

# The two rarer excursions past the delta, 600-900 s: a redraw up and one
# back each, at most.
set(n_tail 0)
foreach(t IN LISTS iaq_times)
    if(t GREATER_EQUAL 600000 AND t LESS 900000)
        math(EXPR n_tail "${n_tail} + 1")
    endif()
endforeach()
if(n_tail GREATER 4)
    message(FATAL_ERROR "${n_tail} IAQ redraws for two excursions past the delta: ${iaq_times}")
endif()

# The events follow their number five points at a time, up and down -- 49
# in all, held to 55 so a slightly different path still passes.
list(LENGTH iaq_times n_iaq)
if(n_iaq GREATER 55)
    message(FATAL_ERROR "${n_iaq} IAQ redraws in one loop, more than 55: ${iaq_times}")
endif()

# Each event on the glass within 10 s of its first row: the index passes the
# delta within that row's three seconds, the sensor is read every 5 s, and
# the redraw goes out in the same poll.
foreach(start_ms 900000 1500000)
    math(EXPR end_ms "${start_ms} + 10000")
    set(seen FALSE)
    foreach(t IN LISTS iaq_times)
        if(t GREATER_EQUAL start_ms AND t LESS_EQUAL end_ms)
            set(seen TRUE)
        endif()
    endforeach()
    if(NOT seen)
        message(FATAL_ERROR "no IAQ redraw between ${start_ms} and ${end_ms} ms: ${iaq_times}")
    endif()
endforeach()
message(STATUS "${n_iaq} IAQ redraws in one loop, at ${iaq_times} ms")
