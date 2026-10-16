# SPDX-License-Identifier: Apache-2.0
#
# The simulator's own round trip (docs/adr/0024): --record a scenario, then
# --recording what it wrote, headless, and the second run must decide the
# same redraws as the first -- the "[policy]" lines of the replay a prefix of
# the recording run's (it stops one sensor cycle earlier, before the
# recording's last row, where its playback loops). This holds the command
# line, the file names and the wall clock taken from the first row; the
# readings themselves are compared in tests/unit/test_recording_round_trip.c.
#
#   cmake -DSIMULATOR=<path> -DSCENARIOS=<dir> -DWORK=<dir> -P simulator_record_replay.cmake

foreach(var SIMULATOR SCENARIOS WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} must be defined")
    endif()
endforeach()

set(record_s 1800)
math(EXPR replay_s "${record_s} - 5")

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

execute_process(
    COMMAND ${SIMULATOR} --headless --duration ${record_s} --wall-time 2026-10-16T12:00:00Z
            --scenario-co2 ${SCENARIOS}/co2_spike_meeting.csv
            --scenario-env ${SCENARIOS}/normal_day.csv
            --record ${WORK}/card
    RESULT_VARIABLE result OUTPUT_VARIABLE recorded ERROR_VARIABLE recorded)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the recording run failed (${result}):\n${recorded}")
endif()
if(NOT recorded MATCHES "\\[sim\\] recorded [0-9]+ rows, [0-9]+ bytes, 0 dropped")
    message(FATAL_ERROR "the recording run did not say what it recorded:\n${recorded}")
endif()
foreach(file 2026-10-16_scd4x.csv 2026-10-16_bme68x.csv)
    if(NOT EXISTS ${WORK}/card/data/${file})
        message(FATAL_ERROR "the recording run wrote no data/${file}")
    endif()
endforeach()

execute_process(
    COMMAND ${SIMULATOR} --headless --duration ${replay_s}
            --recording ${WORK}/card/data/2026-10-16_scd4x.csv
            --recording ${WORK}/card/data/2026-10-16_bme68x.csv
    RESULT_VARIABLE result OUTPUT_VARIABLE replayed ERROR_VARIABLE replayed)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the replay run failed (${result}):\n${replayed}")
endif()
foreach(expected
        "wall clock: 2026-10-16T12:00:00Z \\(the recording's first row\\)"
        "recording 2026-10-16_scd4x.csv plays source id 1"
        "recording 2026-10-16_bme68x.csv plays source id 2")
    if(NOT replayed MATCHES "${expected}")
        message(FATAL_ERROR "the replay run did not say \"${expected}\":\n${replayed}")
    endif()
endforeach()

# The redraw decisions, line by line.
string(REGEX MATCHALL "\\[policy\\][^\n]*" decided_recording "${recorded}")
string(REGEX MATCHALL "\\[policy\\][^\n]*" decided_replay "${replayed}")
list(LENGTH decided_recording n_recording)
list(LENGTH decided_replay n_replay)
if(n_replay LESS 100 OR n_replay GREATER n_recording)
    message(FATAL_ERROR "the replay decided ${n_replay} redraws, the recording run ${n_recording}")
endif()
math(EXPR last "${n_replay} - 1")
foreach(i RANGE ${last})
    list(GET decided_recording ${i} a)
    list(GET decided_replay ${i} b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "redraw ${i} differs:\n  recording run: ${a}\n  replay:        ${b}")
    endif()
endforeach()
message(STATUS "the replay decided the same ${n_replay} redraws as the recording run")
