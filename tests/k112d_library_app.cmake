# Gridline - K-112D: Smooth's Library. A project opened in Smooth shows the Library (the reference
# opens a new project with it); a click on a patch loads it onto the selected track in one undo step;
# B hides it, the save notes that, and the saved project opens again with it hidden.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112d_library_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"logic\", \"keys.profile\": \"logic\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
file(WRITE "${OUT}/input.txt" "5 toasts clear
20 parts lib state
22 parts lib nav 1
24 parts lib load 0
28 parts lib state
30 undo
34 parts lib state
36 key b
42 parts lib state
44 save ${OUT}/closed.gridline.json
48 open ${OUT}/closed.gridline.json
60 parts lib state
62 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout-dev --layout logic --size 1920x1080
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 70
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc})")
endif()
string(REGEX MATCHALL "smoothlib state: shown [01] saved \"[a-z]*\" nav [0-9]+ track -?[0-9]+ sound \"[^\"]*\"" states "${out}")
list(LENGTH states n)
if(NOT n EQUAL 5)
  message(FATAL_ERROR "FAIL: expected 5 state lines, got ${n}: ${states}")
endif()
list(GET states 0 s0)
list(GET states 1 s1)
list(GET states 2 s2)
list(GET states 3 s3)
list(GET states 4 s4)
if(NOT s0 MATCHES "shown 1 saved \"\"")
  message(FATAL_ERROR "FAIL: the Library is not open on a project opened in Smooth: ${s0}")
endif()
string(REGEX REPLACE ".*sound \"([^\"]*)\"" "\\1" before "${s0}")
string(REGEX REPLACE ".*sound \"([^\"]*)\"" "\\1" loaded "${s1}")
string(REGEX REPLACE ".*sound \"([^\"]*)\"" "\\1" undone "${s2}")
if(loaded STREQUAL before)
  message(FATAL_ERROR "FAIL: the click loaded nothing onto the track (${before})")
endif()
if(NOT undone STREQUAL before)
  message(FATAL_ERROR "FAIL: one undo did not bring the track's sound back (${before} / ${undone})")
endif()
if(NOT s3 MATCHES "shown 0 saved \"closed\"")
  message(FATAL_ERROR "FAIL: B did not hide the Library and note it in the project: ${s3}")
endif()
file(READ "${OUT}/closed.gridline.json" saved)
if(NOT saved MATCHES "\"smooth\"")
  message(FATAL_ERROR "FAIL: the save does not carry the Library state")
endif()
if(NOT s4 MATCHES "shown 0 saved \"closed\"")
  message(FATAL_ERROR "FAIL: the saved project did not open with the Library hidden: ${s4}")
endif()
message(STATUS "ok - Library open by default, a patch loads in one undo step, its state saved with the project")
