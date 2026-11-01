# Gridline: K-0, "Make unique" from a playlist clip's menu on a pattern with piano roll notes (a GitHub
# issue: the app closed). The edit replaces the song, and the menu used to keep reading the old
# pattern's notes in the same frame. Runs the app hidden (isolated config and HOME, fixed frame
# time) with the clip menu of the first clip ("Beat": notes and steps) pinned open, clicks
# "Make unique" and checks the app keeps running and the clip now plays a new pattern, one undo step.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k0_make_unique_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

# The pinned menu (ContextMenus drawDevMenu) opens at the work area's (60, 70); "Make unique" is its
# fifth row below the title.
file(WRITE "${OUT}/click.txt" "6 move 130 200\n8 down 0\n10 up 0\n14 dump\n16 quit discard\n")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --menu clip
                        --dev-input "${OUT}/click.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/click.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app did not survive Make unique (${rc}): ${err}\n${out}")
endif()
if(NOT out MATCHES "dump frame 14: [0-9]+ clips, [0-9]+ selected, undo 1")
  message(FATAL_ERROR "FAIL: no single undo step after the click (did the click reach Make unique?)\n${out}")
endif()
if(NOT out MATCHES "\n  clip 31 kind 0 track 0 start 0 [^\n]* pattern ([0-9]+) ")
  message(FATAL_ERROR "FAIL: clip 31 not in the dump\n${out}")
endif()
if(CMAKE_MATCH_1 STREQUAL "19")
  message(FATAL_ERROR "FAIL: clip 31 still plays pattern 19 (Beat), not a unique copy\n${out}")
endif()
message(STATUS "ok - Make unique on a pattern with notes: clip 31 plays new pattern ${CMAKE_MATCH_1}, app still running")
