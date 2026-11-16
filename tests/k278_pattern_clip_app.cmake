# Gridline: K-278, clicks on a pattern clip in the playlist (hidden app run, isolated config and home):
#   * a single click selects the channel the pattern's notes play on and opens nothing;
#   * a double-click on a pattern with notes opens the piano roll for them (every layout, Studio included).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k278_pattern_clip_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(run tag frames script)
  set(d "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${d}/cfg" "${d}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${d}/cfg")
  set(ENV{HOME} "${d}/home")
  file(WRITE "${d}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1440x900 --frames ${frames}
                          --screenshot "${d}/end.ppm" --dev-input "${d}/input.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${d}/run.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app exited with ${rc}:\n${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(fail tag what)
  message(FATAL_ERROR "FAIL (${tag}): ${what}. Output:\n${${tag}_out}")
endfunction()

run(geom 16 "5 toasts clear\n6 closeplugins\n12 plgeom\n" --hide piano --hide rack)
# The first clip on the second track ("Bass": a pattern with notes): its body below the title strip.
# (A steps-only drum pattern opens the step sequencer instead.)
string(REGEX MATCH "track 1 top ([0-9]+)" _ "${geom_out}")
math(EXPR t1 "${CMAKE_MATCH_1} + 1")
if(NOT geom_out MATCHES "\n  clip ([0-9]+) x ([0-9]+)[0-9.]* ([0-9]+)[0-9.]* y (${t1})[0-9.]* ([0-9]+)[0-9.]* head ([0-9]+)")
  fail(geom "no clip geometry")
endif()
math(EXPR cx "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_3}) / 2")
math(EXPR cy "(${CMAKE_MATCH_4} + ${CMAKE_MATCH_6} + ${CMAKE_MATCH_5}) / 2")

run(act 90 "5 toasts clear
6 closeplugins
10 plgeom
12 windows
14 move ${cx} ${cy}
16 down 0
17 up 0
34 plgeom
36 windows
40 down 0
41 up 0
43 down 0
44 up 0
60 plgeom
80 quit discard
" --hide piano --hide rack)
string(REGEX MATCHALL " piano ([01]) pattern [0-9]+ channel ([0-9]+)" pg "${act_out}")
list(LENGTH pg n)
if(n LESS 3)
  fail(act "no playlist geometry")
endif()
list(GET pg 0 p0)
list(GET pg 1 p1)
list(GET pg 2 p2)
if(NOT p1 MATCHES " piano 0 ")
  fail(act "a single click on a pattern clip opened the piano roll (${p1})")
endif()
if(NOT p2 MATCHES " piano 1 ")
  fail(act "a double-click on a pattern clip did not open the piano roll (${p0} / ${p1} / ${p2})")
endif()
string(REGEX MATCHALL "windows frame [^\n]*(\n  window [^\n]*)*" ws "${act_out}")
list(GET ws 0 w0)
list(GET ws 1 w1)
string(REGEX MATCHALL "\n  window " a0 "${w0}")
string(REGEX MATCHALL "\n  window " a1 "${w1}")
list(LENGTH a0 n0)
list(LENGTH a1 n1)
if(NOT n0 EQUAL n1)
  fail(act "a single click on a pattern clip opened a window (${n0} -> ${n1})")
endif()
message(STATUS "k278: click selects (${p0} -> ${p1}), double-click opens the piano roll (${p2})")
