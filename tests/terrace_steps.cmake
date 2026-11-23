# Gridline — Terrace's step menu in the app (hidden, off-screen, isolated config), with real SDL
# mouse events: right-click a step of the low mid band and split it into 3 hits, undo it; Alt + wheel
# over a step of the high mid band gives it 2 hits; right-click the high band and pick the Lane grid
# "Three across the bar" (3 steps of 1/2T). Each is one undo step.
# The editor reports through "terrace": "... drawn <n> hits <h0 h1 h2 h3> grid <steps/size x 4>",
# where h is the hits over one on each band's volume lane.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P terrace_steps.cmake
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
12 sdlpt terrace.lane.1 0.34 0.6
14 sdldown 1
16 sdlup 1
22 sdlpt terrace.split.3 0.5 0.5
24 sdldown 0
26 sdlup 0
30 terrace
32 undo
36 terrace
38 sdlpt terrace.lane.2 0.5 0.6
40 sdlkey #226 down alt
42 wheel 1
44 sdlkey #226 up
48 terrace
50 sdlpt terrace.lane.3 0.1 0.6
52 sdldown 1
54 sdlup 1
58 sdlpt terrace.gridmenu 0.5 0.5
64 sdlpt terrace.grid.6 0.5 0.5
68 sdldown 0
70 sdlup 0
74 terrace
80 quit discard
")
file(WRITE "${OUT}/script.txt" "${script}")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.terrace --size 1600x1000 --frames 90
                        --dev-input "${OUT}/script.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}")
endif()
if(out MATCHES "not shown")
  message(FATAL_ERROR "FAIL: a Terrace control is not on screen. Output:\n${out}")
endif()

string(REGEX MATCHALL "terrace frame [0-9]+: [^\n]*" reports "${out}")
function(report i undoVar hitsVar gridVar)
  list(LENGTH reports n)
  if(i GREATER_EQUAL n)
    message(FATAL_ERROR "FAIL: Terrace report ${i} missing (${n} printed). Output:\n${out}")
  endif()
  list(GET reports ${i} line)
  string(REGEX MATCH "undo ([0-9]+) .* hits ([0-9]+ [0-9]+ [0-9]+ [0-9]+) grid ([0-9/ ]+)" m "${line}")
  set(${undoVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${hitsVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
  string(STRIP "${CMAKE_MATCH_3}" g)
  set(${gridVar} "${g}" PARENT_SCOPE)
endfunction()
report(0 u0 h0 g0)
report(1 u1 h1 g1)
report(2 u2 h2 g2)
report(3 u3 h3 g3)
report(4 u4 h4 g4)
math(EXPR u0p1 "${u0} + 1")
math(EXPR u0p2 "${u0} + 2")
if(NOT h0 STREQUAL "0 0 0 0" OR NOT h1 STREQUAL "0 2 0 0" OR NOT u1 EQUAL u0p1)
  message(FATAL_ERROR "FAIL (split): hits '${h0}' -> '${h1}', undo ${u0} -> ${u1} (wanted one step split into 3 in one undo step). Output:\n${out}")
endif()
message(STATUS "ok - the step menu splits a step into 3 hits in one undo step")
if(NOT h2 STREQUAL "0 0 0 0" OR NOT u2 EQUAL u0)
  message(FATAL_ERROR "FAIL (undo split): hits '${h2}', undo ${u2}. Output:\n${out}")
endif()
message(STATUS "ok - undo takes the split back")
if(NOT h3 STREQUAL "0 0 1 0" OR NOT u3 EQUAL u0p1)
  message(FATAL_ERROR "FAIL (alt wheel): hits '${h3}', undo ${u2} -> ${u3}. Output:\n${out}")
endif()
message(STATUS "ok - Alt + wheel over a step gives it 2 hits in one undo step")
if(NOT g4 MATCHES " 3/10$" OR NOT u4 EQUAL u0p2)
  message(FATAL_ERROR "FAIL (lane grid): grid '${g4}', undo ${u3} -> ${u4}. Output:\n${out}")
endif()
message(STATUS "ok - the Lane grid sets 3 steps of 1/2T in one undo step")
