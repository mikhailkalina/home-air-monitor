# SPDX-License-Identifier: Apache-2.0
#
# A link that flaps cannot spend the panel's partial-refresh budget
# (docs/adr/0031-the-network-on-the-screen.md), as IAQ trembling on 50 did
# before ADR 0026. Three runs of half an hour over the broker model, the
# readings of co2_spike_meeting.csv and normal_day.csv:
#
#   * steady: the network there throughout;
#   * fast: from 120 s on, the access point gone for 3 s in every 10 s --
#     the session dropped and back each time, the link "online" for 4 s of
#     them -- and the screen does not see it at all: the same redraws as the
#     steady run, decision for decision, and "online" held throughout;
#   * slow: from 120 s on, the access point gone 90 s and back 90 s -- each
#     state longer than the minute the screen holds -- and every change
#     reaches the glass, a minute late, never two within a minute of each
#     other, each costing at most a redraw and what it shifts;
#   * the diagnostics screen up while the held link changes: one redraw for
#     it, which changes the picture. Its first version compared the held
#     link without drawing it, and the redraw due came out an empty diff,
#     skipped and tried again every second until the screen timed out -- seen
#     by the owner in the window. No run here may skip a redraw so.
#
#   cmake -DSIMULATOR=<path> -DSCENARIOS=<dir> -P simulator_link_flapping.cmake

foreach(var SIMULATOR SCENARIOS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} must be defined")
    endif()
endforeach()

set(duration_s 1800)
set(hold_ms 60000)

# "<t>:down;<t+down>:up;..." from `start` s, a cycle every `period` s.
function(flapping_script out start period down)
    set(items "")
    set(t ${start})
    while(t LESS duration_s)
        math(EXPR back "${t} + ${down}")
        list(APPEND items "${t}:down" "${back}:up")
        math(EXPR t "${t} + ${period}")
    endwhile()
    list(JOIN items ";" script)
    set(${out} "${script}" PARENT_SCOPE)
endfunction()

flapping_script(fast 120 10 3)
flapping_script(slow 120 180 90)

set(common --headless --duration ${duration_s} --wall-time 2026-10-16T12:00:00Z
           --scenario-co2 ${SCENARIOS}/co2_spike_meeting.csv
           --scenario-env ${SCENARIOS}/normal_day.csv --telemetry loopback)
execute_process(COMMAND ${SIMULATOR} ${common}
                RESULT_VARIABLE result_steady OUTPUT_VARIABLE out_steady
                ERROR_VARIABLE out_steady)
# A script kept whole as one argument: a list variable would split it at ';'.
execute_process(COMMAND ${SIMULATOR} ${common} --net-script "${fast}"
                RESULT_VARIABLE result_fast OUTPUT_VARIABLE out_fast ERROR_VARIABLE out_fast)
execute_process(COMMAND ${SIMULATOR} ${common} --net-script "${slow}"
                RESULT_VARIABLE result_slow OUTPUT_VARIABLE out_slow ERROR_VARIABLE out_slow)

foreach(run steady fast slow)
    if(NOT result_${run} EQUAL 0)
        message(FATAL_ERROR "the run '${run}' failed (${result_${run}}):\n${out_${run}}")
    endif()
    string(REGEX MATCHALL "\\[policy\\] [a-z]+ +reason=[a-z0-9-]+ +partials=[0-9]+ now_ms=[0-9]+"
           policy_${run} "${out_${run}}")
    list(LENGTH policy_${run} n_policy_${run})
    string(REGEX MATCHALL "\\[policy\\] full[^\n]*" fulls "${out_${run}}")
    list(LENGTH fulls n_full_${run})
    string(REGEX MATCHALL "\\[link\\] [a-z-]+ now_ms=[0-9]+" link_${run} "${out_${run}}")
    list(LENGTH link_${run} n_link_${run})
    string(REGEX MATCH "\\[sim\\] telemetry: ([0-9]+) sessions" m "${out_${run}}")
    set(sessions_${run} ${CMAKE_MATCH_1})
endforeach()

# No redraw that turned out to change nothing, in any run.
foreach(run steady fast slow)
    if(out_${run} MATCHES "partial refresh skipped")
        message(FATAL_ERROR "the run '${run}' made a redraw due that changed nothing:\n"
                            "${out_${run}}")
    endif()
endforeach()

# --- fast: the network flaps, the screen does not --------------------------------------

# It did flap: a session lost and found in every cycle.
if(sessions_fast LESS 150)
    message(FATAL_ERROR "only ${sessions_fast} sessions: the network did not flap:\n${out_fast}")
