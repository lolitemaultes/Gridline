# Gridline: the wide mixer's zones in the real app (hidden, isolated config and HOME, a fixed 16 ms
# frame time). The docked mixer is widened by dragging the dock line, so it lays out side by side.
#   1. Two rows of tracks: the strips keep to their content and the inspector takes the rest (the
#      line between them sits right after the "+" button).
#   2. One row of sixteen tracks: dragging the line gives the inspector a width of its own, which is
#      kept in the settings; a double-click on the line goes back to the automatic width.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k338_mixer_zones.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"studio\"}}\n")

function(run name rows script)
  file(WRITE "${OUT}/${name}.txt" "10 sdlmove 1471 500
12 sdldown 0
14 sdlmove 1300 500
16 sdlmove 1000 500
18 sdlmove 600 500
20 sdlup 0
${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size 1920x1080 --mixer-view rows${rows}
                          --dev-input "${OUT}/${name}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${o}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

# The line's centre x at a frame ("sdlat frame N: mixsplit at X,Y").
function(split_x frame var)
  if(NOT out MATCHES "sdlat frame ${frame}: mixsplit at ([0-9]+),")
    message(FATAL_ERROR "FAIL: no line between the strips and the inspector at frame ${frame}\n${out}")
  endif()
  set(${var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()

function(saved var)
  file(READ "${OUT}/cfg/gridline/settings.json" s)
  if(s MATCHES "\"ui.mixerInspectorW\": ([0-9.]+)")
    set(${var} ${CMAKE_MATCH_1} PARENT_SCOPE)
  else()
    set(${var} 0 PARENT_SCOPE)
  endif()
endfunction()

# 1. Two rows: master + eight strips + the "+" button, then the inspector.
run(tworows 2 "30 sdlat mixsplit
32 shot \"${OUT}/tworows.ppm\"
34 quit discard
")
split_x(30 x2)
if(x2 GREATER 1320 OR x2 LESS 1200)
  message(FATAL_ERROR "FAIL: with two rows the inspector should start right after the strips (line at ${x2})")
endif()
message(STATUS "ok: two rows: the strips keep to their content, the inspector takes the rest (line at ${x2})")

# 2. One row: the automatic width, then a drag 200 px to the left, kept; a double-click resets it.
run(drag 1 "30 sdlat mixsplit
32 sdldown 0
34 sdlmove 1500 600
36 sdlmove 1400 600
38 sdlup 0
44 sdlat mixsplit
46 quit discard
")
split_x(30 before)
split_x(44 after)
math(EXPR moved "${before} - ${after}")
if(moved LESS 150 OR moved GREATER 250)
  message(FATAL_ERROR "FAIL: the line should follow the drag (from ${before} to ${after})")
endif()
saved(w)
if(w LESS 450 OR w GREATER 600)
  message(FATAL_ERROR "FAIL: the dragged inspector width should be kept (${w})")
endif()
message(STATUS "ok: dragging the line gives the inspector its own width (${w}), kept in the settings")

run(again 1 "30 sdlat mixsplit
32 sdldown 0
33 sdlup 0
34 sdldown 0
35 sdlup 0
40 sdlat mixsplit
42 quit discard
")
split_x(30 kept)
if(NOT kept EQUAL after)
  message(FATAL_ERROR "FAIL: the next start keeps the width (line at ${kept}, was ${after})")
endif()
split_x(40 reset)
if(NOT reset EQUAL before)
  message(FATAL_ERROR "FAIL: a double-click goes back to the automatic width (line at ${reset}, automatic ${before})")
endif()
saved(w0)
if(NOT w0 EQUAL 0)
  message(FATAL_ERROR "FAIL: the automatic width is saved as 0 (${w0})")
endif()
message(STATUS "ok: the width is kept for the next start, and a double-click resets it")
