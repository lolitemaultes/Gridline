# Gridline: GitHub #89, the mixer's Arm button arms a track and the track then records, in the real app
# (hidden, isolated config and HOME, SDL's dummy audio driver: its input is silence, never a microphone),
# in all four layouts. Settings start as a new installation has them: no input device chosen. A real
# click on the mixer inspector's Arm button (its Input still "(none)") must light it ("Armed"), give the
# track an input, choose the computer's default input device (before, the track was armed with no device
# open, so nothing could ever record), and a recording then makes a take on that track.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> [-DSHOTS=<dir>] -P gh89_arm_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n${out}")
endfunction()

foreach(lay studio live logic tracker)
  set(dir "${OUT}/${lay}")
  file(MAKE_DIRECTORY "${dir}/cfg/gridline" "${dir}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_CONFIG_HOME} "${dir}/cfg")
  set(ENV{HOME} "${dir}/home")
  file(WRITE "${dir}/cfg/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${lay}\"}}\n")
  configure_file("${DEMO}" "${dir}/song.gridline.json" COPYONLY)
  set(shot "")
  if(DEFINED SHOTS)
    set(shot "31 shot \"${SHOTS}/gh89_${lay}_armed.ppm\"\n")
  endif()
  # Studio: a real click on the mixer inspector's Arm. The other layouts have no mixer inspector; their
  # arm controls (the strips' record dots, the track headers, the Record window) arm the same way as the
  # strip's record dot does here ("recmore arm").
  if(lay STREQUAL "studio")
    set(arm "22 atitem \"Arm\"\n24 down 0\n25 up 0\n")
    set(lit "34 atitem \"Armed\"\n")
  else()
    set(arm "22 recmore arm 1\n")
    set(lit "")
  endif()
  file(WRITE "${dir}/input.txt"
       "5 toasts clear\n20 armstate\n21 recmore clips\n${arm}30 armstate\n${shot}${lit}"
       "40 liverec start\n41 waitfor never 1.5\n42 liverec stop\n50 armstate\n55 recmore clips\n60 quit discard\n")
  execute_process(COMMAND "${APP}" "${dir}/song.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1440x900 --layout ${lay}
                          --show mixer --mixer-view sel1 --dev-input "${dir}/input.txt" --fixed-dt 16 --frames 70
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${dir}/run.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    fail("${lay}: gridline-app failed (${rc})")
  endif()
  if(out MATCHES "atitem frame 22: [^\n]*not shown")
    fail("${lay}: the mixer inspector shows no Arm button")
  endif()
  string(REGEX MATCH "armstate frame 20: [^\n]*" before "${out}")
  if(NOT before MATCHES "device \\[\\]")
    fail("${lay}: an input device was chosen before the click: ${before}")
  endif()
  string(REGEX MATCH "armstate frame 30: [^\n]*\n(  arm [^\n]*\n)*" after "${out}")
  if(NOT after MATCHES "  arm 1 armed 1 input [0-9]+")
    fail("${lay}: the click on Arm did not arm insert 1: ${after}")
  endif()
  if(after MATCHES "device \\[\\]")
    fail("${lay}: armed with no input device: nothing can record (${after})")
  endif()
  if(out MATCHES "atitem frame 34: [^\n]*not shown")
    fail("${lay}: the button does not read Armed after the click")
  endif()
  string(REGEX MATCH "armstate frame 50: [^\n]*" late "${out}")
  if(NOT late MATCHES "inputs [1-9]")
    fail("${lay}: no input open for the recording: ${late}")
  endif()
  # The recording made a take on the armed track's playlist track.
  string(REGEX MATCH "recmore frame 21: track -1 clips ([0-9]+)" rc0 "${out}")
  set(clips0 "${CMAKE_MATCH_1}")
  string(REGEX MATCH "recmore frame 55: track -1 clips ([0-9]+)" rc "${out}")
  if(NOT rc0 OR NOT rc OR NOT CMAKE_MATCH_1 GREATER clips0)
    fail("${lay}: the armed track recorded nothing (audio clips before: ${clips0}; after: ${rc})")
  endif()
  message(STATUS "${lay}: ${after}${rc}")
  file(REMOVE_RECURSE "${dir}")
endforeach()
file(REMOVE_RECURSE "${OUT}")
