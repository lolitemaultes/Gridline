# Gridline - K-376: in the Tracker the key focus frame (the quiet accent line round the area with the
# keys, ui::drawKeyFocusFrame) stays put unless the keys really move. Before, it flickered: a click
# on a header band button took the keys to the band for as long as the button was held (the band
# hands them back to the pattern after the click), so the frame jumped up to the band and back, and
# a page brought to the front was hidden for its first two frames, so the frame dropped out.
#   The app counts, each frame, whether the frame was drawn, round which area, and how often it moved
# ("tracker keyframe"). Idle (with and without idle frame pacing), a notification, a hint, a held
# band button and a click on one: drawn every frame, never moved. A page switch: one move each way,
# no frame missed.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k376_tracker_key_frame_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run's log: ${OUT}/run.log)")
endfunction()

# At 1440x900: the band's Metronome button (a toggle, no menu).
set(BTNX 701)
set(BTNY 60)
set(K "tracker keyframe")
file(WRITE "${OUT}/input.txt" "5 toasts clear
20 ${K} reset
320 ${K}
322 ${K} reset
324 move ${BTNX} ${BTNY}
330 save \"${OUT}/no-such-folder/x/song.gridline\"
420 ${K}
422 ${K} reset
430 down 0
438 ${K}
440 up 0
460 ${K}
462 ${K} reset
470 down 0
471 up 0
472 down 0
473 up 0
490 ${K}
492 ${K} reset
494 move 700 600
500 tracker page Mix
520 ${K}
530 tracker page Pattern
550 ${K}
560 quit discard
")

execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --video-clock 60 --size 1440x900 --layout tracker
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 580
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc})")
endif()

# frame, frames drawn at least, moves expected, area expected, borrowed (or "any"), what
macro(expect frame mindrawn moves area borrowed what)
  if(NOT out MATCHES "${K} frame ${frame}: drawn ([0-9]+) missed ([0-9]+) popups ([0-9]+) moves ([0-9]+) area ([^ ]+( [^ ]+)?) borrowed ([01])")
    fail("no key frame report on frame ${frame} (${what})")
  endif()
  set(_drawn ${CMAKE_MATCH_1})
  set(_missed ${CMAKE_MATCH_2})
  set(_moves ${CMAKE_MATCH_4})
  set(_area "${CMAKE_MATCH_5}")
  set(_borrowed ${CMAKE_MATCH_7})
  if(_drawn LESS ${mindrawn})
    fail("${what}: the frame was drawn on ${_drawn} frames, expected at least ${mindrawn}")
  endif()
  if(NOT _missed EQUAL 0)
    fail("${what}: the frame dropped out on ${_missed} frames")
  endif()
  if(NOT _moves EQUAL ${moves})
    fail("${what}: the frame moved ${_moves} times, expected ${moves}")
  endif()
  if(NOT "${_area}" STREQUAL "${area}")
    fail("${what}: the frame is round '${_area}', expected '${area}'")
  endif()
  if(NOT "${borrowed}" STREQUAL "any" AND NOT _borrowed EQUAL ${borrowed})
    fail("${what}: the band borrowing the keys reads ${_borrowed}, expected ${borrowed}")
  endif()
  message(STATUS "${what}: drawn ${_drawn}, moves ${_moves}, round ${_area}")
endmacro()

expect(320 290 0 "Pattern editor" 0 "300 idle frames")
expect(420 90 0 "Pattern editor" 0 "a hint over a band button and a notification")
expect(438 8 0 "Pattern editor" 1 "a band button held (the band borrows the keys)")
expect(460 30 0 "Pattern editor" 0 "the button let go (the keys back on the pattern)")
expect(490 20 0 "Pattern editor" 0 "two quick clicks on a band button")
expect(520 20 1 "Mixer" 0 "the Mix page brought to the front")
expect(550 50 2 "Pattern editor" 0 "the Pattern page back")

# Idle frame pacing on (as in normal use: 10 frames a second, then 4): the frame every frame all the same.
file(WRITE "${OUT}/input2.txt" "5 toasts clear
10 ${K} reset
60 ${K}
62 quit discard
")
set(ENV{GRIDLINE_IDLE_PACING} "1")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout tracker
                        --dev-input "${OUT}/input2.txt" --frames 80
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
unset(ENV{GRIDLINE_IDLE_PACING})
file(WRITE "${OUT}/run2.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed with idle pacing (${rc})")
endif()
expect(60 45 0 "Pattern editor" 0 "idle frames with idle pacing")
message(STATUS "k376_tracker_key_frame_app: ok")
