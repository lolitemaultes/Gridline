# Gridline - K-112c: the Live control bar's Capture, Session record and Draw mode buttons, clicked
# in the real app (hidden, isolated config and HOME):
#   1. Capture: notes played on the typing keyboard while nothing records are kept; the button
#      writes them into the current pattern as one undo step and forgets them;
#   2. Session record: arms recording in pattern mode and starts playback; a second click disarms;
#   3. Draw mode: lit with the pencil; a click switches the editors to select, another back;
#   4. the device title bars draw the hot-swap and save buttons (the device band shown first: since
#      round 45 the clip band takes its place while a clip is selected).
# Screenshot: <OUT>/bar.ppm.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_livebar_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"live\", \"keys.profile\": \"live\", \"ui.menuStyle\": \"live\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
file(WRITE "${OUT}/in.txt" "8 typing on
10 livebar
12 sdlkey z down
16 sdlkey z up
20 sdlkey x down
24 sdlkey x up
28 sdlkey c down
32 sdlkey c up
36 typing off
38 livebar
40 sdlat capture
42 sdldown 0
43 sdlup 0
46 livebar
50 sdlat drawmode
52 sdldown 0
53 sdlup 0
56 livebar
58 sdldown 0
59 sdlup 0
62 livebar
66 sdlat sessrec
68 sdldown 0
69 sdlup 0
74 livebar
76 sdldown 0
77 sdlup 0
82 livebar
83 layout show live.device
86 sdlat devswap
87 sdlat devsave
88 shot \"${OUT}/bar.ppm\"
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 96
                        --screenshot "${OUT}/end.ppm" --dev-input "${OUT}/in.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
file(WRITE "${OUT}/run.out" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# One frame's "livebar" line as <var>_kept, _notes, _undo, _armed, _pat, _playing, _rec, _draw.
function(at var frame)
  string(REGEX MATCH "livebar frame ${frame}: kept ([0-9]+) notes ([0-9]+) undo \"([^\"]*)\" armed ([01]) pattern-mode ([01]) playing ([01]) recording ([01]) draw ([01])" m "${out}")
  if(m STREQUAL "")
    message(FATAL_ERROR "FAIL: no livebar line at frame ${frame}\nOutput:\n${out}")
  endif()
  set(${var}_kept "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${var}_notes "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${var}_undo "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(${var}_armed "${CMAKE_MATCH_4}" PARENT_SCOPE)
  set(${var}_pat "${CMAKE_MATCH_5}" PARENT_SCOPE)
  set(${var}_playing "${CMAKE_MATCH_6}" PARENT_SCOPE)
  set(${var}_rec "${CMAKE_MATCH_7}" PARENT_SCOPE)
  set(${var}_draw "${CMAKE_MATCH_8}" PARENT_SCOPE)
endfunction()

at(b0 10)
at(b1 38)
at(b2 46)
check("nothing kept at first (${b0_kept})" b0_kept EQUAL 0)
check("three notes played: kept (${b1_kept})" b1_kept EQUAL 3)
check("... not written while only played (${b0_notes} -> ${b1_notes})" b1_notes EQUAL b0_notes)
math(EXPR want "${b1_notes} + 3")
check("capture: the three notes in the pattern (${b2_notes})" b2_notes EQUAL want)
set(label "capture")
check("... one undo step (${b2_undo})" b2_undo STREQUAL label)
check("... and forgotten (${b2_kept})" b2_kept EQUAL 0)

at(d0 46)
at(d1 56)
at(d2 62)
check("draw mode lit with the pencil (${d0_draw})" d0_draw EQUAL 1)
check("a click: select (${d1_draw})" d1_draw EQUAL 0)
check("another: the pencil again (${d2_draw})" d2_draw EQUAL 1)

at(s1 74)
at(s2 82)
check("session record: armed in pattern mode and playing (${s1_armed} ${s1_pat} ${s1_playing})" s1_armed EQUAL 1 AND s1_pat EQUAL 1 AND s1_playing EQUAL 1)
check("... recording (${s1_rec})" s1_rec EQUAL 1)
check("a second click: disarmed (${s2_armed})" s2_armed EQUAL 0)

# K-112c: the device title bar's hot-swap and save buttons are drawn
check("device title bar: hot-swap button" out MATCHES "sdlat frame 86: devswap at")
check("device title bar: save button" out MATCHES "sdlat frame 87: devsave at")
