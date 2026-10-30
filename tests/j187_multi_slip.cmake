# Gridline: J-187, the slip tool on a selection (hidden, isolated configs, 1440x900, the playlist
# shown, a fixed 16 ms frame time). A copy of the demo song gets a sample channel ("Take") and three
# long audio clips on tracks 8, 9 and 10 (ids 70, 71, 72).
#   1. the three clips selected (select tool box), the slip tool (S) dragged left inside clip 70:
#      every selected clip's audio moves by the same amount, the clips stay put; one undo step;
#      undo restores all;
#   2. nothing selected: the same drag slips clip 70 alone.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j187_multi_slip.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clips "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(APPEND clips "{\"id\": 71, \"kind\": \"audio\", \"source\": 60, \"track\": 8, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 0.5, \"fadeIn\": 0.2, \"fadeInManual\": true}, ")
string(APPEND clips "{\"id\": 72, \"kind\": \"audio\", \"source\": 60, \"track\": 9, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 2.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clips}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --show playlist
                          --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(WRITE "${OUT}/${tag}.out" "${out}")
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    fail("${msg}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
function(int var v)  # "123.4" -> 123
  string(REGEX REPLACE "\\..*" "" i "${v}")
  set(${var} ${i} PARENT_SCOPE)
endfunction()

# Geometry of clip <id> at <frame>: <id>_x0 _x1 _y0 _y1 _head _m0 _m1 _n0 _n1 (integers).
function(clipgeom frame id)
  string(REGEX MATCH "plgeom frame ${frame}:[^\n]*\n(  [^\n]*\n)*" d "${out}")
  string(REGEX MATCH "  clip ${id} x ([-0-9.]+) ([-0-9.]+) y ([-0-9.]+) ([-0-9.]+) head ([0-9.]+) menu ([-0-9.]+) ([-0-9.]+) name ([-0-9.]+) ([-0-9.]+)" _ "${d}")
  if(CMAKE_MATCH_1 STREQUAL "")
    fail("no geometry for clip ${id} at frame ${frame}")
  endif()
  set(k 1)
  foreach(n x0 x1 y0 y1 head m0 m1 n0 n1)
    int(v "${CMAKE_MATCH_${k}}")
    set(${id}_${n} ${v} PARENT_SCOPE)
    math(EXPR k "${k} + 1")
  endforeach()
endfunction()

# The "dump" of <frame> -> clips_<frame> (the block) and undo_<frame>.
function(dump frame)
  string(REGEX MATCH "dump frame ${frame}: [0-9]+ clips, [0-9]+ selected, undo ([0-9]+)\n(  clip [^\n]*\n)*" d "${out}")
  if(d STREQUAL "")
    fail("no clip dump at frame ${frame}")
  endif()
  set(clips_${frame} "${d}" PARENT_SCOPE)
  set(undo_${frame} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
# Field <key> of clip <id> in dump <frame> -> <var> ("" when the clip is gone).
function(field var frame id key)
  string(REGEX MATCH "  clip ${id} kind [^\n]*" line "${clips_${frame}}")
  string(REGEX MATCH " ${key} ([^ ]+)" _ "${line}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
# A fixed-point field ("0.5012", "0.200000") of clip <id> -> integer in units of the last digit.
function(fixed var frame id key)
  field(v ${frame} ${id} ${key})
  if(v STREQUAL "")
    fail("clip ${id} missing in dump ${frame}")
  endif()
  string(REPLACE "." "" v "${v}")
  string(REGEX MATCH "^0*([0-9]+)$" _ "${v}")  # no leading zeros (one match: REPLACE re-anchors ^)
  set(${var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()
function(near what a b tol)
  math(EXPR d "${a} - ${b}")
  if(d LESS 0)
    math(EXPR d "0 - ${d}")
  endif()
  check("${what} (${a} vs ${b})" d LESS_EQUAL ${tol})
endfunction()

# ---- geometry ----
run(geo 14 "8 plgeom\n")
foreach(id 70 71 72)
  clipgeom(8 ${id})
endforeach()
math(EXPR selX0 "${72_x1} + 40")
math(EXPR selY0 "${72_y1} - 4")
math(EXPR selX1 "${70_x0} + 60")
math(EXPR selY1 "${70_y0} + ${70_head} + 6")
math(EXPR sx "(${70_x0} + ${70_x1}) / 2")
math(EXPR sy "(${70_y0} + ${70_head} + ${70_y1}) / 2")
math(EXPR sx1 "${sx} - 30")
math(EXPR sx2 "${sx} - 60")

run(slip 110 "2 typing off
3 move ${selX0} ${selY0}
4 down 1
5 up 1
7 key e
8 move ${selX0} ${selY0}
10 down 0
12 move ${selX1} ${selY1}
14 move ${selX1} ${selY1}
16 up 0
18 key s
20 dump
22 move ${sx} ${sy}
24 down 0
26 move ${sx1} ${sy}
28 move ${sx2} ${sy}
30 move ${sx2} ${sy}
32 up 0
36 dump
38 undo
40 dump
42 key escape
44 key e
46 move ${selX0} ${selY0}
47 down 0
49 up 0
51 key s
54 dump
56 move ${sx} ${sy}
58 down 0
60 move ${sx1} ${sy}
62 move ${sx2} ${sy}
64 move ${sx2} ${sy}
66 up 0
70 dump
")
foreach(f 20 36 40 54 70)
  dump(${f})
endforeach()
foreach(id 70 71 72)
  field(sel 20 ${id} sel)
  check("the select box selects clip ${id}" sel EQUAL 1)
endforeach()

# 1. every selected clip slipped by the same amount; positions unchanged; one undo step.
fixed(a70 36 70 audioStart)
fixed(a71 36 71 audioStart)
fixed(a72 36 72 audioStart)
check("slipping clip 70 moved its audio (${a70})" a70 GREATER 0)
near("clip 71 slipped with it" ${a71} ${a70} 2)
near("clip 72 slipped with it" ${a72} ${a70} 2)
foreach(id 70 71 72)
  field(st0 20 ${id} start)
  field(st1 36 ${id} start)
  check("clip ${id} stays where it was (${st0} -> ${st1})" st0 EQUAL st1)
endforeach()
math(EXPR want "${undo_20} + 1")
check("one undo step for the slip (${undo_20} -> ${undo_36})" undo_36 EQUAL want)
foreach(id 70 71 72)
  field(u 40 ${id} audioStart)
  check("undo restores clip ${id} (${u})" u STREQUAL "0.000000")
endforeach()

# 2. no selection: the drag slips only the clip under it.
field(n 54 70 sel)
check("selection cleared" n EQUAL 0)
fixed(b70 70 70 audioStart)
field(b71 70 71 audioStart)
field(b72 70 72 audioStart)
check("unselected: clip 70 slips (${b70})" b70 GREATER 0)
check("unselected: clips 71 and 72 stay (${b71} ${b72})" b71 STREQUAL "0.000000" AND b72 STREQUAL "0.000000")
