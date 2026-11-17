# Gridline - K-123 core B in the real app (hidden, isolated config and HOME, no sound device). ROW picks the row:
#   takes   : a take rated (one undo step, saved, the same in every layout), soloed (the engine plays that take,
#             the comp stays), auditioned (plays from the take's start; stopping ends it and goes back).
#   filter  : the MIDI input filter (pitch bend, pressure, program changes) is each person's: kept after a quit.
#   recover : notes played while the transport played (not recording) go back where they were heard; notes
#             played while stopped go in from the pattern's start; one undo step each.
#   arm     : an armed channel takes what is played and records it (another channel selected); Clips keeps its
#             own track arming, the other layouts arm channels.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -DROW=<row> -P k123_core_b_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

set(step 0)
function(run_app song script)
  math(EXPR n "${step} + 1")
  set(step ${n} PARENT_SCOPE)
  file(WRITE "${OUT}/run${n}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${song}" --hidden --layout-dev --no-ui-sounds --fixed-dt 16 --size 1440x900 --dev-input "${OUT}/run${n}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
  file(WRITE "${OUT}/run${n}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app failed in run ${n} (${rc}): ${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# The first line of `out` matching `re`, which must match `want` (a regex).
function(expect_match what re want)
  string(REGEX MATCH "${re}" got "${out}")
  if(NOT got MATCHES "${want}")
    message(FATAL_ERROR "FAIL: ${what}: \"${got}\" does not match \"${want}\"\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

if(ROW STREQUAL "takes")
  run_app("${DEMO}" "4 rectake make 3
6 rectake rate 3 1 2
8 rectake list
10 rectake solo 3 0
14 rectake played 3
16 rectake list
18 undo
20 rectake list
22 redo
24 rectake unsolo
26 rectake played 3
28 rectake audition 3 1
40 rectake list
42 recopt stop
50 rectake list
52 rectake played 3
54 save \"${OUT}/takes.gridline.json\"
60 quit discard
")
  expect_match("a rating is one undo step" "rectake frame 8: listen[^\n]*" "undo \"rate take\"")
  expect_match("take 2 rated two dots" "rectake frame 8: track 3[^\n]*" "take 1 rating 2 comp 0")
  expect_match("solo: the engine plays take 1 only, the song's comp is take 3" "rectake frame 14: played track 3:[^\n]*" "played track 3: 0 \\| song: 2$")
  expect_match("undo takes the rating back" "rectake frame 20: track 3[^\n]*" "take 1 rating 0 comp 0")
  expect_match("solo off: the comp plays" "rectake frame 26: played track 3:[^\n]*" "played track 3: 2 \\| song: 2$")
  expect_match("audition: take 2 plays" "rectake frame 40: listen[^\n]*" "listen track 3 take 1 audition 1 playing 1")
  expect_match("stopping ends the audition" "rectake frame 50: listen[^\n]*" "listen track -1 take -1 audition 0 playing 0")
  # Saved, and the same in every layout.
  run_app("${OUT}/takes.gridline.json" "5 rectake list
25 rectake list
45 rectake list
65 rectake list
80 quit discard
" --switch-layout logic@15 --switch-layout live@35 --switch-layout tracker@55)
  foreach(f 5 25 45 65)
    expect_match("the rating after a save and load, frame ${f} (Studio, Smooth, Clips, Tracker)" "rectake frame ${f}: track 3[^\n]*" "take 1 rating 2 comp 0")
  endforeach()
  file(READ "${OUT}/takes.gridline.json" saved)
  string(REGEX MATCHALL "\"takeRating\": 2" r "${saved}")
  list(LENGTH r nr)
  if(NOT nr EQUAL 1)
    message(FATAL_ERROR "FAIL: the saved song holds the rating once (one clip of the take): ${nr}")
  endif()
  message(STATUS "ok - the rating is saved on the take's clip")
elseif(ROW STREQUAL "filter")
  run_app("${DEMO}" "4 recfilter show
6 recfilter bend 0
7 recfilter program 0
8 recfilter test 0xE0 0
9 recfilter test 0xB0 1
10 recfilter show
16 quit discard
")
  expect_match("the filter's defaults" "recfilter frame 4:[^\n]*" "notes 1 controllers 1 bend 1 pressure 1 program 1")
  expect_match("bend off: pitch bend does not record" "recfilter frame 8:[^\n]*" "takes 0")
  expect_match("controllers still record" "recfilter frame 9:[^\n]*" "takes 1")
  run_app("${DEMO}" "4 recfilter show
8 quit discard
")
  expect_match("the filter is kept after a quit (this person's settings)" "recfilter frame 4:[^\n]*" "notes 1 controllers 1 bend 0 pressure 1 program 0")
  file(READ "${OUT}/cfg/gridline/settings.json" st)
  if(NOT st MATCHES "\"record.midi.bend\": false")
    message(FATAL_ERROR "FAIL: the filter is in this computer's settings\n${st}")
  endif()
  message(STATUS "ok - in this computer's settings")
elseif(ROW STREQUAL "recover")
  # Played while the pattern plays (record off): at 20 BPM a one-bar pass is 12 s, so both keys fall in it.
  run_app("${DEMO}" "4 playmode pattern
5 typing on
7 tempo 20
8 recopt list
9 recopt quantize 0
10 recopt arm 0
12 recopt play
30 sdlkey z down
50 sdlkey z up
60 sdlkey x down
80 sdlkey x up
90 recopt stop
92 recover show
94 recover
96 recopt list
98 undo
100 recopt list
120 sdlkey c down
130 sdlkey c up
140 recover
142 recopt list
150 quit discard
")
  expect_match("notes played while not recording are kept" "recover frame 92:[^\n]*" "kept 2 playing 0")
  expect_match("Recover puts them in, one undo step" "recover frame 94:[^\n]*" "placed 2 timed 1 [^\n]*undo \"recover played notes\"")
  string(REGEX MATCH "recnotes frame 8:[^\n]*" before "${out}")
  string(REGEX MATCH "recnotes frame 96:[^\n]*" after "${out}")
  string(REGEX MATCH "recnotes frame 100:[^\n]*" undone "${out}")
  string(REGEX MATCH " 48@([0-9]+)\\+[0-9]+" m "${after}")
  set(c "${CMAKE_MATCH_1}")
  string(REGEX MATCH " 50@([0-9]+)\\+[0-9]+" m "${after}")
  set(d "${CMAKE_MATCH_1}")
  if(c STREQUAL "" OR d STREQUAL "" OR NOT d GREATER c)
    message(FATAL_ERROR "FAIL: the C and the D where they were heard (C ${c}, D ${d}, after the pattern's start)\n${after}")
  endif()
  message(STATUS "ok - the C at ${c} and the D at ${d}: where they were heard")
  string(REGEX REPLACE " undo [^\n]*" "" b0 "${before}")
  string(REGEX REPLACE " undo [^\n]*" "" u0 "${undone}")
  string(REGEX REPLACE "recnotes frame [0-9]+:" "" b0 "${b0}")
  string(REGEX REPLACE "recnotes frame [0-9]+:" "" u0 "${u0}")
  if(NOT b0 STREQUAL u0)
    message(FATAL_ERROR "FAIL: one undo removes the recovered notes\n${before}\n${undone}")
  endif()
  message(STATUS "ok - one undo removes them")
  expect_match("played while stopped: from the pattern's start" "recnotes frame 142:[^\n]*" " 52@0\\+[0-9]+")
  expect_match("played while stopped: placed, not timed" "recover frame 140:[^\n]*" "placed 1 timed 0")
elseif(ROW STREQUAL "arm")
  run_app("${DEMO}" "4 playmode pattern
5 typing on
6 tempo 20
7 midiarm select 0
9 recopt quantize 0
8 midiarm show
10 midiarm 1 1
12 midiarm show
14 midiarm select 1
16 recopt list
18 midiarm select 0
20 recopt list
22 recopt arm 1
24 recopt play
40 sdlkey z down
60 sdlkey z up
70 recopt stop
74 midiarm select 1
76 recopt list
78 midiarm select 0
80 recopt list
82 midiarm 1 0
84 midiarm show
90 quit discard
")
  expect_match("nothing armed: notes go to the selected channel" "midiarm frame 8:[^\n]*" "applies 1 live 0 armed \\| lives 0$")
  expect_match("channel 2 armed: what is played goes to it" "midiarm frame 12:[^\n]*" "applies 1 live 1 armed 1 \\| lives 1$")
  foreach(pair "16;76;1" "20;80;0")
    list(GET pair 0 f0)
    list(GET pair 1 f1)
    list(GET pair 2 ch)
    string(REGEX MATCH "recnotes frame ${f0}:[^\n]*notes ([0-9]+):" m "${out}")
    set(n0 "${CMAKE_MATCH_1}")
    string(REGEX MATCH "recnotes frame ${f1}:[^\n]*notes ([0-9]+):" m "${out}")
    set(n1 "${CMAKE_MATCH_1}")
    if(ch EQUAL 1)
      math(EXPR want "${n0} + 1")
    else()
      set(want "${n0}")
    endif()
    if(NOT n1 EQUAL want)
      message(FATAL_ERROR "FAIL: channel ${ch}: ${n0} -> ${n1} notes, wanted ${want}\n${out}")
    endif()
  endforeach()
  message(STATUS "ok - the armed channel recorded the note, the selected one did not")
  expect_match("disarmed: back to the selected channel" "midiarm frame 84:[^\n]*" "live 0 armed \\| lives 0$")
  # Every layout: Smooth and the Tracker arm channels; Clips arms its own MIDI tracks.
  run_app("${DEMO}" "5 midiarm show
25 midiarm show
45 midiarm show
65 midiarm show
80 quit discard
" --switch-layout logic@15 --switch-layout live@35 --switch-layout tracker@55)
  expect_match("Studio arms channels" "midiarm frame 5:[^\n]*" "applies 1")
  expect_match("Smooth arms channels (its R buttons)" "midiarm frame 25:[^\n]*" "applies 1")
  expect_match("Clips keeps its own track arming" "midiarm frame 45:[^\n]*" "applies 0")
  expect_match("the Tracker arms channels" "midiarm frame 65:[^\n]*" "applies 1")
else()
  message(FATAL_ERROR "ROW must be takes, filter, recover or arm")
endif()
