# Gridline — J-208: the playlist picker's "+" and plugins dropped on playlist tracks, in the real app
# (hidden, isolated configs, 1440x900, the playlist filling the window, a fixed 16 ms frame time, on a
# copy of the demo song):
#   1. the "+" after the last pattern in the picker makes a new pattern (named as the New pattern
#      command names it), selects it, one undo step; Undo takes it away;
#   2. thirty clicks: the list runs past the panel and the "+" stays on the panel's bottom edge;
#   3. at 150 % and 200 % interface scale the "+" is drawn and works;
#   4. an instrument dragged onto a track header lights the track up and, dropped, makes that track an
#      instrument track playing a new channel of it (the track keeps its pattern clips), one undo step;
#   5. an instrument dropped on the lane of an empty track does the same there;
#   6. another instrument dropped on an instrument track replaces its instrument (same channel);
#   7. an effect dropped on an instrument track goes onto its mixer track; on an ordinary track it is refused.
# Screenshots: <OUT>/plus.ppm, <OUT>/plus_many.ppm, <OUT>/plus_150.ppm, <OUT>/plus_200.ppm,
# (A dropped instrument opens its editor: the script closes it before the next drop.)
# <OUT>/drop_highlight.ppm, <OUT>/drop_result.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j208_playlist_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --show playlist
                          ${ARGN} --frames ${frames} --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
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

