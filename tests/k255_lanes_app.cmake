# Gridline: K-255, hidden app runs (isolated config and home): the channel rack's lane column.
#   - A pattern with lanes shows each row's grid chip and length handle (the Hat on 12 a bar, the
#     Snare on 20 a bar with a length of 7 that runs on, the Clap with a length of 3 that restarts).
#   - Dragging the Snare's handle changes its length in one undo step; undo brings back 7.
#   - The same rows in the Smooth and Clips layouts (screenshots), and a song without lanes shows no
#     lane column (no chip, no handle).
#   cmake -DAPP=<gridline-app> -DLANES=<k255_lanes.gridline.json> -DPLAIN=<song without lanes> -DOUT=<dir> -P k255_lanes_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${LANES}" lanesSong)  # copies: the fixtures are never opened (or saved over) in place
file(WRITE "${OUT}/lanes.gridline.json" "${lanesSong}")
file(READ "${PLAIN}" plainSong)
file(WRITE "${OUT}/plain.gridline.json" "${plainSong}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(run tag frames script)
  set(d "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${d}/cfg" "${d}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${d}/cfg")
  set(ENV{HOME} "${d}/home")
  file(WRITE "${d}/input.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --size 1440x900 --frames ${frames}
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

# "  lane <i> grid G steps N length L mode M chip x0 y0 x1 y1 handle x0 y0 x1 y1 step0 X step1 X" of frame f
function(lane tag f i)
  string(REGEX MATCH "rack frame ${f}:.*" tail "${${tag}_out}")
  # (CMake keeps nine groups a match: the line is read in two parts)
  if(NOT tail MATCHES "\n  lane ${i} grid ([0-9]+) steps ([0-9]+) length ([0-9]+) mode ([a-zA-Z]+) chip ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) handle")
    fail(${tag} "no lane ${i} at frame ${f}")
  endif()
  set(l_grid "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(l_steps "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(l_len "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(l_mode "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(l_chip_x1 "${CMAKE_MATCH_7}" PARENT_SCOPE)
  if(NOT tail MATCHES "\n  lane ${i} grid [^\n]* handle ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) step0 ([-0-9.]+) step1 ([-0-9.]+)")
    fail(${tag} "no lane ${i} handle at frame ${f}")
  endif()
  set(l_hx0 "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(l_hy0 "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(l_hx1 "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(l_hy1 "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(l_step0 "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(l_step1 "${CMAKE_MATCH_6}" PARENT_SCOPE)
endfunction()
function(undoAt tag f out)
  if(NOT ${tag}_out MATCHES "rack frame ${f}: [^\n]* undo ([0-9]+)")
    fail(${tag} "no rack dump at frame ${f}")
  endif()
  set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
function(int out v)
  string(REGEX REPLACE "\\..*" "" r "${v}")
  set(${out} "${r}" PARENT_SCOPE)
endfunction()

# ---- the lanes as the rack shows them ----
run(probe 16 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
14 rack
" "${OUT}/lanes.gridline.json")
lane(probe 14 2)
if(NOT l_grid EQUAL 12 OR NOT l_steps EQUAL 24 OR NOT l_len EQUAL 0)
  fail(probe "the Hat: grid ${l_grid} steps ${l_steps} length ${l_len}, wanted 12 / 24 / 0")
endif()
if(l_chip_x1 STREQUAL "0.0")
  fail(probe "the Hat has no grid chip")
endif()
lane(probe 14 1)
if(NOT l_len EQUAL 3 OR NOT l_mode STREQUAL "restart")
  fail(probe "the Clap: length ${l_len} mode ${l_mode}, wanted 3 restart")
endif()
lane(probe 14 3)
if(NOT l_grid EQUAL 20 OR NOT l_len EQUAL 7 OR NOT l_mode STREQUAL "runOn")
  fail(probe "the Snare: grid ${l_grid} length ${l_len} mode ${l_mode}, wanted 20 / 7 / runOn")
endif()
int(hx0 ${l_hx0})
int(hx1 ${l_hx1})
int(hy0 ${l_hy0})
int(hy1 ${l_hy1})
int(s0 ${l_step0})
int(s1 ${l_step1})
math(EXPR hx "(${hx0} + ${hx1}) / 2")
math(EXPR hy "(${hy0} + ${hy1}) / 2")
math(EXPR pitch "${s1} - ${s0}")
math(EXPR to "${hx} + ${pitch} * 4 + ${pitch} / 3")   # about four steps further
math(EXPR mid "${hx} + ${pitch} * 2")
undoAt(probe 14 u0)

# ---- dragging the Snare's handle: one undo step; undo brings the length back ----
run(drag 50 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
14 rack
16 move ${hx} ${hy}
18 down 0
20 move ${mid} ${hy}
22 move ${to} ${hy}
24 up 0
28 rack
32 undo
36 rack
40 shot \"${OUT}/drag/after-undo.ppm\"
44 quit discard
" "${OUT}/lanes.gridline.json")
lane(drag 28 3)
if(l_len LESS 9 OR l_len GREATER 13)
  fail(drag "the Snare's length after the drag is ${l_len}, wanted about 11")
endif()
if(NOT l_mode STREQUAL "runOn")
  fail(drag "the drag changed the Snare's mode to ${l_mode}")
endif()
undoAt(drag 14 ua)
undoAt(drag 28 ub)
math(EXPR steps "${ub} - ${ua}")
if(NOT steps EQUAL 1)
  fail(drag "the drag took ${steps} undo steps, wanted 1")
endif()
lane(drag 36 3)
if(NOT l_len EQUAL 7)
  fail(drag "undo left the Snare's length at ${l_len}, wanted 7")
endif()

# ---- a song without lanes: no lane column ----
run(plain 16 "5 toasts clear
6 closeplugins
9 focus \"Channel rack\"
14 rack
" "${OUT}/plain.gridline.json")
lane(plain 14 0)
if(NOT l_chip_x1 STREQUAL "0.0" OR NOT l_hx1 STREQUAL "0.0")
  fail(plain "a song without lanes shows a lane chip or handle")
endif()

# ---- the other layouts draw the same rows (screenshots for review) ----
# (logic is Smooth, live is Clips)
foreach(layout logic live tracker)
  run(shot_${layout} 40 "5 toasts clear
6 closeplugins
10 playmode pattern
14 focus \"Channel rack\"
20 rack
30 shot \"${OUT}/shot_${layout}/rack.ppm\"
36 quit discard
" "${OUT}/lanes.gridline.json" --layout-dev --layout ${layout})
endforeach()
message(STATUS "k255 lanes app: OK")
