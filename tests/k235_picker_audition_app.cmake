# Gridline: K-235, auditioning from the playlist's clip sources list (hidden app runs, isolated
# config and home):
#   * pressing a pattern row while the transport is stopped plays that pattern;
#   * letting go stops it, and the current pattern stays the one it was;
#   * with playlist.pickerAudition off, pressing a row plays nothing.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k235_picker_audition_app.cmake
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

run(geom 16 "5 toasts clear\n6 closeplugins\n12 plgeom\n")
string(REGEX MATCH " pattern ([0-9]+) channel [0-9]+" _ "${geom_out}")
set(cur "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "\n  row [0-9]+ x [0-9.]+ y [0-9.]+" rows "${geom_out}")
set(px "")
foreach(r ${rows})
  if(r MATCHES "row ([0-9]+) x ([0-9]+)[0-9.]* y ([0-9]+)" AND NOT CMAKE_MATCH_1 STREQUAL cur AND px STREQUAL "")
    set(pid "${CMAKE_MATCH_1}")
    set(px "${CMAKE_MATCH_2}")
    set(py "${CMAKE_MATCH_3}")
  endif()
endforeach()
if(px STREQUAL "")
  fail(geom "no picker row other than the current pattern ${cur}")
endif()

set(script "5 toasts clear
6 closeplugins
10 sound
12 move ${px} ${py}
14 down 0
24 sound
26 up 0
32 sound
34 plgeom
40 quit discard
")
run(press 60 "${script}")
string(REGEX MATCHALL "sound frame [0-9]+: [^\n]* playing ([01])" snd "${press_out}")
list(LENGTH snd n)
if(n LESS 3)
  fail(press "no sound dumps")
endif()
list(GET snd 0 s0)
list(GET snd 1 s1)
list(GET snd 2 s2)
if(NOT s0 MATCHES "playing 0$" OR NOT s1 MATCHES "playing 1$" OR NOT s2 MATCHES "playing 0$")
  fail(press "pressing the row of pattern ${pid} did not play it until let go: ${s0} / ${s1} / ${s2}")
endif()
string(REGEX MATCH " pattern ([0-9]+) channel [0-9]+" _ "${press_out}")
message(STATUS "k235: pressing pattern ${pid} in the picker plays it while held; current pattern ${cur} -> ${CMAKE_MATCH_1}")

file(MAKE_DIRECTORY "${OUT}/off/cfg/gridline")
file(WRITE "${OUT}/off/cfg/gridline/settings.json" "{\"version\": 1, \"values\": {\"playlist.pickerAudition\": false}}\n")
run(off 60 "${script}")
string(REGEX MATCHALL "sound frame [0-9]+: [^\n]* playing ([01])" snd "${off_out}")
list(GET snd 1 o1)
if(NOT o1 MATCHES "playing 0$")
  fail(off "with auditioning off, pressing a row played: ${o1}")
endif()
message(STATUS "k235: off in settings: nothing plays")
