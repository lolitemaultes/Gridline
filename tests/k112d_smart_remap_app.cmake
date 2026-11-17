# Gridline - K-112D D7 (R13-01, R13-05): in the real app (Smooth), a Smart Control mapped by hand follows
# its plugin when the strip's effects are re-ordered (an undo step: Cmd+Z puts it back with the effect),
# Learn on a parameter already mapped to another control moves that mapping to the control in focus, and
# deleting the mapped plugin drops the mapping (never retargets it). A Learn that maps nothing does not
# mark the project modified.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112d_smart_remap_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"logic\", \"keys.profile\": \"logic\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
file(WRITE "${OUT}/input.txt" "5 toasts clear
10 key y
20 parts smart focus 1
22 parts smart learn param
26 parts smart touch 1 6
32 parts smart learn off
34 parts smart state
36 parts smart move 0 5
40 parts smart state
42 mod super 1
43 key z
44 mod super 0
48 parts smart state
50 mod super 1
51 mod shift 1
52 key z
53 mod shift 0
54 mod super 0
58 parts smart state
60 save ${OUT}/moved.gridline.json
62 parts smart focus 2
64 parts smart learn param
66 parts smart touch 0 6
72 parts smart learn off
74 parts smart state
76 parts smart focus 3
78 parts smart learn param
80 parts smart state
82 parts smart learn off
84 parts smart delete 0
88 parts smart state
92 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout-dev --layout logic --size 1920x1080
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 96
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc})")
endif()
string(REGEX MATCHALL "smart state: [^\n]*" states "${out}")
list(LENGTH states n)
if(NOT n EQUAL 7)
  message(FATAL_ERROR "FAIL: expected 7 state lines, got ${n}: ${states}")
endif()
foreach(i 0 1 2 3 4 5 6)
  list(GET states ${i} s${i})
  string(REGEX REPLACE ".* knobs ([^ ]*) .*" "\\1" k${i} "${s${i}}")
  string(REPLACE "," ";" k${i} "${k${i}}")
endforeach()
list(GET k0 1 m0)
string(REGEX REPLACE ":.*" "" owner "${m0}")
if(NOT s0 MATCHES "mapped 1" OR NOT m0 STREQUAL "${owner}:2006")
  message(FATAL_ERROR "FAIL: the mapping was not made (${s0})")
endif()
# R13-01: the EQ moved from slot 1 to slot 0 (the effect in slot 0 dragged to slot 5): the control follows it
list(GET k1 1 m1)
if(NOT s1 MATCHES "mapped 1" OR NOT m1 STREQUAL "${owner}:1006")
  message(FATAL_ERROR "FAIL: the mapping did not follow its plugin to slot 0 (${s1})")
endif()
list(GET k2 1 m2)
if(NOT m2 STREQUAL "${owner}:2006")
  message(FATAL_ERROR "FAIL: Cmd+Z did not put the mapping back with the effect (${s2})")
endif()
list(GET k3 1 m3)
if(NOT m3 STREQUAL "${owner}:1006")
  message(FATAL_ERROR "FAIL: Cmd+Shift+Z did not redo the move with the mapping (${s3})")
endif()
file(READ "${OUT}/moved.gridline.json" saved)
string(REGEX MATCH "\"controls\"[^]]*]" ctl "${saved}")
if(NOT ctl MATCHES "1006")
  message(FATAL_ERROR "FAIL: the saved mapping does not name the moved slot (${ctl})")
endif()
# R13-05: Learn on control 2 with the parameter mapped to control 1: the mapping moves to control 2
list(GET k4 2 m4b)
list(GET k4 1 m4a)
if(NOT s4 MATCHES "mapped 1" OR NOT m4b STREQUAL "${owner}:1006" OR m4a STREQUAL "${owner}:1006")
  message(FATAL_ERROR "FAIL: Learn on a parameter mapped elsewhere did not move the mapping (${s3} / ${s4})")
endif()
# R13-01: the mapped plugin deleted: the mapping is dropped, never retargeted
if(NOT s6 MATCHES "mapped 0")
  message(FATAL_ERROR "FAIL: deleting the mapped plugin kept the mapping (${s6})")
endif()
message(STATUS "k112d_smart_remap: ok")
