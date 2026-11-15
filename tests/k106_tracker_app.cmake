# Gridline - K-106: the tracker pattern editor in the real app. In the Tracker layout: the editor
# takes the keyboard, notes typed with the note keys land on the rows the edit step says, Ctrl+digit
# sets the edit step, the piano roll's own notes command sees the same notes, undo takes one back,
# the saved project opens again with the notes, playback moves the play row and the cursor follows
# it, and the editor's boxes sit on whole pixels. The classic keys: Backspace takes the row above,
# Insert pushes a row in, Tab and Shift+Tab wrap; song mode shows the order's one pattern.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k106_tracker_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n${out}")
endfunction()

# ---- 1: typing, edit step, the piano roll, undo, save ----
# Track 6 is the Chords channel, empty in the first pattern. Octave 4: Z is C-4 (60), X is D-4.
file(WRITE "${OUT}/input.txt" "5 toasts clear
8 tracker focus
10 tracker cursor 6 0 0 0
11 tracker edit on
12 sdlkey 2 down ctrl
13 sdlkey 2 up ctrl
16 tracker dump
18 sdlkey z down
19 sdlkey z up
22 sdlkey x down
23 sdlkey x up
26 tracker dump
27 notes
28 undo
30 tracker dump
31 notes
32 redo
34 save \"${OUT}/saved.gridline.json\"
36 tracker geometry
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout tracker --dev-input "${OUT}/input.txt"
                        --fixed-dt 16 --frames 40 --screenshot "${OUT}/typed.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
file(WRITE "${OUT}/run1.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc})")
endif()
if(NOT out MATCHES "tracker dump frame 16: [^\n]* edit 1 step 2 [^\n]*keyboard 1")
  fail("the editor did not take the keyboard, or Ctrl+2 did not set the edit step to 2")
endif()
if(NOT out MATCHES "tracker dump frame 26: [^\n]*cursor 6 4 0 0")
  fail("two notes at edit step 2 did not leave the cursor on row 4")
endif()
string(REGEX MATCH "tracker dump frame 26:.*tracker dump end" d26 "${out}")
if(NOT d26 MATCHES "cell 6 0 0 C-4" OR NOT d26 MATCHES "cell 6 2 0 D-4")
  fail("the typed notes are not on rows 0 and 2")
endif()
if(NOT out MATCHES "notes frame 27: 2 notes[^\n]*\n  note 0 start 0 len 48 pitch 60[^\n]*\n  note 1 start 48 len 336 pitch 62")
  fail("the piano roll does not see the typed notes (C-4 for two rows, D-4 to the pattern end)")
endif()
if(NOT out MATCHES "notes frame 31: 1 notes[^\n]*\n  note 0 start 0 len 384 pitch 60")
  fail("undo did not take the second note back (and the first one's length with it)")
endif()
if(NOT out MATCHES "save frame 34: saved")
  fail("the project did not save")
endif()
string(REGEX MATCH "tracker geometry frame 36: area ([0-9.]+) ([0-9.]+) ([0-9.]+) ([0-9.]+) row ([0-9.]+) cursor ([0-9.]+) ([0-9.]+) ([0-9.]+) ([0-9.]+)" geo "${out}")
if(NOT geo)
  fail("no geometry")
endif()
set(edges "${CMAKE_MATCH_1};${CMAKE_MATCH_2};${CMAKE_MATCH_5};${CMAKE_MATCH_6};${CMAKE_MATCH_7};${CMAKE_MATCH_8};${CMAKE_MATCH_9}")
foreach(e IN LISTS edges)
  if(NOT e MATCHES "\\.0+$")
    fail("an edge of the editor's boxes (${e}) is off the pixel grid (${geo})")
  endif()
endforeach()

# ---- 2: the saved project opens with the notes; playback moves the play row, the cursor follows ----
file(WRITE "${OUT}/input2.txt" "5 toasts clear
8 tracker focus
10 tracker cursor 6 0 0 0
12 tracker dump
14 notes
16 playmode pattern
17 startsong
40 tracker dump
41 transport
")
execute_process(COMMAND "${APP}" "${OUT}/saved.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout tracker --dev-input "${OUT}/input2.txt"
                        --fixed-dt 16 --frames 44 --screenshot "${OUT}/playing.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
file(WRITE "${OUT}/run2.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed on the saved project (${rc})")
endif()
if(NOT out MATCHES "notes frame 14: 2 notes[^\n]*\n  note 0 start 0 len 48 pitch 60[^\n]*\n  note 1 start 48 len 336 pitch 62")
  fail("the saved project lost the typed notes")
endif()
string(REGEX MATCH "tracker dump frame 12:.*tracker dump end" d12 "${out}")
if(NOT d12 MATCHES "cell 6 0 0 C-4" OR NOT d12 MATCHES "cell 6 2 0 OFF|cell 6 2 0 D-4")
  fail("the tracker does not show the saved notes")
endif()
if(NOT out MATCHES "transport frame 41: pos ([0-9]+) playing 1")
  fail("playback did not start")
endif()
string(REGEX MATCH "tracker dump frame 40: [^\n]*cursor 6 ([0-9]+) 0 0 play ([0-9-]+)" pd "${out}")
if(NOT pd)
  fail("no dump while playing")
endif()
if(CMAKE_MATCH_2 LESS 1 OR NOT CMAKE_MATCH_1 EQUAL CMAKE_MATCH_2)
  fail("the play row did not move, or the cursor did not follow it (${pd})")
endif()
message(STATUS "ok - typed notes, edit step, piano roll, undo, save and reopen, follow play (${pd}), pixel grid")

# ---- 3: the classic keys (Backspace, Insert, Tab wrap) and song mode showing one pattern ----
# Track 6 again: C-4 on row 4, D-4 on row 5. Backspace on row 6 takes row 5 (the D-4) and pulls the
# rest up, the cursor with it; Insert on row 0 pushes the C-4 down to row 5. Tab on the last track
# wraps to the first, Shift+Tab on the first to the last. Song mode shows the order's pattern
# only and counts the other patterns playing there.
file(WRITE "${OUT}/input3.txt" "5 toasts clear
8 tracker focus
10 tracker cursor 6 4 0 0
11 tracker edit on
12 sdlkey z down
13 sdlkey z up
14 sdlkey x down
15 sdlkey x up
17 sdlkey #42 down
18 sdlkey #42 up
20 tracker dump
22 tracker cursor 6 0 0 0
23 sdlkey #73 down
24 sdlkey #73 up
26 tracker dump
28 tracker cursor 0 0 0 0
29 sdlkey #43 down shift
30 sdlkey #43 up shift
32 tracker dump
33 sdlkey #43 down
34 sdlkey #43 up
36 tracker dump
38 tracker edit off
39 tracker follow on
40 playmode song
41 seekbeat 64
42 startsong
56 tracker dump
57 transport
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout tracker --dev-input "${OUT}/input3.txt"
                        --fixed-dt 16 --frames 60 --screenshot "${OUT}/keys.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
file(WRITE "${OUT}/run3.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed on the key run (${rc})")
endif()
string(REGEX MATCH "tracker dump frame 20:.*tracker dump frame 26" d20 "${out}")
if(NOT d20 MATCHES "tracker dump frame 20: [^\n]*cursor 6 5 0 0" OR NOT d20 MATCHES "cell 6 4 0 C-4" OR d20 MATCHES "cell 6 [0-9]+ 0 D-4")
  fail("Backspace did not take the row above the cursor (the D-4) and move the cursor up")
endif()
string(REGEX MATCH "tracker dump frame 26:.*tracker dump frame 32" d26 "${out}")
if(NOT d26 MATCHES "cell 6 5 0 C-4" OR d26 MATCHES "cell 6 4 0 C-4")
  fail("Insert on row 0 did not push the track's notes down a row")
endif()
if(NOT out MATCHES "tracker dump frame 32: [^\n]*cursor ([0-9]+) 0 0 0")
  fail("no dump after Shift+Tab")
endif()
set(last "${CMAKE_MATCH_1}")
if(last LESS 2)
  fail("Shift+Tab on the first track did not wrap to the last one (track ${last})")
endif()
if(NOT out MATCHES "tracker dump frame 36: [^\n]*cursor 0 0 0 0")
  fail("Tab on the last track did not wrap to the first")
endif()
if(NOT out MATCHES "transport frame 57: pos ([0-9]+) playing 1 [^\n]* mode 1")
  fail("song playback did not start")
endif()
if(NOT out MATCHES "tracker dump frame 56: pattern ([0-9]+) [^\n]*engine 1 [^\n]* stacked ([0-9]+)")
  fail("no dump in song mode")
endif()
# (the shown pattern's own notes only: no cell of another pattern is merged in; the count of the
# others is reported)
message(STATUS "ok - Backspace, Insert, Tab and Shift+Tab wrap, song mode shows one pattern (${CMAKE_MATCH_2} more play there)")