# The picker "+" line of frame <frame>: plus x, y, drop track, pattern count, current pattern, undo position and label.
function(plus frame)
  string(REGEX MATCH "plgeom frame ${frame}: [^\n]*\n  plus ([-0-9.]+) ([-0-9.]+) droptrack ([-0-9]+) patterns ([0-9]+) current ([0-9]+) undo ([0-9]+) \"([^\"]*)\"" _ "${out}")
  if(CMAKE_MATCH_1 STREQUAL "")
    fail("no plus line at frame ${frame}")
  endif()
  set(drop "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(npat "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(cur "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(undo "${CMAKE_MATCH_6}" PARENT_SCOPE)
  set(label "${CMAKE_MATCH_7}" PARENT_SCOPE)
  set(m1 "${CMAKE_MATCH_1}")
  set(m2 "${CMAKE_MATCH_2}")
  string(REGEX REPLACE "\\..*" "" px "${m1}")
  string(REGEX REPLACE "\\..*" "" py "${m2}")
  set(px "${px}" PARENT_SCOPE)
  set(py "${py}" PARENT_SCOPE)
endfunction()
# Track <t> of the plgeom block of frame <frame>: top, height, mode, channel, type, fx count, name.
function(track frame t)
  string(REGEX MATCH "plgeom frame ${frame}: [^\n]*\n(  [^\n]*\n)*  track ${t} top ([-0-9.]+) h ([0-9.]+) mode ([0-9]) channel ([0-9]+) type ([^ ]+) fx ([0-9]+) name \"([^\"]*)\"" _ "${out}")
  if(CMAKE_MATCH_2 STREQUAL "")
    fail("no track ${t} at frame ${frame}")
  endif()
  set(tmode "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(tchan "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(ttype "${CMAKE_MATCH_6}" PARENT_SCOPE)
  set(tfx "${CMAKE_MATCH_7}" PARENT_SCOPE)
  set(tname "${CMAKE_MATCH_8}" PARENT_SCOPE)
  set(m2 "${CMAKE_MATCH_2}")
  set(m3 "${CMAKE_MATCH_3}")
  string(REGEX REPLACE "\\..*" "" top "${m2}")
  string(REGEX REPLACE "\\..*" "" h "${m3}")
  set(ttop "${top}" PARENT_SCOPE)
  set(th "${h}" PARENT_SCOPE)
endfunction()

# ---- 1. the "+" after the last pattern
run(geo 12 "8 plgeom\n")
plus(8)
set(n0 "${npat}")
set(u0 "${undo}")
check("the picker draws the \"+\" (${px} ${py})" px GREATER 0 AND py GREATER 0)
string(REGEX MATCHALL "  row [0-9]+ x [0-9.]+ y ([0-9.]+)" rows "${out}")
list(GET rows -1 lastrow)
string(REGEX MATCH "y ([0-9]+)" _ "${lastrow}")
set(lasty "${CMAKE_MATCH_1}")
check("... under the last pattern row (${lasty} < ${py})" lasty LESS py)
string(REGEX MATCH "tabs ([0-9]+)[0-9.]* [0-9.]+ ([0-9]+)" _ "${out}")
math(EXPR mid "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_2}) / 2")
math(EXPR off "${px} - ${mid}")
check("... centred in the column (${px} vs ${mid})" off GREATER -3 AND off LESS 3)

run(plus 40 "8 plgeom
10 move ${px} ${py}
14 down 0
15 up 0
20 plgeom
24 undo
28 plgeom
")
plus(20)
math(EXPR want "${n0} + 1")
check("a click on \"+\" adds a pattern (${n0} -> ${npat})" npat EQUAL want)
math(EXPR uwant "${u0} + 1")
check("... in one undo step (${u0} -> ${undo}, \"${label}\")" undo EQUAL uwant)
check("... and selects it (current ${cur})" NOT cur EQUAL 0)
check("... and lists it in the picker" out MATCHES "plgeom frame 20: [^\n]*\n(  [^\n]*\n)*  row ${cur} x")
plus(28)
check("Undo takes it away (${npat})" npat EQUAL n0)

# ---- 2. many patterns (40 more in a second copy): the "+" stays on the panel's bottom edge
set(extra "")
foreach(i RANGE 1 40)
  math(EXPR pid "200 + ${i}")
  string(APPEND extra "{\"bars\": 1, \"color\": 7249739, \"id\": ${pid}, \"name\": \"Extra ${i}\"}, ")
endforeach()
string(REPLACE "\"patterns\": [" "\"patterns\": [${extra}" many "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 400" many "${many}")
file(WRITE "${OUT}/fixture.gridline.json" "${many}")
run(geo_many 12 "8 plgeom\n")
plus(8)
set(nm "${npat}")
check("a long list: the \"+\" still inside the window (y ${py})" py GREATER 100 AND py LESS 900)
string(REGEX MATCHALL "  row [0-9]+ x [0-9.]+ y ([0-9.]+)" rows "${out}")
list(GET rows -1 lastrow)
string(REGEX MATCH "y ([0-9]+)" _ "${lastrow}")
set(lr "${CMAKE_MATCH_1}")
check("... the rows run on under it (last row ${lr} > ${py})" lr GREATER py)
run(plus_many 30 "10 move ${px} ${py}\n14 down 0\n15 up 0\n20 plgeom\n")
plus(20)
math(EXPR want "${nm} + 1")
check("... and a click there adds a pattern (${nm} -> ${npat})" npat EQUAL want)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

# ---- 3. interface scale 150 % and 200 %
foreach(sc 150 200)
  if(sc EQUAL 150)
    set(scale 1.5)
  else()
    set(scale 2)
  endif()
  run(geo_${sc} 12 "8 plgeom\n" --ui-scale ${scale})
  plus(8)
  check("${sc} %: the \"+\" is drawn (${px} ${py})" px GREATER 0 AND py GREATER 0)
  run(plus_${sc} 30 "8 plgeom\n10 move ${px} ${py}\n14 down 0\n15 up 0\n20 plgeom\n" --ui-scale ${scale})
  plus(20)
  math(EXPR want "${n0} + 1")
  check("${sc} %: a click adds a pattern (${npat})" npat EQUAL want)
endforeach()

# ---- 4..7 plugins dropped on tracks
run(tracks 12 "8 plgeom\n")
string(REGEX MATCH "x0 ([0-9]+)" _ "${out}")
set(x0 "${CMAKE_MATCH_1}")
math(EXPR hx "${x0} - 50")   # over the track header
math(EXPR lx "${x0} + 260")  # over the lane
track(8 0)
math(EXPR y0 "${ttop} + ${th} / 2")
set(t0chan "${tchan}")
check("track 1 is an ordinary track to start with" tmode EQUAL 0)
track(8 5)
math(EXPR y5 "${ttop} + ${th} / 2")
track(8 6)
math(EXPR y6 "${ttop} + ${th} / 2")
plus(8)
set(u0 "${undo}")
run(drops 120 "8 plgeom
10 dragenter ${hx} ${y0} plugin:gl.synth
12 dragmove ${hx} ${y0}
14 dragmove ${hx} ${y0}
15 plgeom
16 shot \"${OUT}/drop_highlight.ppm\"
17 dragmove ${hx} ${y0}
18 drop
22 closeplugins
24 plgeom
25 dump
30 dragenter ${lx} ${y5} plugin:gl.kick
32 dragmove ${lx} ${y5}
34 dragmove ${lx} ${y5}
36 drop
40 closeplugins
42 plgeom
50 dragenter ${lx} ${y0} plugin:gl.kick
52 dragmove ${lx} ${y0}
54 dragmove ${lx} ${y0}
56 drop
60 closeplugins
62 plgeom
70 dragenter ${hx} ${y0} plugin:gl.delay
72 dragmove ${hx} ${y0}
74 dragmove ${hx} ${y0}
76 drop
82 plgeom
90 dragenter ${lx} ${y6} plugin:gl.delay
92 dragmove ${lx} ${y6}
94 dragmove ${lx} ${y6}
95 plgeom
96 drop
102 plgeom
104 move 700 120
110 shot \"${OUT}/drop_result.ppm\"
")
plus(15)
check("dragging an instrument over track 1's header lights that track (${drop})" drop EQUAL 0)
track(24 0)
check("dropped there: track 1 is an instrument track (mode ${tmode})" tmode EQUAL 2)
check("... playing a new channel of it (${ttype})" ttype STREQUAL "gl.synth")
set(c0 "${tchan}")
plus(24)
math(EXPR uwant "${u0} + 1")
check("... one undo step (${undo}, \"${label}\")" undo EQUAL uwant)
check("... the track keeps its pattern clips" out MATCHES "dump frame 25: [^\n]*\n(  clip [^\n]*\n)*  clip [0-9]+ kind 0 track 0 ")
track(42 5)
check("an instrument dropped on track 6's lane makes it an instrument track (${tmode} ${ttype})" tmode EQUAL 2 AND ttype STREQUAL "gl.kick")
check("... named after its instrument (${tname})" tname STREQUAL "Kick")
track(62 0)
check("another instrument on track 1 replaces its instrument (${ttype}, same channel ${tchan})" ttype STREQUAL "gl.kick" AND tchan EQUAL c0)
track(82 0)
check("an effect dropped on track 1 goes onto its mixer track (fx ${tfx})" tfx EQUAL 1)
plus(95)
check("an effect over an ordinary track is refused (no drop target: ${drop})" drop EQUAL -1)
plus(102)
math(EXPR uwant "${u0} + 4")
check("four drops, four undo steps (${undo})" undo EQUAL uwant)

foreach(f plus plus_many plus_150 plus_200 drops)
  check("screenshot ${f}.ppm written" EXISTS "${OUT}/${f}.ppm")
endforeach()
check("highlight screenshot written" EXISTS "${OUT}/drop_highlight.ppm")
