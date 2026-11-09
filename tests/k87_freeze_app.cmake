# Gridline: K-87, freeze and hibernate in the running app. Runs the app hidden (isolated config and
# HOME, SDL's dummy audio driver) on a copy of the test song with a scripted session:
#   1. the Chords mixer track (three effects) and the Bass channel freeze: the renders are written
#      into Frozen/ next to the project, the frozen plugins and the Chords channel (it plays only
#      into the frozen track) are unloaded;
#   2. a note played by hand on the frozen Bass channel loads it live for the moment;
#   3. the Clap track hibernates: its notes are near the song position, so it is awake (loaded);
#   4. unfreezing the Chords track loads its effects and the Chords instrument again;
#   5. the saved project keeps the freeze of Bass and the hibernation, and opens with them.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k87_freeze_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

# Mixer 7 = Chords (chorus, delay, reverb), channel 5 = Bass, channel 6 = Chords, mixer 2 = Clap.
file(WRITE "${OUT}/input.txt" "5 freeze state
6 freeze freeze m 7
7 freeze freeze c 5
8 freeze wait
40 freeze state
41 noteon 5 48
50 freeze state
51 noteoff 5 48
52 freeze hibernate m 2 1
70 freeze state
71 freeze unfreeze m 7
90 freeze state
91 save \"${OUT}/saved.gridline.json\"
100 freeze state
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 110
                        --dev-input "${OUT}/input.txt" --screenshot "${OUT}/end.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
file(WRITE "${OUT}/run.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc}): ${err}")
endif()

# The state blocks, in order.
string(REPLACE "FREEZE STATE" ";FREEZE STATE" parts "${out}")
list(LENGTH parts n)
if(n LESS 7)
  fail("expected 6 state dumps, got ${n} parts\n${out}")
  message(FATAL_ERROR "${failures} check(s) failed")
endif()
list(GET parts 2 frozen)
list(GET parts 3 played)
list(GET parts 4 asleep)
list(GET parts 5 unfrozen)

function(expect_line block pattern what)
  string(REGEX MATCH "${pattern}" m "${block}")
  if(m STREQUAL "")
    message(SEND_ERROR "FAIL: ${what}\n${block}")
    set(failures_local 1 PARENT_SCOPE)
  else()
    message(STATUS "ok - ${what}")
  endif()
endfunction()

set(failures_local 0)
expect_line("${frozen}" "m Chords frozen 1 stale 0 live 0 hibernate 0 awake 0 rendering 0 loaded 0" "the Chords track is frozen and its effects unloaded")
expect_line("${frozen}" "c Chords frozen 0 [^\n]* loaded 0" "the Chords channel plays only into the frozen track: unloaded")
expect_line("${frozen}" "c Bass frozen 1 stale 0 live 0 [^\n]* loaded 0" "the Bass channel is frozen and its instrument unloaded")
expect_line("${frozen}" "c Kick frozen 0 [^\n]* loaded 1" "other channels stay loaded")
expect_line("${played}" "c Bass frozen 1 stale 0 live 1 [^\n]* loaded 1" "a note played by hand loads the frozen channel live")
expect_line("${asleep}" "m Clap frozen 0 stale 0 live 0 hibernate 1 awake 1 rendering 0 loaded 1" "the hibernating Clap track woke: its notes are near")
expect_line("${unfrozen}" "m Chords frozen 0 [^\n]* loaded 1" "unfreezing loads the effects again")
expect_line("${unfrozen}" "c Chords frozen 0 [^\n]* loaded 1" "and the Chords instrument")
if(failures_local)
  math(EXPR failures "${failures} + 1")
endif()

file(GLOB renders "${OUT}/Frozen/*.wav")
list(LENGTH renders nr)
if(nr LESS 2)
  fail("the renders are in Frozen/ next to the project (${nr} found)")
else()
  message(STATUS "ok - ${nr} renders in Frozen/")
endif()

file(READ "${OUT}/saved.gridline.json" saved)
string(FIND "${saved}" "\"freeze\"" hasFreeze)
string(FIND "${saved}" "\"hibernate\"" hasHib)
if(hasFreeze LESS 0 OR hasHib LESS 0)
  fail("the saved project keeps the freeze and the hibernation")
else()
  message(STATUS "ok - saved with the freeze and the hibernation")
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} check(s) failed")
endif()
