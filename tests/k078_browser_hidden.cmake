# Gridline: hidden browser folders in the real app (hidden, isolated config and HOME, a fixed 16 ms
# frame time). A folder tab on Loops (with Breaks and Pads inside).
#   1. Studio: a right-click on Breaks offers Hide folder; once hidden its row is gone; the eye
#      button shows hidden folders again (dimmed); search leaves out a hidden folder's files unless
#      hidden folders are shown. The list is saved per user (browser-hidden.txt).
#   2. A second start reads it back, and Show hidden (left on at quit) starts off again; Show folder
#      from the menu brings the folder back.
#   3. Clips, Smooth and Tracker leave the folder out too.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k078_browser_hidden.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/hm/Loops/Breaks" "${OUT}/hm/Loops/Pads")
file(WRITE "${OUT}/hm/Loops/Breaks/qqkick.wav" "a file in the hidden folder\n")
file(WRITE "${OUT}/hm/Loops/Pads/qqpad.wav" "a file in a shown folder\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/hm")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(dir "${OUT}/hm/Loops")
if(NOT DEFINED SIZE)
  set(SIZE 1440x900)
endif()

function(settings layout)
  file(WRITE "${OUT}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"${layout}\"}}\n")
endfunction()

function(run name script)
  file(WRITE "${OUT}/${name}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size ${SIZE} --dev-input "${OUT}/${name}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 240)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${o}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

function(expect re what)
  if(NOT out MATCHES "${re}")
    message(FATAL_ERROR "FAIL: ${what}\n(looked for: ${re})\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

# The rows drawn at a frame, as one block of text.
function(rows_at frame var)
  string(REGEX MATCH "browserrows frame ${frame}: [0-9]+\n(  row [^\n]*\n)*" block "${out}")
  set(${var} "${block}" PARENT_SCOPE)
endfunction()

function(has_row frame path want what)
  rows_at(${frame} r)
  string(FIND "${r}" "path ${path}\n" at)
  if(want AND at LESS 0)
    message(FATAL_ERROR "FAIL: ${what}\n(no row for ${path} at frame ${frame})\n${out}")
  elseif(NOT want AND NOT at LESS 0)
    message(FATAL_ERROR "FAIL: ${what}\n(a row for ${path} at frame ${frame})\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

# 1. Studio: where the Breaks row is, then a right-click on it.
settings(studio)
run(where "4 browser rows on
6 browser addtab ${dir}
12 browser rows
14 quit discard
")
has_row(12 "${dir}/Breaks" TRUE "the folder tab lists Breaks")
rows_at(12 r)
string(REGEX MATCH "x ([0-9.]+) ([0-9.]+) y ([0-9.]+) ([0-9.]+) path [^\n]*/Loops/Breaks\n" m "${r}")
set(rx "${CMAKE_MATCH_1}")
set(ry "${CMAKE_MATCH_3}")
string(REGEX REPLACE "\\..*" "" rx "${rx}")
string(REGEX REPLACE "\\..*" "" ry "${ry}")
math(EXPR bx "${rx} + 40")
math(EXPR by "${ry} + 6")

run(studio "4 browser rows on
6 browser hidden
7 browser showtab 5
10 sdlmove ${bx} ${by}
12 sdldown 1
13 sdlup 1
16 sdlat brhidefolder
18 sdldown 0
19 sdlup 0
24 browser rows
24 browser hidden
26 shot \"${OUT}/studio-hidden.ppm\"
28 sdlat brshowhidden
30 sdldown 0
31 sdlup 0
36 browser rows
36 browser hidden
38 shot \"${OUT}/studio-showhidden.ppm\"
40 browser search qq
46 browser rows
48 browser showhidden 0
54 browser rows
56 browser search
58 quit discard
")
expect("browserhidden frame 6: count 0 show 0" "nothing hidden at first")
expect("sdlat frame 16: brhidefolder at " "a folder's right-click menu offers Hide folder")
expect("browserhidden frame 24: count 1 show 0\n  hidden [^\n]*/hm/Loops/Breaks\n" "Hide folder hides it")
has_row(24 "${dir}/Breaks" FALSE "a hidden folder has no row")
has_row(24 "${dir}/Pads" TRUE "its neighbour stays")
expect("sdlat frame 28: brshowhidden at " "the header has the eye button while a folder is hidden")
expect("browserhidden frame 36: count 1 show 1" "the eye button shows hidden folders")
has_row(36 "${dir}/Breaks" TRUE "a hidden folder shows (dimmed) while hidden folders are shown")
has_row(46 "${dir}/Breaks/qqkick.wav" TRUE "search finds files in hidden folders while they are shown")
has_row(54 "${dir}/Breaks/qqkick.wav" FALSE "search leaves out a hidden folder's files")
has_row(54 "${dir}/Pads/qqpad.wav" TRUE "search still finds the other files")
file(READ "${OUT}/cfg/gridline/browser-hidden.txt" saved)
if(NOT saved MATCHES "^gridline-browser-hidden 1\n[^\n]*/hm/Loops/Breaks\n$")
  message(FATAL_ERROR "FAIL: the saved list\n${saved}")
endif()
message(STATUS "ok: the hidden folders are saved per user")

# Show hidden is for this session only (user 2026-11-21): left on at quit, the next start (below)
# still hides the folder.
run(eyeon "4 browser rows on
6 browser showhidden 1
8 browser hidden
10 quit discard
")
expect("browserhidden frame 8: count 1 show 1" "hidden folders shown at quit")

# 2. A second start reads it back; Show folder brings it back.
run(again "4 browser rows on
6 browser hidden
7 browser showtab 5
8 browser showhidden 1
12 sdlmove ${bx} ${by}
14 sdldown 1
15 sdlup 1
18 sdlat brshowfolder
20 sdldown 0
21 sdlup 0
26 browser hidden
26 browser rows
28 quit discard
")
expect("browserhidden frame 6: count 1 show 0" "a second start still hides the folder (Show hidden, left on at quit, is not remembered)")
expect("sdlat frame 18: brshowfolder at " "a hidden folder's menu offers Show folder")
expect("browserhidden frame 26: count 0 " "Show folder shows it again")
has_row(26 "${dir}/Breaks" TRUE "its row is back")

# 3. The other layouts: the folder hidden, the folder place shows the rest.
foreach(lay live logic tracker)
  settings(${lay})
  if(lay STREQUAL "tracker")
    set(show "8 browser showtab 5\n")
  else()
    set(show "8 sdlat brplace0\n10 sdldown 0\n11 sdlup 0\n")
  endif()
  run(${lay} "4 browser rows on
5 layout open Browser
6 browser hide ${dir}/Breaks
${show}16 browser rows
18 shot \"${OUT}/${lay}-hidden.ppm\"
20 browser showhidden 1
26 browser rows
28 shot \"${OUT}/${lay}-showhidden.ppm\"
30 browser unhide ${dir}/Breaks
32 quit discard
")
  has_row(16 "${dir}/Pads" TRUE "${lay}: the folder place lists the shown folder")
  has_row(16 "${dir}/Breaks" FALSE "${lay}: the hidden folder is left out")
  has_row(26 "${dir}/Breaks" TRUE "${lay}: shown again (dimmed) with hidden folders shown")
endforeach()
