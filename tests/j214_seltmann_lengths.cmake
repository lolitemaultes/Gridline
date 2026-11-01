# Gridline — J-214: Seltmann's sequence is its own layer: a block's right edge sets that step's length
# in the sequence, never its region. Driven like a user (hidden window, isolated config and HOME,
# fixed frame time, SDL's dummy audio):
#   1. drag block A's right edge left by half a step: step 1 lasts a quarter beat, the next step follows
#      right after it, the regions (cuts) and the order stay, the loop keeps its whole bars (a rest);
#      one undo step takes the drag back;
#   2. drag block B's right edge right by a step: step 2 lasts a beat, the loop grows to the next bar line;
#   3. right-click the shortened block > Duplicate step: A A B ..., the copy keeps the length; > Reset
#      length: the default length again.
# Positions come from the editor's GRIDLINE_SELTMANN_TRACE geometry lines. Screenshots in <OUT>.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j214_seltmann_lengths.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
execute_process(COMMAND "${TESTBIN}" writeloop "${OUT}/Break 96.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/Break 96.wav")
  message(FATAL_ERROR "FAIL: could not write the synthetic break (${rc})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_SELTMANN_TRACE} "1")
set(ENV{GRIDLINE_SELTMANN_DEMO} "${OUT}/Break 96.wav")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "5 waitfor seltmann-ready 120\n${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.seltmann --size 1440x960 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

function(last_state var text)
  string(REGEX MATCHALL "seltmann: state ready[^\n]*" lines "${text}")
  list(GET lines -1 l)
  set(${var} "${l}" PARENT_SCOPE)
endfunction()

# the regions' first cuts, the step unit, the steps' total and the cycle of a state line
function(parts st)
  string(REGEX MATCH "cuts ([0-9 ]+) stepst" _ "${st}")
  set(cuts "${CMAKE_MATCH_1}" PARENT_SCOPE)
  string(REGEX MATCH "unit ([0-9.]+)" _ "${st}")
  set(unit "${CMAKE_MATCH_1}" PARENT_SCOPE)
  string(REGEX MATCH "stepsbeats ([0-9.]+) cycle ([0-9.]+)" _ "${st}")
  set(steps "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(cycle "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# ---- the editor, ready
run(ready 30 "")
set(last_out "${ready_out}")
last_state(st0 "${ready_out}")
check("in order, no own lengths" st0 MATCHES "order \"A B C D [^\"]*\"" AND st0 MATCHES "lens \"\" ")
parts("${st0}")
set(cuts0 "${cuts}")
set(unit0 "${unit}")
set(cycle0 "${cycle}")
check("the steps fill the loop (${steps} of ${cycle} beats, unit ${unit})" steps STREQUAL cycle)
foreach(i 0 1)
  string(REGEX MATCH "seltmann: steplen ${i} ([0-9]+) ([0-9]+)" _ "${ready_out}")
  set(e${i}x "${CMAKE_MATCH_1}")
  set(e${i}y "${CMAKE_MATCH_2}")
  string(REGEX MATCH "seltmann: seq ${i} ([0-9]+) ([0-9]+)" _ "${ready_out}")
  set(s${i}x "${CMAKE_MATCH_1}")
  set(s${i}y "${CMAKE_MATCH_2}")
endforeach()
check("the length handles and the blocks are traced (${e0x} ${e1x} ${s0x} ${s1x})"
      e0x MATCHES "^[0-9]+$" AND e1x MATCHES "^[0-9]+$" AND s1x MATCHES "^[0-9]+$")
math(EXPR bw "${s1x} - ${s0x}")  # a block's width (one unit)
check("a block is wide enough to drag (${bw} px)" bw GREATER 30)

# ---- 1. shorten step 1 by half a unit (A's right edge to the left)
math(EXPR half "${bw} / 2 + 3")
math(EXPR a1 "${e0x} - 8")
math(EXPR a2 "${e0x} - ${half}")
set(shorten "10 move ${e0x} ${e0y}
12 down 0
14 move ${a1} ${e0y}
16 move ${a2} ${e0y}
18 up 0
22 move 700 120
")
run(shorten 30 "${shorten}")
set(last_out "${shorten_out}")
last_state(st "${shorten_out}")
parts("${st}")
check("dragging A's right edge left gives step 1 its own length (half a unit)" st MATCHES "lens \"1:[0-9.]+\" ")
string(REGEX MATCH "lens \"1:([0-9.]+)\"" _ "${st}")
set(len1 "${CMAKE_MATCH_1}")
check("... ${len1} beats, shorter than the unit ${unit0}" len1 LESS unit0)
check("... the regions keep their cuts (${cuts0})" cuts STREQUAL cuts0)
check("... the order of the steps stays" st MATCHES "order \"A B C D [^\"]*\"")
check("... the loop keeps its whole bars (${cycle} = ${cycle0}): a rest at the end" cycle STREQUAL cycle0 AND NOT steps STREQUAL cycle)
run(shortenundo 34 "${shorten}26 undo
")
set(last_out "${shortenundo_out}")
last_state(st "${shortenundo_out}")
check("... one undo step takes the drag back" st MATCHES "lens \"\" ")

# ---- 2. lengthen step 2 by a unit (B's right edge to the right): the loop grows to the next bar line
math(EXPR b1 "${e1x} + 8")
math(EXPR b2 "${e1x} + ${bw} + 4")
run(lengthen 30 "10 move ${e1x} ${e1y}
12 down 0
14 move ${b1} ${e1y}
16 move ${b2} ${e1y}
18 up 0
22 move ${e1x} ${e1y}
")
set(last_out "${lengthen_out}")
last_state(st "${lengthen_out}")
parts("${st}")
check("dragging B's right edge right gives step 2 its own length" st MATCHES "lens \"2:[0-9.]+\" ")
check("... the regions keep their cuts" cuts STREQUAL cuts0)
check("... the loop grows to the next bar line (${cycle0} -> ${cycle})" cycle GREATER cycle0)

# ---- 3. the shortened block's menu: Duplicate step, then Reset length
string(REGEX MATCH "seltmann: seq 0 ([0-9]+) ([0-9]+)" _ "${shorten_out}")
set(m0x "${CMAKE_MATCH_1}")
set(m0y "${CMAKE_MATCH_2}")
set(menu "${shorten}24 move ${m0x} ${m0y}
26 down 1
27 up 1
")
run(menu 34 "${menu}")
set(last_out "${menu_out}")
string(REGEX MATCHALL "seltmann: menu dup [0-9]+ [0-9]+" ds "${menu_out}")
string(REGEX MATCHALL "seltmann: menu resetlen [0-9]+ [0-9]+" rs "${menu_out}")
list(LENGTH ds nds)
list(LENGTH rs nrs)
check("right-clicking the block opens its menu with Duplicate step and Reset length" nds GREATER 0 AND nrs GREATER 0)
list(GET ds -1 d)
string(REGEX MATCH "menu dup ([0-9]+) ([0-9]+)" _ "${d}")
set(dx "${CMAKE_MATCH_1}")
set(dy "${CMAKE_MATCH_2}")
list(GET rs -1 r)
string(REGEX MATCH "menu resetlen ([0-9]+) ([0-9]+)" _ "${r}")
set(rx "${CMAKE_MATCH_1}")
set(ry "${CMAKE_MATCH_2}")
run(dup 44 "${menu}30 move ${dx} ${dy}
32 down 0
33 up 0
36 move 700 120
")
set(last_out "${dup_out}")
last_state(st "${dup_out}")
parts("${st}")
check("Duplicate step: A A B C ..., the copy keeps the length" st MATCHES "order \"A A B C [^\"]*\"" AND st MATCHES "lens \"1:${len1} 2:${len1}\" ")
check("... the regions stay" cuts STREQUAL cuts0)
run(reset 44 "${menu}30 move ${rx} ${ry}
32 down 0
33 up 0
36 move 700 120
")
set(last_out "${reset_out}")
last_state(st "${reset_out}")
parts("${st}")
check("Reset length: the default length again, the steps fill the loop" st MATCHES "lens \"\" " AND steps STREQUAL cycle0 AND cycle STREQUAL cycle0)

foreach(f ready shorten lengthen menu dup)
  check("screenshot ${f}.ppm written" EXISTS "${OUT}/${f}.ppm")
endforeach()
