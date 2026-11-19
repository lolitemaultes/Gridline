# Gridline - PL-1118: in the Smooth (logic) and Clips (live) layouts' arrangement a drag of one of
# several selected clips moves them all, as Studio's playlist does, driven by scripted input in the
# real app (hidden, isolated config and HOME, fixed frame time) on a copy of the test song. Two pattern
# clips on two tracks (91: track 1, 92: track 2, a beat later), clear of the song's other clips:
#   1. 91 clicked, 92 Shift+clicked (Clips: on its body, which selects time and the clip, and Cmd+clicked on its
#      title bar), then 91 dragged a bar later and a track up: both move a bar and a track, in one
#      undo step, and stay selected once the button is up (frames later too);
#   2. mid-drag (the button still down) both clips are drawn moved by the same distance (the preview
#      follows the pointer for the whole selection, not the clip pressed alone).
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -DLAYOUT=logic|live -P pl1118_multi_drag_app.cmake
cmake_minimum_required(VERSION 3.16)
# The Cmd key: "super" on macOS (the dev input's "ctrl" is the physical Ctrl key there, a right-click),
# Ctrl elsewhere (Super is the system's key on Linux and Windows).
if(CMAKE_HOST_APPLE)
  set(cmd super)
else()
  set(cmd ctrl)
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
file(READ "${DEMO}" demo)  # a copy: the fixture is never opened (or saved over) in place
# The two clips after the song's last clips (bar 17 on): in Studio's units, track 1 at 6144 and
# track 2 at 6528 (a beat later), each two beats long, with free room a bar on and a track down.
set(clips "{\"id\": 91, \"length\": 192, \"pattern\": 29, \"start\": 6144, \"track\": 1}, {\"id\": 92, \"length\": 192, \"pattern\": 30, \"start\": 6240, \"track\": 2}, ")
if(LAYOUT STREQUAL "live")  # a live area of two scenes (the session grid's slots): bars 1 to 8; the Arrangement view
  string(REPLACE "\"playlist\": [" "\"live\": {\"areaEnd\": 3072, \"view\": \"arrangement\"}, \"playlist\": [${clips}" demo "${demo}")
  # grid2-1118: read as a version 10 file: an older one moves a grid slot's further clips into scenes of their
  # own as it loads, which moves the arrangement (and these two clips) down by the scenes inserted.
  string(REGEX REPLACE "\"version\": *[0-9]+" "\"version\": 10" demo "${demo}")
else()
  string(REPLACE "\"playlist\": [" "\"playlist\": [${clips}" demo "${demo}")
endif()
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 200" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

function(fail msg)
  message(FATAL_ERROR "FAIL (${LAYOUT}): ${msg}\n${out}")
endfunction()

# Smooth: zoomed out so bar 17 is in view (its track list is wider).
set(zoomArgs "")
if(LAYOUT STREQUAL "logic")
  set(zoomArgs --zoom 0.5)
endif()

