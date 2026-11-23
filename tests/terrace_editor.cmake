# Gridline — Terrace's editor in the app (hidden, off-screen, isolated config). Opens Terrace,
# draws across the low band's volume steps with real SDL mouse events, solos the low mid band, then
# undoes both: each edit is one undo step and undo takes it back.
# The editor reports through "terrace" (bands, pattern, undo position, solos, lanes, steps drawn)
# and the dev points "terrace.lane.<band>" (the band's step lane) and "terrace.solo.<band>".
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P terrace_editor.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{SDL_AUDIODRIVER} "dummy")

set(script "5 toasts clear
10 terrace
12 sdlpt terrace.lane.0 0.03 0.25
14 sdldown 0
16 sdlpt terrace.lane.0 0.15 0.25
18 sdlpt terrace.lane.0 0.30 0.25
20 sdlpt terrace.lane.0 0.45 0.25
22 sdlup 0
26 terrace
28 sdlpt terrace.solo.1 0.5 0.5
30 sdldown 0
32 sdlup 0
36 terrace
38 undo
42 terrace
44 undo
48 terrace
60 quit discard
")
file(WRITE "${OUT}/script.txt" "${script}")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.terrace --size 1600x1000 --frames 70
                        --screenshot "${OUT}/terrace.ppm" --dev-input "${OUT}/script.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}")
endif()
if(out MATCHES "not shown")
  message(FATAL_ERROR "FAIL: a Terrace control is not on screen. Output:\n${out}")
endif()

# The reports in the order the script asks for them (the editor prints each on its next frame):
# "terrace frame <f>: bands 4 pattern 0 undo <u> solo a b c d lanes ... drawn <n>"
string(REGEX MATCHALL "terrace frame [0-9]+: [^\n]*" reports "${out}")
function(report i undoVar soloVar drawnVar)
  list(LENGTH reports n)
  if(i GREATER_EQUAL n)
    message(FATAL_ERROR "FAIL: Terrace report ${i} missing (${n} printed). Output:\n${out}")
  endif()
  list(GET reports ${i} line)
  string(REGEX MATCH "undo ([0-9]+) solo ([0-9]) ([0-9]) [0-9] [0-9] lanes [0-9 ]+ drawn ([0-9]+)" m "${line}")
  set(${undoVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${soloVar} "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${drawnVar} "${CMAKE_MATCH_4}" PARENT_SCOPE)
endfunction()

report(0 u0 s0 d0)
report(1 u1 s1 d1)
report(2 u2 s2 d2)
report(3 u3 s3 d3)
report(4 u4 s4 d4)
math(EXPR u0p1 "${u0} + 1")
math(EXPR u0p2 "${u0} + 2")
if(NOT d0 EQUAL 0 OR d1 LESS 4 OR NOT u1 EQUAL u0p1)
  message(FATAL_ERROR "FAIL (draw): steps drawn ${d0} -> ${d1}, undo ${u0} -> ${u1} (one step expected). Output:\n${out}")
endif()
message(STATUS "ok - drawing across the low band's steps sets ${d1} steps in one undo step")
if(NOT s1 EQUAL 0 OR NOT s2 EQUAL 1 OR NOT u2 EQUAL u0p2)
  message(FATAL_ERROR "FAIL (solo): solo ${s1} -> ${s2}, undo ${u1} -> ${u2}. Output:\n${out}")
endif()
message(STATUS "ok - S solos the low mid band in one undo step")
if(NOT s3 EQUAL 0 OR NOT d3 EQUAL d1)
  message(FATAL_ERROR "FAIL (undo solo): solo ${s3}, drawn ${d3} (wanted 0 and ${d1}). Output:\n${out}")
endif()
if(NOT d4 EQUAL 0)
  message(FATAL_ERROR "FAIL (undo draw): drawn ${d4} after the second undo. Output:\n${out}")
endif()
message(STATUS "ok - undo takes back the solo, then the drawing")
