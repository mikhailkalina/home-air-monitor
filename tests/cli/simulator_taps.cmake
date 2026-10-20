# SPDX-License-Identifier: Apache-2.0
#
# The simulator's scripted taps (docs/adr/0025-touch-input-gt911.md): a
# headless run taps through both screens -- home to diagnostics and back by
# taps, then to diagnostics again and home by the screen's own timeout --
# and says, for each change a tap made, how long it took to reach the glass;
# each change is a redraw a person asked for, so it goes out at once as a
# partial refresh. A malformed --taps is refused before anything runs.
#
#   cmake -DSIMULATOR=<path> -DSCENARIOS=<dir> -P simulator_taps.cmake

foreach(var SIMULATOR SCENARIOS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} must be defined")
    endif()
endforeach()

execute_process(
    COMMAND ${SIMULATOR} --headless --duration 200 --wall-time 2026-10-16T12:00:00Z
            --scenario-co2 ${SCENARIOS}/co2_spike_meeting.csv
            --scenario-env ${SCENARIOS}/normal_day.csv
            --taps "30:480,270;45:100,100;100:480,270"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the run with taps failed (${result}):\n${out}")
endif()

# In this order: each a tap's, the last the timeout's, 60 s after the last tap.
set(expected
    "\\[screen\\] tap at 480,270: diagnostics next"
    "\\[policy\\] partial  reason=request"
    "\\[screen\\] diagnostics \\(tap\\) latency_ms=[0-9]+"
    "\\[screen\\] tap at 100,100: home next"
    "\\[screen\\] home \\(tap\\) latency_ms=[0-9]+"
    "\\[screen\\] tap at 480,270: diagnostics next"
    "\\[screen\\] home \\(the diagnostics screen's timeout\\)")
set(rest "${out}")
foreach(line IN LISTS expected)
    string(REGEX MATCH "${line}" found "${rest}")
    if(NOT found)
        message(FATAL_ERROR "expected \"${line}\" (in order) in:\n${out}")
    endif()
    string(FIND "${rest}" "${found}" at)
    string(LENGTH "${found}" len)
    math(EXPR after "${at} + ${len}")
    string(SUBSTRING "${rest}" ${after} -1 rest)
endforeach()

# No change of screen waited for the sensor cycle: none was ever held.
string(REGEX MATCHALL "\\[screen\\] [a-z]+ \\(tap\\) latency_ms=[0-9]+: ([0-9]+) waiting" waits "${out}")
foreach(w IN LISTS waits)
    if(NOT w MATCHES ": 0 waiting")
        message(FATAL_ERROR "a tap's screen waited for the loop: ${w}\n${out}")
    endif()
endforeach()

execute_process(
    COMMAND ${SIMULATOR} --headless --duration 10
            --scenario-co2 ${SCENARIOS}/co2_spike_meeting.csv --taps "10:2000,10"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(result EQUAL 0 OR NOT out MATCHES "--taps: the tap \"10:2000,10\" is off the 960x540 panel")
    message(FATAL_ERROR "a tap off the panel was not refused (${result}):\n${out}")
endif()
