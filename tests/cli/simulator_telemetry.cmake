# SPDX-License-Identifier: Apache-2.0
#
# The simulator's telemetry (docs/adr/0030-telemetry-ports-and-the-reporter.md):
#
#   * a day: normal_day.csv for 24 virtual hours over the broker model, and
#     the state messages within bounds -- at least one a heartbeat, never more
#     than a minute apart, not many more than the heartbeat's;
#   * an outage: ten minutes of no network change nothing the card sees --
#     the recordings of three runs, one without telemetry, one with, one with
#     the outage, are the same, byte for byte (requirements 15.1) -- and reach
#     the screen only as the status bar's link, a minute late each way
#     (docs/adr/0031-the-network-on-the-screen.md): the same redraws with and
#     without the outage until it has lasted a minute, then "no-wifi" and,
#     a minute after the session is back, "online", each on the glass within
#     a sensor cycle; while the broker sees the session dropped, the last will
#     one and a half keepalives after the last message it had, nothing
#     published meanwhile, and the next session announce itself again: online,
#     every discovery config, the state;
#   * stdout: every message the broker takes, printed;
#   * a malformed --device-id or --net-script refused before anything runs.
#
#   cmake -DSIMULATOR=<path> -DSCENARIOS=<dir> -DWORK=<dir> -P simulator_telemetry.cmake

foreach(var SIMULATOR SCENARIOS WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} must be defined")
    endif()
endforeach()

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

# --- a day ---------------------------------------------------------------------------

execute_process(
    COMMAND ${SIMULATOR} --headless --duration 86400 --wall-time 2026-10-16T00:00:00Z
            --scenario-env ${SCENARIOS}/normal_day.csv --telemetry loopback
    RESULT_VARIABLE result OUTPUT_VARIABLE day ERROR_VARIABLE day)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the day's run failed (${result}):\n${day}")
endif()
string(REGEX MATCHALL "\\[publish\\] state reason=[a-z]+ session=[0-9]+ now_ms=[0-9]+"
       states "${day}")
list(LENGTH states n_states)
# The heartbeat's floor, the session's first message at 3 s included, and a
# quarter more at most for the deltas.
if(n_states LESS 1440 OR n_states GREATER 1800)
    message(FATAL_ERROR "${n_states} state messages in a day, not 1440 to 1800:\n${day}")
endif()
set(last 0)
set(longest 0)
foreach(s IN LISTS states)
    string(REGEX REPLACE ".*now_ms=([0-9]+)" "\\1" t "${s}")
    math(EXPR gap "${t} - ${last}")
    if(gap GREATER longest)
        set(longest ${gap})
    endif()
    set(last ${t})
endforeach()
if(longest GREATER 60000)
    message(FATAL_ERROR "${longest} ms between two state messages, past the heartbeat")
endif()
if(NOT day MATCHES "\\[sim\\] telemetry: 1 sessions, 0 lost, 1 attempts \\(0 failed\\)")
    message(FATAL_ERROR "the day's run was not one session:\n${day}")
endif()
message(STATUS "a day: ${n_states} state messages, at most ${longest} ms apart")

# --- an outage ----------------------------------------------------------------------------

set(common --headless --duration 1800 --wall-time 2026-10-16T12:00:00Z
           --scenario-co2 ${SCENARIOS}/co2_spike_meeting.csv
           --scenario-env ${SCENARIOS}/normal_day.csv)
# The script's ';' stays inside one argument only when written out here: a
# list variable would split it.
execute_process(COMMAND ${SIMULATOR} ${common} --record ${WORK}/none
                RESULT_VARIABLE result_none OUTPUT_VARIABLE out_none ERROR_VARIABLE out_none)
execute_process(COMMAND ${SIMULATOR} ${common} --record ${WORK}/loopback --telemetry loopback
                RESULT_VARIABLE result_loopback OUTPUT_VARIABLE out_loopback
                ERROR_VARIABLE out_loopback)
execute_process(COMMAND ${SIMULATOR} ${common} --record ${WORK}/outage --telemetry loopback
                        --net-script "600:down;1200:up"
                RESULT_VARIABLE result_outage OUTPUT_VARIABLE out_outage
                ERROR_VARIABLE out_outage)
