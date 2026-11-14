# Gridline: GitHub issue 58 follow-up. The channel rack draws a pattern's automation clips as rows
# under the channels; a row's menu item "Move to the playlist" is a whole-song edit that replaces the
# song's patterns, and the rows went on reading the pattern they held (freed memory: an AddressSanitizer
# build stopped there). Runs the app hidden (isolated config and HOME, silent audio, fixed frame time):
# a mixer knob's menu creates an automation clip in the pattern, the automation editor is closed, the
# clip's rack row menu moves it to the playlist, then undo and redo twice. Checks the app keeps running
# and the playlist holds automation placements after each redo and none after the undo.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P issue58_lane_to_playlist_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

# At 1440x900: the first mixer insert's pan knob (item 46eb0dbe); its menu's "Create automation clip
# in pattern" sits 33 px right and 197 px down from the knob centre. The automation editor that opens
# closes with its x (1263, 152); the new clip's rack row name is at (450, 573) and its menu's "Move
# to the playlist" at (538, 696).
file(WRITE "${OUT}/click.txt"
  "18 knobs\n19 atknob 46eb0dbe\n20 down 1\n21 up 1\n24 atknob 46eb0dbe 33 197\n25 down 0\n26 up 0\n"
  "29 move 1263 152\n30 down 0\n31 up 0\n34 move 450 573\n35 down 1\n36 up 1\n39 move 538 696\n40 down 0\n41 up 0\n"
  "44 dump\n45 undo\n46 dump\n47 redo\n48 dump\n49 undo\n51 redo\n52 dump\n55 quit discard\n")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900 --frames 80
                        --dev-input "${OUT}/click.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/click.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app did not survive Move to the playlist from a rack automation row (${rc}): ${err}\n${out}")
endif()
if(NOT out MATCHES "knob item 46eb0dbe win \"Mixer\" id \"pan\"")
  message(FATAL_ERROR "FAIL: the first mixer insert's pan knob is not item 46eb0dbe any more (update the script)\n${out}")
endif()
message(STATUS "ok - the app keeps running after Move to the playlist from a rack automation row, undo and redo")
function(dumpblock f var)
  string(FIND "${out}" "dump frame ${f}:" at)
  if(at LESS 0)
    message(FATAL_ERROR "FAIL: no dump at frame ${f}\n${out}")
  endif()
  string(SUBSTRING "${out}" ${at} -1 rest)
  string(REGEX MATCH "^[^\n]*\n(  clip [^\n]*\n)*" block "${rest}")
  set(${var} "${block}" PARENT_SCOPE)
endfunction()
foreach(f 44 48 52)
  dumpblock(${f} block)
  if(NOT block MATCHES "\n  clip [0-9]+ kind 2 ")
    message(FATAL_ERROR "FAIL: no automation placement in the playlist at frame ${f}\n${out}")
  endif()
endforeach()
dumpblock(46 block46)
if(block46 MATCHES "\n  clip [0-9]+ kind 2 ")
  message(FATAL_ERROR "FAIL: the undo left automation placements in the playlist\n${out}")
endif()
message(STATUS "ok - the lane is a playlist clip after each redo and back in the pattern after the undo")
