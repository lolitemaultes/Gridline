# Gridline: Platter in a collaborative project: what the jog changes in the project reaches the
# other person, what the hand does live stays on this computer. Two hidden app windows (each with
# its own config, SDL's dummy audio driver: nothing shows, nothing sounds) over the app's real
# network path, once per window layout:
#   1. Alex has a Platter channel (a track on each deck, a controller profile with its jog wheels,
#      the mapping drawer open), makes the project collaborative and invites Sam;
#   2. Alex clicks the jog wheel's encoding button: the binding's encoding changes, and Sam's
#      project gets the same song;
#   3. Alex presses Undo: the encoding is back, on both computers;
#   4. Alex scratches the record and nudges it with the rim and the scroll wheel: a performance,
#      not an edit (the song is unchanged on both computers).
# The button is found first in a run of Alex's window on its own (same size and layout).
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_test_platter> -DOUT=<dir> -P platter_jog_collab_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/Deck A 120.wav" 120 RESULT_VARIABLE rc1)
execute_process(COMMAND "${TESTBIN}" writetrack "${OUT}/Deck B 100.wav" 100 RESULT_VARIABLE rc2)
if(NOT rc1 EQUAL 0 OR NOT rc2 EQUAL 0)
  message(FATAL_ERROR "FAIL: could not write the test tracks (${rc1} ${rc2})")
endif()
if(APPLE)
  set(M super)
else()
  set(M ctrl)
endif()
set(ALEXENV GRIDLINE_PLATTER_TRACE=1 "GRIDLINE_PLATTER_DEMO=${OUT}/Deck A 120.wav" "GRIDLINE_PLATTER_DEMO_B=${OUT}/Deck B 100.wav"
            GRIDLINE_PLATTER_PROFILE=ddj200 GRIDLINE_PLATTER_DRAWER=1jog)

function(songOf file out)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "FAIL: no report ${file}")
  endif()
  file(READ "${file}" j)
  string(JSON song GET "${j}" song)
  set(${out} "${song}" PARENT_SCOPE)
endfunction()

