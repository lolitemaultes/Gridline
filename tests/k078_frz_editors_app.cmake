# Gridline - 0.7.8: the editors of instruments whose files sit on a network drive that stops answering
# never hold a frame. FRZ-1118 kept the program builds off such a drive; the editors still read it
# on the interface thread: the Multisampler's SFZ view loaded its instrument there, and the image
# synth's editor read its picture there (and again every frame after a failure).
# Here the "share" is a folder named in GRIDLINE_DEV_REMOTE_PATHS and both files are named pipes:
# opening one blocks until someone writes, as a stalled mount does. The real app (hidden, dummy
# audio, isolated config and HOME) opens each editor for sixty frames and reports its longest frame.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k078_frz_editors_app.cmake
cmake_minimum_required(VERSION 3.16)
if(NOT UNIX)
  message(STATUS "skipped: named pipes are a POSIX test device")
  return()
endif()
file(REMOVE_RECURSE "${OUT}")
set(share "${OUT}/share")
file(MAKE_DIRECTORY "${share}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_DEV_REMOTE_PATHS} "${share}")
foreach(f Stalled.sfz Stalled.bmp)
  execute_process(COMMAND mkfifo "${share}/${f}" RESULT_VARIABLE frc)
  if(NOT frc EQUAL 0)
    message(FATAL_ERROR "mkfifo failed (${frc})")
  endif()
endforeach()

file(READ "${DEMO}" demo)
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 61, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Stalled SFZ\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.multisampler\", \"data\": \"${share}/Stalled.sfz\", \"params\": []}}, ")
string(APPEND chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 62, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Stalled picture\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.imagesynth\", \"data\": \"${share}/Stalled.bmp\", \"params\": [1]}}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${share}/song.gridline.json" "${demo}")
file(WRITE "${OUT}/script.txt" "20 scan state\n80 scan state\n82 quit discard\n")

foreach(c 0 1)
  execute_process(COMMAND "${APP}" "${share}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1280x800 --chan ${c}
                          --dev-input "${OUT}/script.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 90)
  file(WRITE "${OUT}/app${c}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    file(REMOVE "${share}/Stalled.sfz" "${share}/Stalled.bmp")
    message(FATAL_ERROR "FAIL: the app did not finish with editor ${c} open (${rc}): the interface waited for the stalled share?\n${out}\n${err}")
  endif()
  string(REGEX MATCHALL "longest-frame [0-9.]+ ms" lines "${out}")
  list(LENGTH lines n)
  if(n LESS 2)
    message(FATAL_ERROR "FAIL: no frame report for editor ${c}\n${out}")
  endif()
  list(GET lines 1 last)
  string(REGEX REPLACE "longest-frame ([0-9.]+) ms" "\\1" ms "${last}")
  if(ms GREATER_EQUAL 100)
    message(FATAL_ERROR "FAIL: editor ${c}: a frame took ${ms} ms with the share stalled\n${out}")
  endif()
  message(STATUS "ok - editor ${c} open on a stalled share, longest frame ${ms} ms")
endforeach()
file(REMOVE "${share}/Stalled.sfz" "${share}/Stalled.bmp")
