# Gridline: the typing keyboard goes by where keys sit, driven by real SDL key events in the real app
# (hidden, isolated config and HOME). "sdlkey ;@z" is the key in the Z position typing ';', as on
# US Dvorak; on its own layout it plays what the Z position plays everywhere.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P typing_key_positions_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(WRITE "${OUT}/input.txt" "4 typing on
6 typing show
8 sdlkey z down
10 typing show
12 sdlkey z up
14 typing show
16 sdlkey ;@z down
18 typing show
20 sdlkey ;@z up
22 typing show
24 sdlkey k@v down
26 typing show
28 sdlkey k@v up
30 typing show
32 sdlkey q@x down
34 typing show
36 sdlkey x@x up
38 typing show
40 sdlkey [@- down
42 sdlkey [@- up
44 typing show
46 sdlkey ]@= down
48 sdlkey ]@= up
50 typing show
52 quit discard
")
execute_process(COMMAND "${APP}" "${DEMO}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --edit 0 6 --hide rack
                        --dev-input "${OUT}/input.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 100)
file(WRITE "${OUT}/app.log" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}")
endif()
string(REGEX MATCHALL "typing notes[^\n]*" lines "${out}")
set(want
  "typing notes octave 4 velocity 100 chord 0"     # nothing held
  "typing notes 48 octave 4 velocity 100 chord 0"  # Z on US: C4
  "typing notes octave 4 velocity 100 chord 0"
  "typing notes 48 octave 4 velocity 100 chord 0"  # Dvorak's Z position, typing ';': C4, not D#5
  "typing notes octave 4 velocity 100 chord 0"
  "typing notes 53 octave 4 velocity 100 chord 0"  # V position, typing 'k': F4, not nothing
  "typing notes octave 4 velocity 100 chord 0"
  "typing notes 50 octave 4 velocity 100 chord 0"  # X position, typing 'q': D4, not C5
  "typing notes octave 4 velocity 100 chord 0"     # released by position, whatever it types at key-up
  "typing notes octave 4 velocity 90 chord 0"      # - position, typing '[': velocity, not octave
  "typing notes octave 4 velocity 100 chord 0")    # = position, typing ']': velocity, not octave
if(NOT "${lines}" STREQUAL "${want}")
  string(REPLACE ";" "\n" a "${lines}")
  string(REPLACE ";" "\n" b "${want}")
  message(FATAL_ERROR "FAIL: typed notes\n--- got\n${a}\n--- wanted\n${b}\n")
endif()
message(STATUS "ok - every key plays by its position")
