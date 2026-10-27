# Gridline: J-142, playlist clip edits in the real app (hidden, isolated configs, 1440x900, the
# playlist shown, a fixed 16 ms frame time). A copy of the demo song gets a sample channel ("Take")
# and three long audio clips on tracks 8, 9 and 10 (ids 70, 71, 72) with different levels
# (0 dB, -6 dB, +6 dB) and fade-ins (none, 0.2 s, none).
#   1. the three clips selected (select tool box), the volume handle of clip 70 dragged up: every clip
#      gains the same number of dB (they keep their differences); one undo step; undo restores all;
#   2. the fade-in handle of clip 70 dragged right: every clip's fade-in grows by the same time
#      (clip 71 keeps its 0.2 s lead); one undo step;
#   3. clip 72 alone (not selected): its volume drag moves only that clip;
#   4. right-clicks on a clip's title strip: the name box and the menu arrow open the clip menu
#      (nothing deleted); the rest of the strip deletes the clip, as does the body; a right-drag
#      that starts on a strip deletes every clip it crosses.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j142_clip_edits.cmake
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
check("clip 70 is drawn with its menu arrow and name box (${70_m0}..${70_m1}, ${70_n0}..${70_n1})"
      70_m1 GREATER 70_m0 AND 70_n1 GREATER 70_n0 AND 70_n0 GREATER_EQUAL 70_m1 AND 70_x1 GREATER 70_n1)

# Points: the selection drag (empty space right of the clips, up to the left of clip 70's body).
math(EXPR selX0 "${72_x1} + 40")
math(EXPR selY0 "${72_y1} - 4")
math(EXPR selX1 "${70_x0} + 60")
math(EXPR selY1 "${70_y0} + ${70_head} + 6")
# The volume handle of clip 70 / 72 (bottom middle) and clip 70's fade-in handle (no fade: at the left edge + 4).
math(EXPR g70x "(${70_x0} + ${70_x1}) / 2")
math(EXPR g70y "${70_y1} - 4")
math(EXPR g72x "(${72_x0} + ${72_x1}) / 2")
math(EXPR g72y "${72_y1} - 4")
math(EXPR fi70x "${70_x0} + 4")
math(EXPR fi70y "${70_y0} + ${70_head} + 2")
math(EXPR up1 "${g70y} - 15")
math(EXPR up2 "${g70y} - 30")
math(EXPR up72 "${g72y} - 15")
math(EXPR fiTo1 "${fi70x} + 20")
math(EXPR fiTo2 "${fi70x} + 40")

