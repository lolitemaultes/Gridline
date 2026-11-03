# Gridline - K-37: closing a bridged plugin's interface in the real app (hidden, isolated config,
# off-screen, SDL's dummy audio driver) with the in-tree test plugin standing in for a bridged one
# (GRIDLINE_DEV_BRIDGED_PLUGINS) and an interface that draws nothing (GRIDLINE_TEST_PLAIN_GUI):
#   1. an interface that closes: closed at once, the plugin alive, the frames go on;
#   2. an interface that never closes (GRIDLINE_TEST_GUI_CLOSE_HANGS): the frames go on, the plugin
#      counts as lost when the wait is over, and the app quits;
#   3. the same with the window open at quit: the app quits.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DCLAP=<bundle> -DOUT=<dir> -P k37_bridged_close_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_DEV_BRIDGED_PLUGINS} "1")
set(ENV{GRIDLINE_DEV_BRIDGED_GIVE_UP_MS} "1")
set(ENV{GRIDLINE_TEST_PLAIN_GUI} "1")
file(READ "${DEMO}" demo)
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")

function(run tag script)
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --size 1440x900 --frames 120
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" --plugin gl.patcher
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
  file(WRITE "${OUT}/${tag}.out.txt" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL ${tag}: gridline-app ended with ${rc}\n${out}\n${err}")
  endif()
  set(out "${out}\n${err}" PARENT_SCOPE)
endfunction()

set(open "10 junction scan clap ${CLAP}
14 junction add clap:org.gridline.test.synth
30 junction open last
36 plugwin info
")
set(after "50 plugwin close
52 epoch
70 epoch
76 plugwin info
")

run(closes "${open}${after}")
if(NOT out MATCHES "junction open last: 1 shown 1")
  if(out MATCHES "junction open last: 0")
    message(STATUS "SKIP: no plugin window on this machine\n${out}")
    return()
  endif()
  message(FATAL_ERROR "FAIL: the interface did not open\n${out}")
endif()
if(NOT out MATCHES "plugwin frame 36: 1 windows" OR NOT out MATCHES "plugwin frame 76: 0 windows")
  message(FATAL_ERROR "FAIL: the window should be there, then gone\n${out}")
endif()
if(NOT out MATCHES "bridged close: closed" OR out MATCHES "bridged close: gave up")
  message(FATAL_ERROR "FAIL: an interface that closes should just close\n${out}")
endif()
if(NOT out MATCHES "epoch frame 70")
  message(FATAL_ERROR "FAIL: the frames stopped after closing\n${out}")
endif()
message(STATUS "ok - an interface that closes closes")

set(ENV{GRIDLINE_TEST_GUI_CLOSE_HANGS} "1")
run(hangs "${open}${after}")
if(NOT out MATCHES "bridged close: waiting" OR NOT out MATCHES "bridged close: gave up")
  message(FATAL_ERROR "FAIL: an interface that never closes should be waited for, then given up on\n${out}")
endif()
if(NOT out MATCHES "epoch frame 52" OR NOT out MATCHES "epoch frame 70" OR NOT out MATCHES "plugwin frame 76: 0 windows")
  message(FATAL_ERROR "FAIL: the frames should go on and the window go away\n${out}")
endif()
message(STATUS "ok - an interface that never closes does not stop the app, and the app quits")

run(quits "${open}")
if(NOT out MATCHES "plugwin frame 36: 1 windows")
  message(FATAL_ERROR "FAIL: the window should be open at quit\n${out}")
endif()
message(STATUS "ok - quitting with such an interface open ends")
