# Gridline — J-151: Platter's two-deck setup, driven like a user through the dev input hooks.
# Runs the app hidden (off-screen, isolated config) with a Platter channel open and a track on
# each deck (the editor's GRIDLINE_PLATTER_DEMO / _DEMO_B hooks):
#   1. the first open asks about a controller (both decks, no "which side" question);
#   2. "No controller" answers it: the decks and the mixer show, nothing is mapped;
#   3. a click on each deck's play button starts that deck; both play at once;
#   4. dragging the crossfader to the B end moves the mixer's crossfader parameter.
# The trace lines come from the editor's GRIDLINE_PLATTER_TRACE hook ("platter: state ...",
# "platter: rect <control> x0 y0 x1 y1"). Screenshots: <OUT>/setup.ppm, <OUT>/decks.ppm, <OUT>/play.ppm.
# The engine runs on SDL's dummy audio driver (no sound device needed, nothing sounds).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_platter> -DOUT=<dir> -P platter_flow.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
file(MAKE_DIRECTORY "${OUT}/home")
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/Deck A 120.wav" 120 RESULT_VARIABLE rc1)
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/Deck B 100.wav" 100 RESULT_VARIABLE rc2)
if(NOT rc1 EQUAL 0 OR NOT rc2 EQUAL 0)
  message(FATAL_ERROR "FAIL: could not write the test tracks (${rc1} ${rc2})")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_PLATTER_TRACE} "1")
set(ENV{GRIDLINE_PLATTER_DEMO} "${OUT}/Deck A 120.wav")
set(ENV{GRIDLINE_PLATTER_DEMO_B} "${OUT}/Deck B 100.wav")
set(ENV{SDL_AUDIODRIVER} "dummy")

function(run tag frames script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --plugin gl.platter --size 1700x1150 --frames ${frames}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# centre of a traced control where it last was: <name>_x / <name>_y (and its ends <name>_x0 / <name>_x1)
macro(locate out name)
  string(REGEX MATCHALL "platter: rect ${name} [0-9-]+ [0-9-]+ [0-9-]+ [0-9-]+" all_ "${out}")
  set(last_ "")
  if(all_)
    list(GET all_ -1 last_)
  endif()
  string(REGEX MATCH "platter: rect ${name} ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" _ "${last_}")
  set(${name}_x0 "${CMAKE_MATCH_1}")
  set(${name}_x1 "${CMAKE_MATCH_3}")
  check("the ${name} control is shown" CMAKE_MATCH_1 MATCHES "[0-9]")
  math(EXPR ${name}_x "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR ${name}_y "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
endmacro()

# ---- 1. the first open: the controller question
run(setup 30 "# nothing\n")
set(last_out "${setup_out}")
check("the first open asks about a controller" setup_out MATCHES "platter: state setup layout 2 asked 0")
check("... with both tracks on their decks, analysed" setup_out MATCHES "A\\[bpm 1(19\\.9|20\\.0)[0-9][^\n]*B\\[bpm (99\\.9|100\\.0)[0-9]")
locate("${setup_out}" none)
locate("${setup_out}" use)

# ---- 2. "No controller": the decks and the mixer, nothing mapped
set(answer "8 move ${none_x} ${none_y}
10 down 0
11 up 0
12 waitfor platter-ready 300
")
run(decks 20 "${answer}")
set(last_out "${decks_out}")
check("\"No controller\" answers the question" decks_out MATCHES "platter: state ready layout 2 asked 1 mapped 0")
locate("${decks_out}" playA)
locate("${decks_out}" playB)
locate("${decks_out}" xfader)
check("... the decks sit either side of the mixer" playA_x LESS xfader_x AND playB_x GREATER xfader_x)

# ---- 3. play both decks; 4. crossfader to B
run(play 60 "${answer}14 move ${playA_x} ${playA_y}
16 down 0
17 up 0
19 move ${playB_x} ${playB_y}
21 down 0
22 up 0
24 waitfor platter-playing 120
30 move ${xfader_x} ${xfader_y}
32 down 0
34 move ${xfader_x1} ${xfader_y}
36 move ${xfader_x1} ${xfader_y}
38 up 0
50 move 10 10
")
set(last_out "${play_out}")
check("deck A's play button starts deck A" play_out MATCHES "A\\[bpm [0-9.]+ playing 1")
check("deck B's play button starts deck B, both play" play_out MATCHES "A\\[bpm [0-9.]+ playing 1[^\n]*B\\[bpm [0-9.]+ playing 1")
check("the crossfader drags to the B end" play_out MATCHES "playing 1\\] xf (0\\.9[5-9]|1\\.00)")
foreach(f setup decks play)
  check("screenshot ${f}.ppm written" EXISTS "${OUT}/${f}.ppm")
endforeach()