# ---- 1-3: relative edits of a selection ----
run(multi 120 "2 typing off
3 move ${selX0} ${selY0}
4 down 1
5 up 1
6 dump
7 key e
8 move ${selX0} ${selY0}
10 down 0
12 move ${selX1} ${selY1}
14 move ${selX1} ${selY1}
16 up 0
18 key p
20 dump
22 move ${g70x} ${g70y}
24 down 0
26 move ${g70x} ${up1}
28 move ${g70x} ${up2}
30 move ${g70x} ${up2}
32 up 0
36 dump
38 undo
40 dump
42 move ${fi70x} ${fi70y}
44 down 0
46 move ${fiTo1} ${fi70y}
48 move ${fiTo2} ${fi70y}
50 move ${fiTo2} ${fi70y}
52 up 0
56 dump
60 key escape
62 move ${selX0} ${selY0}
63 down 0
65 up 0
68 dump
70 move ${g72x} ${g72y}
72 down 0
74 move ${g72x} ${up72}
76 move ${g72x} ${up72}
78 up 0
82 dump
")
foreach(f 6 20 36 40 56 68 82)
  dump(${f})
endforeach()
foreach(id 70 71 72)
  field(sel 20 ${id} sel)
  check("the select box selects clip ${id}" sel EQUAL 1)
endforeach()

# 1. volume: the same number of dB on each (gain ratios kept: 71 = 70 / 2, 72 = 70 * 2).
fixed(g70 36 70 gain)
fixed(g71 36 71 gain)
fixed(g72 36 72 gain)
check("volume drag on clip 70 raised it (${g70})" g70 GREATER 15000)
math(EXPR g71x2 "${g71} * 2")
math(EXPR g70x2 "${g70} * 2")
near("clip 71 rose by the same dB (stays 6 dB under clip 70)" ${g71x2} ${g70} 6)
near("clip 72 rose by the same dB (stays 6 dB over clip 70)" ${g72} ${g70x2} 10)
math(EXPR want "${undo_20} + 1")
check("... one undo step for the drag (${undo_20} -> ${undo_36})" undo_36 EQUAL want)
fixed(u70 40 70 gain)
fixed(u71 40 71 gain)
fixed(u72 40 72 gain)
check("undo restores every clip (${u70} ${u71} ${u72})" u70 EQUAL 10000 AND u71 EQUAL 5000 AND u72 EQUAL 20000)

# 2. fade-in: the same time added to each (clip 71 keeps its 0.2 s lead).
fixed(f70 56 70 fadeIn)
fixed(f71 56 71 fadeIn)
fixed(f72 56 72 fadeIn)
check("fade-in drag on clip 70 gave it a fade (${f70})" f70 GREATER 1000)
math(EXPR lead "${f71} - ${f70}")
near("clip 71's fade-in grew by the same time" ${lead} 200000 2)
near("clip 72's fade-in grew by the same time" ${f72} ${f70} 2)
math(EXPR want "${undo_40} + 1")
check("... one undo step for the drag (${undo_40} -> ${undo_56})" undo_56 EQUAL want)

# 3. a click on empty space clears the selection; clip 72's volume alone.
field(sel 68 72 sel)
check("the selection is cleared" sel EQUAL 0)
fixed(a70 82 70 gain)
fixed(a71 82 71 gain)
fixed(a72 82 72 gain)
fixed(b70 68 70 gain)
fixed(b71 68 71 gain)
fixed(b72 68 72 gain)
check("single clip: only clip 72 moved (${b72} -> ${a72}; 70 ${b70} -> ${a70}, 71 ${b71} -> ${a71})"
      a72 GREATER b72 AND a70 EQUAL b70 AND a71 EQUAL b71)
near("... by 15 px = +3 dB, as before J-142 (x 1.4125)" ${a72} 28251 10)

# ---- 4: right-clicks on the title strip ----
math(EXPR hy70 "${70_y0} + ${70_head} / 2")
math(EXPR hy71 "${71_y0} + ${71_head} / 2")
math(EXPR hy72 "${72_y0} + ${72_head} / 2")
math(EXPR nameX "(${71_n0} + ${71_n1}) / 2")
math(EXPR arrowX "(${71_m0} + ${71_m1}) / 2")
math(EXPR restX "${72_x1} - 12")
math(EXPR bodyY70 "(${70_y0} + ${70_y1}) / 2 + 4")
math(EXPR bodyX70 "${70_x1} - 30")
# One right-click per run (a menu left open would take the next click): dumps at 6 and 16, windows at 15.
function(rclick tag x y)
  run(${tag} 24 "4 typing off\n6 dump\n8 move ${x} ${y}\n10 down 1\n12 up 1\n14 windows\n16 dump\n")
  dump(6)
  dump(16)
  string(REGEX MATCH "windows frame 15:[^\n]*\n(  [^\n]*\n)*" w "${out}")
  if(w STREQUAL "")
    fail("no windows dump in ${tag}")
  endif()
  set(menu 0)
  if(w MATCHES "Popup")
    set(menu 1)
  endif()
  set(menu ${menu} PARENT_SCOPE)
  set(clips_16 "${clips_16}" PARENT_SCOPE)
  set(out "${out}" PARENT_SCOPE)
endfunction()
function(gone var id)  # 1 when clip <id> is not in dump 16
  set(r 1)
  if(clips_16 MATCHES "  clip ${id} kind")
    set(r 0)
  endif()
  set(${var} ${r} PARENT_SCOPE)
endfunction()
rclick(rname ${nameX} ${hy71})
gone(g 71)
check("right-click on the name box: the clip stays and its menu opens" g EQUAL 0 AND menu EQUAL 1)
rclick(rarrow ${arrowX} ${hy71})
gone(g 71)
check("right-click on the menu arrow: the clip stays and its menu opens" g EQUAL 0 AND menu EQUAL 1)
rclick(rrest ${restX} ${hy72})
gone(g 72)
check("right-click on the title strip past the name: the clip is deleted, no menu" g EQUAL 1 AND menu EQUAL 0)
math(EXPR padX "${71_n1} + 6")
rclick(rpast ${padX} ${hy71})
gone(g 71)
check("right-click just past the name box: deleted" g EQUAL 1 AND menu EQUAL 0)
rclick(rbody ${bodyX70} ${bodyY70})
gone(g 70)
check("right-click on the body: the clip is deleted" g EQUAL 1 AND menu EQUAL 0)

# Right-drag from clip 72's strip (past the name) down across clips 71 and 70 deletes all three.
math(EXPR downY "${70_y1} - 6")
run(rdrag 40 "4 typing off
6 dump
8 move ${restX} ${hy72}
10 down 1
12 move ${restX} ${hy71}
14 move ${bodyX70} ${bodyY70}
16 move ${bodyX70} ${downY}
18 up 1
22 dump
")
dump(6)
dump(22)
foreach(id 70 71 72)
  set(g 1)
  if(clips_22 MATCHES "  clip ${id} kind")
    set(g 0)
  endif()
  check("right-drag from a title strip deletes clip ${id}" g EQUAL 1)
endforeach()
math(EXPR want "${undo_6} + 1")
check("... in one undo step (${undo_6} -> ${undo_22})" undo_22 EQUAL want)
