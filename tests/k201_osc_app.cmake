# Gridline: OSC in the real app (hidden, isolated config, SDL's dummy audio driver). OSC is switched
# on in the settings on any free port of 127.0.0.1, with feedback to the sender. The test binary runs
# beside the app (its stdin is the app's output): it reads the port the app prints, sends tempo, a
# mixer volume, a plugin parameter, a note and Play, waits for the app's feedback (tempo and play
# state), then pings; the app's script waits for that and prints the mixer.
#   cmake -DAPP=<gridline-app> -DTESTBIN=<gridline_tests_k201> -DOUT=<dir> -P k201_osc_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"version\": 1, \"values\": {\"osc.enabled\": true, \"osc.port\": 0, \"osc.feedback\": true, \"osc.feedbackPort\": 0}}\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_OSC_TRACE} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")
# 6 messages and the helper's ping = 7 received.
file(WRITE "${OUT}/script.txt" "3 waitfor osc-received:7 90\n5 mixer\n6 transport\n")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size 1280x800 --frames 10 --screenshot "${OUT}/app.ppm" --dev-input "${OUT}/script.txt"
                COMMAND "${TESTBIN}" apphelper
                RESULTS_VARIABLE rcs OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
file(WRITE "${OUT}/out.txt" "${out}\n--- stderr\n${err}")
list(GET rcs 0 rcApp)
list(GET rcs 1 rcHelper)
if(NOT rcApp EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rcApp}): ${err}\n${out}")
endif()

function(check msg re)
  if(NOT out MATCHES "${re}")
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}\n${err}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

check("the app listens on 127.0.0.1" "osc: listening [1-9][0-9]*")
check("tempo arrives and is applied" "osc: /transport/tempo ,f 133 -> handled")
check("a mixer volume arrives" "osc: /mixer/1/volume ,f 0.25 -> handled")
check("a note on and off arrive" "osc: /channel/1/noteoff ,i 60 -> [a-z]")
check("play arrives" "osc: /transport/play ,i 1 -> handled")
check("feedback reaches the sender (tempo and play state)" "helper: feedback tempo yes play yes")
check("the mixer track's volume changed (0.25 of the fader = 0.3125)" "track 1 volume 0\\.3125")
check("the script waited for every message" "waitfor osc-received:7: reached")
if(NOT rcHelper EQUAL 0)
  message(FATAL_ERROR "FAIL: the sender reported a failure (${rcHelper})\n${out}")
endif()
message(STATUS "ok - OSC in the app")
