# Gridline - FRZ-1118: a project on a network drive that stops answering never freezes the interface.
# The interface froze in 0.7.7-rc.3 with a project opened from an SMB share: every edit (a plugin's
# parameters dragged) builds the program on the interface thread, and the build read files next to
# the project (an SFZ instrument's file, statted and hashed on every build; samples Loupe, Plasma or
# the amp rack read directly), so a mount that stopped answering held the thread in the kernel.
# Here the "share" is a folder named in GRIDLINE_DEV_REMOTE_PATHS, and its SFZ instrument is a named
# pipe: opening it blocks forever, as a stalled mount does. The real app (hidden, dummy audio,
# isolated config and HOME) opens the project, takes thirty edits, reports its builds and quits;
# before the fix it never got past opening the project.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P frz1118_remote_app.cmake
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
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${share}/Take.wav" COPYONLY)
execute_process(COMMAND mkfifo "${share}/Stalled.sfz" RESULT_VARIABLE frc)
if(NOT frc EQUAL 0)
  message(FATAL_ERROR "mkfifo failed (${frc})")
endif()

file(READ "${DEMO}" demo)
set(chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 61, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Stalled\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.multisampler\", \"data\": \"${share}/Stalled.sfz\", \"params\": []}}, ")
string(APPEND chan "{\"color\": 5219296, \"gain\": 0.8, \"id\": 62, \"mixerTrack\": 0, \"muted\": false, \"name\": \"Take\", \"pan\": 0, \"pitch\": 0, \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${share}/Take.wav\", \"params\": []}}, ")
string(REPLACE "\"channels\": [" "\"channels\": [${chan}" demo "${demo}")
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${share}/song.gridline.json" "${demo}")

set(script "5 builds\n")
foreach(i RANGE 0 29)
  math(EXPR f "10 + ${i} * 2")
  math(EXPR bpm "120 + ${i}")
  string(APPEND script "${f} tempo ${bpm}\n")
endforeach()
string(APPEND script "80 builds\n82 tempo\n84 quit discard\n")
file(WRITE "${OUT}/script.txt" "${script}")
execute_process(COMMAND "${APP}" "${share}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1280x800
                        --dev-input "${OUT}/script.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 90)
file(WRITE "${OUT}/app.log" "${out}\n${err}")
file(REMOVE "${share}/Stalled.sfz")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: the app did not finish (${rc}): the interface waited for the stalled share?\n${out}\n${err}")
endif()
if(NOT out MATCHES "builds frame 80: builds ([0-9]+)")
  message(FATAL_ERROR "FAIL: no build report at frame 80\n${out}")
endif()
set(builds "${CMAKE_MATCH_1}")
if(builds LESS 10)
  message(FATAL_ERROR "FAIL: only ${builds} builds for thirty edits\n${out}")
endif()
if(NOT out MATCHES "tempo frame 82: 149\\.000")
  message(FATAL_ERROR "FAIL: the edits did not all land\n${out}")
endif()
message(STATUS "ok - ${builds} builds with the share's instrument stalled; the interface kept going")