foreach(run none loopback outage)
    if(NOT result_${run} EQUAL 0)
        message(FATAL_ERROR "the run '${run}' failed (${result_${run}}):\n${out_${run}}")
    endif()
    string(REGEX MATCHALL "\\[policy\\][^\n]*" policy_${run} "${out_${run}}")
endforeach()

list(LENGTH policy_none n_policy)
if(n_policy LESS 20)
    message(FATAL_ERROR "only ${n_policy} redraw decisions without telemetry:\n${out_none}")
endif()

# The link on the screen: nothing without telemetry; with it, a place kept
# from the first frame and "online" once the session has held a minute -- the
# association 3 s, the session at once -- and in the outage "no-wifi" a minute
# after the network went, "online" a minute after the session was back.
set(links_none "")
set(links_loopback "unknown@0;online@63000")
set(links_outage "unknown@0;online@63000;no-wifi@660000;online@1263000")
foreach(run none loopback outage)
    string(REGEX MATCHALL "\\[link\\] [a-z-]+ now_ms=[0-9]+" lines "${out_${run}}")
    set(links "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "\\[link\\] ([a-z-]+) now_ms=([0-9]+)" "\\1@\\2" link "${line}")
        list(APPEND links ${link})
    endforeach()
    if(NOT "${links}" STREQUAL "${links_${run}}")
        message(FATAL_ERROR "the link on the screen in '${run}' was \"${links}\", "
                            "not \"${links_${run}}\":\n${out_${run}}")
    endif()
    # Each change after the first frame on the glass within a sensor cycle:
    # a redraw for it, or for anything else that carries it.
    foreach(link IN LISTS links)
        string(REGEX REPLACE ".*@" "" at "${link}")
        if(at EQUAL 0)
            continue()
        endif()
        math(EXPR by "${at} + 5000")
        set(drawn FALSE)
        foreach(p IN LISTS policy_${run})
            string(REGEX REPLACE ".*now_ms=([0-9]+).*" "\\1" t "${p}")
            if(t GREATER_EQUAL at AND t LESS_EQUAL by)
                set(drawn TRUE)
            endif()
        endforeach()
        if(NOT drawn)
            message(FATAL_ERROR "the link's change ${link} in '${run}' reached no redraw by "
                                "${by} ms:\n${out_${run}}")
        endif()
    endforeach()
endforeach()

# Until the outage has lasted the hold, the screen does not know of it: the
# same redraws as without it.
foreach(run loopback outage)
    set(early_${run} "")
    foreach(p IN LISTS policy_${run})
        string(REGEX REPLACE ".*now_ms=([0-9]+).*" "\\1" t "${p}")
        if(t LESS 660000)
            list(APPEND early_${run} "${p}")
        endif()
    endforeach()
endforeach()
if(NOT early_outage STREQUAL early_loopback)
    message(FATAL_ERROR "the outage reached the screen before it had lasted a minute:\n"
                        "${out_outage}")
endif()

foreach(run loopback outage)
    foreach(file 2026-10-16_scd4x.csv 2026-10-16_bme68x.csv)
        file(READ ${WORK}/none/data/${file} a)
        file(READ ${WORK}/${run}/data/${file} b)
        if(NOT a STREQUAL b)
            message(FATAL_ERROR "the recording ${file} of '${run}' differs from the one "
                                "without telemetry")
        endif()
    endforeach()
endforeach()

set(out "${out_outage}")
# The will falls due a keepalive and a half (90 s) after the last packet the
# broker had from the device, as a real broker counts it (MQTT 3.1.1,
# 3.1.2.10; ADR 0030's amendment) -- its last message before the network
# went, since the heartbeat sends one at least every keepalive and so no ping
# came between -- not 90 s after the network went.
string(REGEX MATCHALL "\\[publish\\][^\n]*now_ms=[0-9]+" published "${out}")
set(heard 0)
foreach(p IN LISTS published)
    string(REGEX REPLACE ".*now_ms=([0-9]+)" "\\1" t "${p}")
    if(t LESS_EQUAL 600000 AND t GREATER heard)
        set(heard ${t})
    endif()
endforeach()
math(EXPR quiet "600000 - ${heard}")
if(heard EQUAL 0 OR quiet GREATER 60000)
    message(FATAL_ERROR "the last message before the outage was at ${heard} ms, "
                        "not within a heartbeat of it:\n${out}")
endif()
math(EXPR will_at "${heard} + 90000")
message(STATUS "an outage: the last message at ${heard} ms, the will at ${will_at} ms")

# In this order: the network goes, the session with it, the will, the
# network back after its association, the second session announcing itself.
set(expected
    "\\[broker\\] simulator dropped: its network went now_ms=600000"
    "\\[net\\] connecting now_ms=600000"
    "\\[INFO\\] reporter: session lost session=1 why=network now_ms=600000"
    "\\[broker\\] last will of simulator: homeair/simulator/status offline now_ms=${will_at}"
    "\\[net\\] up now_ms=1203000"
    "\\[broker\\] simulator connected now_ms=1203000"
    "\\[publish\\] online, 9 discovery configs session=2 now_ms=1203000"
    "\\[publish\\] state reason=session session=2 now_ms=1203000")
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

# Nothing published while the network was gone: nothing is kept for later.
string(REGEX MATCHALL "\\[publish\\][^\n]*now_ms=[0-9]+" published "${out}")
foreach(p IN LISTS published)
    string(REGEX REPLACE ".*now_ms=([0-9]+)" "\\1" t "${p}")
    if(t GREATER 600000 AND t LESS 1203000)
        message(FATAL_ERROR "published during the outage: ${p}")
    endif()
endforeach()

# Two sessions, each with every config; the status back to online.
foreach(line
        "\\[sim\\] telemetry: 2 sessions, 1 lost, 2 attempts \\(0 failed\\)"
        "18 discovery configs"
        "\\[broker\\] homeair/simulator/status: 2 messages, kept: online"
        "\\[broker\\] homeassistant/sensor/simulator/co2/config: 2 messages"
        "\\[broker\\] 1 wills published")
    if(NOT out MATCHES "${line}")
        message(FATAL_ERROR "expected \"${line}\" in:\n${out}")
    endif()
endforeach()

# --- stdout ------------------------------------------------------------------------------------

execute_process(
    COMMAND ${SIMULATOR} --headless --duration 10 --scenario-co2 ${SCENARIOS}/co2_spike_meeting.csv
            --telemetry stdout --device-id kitchen_1
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the stdout run failed (${result}):\n${out}")
endif()
foreach(line
        "\\[broker\\] kitchen_1 homeair/kitchen_1/status qos=1 retain=1 online"
        "\\[broker\\] kitchen_1 homeassistant/sensor/kitchen_1/co2/config qos=1 retain=1 [{]"
        "\\[broker\\] kitchen_1 homeassistant/sensor/kitchen_1/pressure/config qos=1 retain=1 \\(empty\\)"
        "\\[broker\\] kitchen_1 homeair/kitchen_1/state qos=1 retain=1 [{]\"temperature\":")
    if(NOT out MATCHES "${line}")
        message(FATAL_ERROR "expected \"${line}\" in:\n${out}")
    endif()
endforeach()

# --- refused before anything runs ---------------------------------------------------------------

foreach(case
        "--telemetry|loopback|--device-id|bad id|--device-id: \"bad id\" is not 1 to 32 characters"
        "--telemetry|loopback|--net-script|10:sideways|--net-script: a change is <seconds>:down or <seconds>:up, not \"10:sideways\""
        "--net-script|10:down|--duration|10|--device-id and --net-script need --telemetry"
        "--telemetry|carrier-pigeon|--duration|10|--telemetry must be stdout or loopback")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 4 says)
    list(SUBLIST parts 0 4 args)
    execute_process(
        COMMAND ${SIMULATOR} --headless --scenario-co2 ${SCENARIOS}/co2_spike_meeting.csv ${args}
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(FIND "${out}" "${says}" at)
    if(result EQUAL 0 OR at LESS 0)
        message(FATAL_ERROR "${args} was not refused with \"${says}\" (${result}):\n${out}")
    endif()
endforeach()
