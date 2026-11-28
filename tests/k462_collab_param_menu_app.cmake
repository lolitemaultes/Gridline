# Gridline - 0.7.9: a reset from the Audio clip window's right-click parameter menu reaches the other person
# in a collaborative project, and so does its undo. Two hidden app windows (each with its own config) over
# the app's real network path, driven by dev-input "collab" commands, on a copy of the test song with a
# Sampler (Take, channel 60) playing an audio clip (clip 70):
#   1. Alex makes the project collaborative and invites Sam; Sam joins and has the same song;
#   2. Alex opens the clip's Audio clip window and switches Loop on (a click): Sam's song is Alex's;
#   3. Alex right-clicks Loop and picks Reset to default: Sam's song is Alex's again, not the one before;
#   4. Alex undoes the reset: Sam's song is Alex's, the one from step 2.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k462_collab_param_menu_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)
set(chanJson "{\"color\": 5219296, \"gain\": 0.8, \"id\": 60, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}, ")
set(clipJson "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 0, \"length\": 1536, \"pattern\": 0, \"gain\": 1.0}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chanJson}" demo "${demo}")
string(REPLACE "\"playlist\": [" "\"playlist\": [${clipJson}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")
foreach(who A B)
  file(MAKE_DIRECTORY "${OUT}/cfg${who}/gridline" "${OUT}/home${who}")
  file(WRITE "${OUT}/cfg${who}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"studio\", \"keys.profile\": \"studio\", \"ui.menuStyle\": \"studio\", \"ui.clipWindowView\": \"main\"}}\n")
endforeach()

set(A "5 collab profile Alex
10 collab new \"${OUT}/Alex/Song\"
20 collab invite \"${OUT}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
32 waitfor never 3
33 collab report \"${OUT}/a0.json\"
40 clipeditor dbl 70
60 atpid 60 p8
61 down 0
62 up 0
63 waitfor collab-synced 60
64 waitfor never 3
65 collab report \"${OUT}/a1.json\"
66 waitfor \"file:${OUT}/b1.json\" 150
70 atpid 60 p8
71 down 1
72 up 1
75 popuplabels loopmenu
76 atitem \"Reset to default*\"
78 down 0
79 up 0
80 waitfor collab-synced 60
81 waitfor never 3
82 collab report \"${OUT}/a2.json\"
83 waitfor \"file:${OUT}/b2.json\" 150
85 undo
86 waitfor collab-synced 60
87 waitfor never 3
88 collab report \"${OUT}/a3.json\"
89 waitfor \"file:${OUT}/b3.json\" 150
95 quit discard
")
set(B "5 collab profile Sam
6 waitfor never 3
10 collab join \"${OUT}/invite.txt\" \"${OUT}/Sam/Song\"
11 waitfor collab-joined 120
20 waitfor \"file:${OUT}/a1.json\" 300
21 waitfor never 3
22 collab report \"${OUT}/b1.json\"
30 waitfor \"file:${OUT}/a2.json\" 150
31 waitfor never 3
32 collab report \"${OUT}/b2.json\"
40 waitfor \"file:${OUT}/a3.json\" 150
41 waitfor never 3
42 collab report \"${OUT}/b3.json\"
50 quit discard
")
file(WRITE "${OUT}/a.txt" "${A}")
file(WRITE "${OUT}/b.txt" "${B}")
execute_process(
  # (Sam first: the output file is the last command's, Alex's)
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB HOME=${OUT}/homeB "${APP}" --hidden --no-ui-sounds --no-update-check --dev-input "${OUT}/b.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA HOME=${OUT}/homeA "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --dev-input "${OUT}/a.txt"
  RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/run.out" ERROR_FILE "${OUT}/run.err" TIMEOUT 600)
message(STATUS "exit codes: ${rcs}")

function(digest name out)
  if(NOT EXISTS "${OUT}/${name}.json")
    message(FATAL_ERROR "FAIL: no report ${name}.json (exit codes ${rcs})")
  endif()
  file(READ "${OUT}/${name}.json" j)
  string(JSON d GET "${j}" song)
  set(${out} "${d}" PARENT_SCOPE)
endfunction()
function(must what)
  if(${ARGN})
    message(STATUS "ok: ${what}")
  else()
    message(FATAL_ERROR "FAIL: ${what} (${ARGN})")
  endif()
endfunction()

file(READ "${OUT}/run.out" alexOut)
must("Alex's right-click on Loop opened its parameter menu" alexOut MATCHES "popuplabels frame [0-9]+ loopmenu:[^\n]*\"Reset to default")
must("the menu's Reset to default row was there to click" NOT alexOut MATCHES "atitem frame [0-9]+: \"Reset to default\\*\" not shown")
digest(a0 A0)
digest(a1 A1)
digest(b1 B1)
digest(a2 A2)
digest(b2 B2)
digest(a3 A3)
digest(b3 B3)
must("the click on Loop changed Alex's song" NOT A1 STREQUAL A0)
must("Sam has Alex's song after the click" B1 STREQUAL A1)
must("the reset from the right-click menu changed Alex's song" NOT A2 STREQUAL A1)
must("Sam has Alex's song after the reset" B2 STREQUAL A2)
must("undo brought Alex's song back to Loop on" A3 STREQUAL A1)
must("Sam has Alex's song after the undo" B3 STREQUAL A3)
if(NOT rcs STREQUAL "0;0")
  message(FATAL_ERROR "FAIL: exit codes ${rcs}")
endif()
