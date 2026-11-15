# Gridline: recording notes in the real app (hidden, isolated config and HOME, no sound device).
#   1. The record options (merge or replace, quantize, takes, controller target, count-in, punch, what
#      records) are kept after a quit and a new start.
#   2. Two keys played on the typing keyboard while recording in pattern mode become two notes in the
#      selected pattern, each once, in the order played, with their own lengths; one undo removes both.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k226_record_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

set(step 0)
function(run_app script)
  math(EXPR n "${step} + 1")
  set(step ${n} PARENT_SCOPE)
  file(WRITE "${OUT}/run${n}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${DEMO}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --dev-input "${OUT}/run${n}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 150)
  file(WRITE "${OUT}/run${n}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed in run ${n} (${rc}): ${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(expect_line what want)
  string(REGEX MATCH "recopt overdub [^\n]*" got "${out}")
  if(NOT got STREQUAL "${want}")
    message(FATAL_ERROR "FAIL: ${what}: \"${got}\", wanted \"${want}\"\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

# 1. A fresh config starts with the defaults; options changed before the quit...
run_app("4 recopt show
6 recopt overdub 0
7 recopt quantize 0
8 recopt qlen 1
9 recopt takes 1
10 recopt target 2
11 recopt countin 2
12 recopt punch 0
13 recopt audio 0
16 quit discard
")
expect_line("record options: the defaults at the first start" "recopt overdub 1 quantize 1 qlen 0 takes 0 notes 1 auto 1 target 0 countin 0 punch 1 audio 1")
# ...are there at the next start.
run_app("4 recopt show
6 recopt overdub 1
7 recopt takes 0
8 recopt countin 0
16 quit discard
")
expect_line("record options: kept after a quit (replace, no quantize, length quantize, takes, clips, 2-bar count-in, no punch, no audio)"
            "recopt overdub 0 quantize 0 qlen 1 takes 1 notes 1 auto 1 target 2 countin 2 punch 0 audio 0")

# 2. The typing keyboard records while the transport plays (pattern mode, merge, no quantize).
#    The keys are pressed on frames while the engine plays in real time: at the song's 124 BPM the
#    one-bar pattern loops every 1.9 s, which a slow runner's 80 frames can outlast (Weston CI: the C
#    held across the loop's end was cut there, 207+177 of 384 ticks, and the D landed on the next
#    pass at 191). At 20 BPM a pass is 12 s, so both keys fall in the first one (the tempo is its
#    own undo step, before the take).
run_app("4 playmode pattern
5 typing on
6 recopt qlen 0
7 tempo 20
8 recopt list
10 recopt arm 1
12 recopt play
30 sdlkey z down
50 sdlkey z up
60 sdlkey x down
80 sdlkey x up
90 recopt stop
94 recopt list
96 undo
98 recopt list
100 quit discard
")
string(REGEX MATCHALL "recnotes frame [0-9]+: [^\n]*" lists "${out}")
list(LENGTH lists nl)
if(NOT nl EQUAL 3)
  message(FATAL_ERROR "FAIL: three note lists expected, got ${nl}\n${out}")
endif()
list(GET lists 0 before)
list(GET lists 1 after)
list(GET lists 2 undone)
string(REGEX MATCH "notes ([0-9]+):" m "${before}")
set(n0 "${CMAKE_MATCH_1}")
string(REGEX MATCH "notes ([0-9]+):" m "${after}")
set(n1 "${CMAKE_MATCH_1}")
string(REGEX MATCH "notes ([0-9]+):" m "${undone}")
set(n2 "${CMAKE_MATCH_1}")
math(EXPR want "${n0} + 2")
if(NOT n1 EQUAL want)
  message(FATAL_ERROR "FAIL: two keys should add two notes (${n0} -> ${n1})\n${before}\n${after}\n${out}")
endif()
message(STATUS "ok - two typed keys while recording: two notes, none doubled or lost (${n0} -> ${n1})")
# The two new notes: the C and the D of the typing keyboard's octave, the D after the C.
string(REGEX MATCHALL " 48@[0-9]+\\+[0-9]+" cs "${after}")
string(REGEX MATCHALL " 50@[0-9]+\\+[0-9]+" ds "${after}")
string(REGEX MATCHALL " 48@[0-9]+\\+[0-9]+" cs0 "${before}")
string(REGEX MATCHALL " 50@[0-9]+\\+[0-9]+" ds0 "${before}")
list(LENGTH cs ncs)
list(LENGTH ds nds)
list(LENGTH cs0 ncs0)
list(LENGTH ds0 nds0)
math(EXPR wc "${ncs0} + 1")
math(EXPR wd "${nds0} + 1")
if(NOT ncs EQUAL wc OR NOT nds EQUAL wd)
  message(FATAL_ERROR "FAIL: one C (48) and one D (50) recorded\n${after}")
endif()
foreach(x IN LISTS cs0)
  list(REMOVE_ITEM cs "${x}")
endforeach()
foreach(x IN LISTS ds0)
  list(REMOVE_ITEM ds "${x}")
endforeach()
string(REGEX MATCH "@([0-9]+)\\+([0-9]+)" m "${cs}")
set(cStart "${CMAKE_MATCH_1}")
set(cLen "${CMAKE_MATCH_2}")
string(REGEX MATCH "@([0-9]+)\\+([0-9]+)" m "${ds}")
set(dStart "${CMAKE_MATCH_1}")
set(dLen "${CMAKE_MATCH_2}")
if(NOT dStart GREATER cStart OR cLen LESS 1 OR dLen LESS 1)
  message(FATAL_ERROR "FAIL: the D after the C, both with a length (C ${cStart}+${cLen}, D ${dStart}+${dLen})\n${after}")
endif()
message(STATUS "ok - the notes in the order played, with their own lengths (C ${cStart}+${cLen}, D ${dStart}+${dLen})")
if(NOT n2 EQUAL n0)
  message(FATAL_ERROR "FAIL: one undo should remove the whole take (${n1} -> ${n2}, before ${n0})\n${undone}")
endif()
message(STATUS "ok - one undo removes the take")
