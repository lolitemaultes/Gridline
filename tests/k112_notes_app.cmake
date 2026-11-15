# Gridline - K-112: the Notes panel keeps long notes whole. 20 KB of notes in several scripts
# (two-, three- and four-byte characters) are set, one character is typed at the start in the
# Notes panel, and the notes saved are that character followed by the old notes, byte for byte.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_notes_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
string(REPEAT "Grüße, Ünïcödé 漢字かな 🎹🎛 line\n" 500 notes)
file(WRITE "${OUT}/notes.txt" "${notes}")
file(SIZE "${OUT}/notes.txt" size)
if(size LESS 20000)
  message(FATAL_ERROR "FAIL: the notes are only ${size} bytes")
endif()
file(WRITE "${OUT}/input.txt" "5 toasts clear
6 parts notes set ${OUT}/notes.txt
8 layout open Notes
14 parts notes focus
18 text X
22 parts notes blur
28 parts notes dump ${OUT}/after.txt
")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --dev-input "${OUT}/input.txt"
                        --fixed-dt 16 --frames 40 --screenshot "${OUT}/notes.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc})")
endif()
if(NOT EXISTS "${OUT}/after.txt")
  message(FATAL_ERROR "FAIL: no notes dump")
endif()
file(READ "${OUT}/after.txt" after HEX)
file(READ "${OUT}/notes.txt" before HEX)
if(NOT after STREQUAL "58${before}")
  file(SIZE "${OUT}/after.txt" asize)
  message(FATAL_ERROR "FAIL: the notes changed beyond the typed character (${size} bytes before, ${asize} after)")
endif()
message(STATUS "ok - ${size} bytes of notes kept whole after typing one character")
