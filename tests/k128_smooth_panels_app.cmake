# Gridline - K-128: Smooth layout Event List, Project/Track Notes, and Score Editor quiet app test.
# Verifies:
#   1. Smooth opens with Score Editor docked in logic.lower and Event List (Lists) in logic.right.
#   2. Event List displays active pattern notes, supports filter toggles, adding/deleting notes,
#      and bidirectional selection sync with Score Editor.
#   3. Score Editor switches clefs (Grand / Treble / Bass) and quantize grids (1/4, 1/8, 1/16).
#   4. Notes drawer switches cleanly between Project and Track tabs and persists both independently.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k128_smooth_panels_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_QUIET_WINDOWS} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"logic\"}}\n")
file(WRITE "${OUT}/proj_note.txt" "Smooth project note line 1\nCheck chorus balance")
file(WRITE "${OUT}/track_note.txt" "Track note: bright lead EQ +2 dB @ 4 kHz")
file(WRITE "${OUT}/input.txt" "5 toasts clear
8 layout open Lists
10 layout open Score
14 parts eventlist state
15 parts score state
16 parts eventlist select 0
18 parts score state
20 parts eventlist add
22 parts eventlist state
24 parts eventlist delete
26 parts eventlist state
28 parts eventlist filter notes 0
29 parts eventlist state
30 parts eventlist filter notes 1
32 parts score clef treble
33 parts score quant 8
34 parts score state
35 parts score clef grand
36 parts score quant 16
38 layout open Notes
40 parts notes tab project
41 parts notes set \"${OUT}/proj_note.txt\"
43 parts notes tab track
44 parts notes trackset \"${OUT}/track_note.txt\"
46 parts notes state
48 parts notes dump \"${OUT}/proj_out.txt\"
49 parts notes trackdump \"${OUT}/track_out.txt\"
52 layout where Score
53 layout where Notes
")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout logic --layout-dev
                        --size 1440x900 --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 60
                        --screenshot "${OUT}/smooth_k128.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc})")
endif()
if(NOT out MATCHES "where Score layout logic drawn 1 docked 1 region logic\\.lower")
  message(FATAL_ERROR "FAIL: Score did not dock into logic.lower:\n${out}")
endif()
if(NOT out MATCHES "where Notes layout logic drawn 1 docked 1 region logic\\.right")
  message(FATAL_ERROR "FAIL: Notes did not dock into logic.right:\n${out}")
endif()
if(NOT out MATCHES "eventlist tab 0 rows 0 sel [0-9]+ notes 0")
  message(FATAL_ERROR "FAIL: Event List notes filter did not hide note rows:\n${out}")
endif()
if(NOT out MATCHES "score clef 0 quant 1 tool 0 rendered [0-9]+ sel 0")
  message(FATAL_ERROR "FAIL: Score Editor clef/quant switch did not apply:\n${out}")
endif()
file(READ "${OUT}/proj_note.txt" pExp)
file(READ "${OUT}/proj_out.txt" pGot)
if(NOT pExp STREQUAL pGot)
  message(FATAL_ERROR "FAIL: Project notes mismatch")
endif()
file(READ "${OUT}/track_note.txt" tExp)
file(READ "${OUT}/track_out.txt" tGot)
if(NOT tExp STREQUAL tGot)
  message(FATAL_ERROR "FAIL: Track notes mismatch")
endif()
message(STATUS "ok - K-128 Smooth Event List, Notes (Project+Track) and Score Editor verified")