foreach(layout studio live logic tracker)
  set(D "${OUT}/${layout}")
  file(MAKE_DIRECTORY "${D}")
  # ---- where the jog wheel's encoding button is (Alex's window by itself, a fresh config as
  # Alex's is; the drawer keeps the jog wheel's row in view: GRIDLINE_PLATTER_DRAWER=1jog)
  file(REMOVE_RECURSE "${D}/cfgF" "${D}/homeF")
  file(WRITE "${D}/find.txt" "1 waitfor platter-steady 60\n30 waitfor never 1\n40 quit discard\n")
  execute_process(COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgF HOME=${D}/homeF ${ALEXENV}
                          "${APP}" --hidden --no-ui-sounds --no-update-check --layout ${layout} --plugin gl.platter --size 1700x1150
                          --dev-input "${D}/find.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE fout ERROR_VARIABLE ferr TIMEOUT 120)
  string(REGEX MATCHALL "platter: rect encjogA([0-9]) ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" encs "${fout}")
  if(NOT encs)
    message(FATAL_ERROR "FAIL (${layout}): the jog wheel's encoding button is not shown (${rc})\n${fout}\n${ferr}")
  endif()
  list(GET encs -1 enc)
  string(REGEX MATCH "encjogA([0-9]) ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" _ "${enc}")
  set(enc0 "${CMAKE_MATCH_1}")
  math(EXPR ex "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  math(EXPR ey "(${CMAKE_MATCH_3} + ${CMAKE_MATCH_5}) / 2")
  if(ey GREATER 1140)
    message(FATAL_ERROR "FAIL (${layout}): the encoding button is outside the window (y ${ey})")
  endif()
  string(REGEX MATCHALL "platter: rect platterA [0-9-]+ [0-9-]+ [0-9-]+ [0-9-]+" pls "${fout}")
  list(GET pls -1 pl)
  string(REGEX MATCH "platterA ([0-9-]+) ([0-9-]+) ([0-9-]+) ([0-9-]+)" _ "${pl}")
  math(EXPR px "(${CMAKE_MATCH_1} + ${CMAKE_MATCH_3}) / 2")
  math(EXPR py "(${CMAKE_MATCH_2} + ${CMAKE_MATCH_4}) / 2")
  math(EXPR pr "(${CMAKE_MATCH_3} - ${CMAKE_MATCH_1}) / 4")
  math(EXPR pxr "${px} + ${pr}")
  math(EXPR pyd "${py} + ${pr}")
  math(EXPR pr2 "(${CMAKE_MATCH_3} - ${CMAKE_MATCH_1}) / 2 - 5")
  math(EXPR pyrim "${py} - ${pr2}")
  math(EXPR pxrim "${px} + ${pr2} / 2")

  # ---- the two of them
  set(A "5 collab profile Alex
10 collab new \"${D}/Alex/Song\"
20 collab invite \"${D}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
32 waitfor never 2
33 collab report \"${D}/a1.json\"
34 waitfor platter-steady 60
80 move ${ex} ${ey}
81 down 0
82 up 0
83 waitfor collab-synced 60
84 waitfor never 3
85 collab report \"${D}/a2.json\"
86 waitfor \"file:${D}/b2.json\" 120
90 sdlkey z down ${M}
91 sdlkey z up ${M}
92 waitfor collab-synced 60
93 waitfor never 3
94 collab report \"${D}/a3.json\"
95 waitfor \"file:${D}/b3.json\" 120
100 move ${px} ${py}
101 move ${pxr} ${py}
102 down 0
104 move ${px} ${pyd}
106 move ${px} ${py}
108 up 0
110 move ${pxrim} ${pyrim}
111 down 0
113 move ${pxr} ${pyrim}
115 up 0
116 move ${px} ${py}
117 wheel 3
118 wheel 3
119 waitfor never 4
120 collab report \"${D}/a4.json\"
121 waitfor \"file:${D}/b4.json\" 120
130 quit discard
")
  set(B "5 collab profile Sam
10 collab join \"${D}/invite.txt\" \"${D}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
20 waitfor \"file:${D}/a2.json\" 180
21 waitfor collab-synced 60
22 waitfor never 3
23 collab report \"${D}/b2.json\"
30 waitfor \"file:${D}/a3.json\" 120
31 waitfor collab-synced 60
32 waitfor never 3
33 collab report \"${D}/b3.json\"
40 waitfor \"file:${D}/a4.json\" 120
41 waitfor never 2
42 collab report \"${D}/b4.json\"
90 quit discard
")
  file(WRITE "${D}/a.txt" "${A}")
  file(WRITE "${D}/b.txt" "${B}")
  # (Sam first: the commands run side by side with Sam's few lines piped into Alex, and Alex's
  # trace, which the checks read, goes to run.out)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgB HOME=${D}/homeB
            "${APP}" --hidden --no-ui-sounds --no-update-check --layout ${layout} --dev-input "${D}/b.txt"
    COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${D}/cfgA HOME=${D}/homeA ${ALEXENV}
            "${APP}" --hidden --no-ui-sounds --no-update-check --layout ${layout} --plugin gl.platter --size 1700x1150 --dev-input "${D}/a.txt"
    RESULTS_VARIABLE rcs OUTPUT_FILE "${D}/run.out" ERROR_FILE "${D}/run.err" TIMEOUT 420)
  message(STATUS "${layout}: exit codes ${rcs}")
  file(READ "${D}/run.out" aout)
  songOf("${D}/a1.json" a1)
  songOf("${D}/a2.json" a2)
  songOf("${D}/b2.json" b2)
  songOf("${D}/a3.json" a3)
  songOf("${D}/b3.json" b3)
  songOf("${D}/a4.json" a4)
  songOf("${D}/b4.json" b4)
  # the hand turned the record (the trace shows the deck's speed while it does)
  if(NOT aout MATCHES "speed [1-9]\\.[0-9]+\\]")
    message(FATAL_ERROR "FAIL (${layout}): Alex's scratch did not turn the record")
  endif()
  # the click went to the button: the encoding it shows moved on
  string(REGEX MATCHALL "platter: rect encjogA[0-9]" shown "${aout}")
  list(REMOVE_DUPLICATES shown)
  list(LENGTH shown nshown)
  if(nshown LESS 2)
    message(FATAL_ERROR "FAIL (${layout}): the encoding button never changed (${shown}, first ${enc0})")
  endif()
  if(a2 STREQUAL a1)
    message(FATAL_ERROR "FAIL (${layout}): the encoding change did not change the project")
  endif()
  if(NOT b2 STREQUAL a2)
    message(FATAL_ERROR "FAIL (${layout}): Sam's project does not have Alex's encoding change")
  endif()
  if(NOT a3 STREQUAL a1 OR NOT b3 STREQUAL a3)
    message(FATAL_ERROR "FAIL (${layout}): the undo did not bring the encoding back on both computers")
  endif()
  if(NOT a4 STREQUAL a3 OR NOT b4 STREQUAL a3)
    message(FATAL_ERROR "FAIL (${layout}): scratching and nudging changed the project")
  endif()
  if(NOT rcs STREQUAL "0;0")
    message(FATAL_ERROR "FAIL (${layout}): exit codes ${rcs}")
  endif()
  message(STATUS "ok - ${layout}: the encoding change and its undo reach Sam; scratching and nudging stay local")
endforeach()
