# Gridline — J-185: Seltmann's slice pitch, "start the sequence here" and direct slice resizing, driven
# like a user (hidden window, isolated config and HOME, fixed frame time, SDL's dummy audio driver):
#   1. drag a pad's pitch readout up: the region plays 3 semitones higher (one undo step);
#   2. right-click a sequence block, pick "Start the sequence here": the sequence plays from that
#      cell on (the cells before move to the end), the START flag moves onto it, Undo brings it back;
#   3. drag the cut between two regions in the waveform: both resize, the cuts are kept by hand.
# Positions come from the editor's GRIDLINE_SELTMANN_TRACE geometry lines. Screenshots in <OUT>.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_seltmann> -DOUT=<dir> -P j185_seltmann_app.cmake
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

# the last state line of a run
function(last_state var text)
  string(REGEX MATCHALL "seltmann: state ready[^\n]*" lines "${text}")
  list(GET lines -1 l)
  set(${var} "${l}" PARENT_SCOPE)
endfunction()

# ---- the editor, ready: where things are
run(ready 30 "")
set(last_out "${ready_out}")
check("the break loads and is chopped" ready_out MATCHES "seltmann: state ready kind [a-z]+ bpm [0-9.]+ slices ([0-9]+)")
string(REGEX MATCH "seltmann: state ready kind [a-z]+ bpm [0-9.]+ slices ([0-9]+)" _ "${ready_out}")
set(slices "${CMAKE_MATCH_1}")
check("... in order, starting on A, nothing pitched" ready_out MATCHES "order \"A B C D [^\"]*\" pitch \"\" start A")
string(REGEX MATCH "seltmann: padchip 2 ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(pcx "${CMAKE_MATCH_1}")
set(pcy "${CMAKE_MATCH_2}")
string(REGEX MATCH "seltmann: seq 3 ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(s3x "${CMAKE_MATCH_1}")
set(s3y "${CMAKE_MATCH_2}")
string(REGEX MATCH "seltmann: startflag ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(flag0 "${CMAKE_MATCH_1}")
string(REGEX MATCH "seltmann: cut 2 ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(c2x "${CMAKE_MATCH_1}")
set(c2y "${CMAKE_MATCH_2}")
check("... the pad pitch readouts, the blocks, the START flag and the cut handles are traced"
      pcx MATCHES "^[0-9]+$" AND s3x MATCHES "^[0-9]+$" AND flag0 MATCHES "^[0-9]+$" AND c2x MATCHES "^[0-9]+$")
last_state(st0 "${ready_out}")
string(REGEX MATCH "cuts ([0-9]+) ([0-9]+)" _ "${st0}")
set(cut2before "${CMAKE_MATCH_2}")

# ---- 1. pitch: drag pad C's readout up 24 px (8 px a semitone)
math(EXPR py1 "${pcy} - 10")
math(EXPR py2 "${pcy} - 24")
set(pitch "10 move ${pcx} ${pcy}
12 down 0
14 move ${pcx} ${py1}
16 move ${pcx} ${py2}
18 up 0
")
run(pitch 30 "${pitch}")
set(last_out "${pitch_out}")
last_state(st "${pitch_out}")
check("dragging pad C's pitch readout up pitches C 3 semitones" st MATCHES "pitch \"C:\\+3\"")
run(pitchundo 34 "${pitch}24 undo
")
set(last_out "${pitchundo_out}")
last_state(st "${pitchundo_out}")
check("... one undo step takes it back" st MATCHES "pitch \"\" ")

# ---- 2. right-click block D: the menu; pick "Start the sequence here"
set(menu "${pitch}20 move ${s3x} ${s3y}
22 down 1
23 up 1
")
run(menu 30 "${menu}")
set(last_out "${menu_out}")
string(REGEX MATCHALL "seltmann: menu start [0-9]+ [0-9]+" ms "${menu_out}")
list(LENGTH ms nms)
check("right-clicking a block opens the region menu with Start here" nms GREATER 0)
list(GET ms -1 m)
string(REGEX MATCH "menu start ([0-9]+) ([0-9]+)" _ "${m}")
set(mx "${CMAKE_MATCH_1}")
set(my "${CMAKE_MATCH_2}")
check("... and Pitch and \"leave out the steps before\"" menu_out MATCHES "seltmann: menu pitch" AND menu_out MATCHES "seltmann: menu startcut")
set(start "${menu}26 move ${mx} ${my}
28 down 0
29 up 0
32 move 700 120
")
run(start 40 "${start}")
set(last_out "${start_out}")
last_state(st "${start_out}")
check("Start the sequence here: it plays from D on, A B C after" st MATCHES "order \"D E F [^\"]* A B C\" pitch \"C:\\+3\" start D")
check("... the regions stay (${slices})" st MATCHES "slices ${slices} ")
string(REGEX MATCHALL "seltmann: startflag ([0-9]+) [0-9]+" fl "${start_out}")
list(GET fl -1 f)
string(REGEX MATCH "startflag ([0-9]+)" _ "${f}")
set(flag1 "${CMAKE_MATCH_1}")
check("... the START flag moved onto D (${flag0} -> ${flag1})" flag1 GREATER flag0)
run(startundo 44 "${start}36 undo
")
set(last_out "${startundo_out}")
last_state(st "${startundo_out}")
check("... one undo step brings A B C D back" st MATCHES "order \"A B C D [^\"]*\" pitch \"C:\\+3\" start A")

# ---- 3. resize: drag the B|C cut 40 px to the right in the waveform (past the next sixteenth: the cuts snap)
math(EXPR c2a "${c2x} + 12")
math(EXPR c2b "${c2x} + 40")
run(resize 40 "10 move ${c2x} ${c2y}
12 down 0
14 move ${c2a} ${c2y}
16 move ${c2b} ${c2y}
18 up 0
22 move 700 120
")
set(last_out "${resize_out}")
last_state(st "${resize_out}")
string(REGEX MATCH "chop ([0-9]+) cuts ([0-9]+) ([0-9]+)" _ "${st}")
set(chop "${CMAKE_MATCH_1}")
set(cut2after "${CMAKE_MATCH_3}")
check("dragging the cut resizes B and C (${cut2before} -> ${cut2after})" cut2after GREATER cut2before)
check("... the cuts are kept by hand (chop mode ${chop})" chop EQUAL 5)
check("... same number of regions, still in order" st MATCHES "slices ${slices} [^\n]*order \"A B C D [^\"]*\"")

# ---- 3b. in order, the edge between two sequence blocks is the same cut: drag A|B to the right; and a
# plain hover over a cut (the handle shows; screenshot)
string(REGEX MATCH "seltmann: lanecut 1 ([0-9]+) ([0-9]+)" _ "${ready_out}")
set(l1x "${CMAKE_MATCH_1}")
set(l1y "${CMAKE_MATCH_2}")
string(REGEX MATCH "cuts ([0-9]+)" _ "${st0}")
set(cut1before "${CMAKE_MATCH_1}")
math(EXPR l1a "${l1x} + 12")
math(EXPR l1b "${l1x} + 40")
run(lane 40 "10 move ${l1x} ${l1y}
12 down 0
14 move ${l1a} ${l1y}
16 move ${l1b} ${l1y}
18 up 0
22 move ${c2x} ${c2y}
")
set(last_out "${lane_out}")
last_state(st "${lane_out}")
string(REGEX MATCH "cuts ([0-9]+)" _ "${st}")
set(cut1after "${CMAKE_MATCH_1}")
check("dragging the A|B edge in the sequence resizes A and B (${cut1before} -> ${cut1after})" cut1after GREATER cut1before)
check("... the sequence still plays every region in order" st MATCHES "slices ${slices} [^\n]*order \"A B C D [^\"]*\"")

foreach(f ready pitch menu start resize lane)
  check("screenshot ${f}.ppm written" EXISTS "${OUT}/${f}.ppm")
endforeach()
