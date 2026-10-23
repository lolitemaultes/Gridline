# Gridline — J-91 (GitHub issue #1): the top bar's transport buttons answer
# real clicks at every interface scale. Runs the app hidden (off-screen,
# isolated config) at 100 %, 150 % and 200 % and clicks play, pause, play,
# stop and record with SDL mouse events in window pixels (the path an OS click
# takes: SDL -> Dear ImGui backend -> interface scale), then checks the
# transport. A press that slides off the button before the release still
# counts (the transport acts on press). With the metronome on, the metronome
# glyph ticks while playing (J-97: a hard snap on each beat, no in-between
# frames), shows its rest pose when switched on and rests when stopped.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j91_transport_click.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")

set(script "5 toasts clear
8 sdlat play
10 sdldown 0
11 sdlup 0
13 transport
15 sdlat play
17 sdldown 0
18 sdlup 0
20 transport
22 sdlat play
24 sdldown 0
25 sdlup 0
27 transport
29 sdlat stop
31 sdldown 0
32 sdlup 0
34 transport
36 sdlat rec
38 sdldown 0
39 sdlup 0
41 recording
43 sdlat play
45 sdldown 0
46 sdlmove 4 4
47 sdlup 0
49 transport
50 sdlat metro
51 seekbeat 0.1
52 sdldown 0
53 sdlup 0
55 metro
60 seekbeat 0.6
62 metro
64 seekbeat 1.6
66 metro
68 seekbeat 2.6
70 metro
82 sdlat stop
84 sdldown 0
85 sdlup 0
88 metro
")
file(WRITE "${OUT}/click.txt" "${script}")

foreach(scale 1 1.5 2)
  if(scale STREQUAL "1")
    set(size 1600x900)
  elseif(scale STREQUAL "1.5")
    set(size 2400x1350)
  else()
    set(size 3200x1800)
  endif()
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size ${size} --ui-scale ${scale} --frames 92
                          --screenshot "${OUT}/s${scale}.ppm" --dev-input "${OUT}/click.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "scale ${scale}: gridline-app failed (${rc}): ${err}")
  endif()
  foreach(button play stop rec metro)
    if(out MATCHES "sdlat frame [0-9]+: ${button} not shown")
      message(FATAL_ERROR "FAIL (scale ${scale}): the ${button} button is not in the top bar. Output:\n${out}")
    endif()
  endforeach()
  set(expect
      "transport frame 13: pos [0-9]+ playing 1"   # play
      "transport frame 20: pos [0-9]+ playing 0"   # pause
      "transport frame 27: pos [0-9]+ playing 1"   # play again
      "transport frame 34: pos 0 playing 0"        # stop: back to the start
      "recording frame 41: armed 1"                # record armed
      "transport frame 49: pos [0-9]+ playing 1"   # pressed, slid off, released: still played
      "metro frame 55: on 1 playing 1")
  foreach(e IN LISTS expect)
    if(NOT out MATCHES "${e}")
      message(FATAL_ERROR "FAIL (scale ${scale}): expected \"${e}\". Output:\n${out}")
    endif()
  endforeach()
  # J-97: switching the metronome on only shows the rest pose (no animation on toggle).
  if(NOT out MATCHES "metro frame 55: on 1 playing 1 mirror 1\\.000")
    message(FATAL_ERROR "FAIL (scale ${scale}): switched on, the metronome should rest facing right. Output:\n${out}")
  endif()
  # Tick-tock: while playing, the glyph faces right on the beat it was switched on in and on every
  # second beat after it, mirrored on the others (the song position is set to beats 0, 1 and 2 of
  # the one-bar pattern); after Stop it rests facing right. Only +1 / -1 ever show (no turn).
  foreach(pair "62;1" "66;-1" "70;1")
    list(GET pair 0 fr)
    list(GET pair 1 side)
    if(NOT out MATCHES "metro frame ${fr}: on 1 playing 1 mirror ${side}\\.000")
      message(FATAL_ERROR "FAIL (scale ${scale}): frame ${fr}: the metronome should face ${side}. Output:\n${out}")
    endif()
  endforeach()
  string(REGEX MATCHALL "mirror -?[0-9.]+" faces "${out}")
  foreach(f IN LISTS faces)
    if(NOT f STREQUAL "mirror 1.000" AND NOT f STREQUAL "mirror -1.000")
      message(FATAL_ERROR "FAIL (scale ${scale}): an in-between metronome pose (${f}). Output:\n${out}")
    endif()
  endforeach()
  if(NOT out MATCHES "metro frame 88: on 1 playing 0 mirror 1\\.000")
    message(FATAL_ERROR "FAIL (scale ${scale}): the metronome does not rest after Stop. Output:\n${out}")
  endif()
  message(STATUS "ok - scale ${scale}: play / pause / play / stop / record by click, press-and-slide plays, metronome snaps and rests")
endforeach()
