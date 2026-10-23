# Gridline — J-98 user report: "the split parameter in Tideline doesn't properly
# follow the cursor when tweaking". Opens Tideline (hidden, off-screen,
# isolated config) at 100 %, 150 % and 200 % interface scale and drags its
# Split slider with real SDL mouse events in window pixels. After every move
# the slider's handle must sit under the pointer within one logical pixel
# (minus the offset at which it was grabbed): no 1.25x gain, no drift after
# passing either end, no jump on a click, Shift drags 8x finer.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j98_slider_follow.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")

# 10 = Tideline's Split (gl.tideline BwBandFreq). "ctl 10 <f> [px]" checks the handle
# against fraction f of the track minus a grab offset of px logical pixels.
set(script "5 toasts clear
10 sdlctl 10 handle
12 sdldown 0
14 sdlctl 10 0.6
16 ctl 10 0.6
18 sdlctl 10 0.15
20 ctl 10 0.15
22 sdlctl 10 1.3
24 ctl 10 1.0
26 sdlctl 10 0.8
28 ctl 10 0.8
30 sdlctl 10 -0.2
32 ctl 10 0
34 sdlctl 10 0.4
36 ctl 10 0.4
38 sdlup 0
40 sdlctl 10 handle 4
42 sdldown 0
44 sdlup 0
46 ctl 10 0.4
47 sdlctl 10 handle -3
48 sdldown 0
50 sdlctl 10 0.7
52 ctl 10 0.7 -3
54 sdlup 0
55 sdlctl 10 0.1
56 sdlctl 10 handle
58 sdldown 0
60 mod shift 1
62 sdlctl 10 handle 80
64 ctl 10 0.7 -13
66 mod shift 0
68 sdlctl 10 0.5
70 ctl 10 0.5 70
72 sdlup 0
")
file(WRITE "${OUT}/drag.txt" "${script}")

foreach(scale 1 1.5 2)
  if(scale STREQUAL "1")
    set(size 1600x900)
  elseif(scale STREQUAL "1.5")
    set(size 2400x1350)
  else()
    set(size 3200x1800)
  endif()
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.tideline --size ${size} --ui-scale ${scale} --frames 76
                          --screenshot "${OUT}/s${scale}.ppm" --dev-input "${OUT}/drag.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "scale ${scale}: gridline-app failed (${rc}): ${err}")
  endif()
  if(out MATCHES "not shown")
    message(FATAL_ERROR "FAIL (scale ${scale}): the Split slider is not on screen. Output:\n${out}")
  endif()
  foreach(fr 16 20 24 28 32 36 46 52 64 70)
    if(NOT out MATCHES "ctl frame ${fr}: param 10 [^\n]* ok")
      message(FATAL_ERROR "FAIL (scale ${scale}): frame ${fr}: the Split handle is not under the pointer. Output:\n${out}")
    endif()
  endforeach()
  if(NOT out MATCHES "ctl frame 24: param 10 value 5120\\.0000" OR NOT out MATCHES "ctl frame 32: param 10 value 20\\.0000")
    message(FATAL_ERROR "FAIL (scale ${scale}): dragging past the ends does not reach 20 Hz / 5.12 kHz. Output:\n${out}")
  endif()
  message(STATUS "ok - scale ${scale}: Split handle follows the pointer (drag, past the ends and back, click without a jump, grab offset, Shift fine)")
endforeach()