endif()
if(NOT link_fast STREQUAL link_steady OR NOT n_link_steady EQUAL 2)
    message(FATAL_ERROR "the flapping link reached the screen: \"${link_fast}\", not "
                        "\"${link_steady}\":\n${out_fast}")
endif()
if(NOT policy_fast STREQUAL policy_steady)
    message(FATAL_ERROR "a flapping link changed the redraws: ${n_policy_fast} against "
                        "${n_policy_steady}:\n${out_fast}")
endif()
message(STATUS "fast: ${sessions_fast} sessions, ${n_policy_fast} redraws (${n_full_fast} full), "
               "the same as steady")

# --- slow: every change shown, a hold apart --------------------------------------------

set(last_at -1)
set(changes 0)
foreach(line IN LISTS link_slow)
    string(REGEX REPLACE ".*now_ms=([0-9]+)" "\\1" at "${line}")
    if(at EQUAL 0)
        continue()  # the first frame's place for the word
    endif()
    math(EXPR changes "${changes} + 1")
    if(last_at GREATER_EQUAL 0)
        math(EXPR apart "${at} - ${last_at}")
        if(apart LESS hold_ms)
            message(FATAL_ERROR "two changes of the link ${apart} ms apart:\n${link_slow}")
        endif()
    endif()
    set(last_at ${at})
    # On the glass within a sensor cycle.
    math(EXPR by "${at} + 5000")
    set(drawn FALSE)
    foreach(p IN LISTS policy_slow)
        string(REGEX REPLACE ".*now_ms=([0-9]+)" "\\1" t "${p}")
        if(t GREATER_EQUAL at AND t LESS_EQUAL by)
            set(drawn TRUE)
        endif()
    endforeach()
    if(NOT drawn)
        message(FATAL_ERROR "the link's change at ${at} ms reached no redraw by ${by} ms:\n"
                            "${out_slow}")
    endif()
endforeach()
# "online" at 63 s, then one per 90 s state, a minute into each, from 180 s
# on: 20 in half an hour.
if(changes LESS 18 OR changes GREATER 21)
    message(FATAL_ERROR "${changes} changes of the link in half an hour, not 18 to 21:\n"
                        "${link_slow}")
endif()
# Each change a redraw at most, and as much again for the deltas it moved.
math(EXPR bound "${n_policy_steady} + 2 * ${changes}")
if(n_policy_slow GREATER bound)
    message(FATAL_ERROR "${n_policy_slow} redraws with the slow link, more than ${bound} "
                        "(${n_policy_steady} steady, ${changes} changes of the link):\n${out_slow}")
endif()
message(STATUS "slow: ${changes} changes of the link, ${n_policy_slow} redraws "
               "(${n_full_slow} full) against ${n_policy_steady} (${n_full_steady} full) steady")

# --- the diagnostics screen up while the held link changes ------------------------------

# A tap at 130 s brings the diagnostics screen; the network went at 100 s, so
# the bar's link changes at 160 s while it is up, and it goes home at 190 s.
execute_process(
    COMMAND ${SIMULATOR} --headless --duration 230 --wall-time 2026-10-16T12:00:00Z
            --scenario-env ${SCENARIOS}/normal_day.csv --telemetry loopback
            --net-script "100:down" --taps "130:480,270"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the diagnostics run failed (${result}):\n${out}")
endif()
if(NOT out MATCHES "\\[link\\] no-wifi now_ms=160000")
    message(FATAL_ERROR "the link did not change at 160 s:\n${out}")
endif()
string(REGEX MATCHALL "\\[policy\\] [a-z]+ +reason=[a-z0-9-]+ +partials=[0-9]+ now_ms=[0-9]+"
       decisions "${out}")
set(while_up "")
foreach(d IN LISTS decisions)
    string(REGEX REPLACE ".*now_ms=([0-9]+)" "\\1" t "${d}")
    if(t GREATER 130100 AND t LESS 190000)
        list(APPEND while_up "${d}")
    endif()
endforeach()
list(LENGTH while_up n_while_up)
if(NOT n_while_up EQUAL 1 OR NOT while_up MATCHES "reason=status +partials=[0-9]+ now_ms=160000"
   OR out MATCHES "partial refresh skipped")
    message(FATAL_ERROR "the diagnostics screen's redraws while the link changed were "
                        "\"${while_up}\", not one at 160 s:\n${out}")
endif()
message(STATUS "diagnostics: one redraw for the link's change, none skipped")