function(run tag script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layoutPreset\": \"${LAYOUT}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900
                          --layout ${LAYOUT} --show playlist ${zoomArgs} --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${o}\n${err}")
  set(out "${o}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${LAYOUT}) ${tag}: gridline-app failed (${rc}): ${err}")
  endif()
endfunction()

# rect <prefix> <frame> <id>: the clip's drawn box in the plgeom of that frame: <p>_x0 <p>_y0 (whole px).
function(rect p frame id)
  string(FIND "${out}" "plgeom frame ${frame}:" at)
  if(at LESS 0)
    fail("no plgeom at frame ${frame}")
  endif()
  string(SUBSTRING "${out}" ${at} -1 rest)
  if(NOT rest MATCHES "\n  clip ${id} x ([-0-9]+)[.0-9]* ([-0-9]+)[.0-9]* y ([-0-9]+)[.0-9]* ([-0-9]+)")
    fail("clip ${id} is not drawn at frame ${frame}")
  endif()
  set(${p}_x0 ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${p}_x1 ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(${p}_y0 ${CMAKE_MATCH_3} PARENT_SCOPE)
  set(${p}_y1 ${CMAKE_MATCH_4} PARENT_SCOPE)
endfunction()

# clipAt <frame> <id>: c_track, c_start, c_sel, nsel (selected clips), undo, from that frame's dump.
function(clipAt frame id)
  if(NOT out MATCHES "dump frame ${frame}: ([0-9]+) clips, ([0-9]+) selected, undo ([0-9]+)\n")
    fail("no dump at frame ${frame}")
  endif()
  set(nsel ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(undo ${CMAKE_MATCH_3} PARENT_SCOPE)
  string(FIND "${out}" "dump frame ${frame}:" at)
  string(SUBSTRING "${out}" ${at} -1 rest)
  string(REGEX MATCH "\n  clip ${id} kind [0-9]+ track ([0-9]+) start ([0-9]+) len [0-9]+ [^\n]* sel ([01])" _ "${rest}")
  set(c_track "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(c_start "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(c_sel "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

function(want what got expect)
  if(NOT "${got}" STREQUAL "${expect}")
    fail("${what}: want ${expect}, got ${got}")
  endif()
  message(STATUS "ok - ${LAYOUT}: ${what}")
endfunction()

run(geo "4 typing off\n6 pltool select\n12 plgeom\n16 quit discard\n")
if(NOT out MATCHES "plgeom frame [0-9]+: valid 1 x0 [-0-9.]+ pxPerTick ([0-9]+)\\.([0-9][0-9][0-9])")
  fail("no playlist geometry")
endif()
set(whole ${CMAKE_MATCH_1})
string(REGEX REPLACE "^0+(.)" "\\1" f "${CMAKE_MATCH_2}")
math(EXPR ppt "${whole} * 1000 + ${f}")
math(EXPR bar "384 * ${ppt} / 1000")
rect(a 12 91)
rect(b 12 92)
foreach(k 0 1 2)
  if(NOT out MATCHES "\n  track ${k} top ([-0-9]+)")
    fail("no geometry for track ${k}")
  endif()
  set(top${k} ${CMAKE_MATCH_1})
endforeach()
math(EXPR dTrack "${top0} - ${top1}")  # a track up
# Where each clip is pressed: its title bar (Clips: a clip moves by its title bar; the body selects
# time), else its body; the middle, clear of the edges.
math(EXPR ax "(${a_x0} + ${a_x1}) / 2")
math(EXPR bx "(${b_x0} + ${b_x1}) / 2")
math(EXPR ay "${a_y0} + 5")
math(EXPR by "${b_y0} + 5")
math(EXPR byBody "${b_y1} - 4")
if(NOT LAYOUT STREQUAL "live")
  math(EXPR ay "${a_y1} - 4")
  math(EXPR by "${b_y1} - 4")
endif()

# multi <tag> <the press on 92: y> <modifier>: 91 clicked, 92 clicked there with the modifier held, 91
# dragged a bar on and a track up (plgeom mid-drag at frame 71, the button up at 76), dumps before (50),
# after (80) and frames later (100).
function(multi tag y92 mod)
  math(EXPR xe "${ax} + ${bar}")
  math(EXPR ye "${ay} + ${dTrack}")
  if(mod STREQUAL "box")  # a box dragged over both from the empty lane right of 92
    math(EXPR sx "${b_x1} + 24")
    math(EXPR sy "${b_y1} - 3")
    math(EXPR ex "${a_x0} + 4")
    math(EXPR ey "${a_y0} + 4")
    set(sc "4 typing off\n6 pltool select\n30 move ${sx} ${sy}\n32 down 0\n34 move ${ex} ${sy}\n36 move ${ex} ${ey}\n38 move ${ex} ${ey}\n40 up 0\n50 dump\n")
  else()
    set(sc "4 typing off\n6 pltool select\n30 move ${ax} ${ay}\n32 down 0\n34 up 0\n36 mod ${mod} 1\n38 move ${bx} ${y92}\n40 down 0\n42 up 0\n44 mod ${mod} 0\n50 dump\n")
  endif()
  string(APPEND sc "60 move ${ax} ${ay}\n62 down 0\n")
  foreach(i 1 2 3 4)
    math(EXPR fi "62 + 2 * ${i}")
    math(EXPR xi "${ax} + ${bar} * ${i} / 4")
    math(EXPR yi "${ay} + ${dTrack} * ${i} / 4")
    string(APPEND sc "${fi} move ${xi} ${yi}\n")
  endforeach()
  string(APPEND sc "71 plgeom\n76 up 0\n80 dump\n100 dump\n104 quit discard\n")
  run(${tag} "${sc}")
  clipAt(50 91)
  want("${tag}: before the drag, two clips selected" "${nsel}" 2)
  set(u0 ${undo})
  clipAt(80 91)
  math(EXPR at1 "6144 + 384")
  want("${tag}: the clip dragged moves a bar later and a track up" "${c_track}/${c_start}" "0/${at1}")
  set(du ${undo})
  clipAt(80 92)
  math(EXPR at2 "6240 + 384")
  want("${tag}: the other selected clip moves with it" "${c_track}/${c_start}" "1/${at2}")
  math(EXPR du "${du} - ${u0}")
  want("${tag}: one undo step" "${du}" 1)
  clipAt(100 92)
  want("${tag}: both stay selected after the drag" "${nsel}/${c_sel}" "2/1")
  # Mid-drag: both drawn moved by the same distance.
  rect(ma 71 91)
  rect(mb 71 92)
  math(EXPR dax "${ma_x0} - ${a_x0}")
  math(EXPR dbx "${mb_x0} - ${b_x0}")
  math(EXPR day "${ma_y0} - ${a_y0}")
  math(EXPR dby "${mb_y0} - ${b_y0}")
  math(EXPR ddx "${dax} - ${dbx}")
  math(EXPR ddy "${day} - ${dby}")
  if(ddx GREATER 1 OR ddx LESS -1 OR ddy GREATER 1 OR ddy LESS -1)
    fail("${tag}: mid-drag the clips are drawn moved apart: 91 by ${dax},${day}, 92 by ${dbx},${dby}")
  endif()
  if(dax LESS 4)
    fail("${tag}: mid-drag the clip pressed is not drawn moved (${dax} px)")
  endif()
  message(STATUS "ok - ${LAYOUT}: ${tag}: mid-drag both drawn moved together (${dax},${day} / ${dbx},${dby})")
endfunction()

if(LAYOUT STREQUAL "live")
  # Clips: Shift+drag on a title bar copies; Cmd+click adds a clip to the selection, and so does a
  # Shift+click on a body (N47-08: it dropped the selection once the button was up).
  multi(title ${by} ${cmd})  # (the Cmd key)
  multi(body ${byBody} shift)
else()
  multi(title ${by} shift)
  multi(box ${by} box)  # Smooth: the pointer on an empty lane drags a selection box
endif()
message(STATUS "pl1118_multi_drag_app (${LAYOUT}): all checks passed")
