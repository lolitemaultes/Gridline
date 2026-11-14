# Gridline: GitHub issue 58, "Automation crashes" when creating automation clips. Right-click the
# first channel rack row's volume knob > "Create automation clip" (a playlist clip, song mode) used
# to close the app at once: the whole-song edit copied every container of the song and the rack
# went on drawing the row from the pattern and channel it held, now freed (Document.cpp, GL_SET).
# Runs the app hidden (isolated config and HOME, silent audio, fixed frame time), clicks the menu
# item, then undoes and redoes the edit twice, and checks the app keeps running, the editor opens
# on the new clip and the playlist holds it after each redo.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P issue58_rack_auto_clip_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

# The rack's first row volume knob (item 3682d582 in the fixture's layout at 1440x900); its menu
# opens at the pointer, "Create automation clip" 96 px right and 176 px down from the knob centre.
# (The knob report at frame 19 names the knob, so a moved knob fails the check below, not the click.)
file(WRITE "${OUT}/click.txt"
  "19 knobs\n20 atknob 3682d582\n21 down 1\n22 up 1\n25 atknob 3682d582 96 176\n26 down 0\n27 up 0\n"
  "32 autoedit\n33 dump\n40 undo\n42 dump\n45 redo\n47 dump\n50 undo\n52 redo\n55 dump\n60 quit discard\n")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 80
                        --dev-input "${OUT}/click.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/click.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app did not survive Create automation clip from the rack knob (${rc}): ${err}\n${out}")
endif()
if(NOT out MATCHES "knob item 3682d582 win \"Channel rack\" id \"vol\"")
  message(FATAL_ERROR "FAIL: the rack's first volume knob is not item 3682d582 any more (update the script)\n${out}")
endif()
message(STATUS "ok - the app keeps running after Create automation clip from a channel rack knob, undo and redo")
if(NOT out MATCHES "autoedit frame 32: open 1 automation [1-9][0-9]* pattern 0 ")
  message(FATAL_ERROR "FAIL: the automation editor is not open on a new playlist automation clip\n${out}")
endif()
message(STATUS "ok - the automation editor opens on the new playlist clip")
# dumpblock(frame var): the "dump" line of that frame and its clip lines.
function(dumpblock f var)
  string(FIND "${out}" "dump frame ${f}:" at)
  if(at LESS 0)
    message(FATAL_ERROR "FAIL: no dump at frame ${f}\n${out}")
  endif()
  string(SUBSTRING "${out}" ${at} -1 rest)
  string(REGEX MATCH "^[^\n]*\n(  clip [^\n]*\n)*" block "${rest}")
  set(${var} "${block}" PARENT_SCOPE)
endfunction()
foreach(f 33 47 55)
  dumpblock(${f} block)
  if(NOT block MATCHES "\n  clip [0-9]+ kind 2 ")
    message(FATAL_ERROR "FAIL: no automation placement in the playlist at frame ${f}\n${out}")
  endif()
endforeach()
dumpblock(42 block42)
if(block42 MATCHES "\n  clip [0-9]+ kind 2 ")
  message(FATAL_ERROR "FAIL: the undo left the automation placement in the playlist\n${out}")
endif()
message(STATUS "ok - the clip is in the playlist after each redo and gone after the undo")
