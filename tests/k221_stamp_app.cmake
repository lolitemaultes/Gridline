# Gridline - the piano roll's chord stamp button, in the real app (hidden, off-screen, isolated config):
#   1. the stamp button sits beside the menu button at the left of the piano roll's toolbar, at
#      1440x900 and at 1280x800 (where the old toolbar stamp did not fit), and the toolbar has no
#      stamp of its own any more;
#   2. a click on it opens the chord picker; a click on the Minor 7th chip turns the stamp on with
#      that chord (recent list: it first) and the picker closes;
#   3. a click on the note grid writes the chord (four notes, root, +3, +7, +10, one start) as one
#      undo step; undo takes it all away;
#   4. Esc over the piano roll turns the stamp off; a click on the lit stamp does too;
#   5. Alt+C opens the picker from the keyboard.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DSRC=<source dir> -DOUT=<dir> -P k221_stamp_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/song.gridline.json" "${demo}")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")  # never the real speakers
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GL_DEV_STAMP_CHIP} "11")  # Minor 7th

function(run tag size frames script)
  file(REMOVE_RECURSE "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${OUT}/${tag}/cfg" "${OUT}/${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  set(ENV{HOME} "${OUT}/${tag}/home")
  file(WRITE "${OUT}/${tag}/in.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size ${size} --hide rack
                          --edit 0 0 --show piano --frames ${frames} --dev-input "${OUT}/${tag}/in.txt" --screenshot "${OUT}/${tag}/shot.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}/out.txt" "${out}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${tag}: gridline-app failed (${rc}):\n${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# ---- 1. the toolbar has no stamp of its own; the button is shown at both sizes, beside the menu
file(READ "${SRC}/src/app/PianoRoll.cpp" pr)
if(pr MATCHES "\"##stamp\"" OR pr MATCHES "\"stampicon\"")
  message(FATAL_ERROR "FAIL: the piano roll toolbar still draws a stamp of its own")
endif()
foreach(size 1440x900 1280x800)
  run(at_${size} ${size} 40 "30 sdlat prstamp\n30 lane\n")
  if(NOT out MATCHES "sdlat frame 30: prstamp at ([0-9]+),([0-9]+)")
    message(FATAL_ERROR "FAIL: ${size}: the stamp button is not shown:\n${out}")
  endif()
  set(bx ${CMAKE_MATCH_1})
  if(NOT out MATCHES "lane frame 30: [^\n]* grid ([0-9.]+) ")
    message(FATAL_ERROR "FAIL: ${size}: no piano roll lane:\n${out}")
  endif()
  string(REGEX REPLACE "\\..*" "" gx "${CMAKE_MATCH_1}")
  # the grid starts 64 px of keyboard (and a slim scroller) right of the window: the button sits
  # within the first ~80 px of the toolbar, left of where the grid starts
  if(bx GREATER gx)
    message(FATAL_ERROR "FAIL: ${size}: the stamp button (x ${bx}) is not at the toolbar's left (grid at ${gx})")
  endif()
  message(STATUS "ok - ${size}: the stamp button is shown at the toolbar's left (x ${bx})")
endforeach()

# ---- geometry of the note grid (1440x900)
run(geo 1440x900 40 "30 lane\n")
string(REGEX MATCH "pitchY0 ([-0-9.]+) rowH ([0-9.]+) .* grid ([0-9.]+) ([0-9.]+) ([0-9.]+) ([0-9.]+)" _ "${out}")
set(py0 "${CMAKE_MATCH_1}")
set(rowh "${CMAKE_MATCH_2}")
set(gx0 "${CMAKE_MATCH_3}")
set(gy0 "${CMAKE_MATCH_4}")
set(gy1 "${CMAKE_MATCH_6}")
if(rowh STREQUAL "")
  message(FATAL_ERROR "FAIL: no grid geometry:\n${out}")
endif()
string(REGEX REPLACE "\\..*" "" gx0 "${gx0}")
string(REGEX REPLACE "\\..*" "" gy0 "${gy0}")
string(REGEX REPLACE "\\..*" "" gy1 "${gy1}")
math(EXPR cx "${gx0} + 30")
math(EXPR cy "(${gy0} + ${gy1}) / 2")

# ---- 2 + 3 + 4
run(stamp 1440x900 120 "10 mknotes 0 1 1
14 notes
15 browser state
20 stampstate
30 sdlat prstamp
32 sdldown 0
34 sdlup 0
40 sdlat prstampchip
42 sdldown 0
44 sdlup 0
50 stampstate
54 move ${cx} ${cy}
56 down 0
58 up 0
64 notes
65 browser state
66 undo
68 notes
70 stampstate
72 move ${cx} ${cy}
73 key escape
76 stampstate
80 sdlat prstamp
82 sdldown 0
84 sdlup 0
88 sdlat prstampchip
90 sdldown 0
92 sdlup 0
96 stampstate
100 sdlat prstamp
102 sdldown 0
104 sdlup 0
108 stampstate
")
if(NOT out MATCHES "stampstate frame 20: on 0 chord -1")
  message(FATAL_ERROR "FAIL: the stamp is on at first:\n${out}")
endif()
if(NOT out MATCHES "sdlat frame 40: prstampchip at")
  message(FATAL_ERROR "FAIL: the click on the stamp button did not open the chord picker:\n${out}")
endif()
if(NOT out MATCHES "stampstate frame 50: on 1 chord 11 inversion 0 auto 0 label \"m7\" recent \"11\"")
  message(FATAL_ERROR "FAIL: picking Minor 7th did not turn the stamp on with it:\n${out}")
endif()
message(STATUS "ok - the picker opened from the button and Minor 7th turned the stamp on")
string(REGEX MATCH "notes frame 14: ([0-9]+) notes" _ "${out}")
set(n0 ${CMAKE_MATCH_1})
string(REGEX MATCH "browserstate frame 15: [^\n]* undo ([0-9]+)" _ "${out}")
set(u0 ${CMAKE_MATCH_1})
string(REGEX MATCH "notes frame 64: ([0-9]+) notes[^\n]*\n((  note [^\n]*\n)*)" _ "${out}")
set(n1 ${CMAKE_MATCH_1})
set(lines "${CMAKE_MATCH_2}")
string(REGEX MATCH "browserstate frame 65: [^\n]* undo ([0-9]+)" _ "${out}")
set(u1 ${CMAKE_MATCH_1})
if(NOT n0 EQUAL 0 OR NOT n1 EQUAL 4)
  message(FATAL_ERROR "FAIL: one click wrote ${n1} notes (from ${n0}), not the four of a minor 7th:\n${out}")
endif()
math(EXPR du "${u1} - ${u0}")
if(NOT du EQUAL 1)
  message(FATAL_ERROR "FAIL: the chord took ${du} undo steps:\n${out}")
endif()
string(REGEX MATCHALL "start [0-9]+ len [0-9]+ pitch [0-9]+" ns "${lines}")
set(pitches "")
set(starts "")
foreach(x ${ns})
  string(REGEX MATCH "start ([0-9]+) len [0-9]+ pitch ([0-9]+)" _ "${x}")
  list(APPEND starts ${CMAKE_MATCH_1})
  list(APPEND pitches ${CMAKE_MATCH_2})
endforeach()
list(REMOVE_DUPLICATES starts)
list(LENGTH starts nstarts)
list(SORT pitches COMPARE NATURAL)
list(GET pitches 0 root)
set(iv "")
foreach(p ${pitches})
  math(EXPR d "${p} - ${root}")
  list(APPEND iv ${d})
endforeach()
if(NOT nstarts EQUAL 1 OR NOT iv STREQUAL "0;3;7;10")
  message(FATAL_ERROR "FAIL: the stamped notes are not one minor 7th chord (intervals ${iv}, ${nstarts} starts):\n${lines}")
endif()
message(STATUS "ok - one click stamped a minor 7th (root ${root}) as one undo step")
if(NOT out MATCHES "notes frame 68: 0 notes")
  message(FATAL_ERROR "FAIL: one undo did not take the whole chord away:\n${out}")
endif()
message(STATUS "ok - one undo removes the chord")
if(NOT out MATCHES "stampstate frame 76: on 0")
  message(FATAL_ERROR "FAIL: Esc over the piano roll did not turn the stamp off:\n${out}")
endif()
message(STATUS "ok - Esc turns the stamp off")
if(NOT out MATCHES "stampstate frame 96: on 1 chord 11" OR NOT out MATCHES "stampstate frame 108: on 0")
  message(FATAL_ERROR "FAIL: a click on the lit stamp did not turn it off:\n${out}")
endif()
message(STATUS "ok - a click on the lit stamp turns it off")

# ---- 5. Alt+C (the default key of "Chord stamp: pick a chord to stamp") opens the picker
run(altc 1440x900 70 "10 move ${cx} ${cy}
14 stampstate
20 sdlkey c down alt
22 sdlkey c up alt
30 sdlat prstampchip
32 sdldown 0
34 sdlup 0
44 stampstate
")
if(NOT out MATCHES "sdlat frame 30: prstampchip at")
  message(FATAL_ERROR "FAIL: Alt+C over the piano roll did not open the chord picker:\n${out}")
endif()
if(NOT out MATCHES "stampstate frame 44: on 1 chord 11")
  message(FATAL_ERROR "FAIL: a chord picked after Alt+C did not turn the stamp on:\n${out}")
endif()
message(STATUS "ok - Alt+C opens the chord picker")
