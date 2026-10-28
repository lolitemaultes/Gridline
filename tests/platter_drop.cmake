cmake_minimum_required(VERSION 3.16)
# Gridline — J-160: dragging audio onto Platter's decks, driven like a user through the dev input hooks.
# Runs the app hidden (off-screen, isolated config) with an empty two-deck Platter channel open:
#   1. "No controller" answers the first-open question; each deck reports where it takes drops
#      (its waveform lane and its panel: "platter: rect dropA0 / dropA1 / dropB0 / dropB1 ...");
#   2. a file dragged from the desktop onto deck B's lane loads on deck B, another onto deck A's
#      panel loads on deck A ("platter: tracks A=<file> B=<file>");
#   3. a file dropped onto deck B's panel replaces deck B's track, deck A keeps its own;
#   4. a playlist audio clip source dragged onto deck B loads its file there.
# Screenshots: <OUT>/hover.ppm (deck B lit while a file hovers over it), <OUT>/drops.ppm.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_platter> -DOUT=<dir> -P platter_drop.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
file(MAKE_DIRECTORY "${OUT}/home")
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/DropA.wav" 120 RESULT_VARIABLE rc1)
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/DropB.wav" 100 RESULT_VARIABLE rc2)
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/DropC.wav" 124 RESULT_VARIABLE rc3)
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/Clip.wav" 90 RESULT_VARIABLE rc4)
if(NOT rc1 EQUAL 0 OR NOT rc2 EQUAL 0 OR NOT rc3 EQUAL 0 OR NOT rc4 EQUAL 0)
  message(FATAL_ERROR "FAIL: could not write the test tracks")
endif()
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_PLATTER_TRACE} "1")
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

# centre of a traced rect where it last was: <name>_x / <name>_y
macro(locate out name)
  string(REGEX MATCHALL "platter: rect ${name} [0-9-]+ [0-9-]+ [0-9-]+ [0-9-]+" all_ "${out}")
  set(last_ "")
  if(all_)
    list(GET all_ -1 last_)
  endif()
  string(REGEX MATCH "platter: rect ${name} ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" _ "${last_}")
  check("the ${name} area is shown" CMAKE_MATCH_1 MATCHES "[0-9]")
  math(EXPR ${name}_x "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR ${name}_y "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
endmacro()

# the last "platter: tracks" line of an output
macro(tracks out var)
  string(REGEX MATCHALL "platter: tracks [^\n]*" all_ "${out}")
  set(${var} "")
  if(all_)
    list(GET all_ -1 ${var})
  endif()
endmacro()

# ---- 1. answer the question, find the decks' drop areas
run(setup 40 "# nothing\n")
set(last_out "${setup_out}")
string(REGEX MATCHALL "platter: rect none [0-9]+ [0-9]+ [0-9]+ [0-9]+" nones "${setup_out}")
list(GET nones -1 none_)
string(REGEX MATCH "none ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)" _ "${none_}")
math(EXPR none_x "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
math(EXPR none_y "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
set(answer "20 move ${none_x} ${none_y}
22 down 0
23 up 0
")
run(areas 50 "${answer}")
set(last_out "${areas_out}")
foreach(r dropA0 dropA1 dropB0 dropB1)
  locate("${areas_out}" ${r})
endforeach()
check("deck A's lane is above deck B's" dropA0_y LESS dropB0_y)
check("deck A's panel is left of deck B's" dropA1_x LESS dropB1_x)

# ---- 2. desktop files: one on deck B's lane, one on deck A's panel; 3. another on deck B's panel
math(EXPR bx2 "${dropB0_x} + 4")
math(EXPR ax2 "${dropA1_x} + 4")
math(EXPR cx2 "${dropB1_x} + 4")
run(drops 170 "${answer}40 move ${dropB0_x} ${dropB0_y}
42 dragenter ${dropB0_x} ${dropB0_y} \"${OUT}/DropB.wav\"
44 dragmove ${bx2} ${dropB0_y}
46 dragmove ${bx2} ${dropB0_y}
47 shot \"${OUT}/hover.ppm\"
48 drop ${bx2} ${dropB0_y}
70 move ${dropA1_x} ${dropA1_y}
72 dragenter ${dropA1_x} ${dropA1_y} \"${OUT}/DropA.wav\"
74 dragmove ${ax2} ${dropA1_y}
76 dragmove ${ax2} ${dropA1_y}
78 drop ${ax2} ${dropA1_y}
110 move ${dropB1_x} ${dropB1_y}
112 dragenter ${dropB1_x} ${dropB1_y} \"${OUT}/DropC.wav\"
114 dragmove ${cx2} ${dropB1_y}
116 dragmove ${cx2} ${dropB1_y}
118 drop ${cx2} ${dropB1_y}
150 move 10 10
")
set(last_out "${drops_out}")
check("a file dropped on deck B's lane loads on deck B" drops_out MATCHES "platter: tracks A= B=DropB\\.wav")
check("a file dropped on deck A's panel loads on deck A, deck B keeps its track" drops_out MATCHES "platter: tracks A=DropA\\.wav B=DropB\\.wav")
tracks("${drops_out}" final)
check("a file dropped on deck B's panel replaces deck B's track" final STREQUAL "platter: tracks A=DropA.wav B=DropC.wav")
check("the hover screenshot is written" EXISTS "${OUT}/hover.ppm")

# ---- 4. a playlist audio clip source onto deck B
run(clipsrc 120 "${answer}30 audio \"${OUT}/Clip.wav\" 0 0
40 move ${dropB1_x} ${dropB1_y}
42 dragenter ${dropB1_x} ${dropB1_y} clipsrc:last
44 dragmove ${cx2} ${dropB1_y}
46 dragmove ${cx2} ${dropB1_y}
48 drop ${cx2} ${dropB1_y}
")
set(last_out "${clipsrc_out}")
tracks("${clipsrc_out}" final)
check("a playlist clip source dropped on deck B loads its file on deck B" final STREQUAL "platter: tracks A= B=Clip.wav")
