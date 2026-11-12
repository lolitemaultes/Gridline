# Gridline: the app survives a system MIDI service that refuses or hangs, in the real app
# (hidden, isolated config and home, dummy audio, test hook GRIDLINE_TEST_MIDI; the real MIDI
# service is never asked):
#   MODE=fail: the service refuses. The app starts, plays the project (sound comes out), the
#     MIDI settings page shows the message, the state is "failed" with the reason, and Retry
#     asks again (one more attempt) without a crash.
#   MODE=hang: the service never answers. Startup still finishes at once (the first frames run
#     while the probe is out), the state turns "not-responding" after the timeout, playback
#     works, and the app quits normally with the probe still hung.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -DMODE=fail|hang -P midi_unavailable_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_TEST_MIDI} "${MODE}")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# "midistate frame F: state S attempts A inputs I outputs O retryin R age T error "E""
function(midistate frame)
  if(NOT out MATCHES "midistate frame ${frame}: state ([a-z-]+) attempts ([0-9]+) inputs ([0-9]+) outputs ([0-9]+) retryin ([-0-9.]+) age ([0-9]+)\\.[0-9]+ error \"([^\"]*)\"")
    message(FATAL_ERROR "FAIL: no midistate report at frame ${frame}\nOutput:\n${out}")
  endif()
  set(m_state "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(m_attempts "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(m_inputs "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(m_outputs "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(m_retry "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(m_age "${CMAKE_MATCH_6}" PARENT_SCOPE)  # whole seconds
  set(m_error "${CMAKE_MATCH_7}" PARENT_SCOPE)
endfunction()

function(playing frame)
  if(NOT out MATCHES "playstate frame ${frame}: playing ([01])")
    message(FATAL_ERROR "FAIL: no playstate report at frame ${frame}\nOutput:\n${out}")
  endif()
  set(p_playing "${CMAKE_MATCH_1}" PARENT_SCOPE)
  if(NOT out MATCHES "sound frame ${frame}: peak ([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "FAIL: no sound report at frame ${frame}\nOutput:\n${out}")
  endif()
  set(p_peak "${CMAKE_MATCH_1}${CMAKE_MATCH_2}" PARENT_SCOPE)  # millionths (6 decimals)
endfunction()

if(MODE STREQUAL "fail")
  # Retry is judged within one frame (the attempts just before and just after it), so the service's
  # own retries (5 s after a refusal, whenever a slow machine reaches them) never count as its. The
  # press at 34 starts that 5 s over; the one judged at 36 follows once its answer is in.
  file(WRITE "${OUT}/script.txt" "4 midistate\n6 startsong\n12 waitfor midi-none 1.5\n30 playstate\n30 sound\n32 midistate\n34 midiretry\n35 forget midi-settled\n35 waitfor midi-settled 10\n36 midistate\n36 midiretry\n36 midistate\n40 waitfor midi-none 0.5\n50 midistate\n")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames 60 --show midi
                          --screenshot "${OUT}/midi_failed.ppm" --dev-input "${OUT}/script.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  check("the app runs and quits normally with MIDI refused (${rc}): ${err}" rc EQUAL 0)
  midistate(4)
  check("refused at startup: state failed (${m_state})" m_state STREQUAL "failed")
  check("the reason is kept (${m_error})" m_error MATCHES "simulated")
  check("no ports listed (${m_inputs} ${m_outputs})" m_inputs EQUAL 0 AND m_outputs EQUAL 0)
  check("a retry is scheduled (${m_retry})" m_retry GREATER 0)
  playing(30)
  check("the project plays with MIDI unavailable" p_playing EQUAL 1)
  check("and sounds (peak ${p_peak} millionths)" p_peak GREATER 1000)
  check("the first Retry's answer came" out MATCHES "waitfor midi-settled: reached")
  string(REGEX MATCHALL "midistate frame 36: state [a-z-]+ attempts [0-9]+" m36 "${out}")
  list(LENGTH m36 n36)
  check("two states around the judged Retry (${n36})" n36 EQUAL 2)
  list(GET m36 0 b36)
  list(GET m36 1 a36)
  string(REGEX REPLACE ".* state ([a-z-]+) attempts ([0-9]+)" "\\1" bstate "${b36}")
  string(REGEX REPLACE ".* attempts ([0-9]+)" "\\1" before "${b36}")
  string(REGEX REPLACE ".* attempts ([0-9]+)" "\\1" after "${a36}")
  check("no probe out when Retry is pressed (${bstate})" bstate STREQUAL "failed")
  math(EXPR one_more "${before} + 1")
  check("Retry asks the service again (${before} to ${after})" after EQUAL one_more)
  midistate(50)
  check("still failed after the retry, no crash (${m_state})" m_state STREQUAL "failed")
  check("the settings page was drawn" EXISTS "${OUT}/midi_failed.ppm")
elseif(MODE STREQUAL "hang")
  file(WRITE "${OUT}/script.txt" "2 midistate\n4 startsong\n6 waitfor midi-none 3.6\n10 midistate\n20 playstate\n20 sound\n")
  string(TIMESTAMP t0 "%s")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames 30 --show midi
                          --screenshot "${OUT}/midi_not_responding.ppm" --dev-input "${OUT}/script.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  string(TIMESTAMP t1 "%s")
  math(EXPR took "${t1} - ${t0}")
  check("the app runs and quits normally with MIDI hung (${rc}): ${err}" rc EQUAL 0)
  midistate(2)
  check("the first frames run while the service has not answered (${m_state})"
        (m_state STREQUAL "opening" OR m_state STREQUAL "not-responding") AND m_attempts EQUAL 1)
  check("startup finished within a few seconds of asking the service (${m_age} s)" m_age LESS 5)
  midistate(10)
  check("past the timeout: not responding (${m_state})" m_state STREQUAL "not-responding")
  check("no ports listed (${m_inputs} ${m_outputs})" m_inputs EQUAL 0 AND m_outputs EQUAL 0)
  playing(20)
  check("the project plays with MIDI hung" p_playing EQUAL 1)
  check("and sounds (peak ${p_peak} millionths)" p_peak GREATER 1000)
  # The run holds 3.6 s on purpose; the rest is an ordinary hidden start and quit.
  check("startup and quit do not wait on the hung service (${took} s in all)" took LESS 25)
  check("the settings page was drawn" EXISTS "${OUT}/midi_not_responding.ppm")
else()
  message(FATAL_ERROR "MODE must be fail or hang")
endif()
