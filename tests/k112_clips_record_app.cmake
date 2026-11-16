# Gridline - K-112c (checkpoint 48): Session record and Capture in the Clips layout's clip grid, in the
# real app (a new set, hidden, isolated config, fixed frame time, the typing keyboard on):
#   1. the selected MIDI track is armed: a click on an empty slot's record button records a new clip
#      there while the transport runs in the clip launch mode (never the pattern mode, so the other
#      clips would play on); the notes typed go into it, and it grows by whole bars;
#   2. a click on the recording clip's button stops the recording: the clip keeps its notes and whole
#      bars and plays on;
#   3. Capture with nothing playing on the track: the notes just played become a new clip in the
#      selected slot, which then plays.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k112_clips_record_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

function(run tag input frames)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"live\", \"keys.profile\": \"live\", \"ui.menuStyle\": \"live\"}}\n")
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n8 typing on\n${input}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --layout live --layout-dev
                          --dev-input "${cfg}/input.txt" --frames ${frames} --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc})")
  endif()
  set(${tag}_log "${out}" PARENT_SCOPE)
endfunction()

# The first slot of the first column, from the grid's own report ("livegrid" prints each slot's centre).
run(probe "10 livegrid\n" 20)
# (A new set has no scenes stored yet, so no slot lines: the first title's x and the first row's y.)
string(REGEX MATCH "livegrid title 0 \"[^\"]*\" ([0-9]+) " m "${probe_log}")
if(NOT m)
  message(FATAL_ERROR "FAIL: the new set's grid printed no first title")
endif()
math(EXPR cx "${CMAKE_MATCH_1} + 1")
string(REGEX MATCH "livegrid row 0 y ([0-9]+)\\.[0-9]" m "${probe_log}")
if(NOT m)
  message(FATAL_ERROR "FAIL: the new set's grid printed no first row")
endif()
set(y0 "${CMAKE_MATCH_1}")
math(EXPR bx "${cx} - 36")   # the launch / record button cell at the slot's left (cells are 96 wide)
math(EXPR y2 "${y0} + 40")   # the third slot (rows 20 apart)

# 1 and 2. Select the first slot (its track is armed), record into the third slot, type, stop.
# N48-08 (checkpoint 49): Clips plays the A-row note keys (A = C, D = E, G = G; Z X C V set the octave and velocity).
run(rec "10 move ${cx} ${y0}\n11 down 0\n12 up 0\n20 move ${bx} ${y2}\n21 down 0\n22 up 0\n60 transport\n80 sdlkey a down\n90 sdlkey a up\n130 sdlkey d down\n140 sdlkey d up\n200 dump\n201 transport\n240 move ${bx} ${y2}\n241 down 0\n242 up 0\n270 dump\n271 notes\n272 playstate\n273 transport\n280 quit discard\n" 290)
string(REGEX MATCH "transport frame 60: pos [0-9]+ playing 1 seek [0-9]+ mode 2" m "${rec_log}")
if(NOT m)
  message(FATAL_ERROR "FAIL: the recording should run with the transport in the clip launch mode (mode 2)\n${rec_log}")
endif()
string(REGEX MATCH "dump frame 270: 1 clips[^\n]*\n  clip [0-9]+ kind 0 track 0 start ([0-9]+) len ([0-9]+)" m "${rec_log}")
if(NOT m)
  message(FATAL_ERROR "FAIL: one MIDI clip on the first track after the recording\n${rec_log}")
endif()
set(start "${CMAKE_MATCH_1}")
set(len "${CMAKE_MATCH_2}")
string(REGEX MATCH "dump frame 200: 1 clips[^\n]*\n  clip [0-9]+ kind 0 track 0 start [0-9]+ len ([0-9]+)" m "${rec_log}")
set(len200 "${CMAKE_MATCH_1}")
message(STATUS "recorded clip: start ${start}, length ${len} (while recording: ${len200})")
if(start EQUAL 0)
  message(FATAL_ERROR "FAIL: the clip went into the first slot, not the third one clicked")
endif()
math(EXPR rem "${len} % 384")
if(len LESS 384 OR NOT rem EQUAL 0)
  message(FATAL_ERROR "FAIL: a recorded clip keeps whole bars (${len} ticks)")
endif()
if(len200 LESS 768)
  message(FATAL_ERROR "FAIL: a new clip grows by bars while it records (${len200} ticks after 200 frames)")
endif()
string(REGEX MATCH "notes frame 271: ([0-9]+) notes" m "${rec_log}")
if(NOT m OR CMAKE_MATCH_1 LESS 2)
  message(FATAL_ERROR "FAIL: the two typed notes should be in the recorded clip\n${rec_log}")
endif()
string(REGEX MATCH "playstate frame 272: playing 1" m "${rec_log}")
if(NOT m)
  message(FATAL_ERROR "FAIL: the clip plays on after the recording stops\n${rec_log}")
endif()
message(STATUS "ok - recorded into the clicked slot, notes kept, whole bars, still playing")

# 3. Capture: a clip in the second slot first (the track's instrument: the typing keyboard plays it),
# then select the first slot, play two notes, Capture: a new clip there, which plays.
math(EXPR y1 "${y0} + 20")
run(cap "6 move ${cx} ${y1}\n7 down 0\n8 up 0\n9 down 0\n10 up 0\n20 move ${cx} ${y0}\n21 down 0\n22 up 0\n30 sdlkey a down\n36 sdlkey a up\n46 sdlkey g down\n52 sdlkey g up\n60 sdlat capture\n61 sdldown 0\n62 sdlup 0\n90 dump\n91 notes\n92 playstate\n93 transport\n100 quit discard\n" 110)
string(REGEX MATCH "dump frame 90: 2 clips[^\n]*\n(  clip [^\n]*\n)*  clip [0-9]+ kind 0 track 0 start 0 len ([0-9]+)" m "${cap_log}")
if(NOT m)
  message(FATAL_ERROR "FAIL: Capture should make one clip in the selected (first) slot\n${cap_log}")
endif()
string(REGEX MATCH "notes frame 91: ([0-9]+) notes" m "${cap_log}")  # (the selected new clip's notes)
if(NOT m OR NOT CMAKE_MATCH_1 EQUAL 2)
  message(FATAL_ERROR "FAIL: the captured clip holds the two notes played\n${cap_log}")
endif()
string(REGEX MATCH "playstate frame 92: playing 1" m "${cap_log}")
if(NOT m)
  message(FATAL_ERROR "FAIL: the captured clip plays\n${cap_log}")
endif()
message(STATUS "ok - Capture made a clip of the notes in the selected slot, and it plays")
