# Gridline - the Clips layout's device band: a device's title arrow folds it in place (0.7.8 fix).
# Runs the app hidden (isolated config and HOME, dummy audio, fixed frame time) in the Clips layout
# on the test song with the Abacus channel (Perc) selected and its device band shown, and clicks with
# the SDL pointer:
#   1. the arrow next to the name folds the card to a strip, and opens no window;
#   2. the strip's arrow unfolds it again (still no window);
#   3. the window button at the right of the title opens the device's editor in its window: the
#      channel settings body is drawn once, by its window, not by the band;
#   4. the window button again closes it;
#   5. on a chain with effects (Chords: an instrument and three inserts) the first card's arrow folds
#      only that card;
#   6. an effect opened from elsewhere (as the mixer's slot does) opens its editor in its floating
#      window (the effect editor body drawn once, by its window, never by the band: nothing is drawn
#      above the band), and the arrow of another card does not touch it;
#   7. an instrument opened from elsewhere (Channel settings, as the rack does) opens its window too.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P k379_clips_fold_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"live\", \"keys.profile\": \"live\", \"ui.menuStyle\": \"live\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

function(run tag script frames)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900
                          --frames ${frames} --dev-input "${OUT}/${tag}.txt" --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}.out" "${o}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL ${tag}: gridline-app ended with ${rc}\n${o}\n${err}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()
function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()
# The card line of device `name` in the liveband report of frame `f`: <var>_folded, _window.
function(card var f name)
  string(REGEX MATCH "liveband frame ${f}: devices [0-9]+\n(  liveband [^\n]*\n)*" block "${out}")
  if(block STREQUAL "")
    message(FATAL_ERROR "FAIL: no liveband report at frame ${f}\n${out}")
  endif()
  string(REGEX MATCH "liveband \"${name}\" [a-z]+ folded ([01]) window ([01])" m "${block}")
  if(m STREQUAL "")
    message(FATAL_ERROR "FAIL: no card \"${name}\" at frame ${f}\n${block}")
  endif()
  set(${var}_folded "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${var}_window "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# 1-4: Abacus (channel 7, Perc).
run(abacus "6 typing select 7
8 layout show live.device
20 liveband
22 sdlat devfold
24 sdldown 0
25 sdlup 0
34 liveband
36 sdlat devfold
38 sdldown 0
39 sdlup 0
48 liveband
50 sdlat devwindow
52 sdldown 0
53 sdlup 0
64 liveband
64 layout bodies
66 sdlat devwindow
68 sdldown 0
69 sdlup 0
80 liveband
80 layout bodies
84 quit discard
" 90)
card(a0 20 "Abacus")
check("Abacus shown unfolded, no window (${a0_folded} ${a0_window})" a0_folded EQUAL 0 AND a0_window EQUAL 0)
check("the fold arrow is drawn" out MATCHES "sdlat frame 22: devfold at")
card(a1 34 "Abacus")
check("1. the arrow folds the card in place (${a1_folded})" a1_folded EQUAL 1)
check("1. ... nor a window (${a1_window})" a1_window EQUAL 0)
check("2. the folded strip draws its arrow" out MATCHES "sdlat frame 36: devfold at")
card(a2 48 "Abacus")
check("2. the strip's arrow unfolds it (${a2_folded})" a2_folded EQUAL 0)
check("2. ... still no window (${a2_window})" a2_window EQUAL 0)
card(a3 64 "Abacus")
check("3. the window button opens its window (${a3_window})" a3_window EQUAL 1)
string(REGEX MATCH "frame 64 bodies \\[[^]\n]*\\]" b64 "${out}")
check("3. the channel settings body is drawn once, by its window (${b64})" b64 MATCHES "channel settings [0-9-]+/[0-9-]+ x1 @" AND NOT b64 MATCHES "channel settings [^;]*@Device chain")
card(a4 80 "Abacus")
check("4. the window button again closes it (${a4_window})" a4_window EQUAL 0)
string(REGEX MATCH "frame 80 bodies \\[[^]\n]*\\]" b80 "${out}")
check("4. ... and nothing draws its body (${b80})" NOT b80 MATCHES "channel settings")

# 5-6: Chords (channel 6): Keys, then Chorus, Tracer and Reverb on mixer track 7.
run(chain "6 typing select 6
8 layout show live.device
20 liveband
22 sdlat devfold
24 sdldown 0
25 sdlup 0
34 liveband
36 plugwin fx 7 1
50 liveband
50 layout bodies
52 sdlat devfold
54 sdldown 0
55 sdlup 0
64 liveband
66 layout close ###fxeditor
70 plugwin chan 6
84 liveband
84 layout bodies
88 quit discard
" 94)
card(c0 20 "Keys")
check("5. Keys shown unfolded (${c0_folded})" c0_folded EQUAL 0)
card(c1 34 "Keys")
card(c1r 34 "Reverb")
check("5. the first card's arrow folds it (${c1_folded})" c1_folded EQUAL 1)
check("5. ... the other cards stay open (${c1r_folded})" c1r_folded EQUAL 0)
card(c2 50 "Tracer")
check("6. an effect opened from the mixer opens its window (${c2_window})" c2_window EQUAL 1)
string(REGEX MATCH "frame 50 bodies \\[[^]\n]*\\]" b50 "${out}")
check("6. ... its editor drawn once, by its window, not by the band (${b50})" b50 MATCHES "effect editor [0-9-]+/[0-9-]+ x1 @" AND NOT b50 MATCHES "effect editor [^;]*@Device chain")
card(c3 64 "Keys")
card(c3t 64 "Tracer")
check("6. the first card's arrow unfolds only it (${c3_folded})" c3_folded EQUAL 0)
check("6. ... the effect's window stays (${c3t_window})" c3t_window EQUAL 1)
card(c4 84 "Keys")
check("7. Channel settings of an instrument open its window (${c4_window}), the card unfolded (${c4_folded})" c4_window EQUAL 1 AND c4_folded EQUAL 0)
string(REGEX MATCH "frame 84 bodies \\[[^]\n]*\\]" b84 "${out}")
check("7. ... the channel settings body drawn once, by its window (${b84})" b84 MATCHES "channel settings [0-9-]+/[0-9-]+ x1 @" AND NOT b84 MATCHES "channel settings [^;]*@Device chain")
message(STATUS "ok - the Clips device band's arrow folds in place; editors open in their windows")
