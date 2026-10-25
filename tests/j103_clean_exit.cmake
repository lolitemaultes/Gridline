# Gridline — J-103: the app exits cleanly (exit code 0, no abort) through its normal quit path.
# Quitting used to abort ("libc++abi: terminating ... mutex lock failed: Invalid argument",
# exit 134) when the background plugin scan was still running: the scan thread outlived
# main() and locked a mutex that static destruction had already destroyed.
# Each run uses a fresh isolated config, so the startup plugin scan runs (no --screenshot,
# which skips it) and is still going at quit on a machine with plugins installed.
#   1. session: the demo copy opened, playing, a channel's editor open, the window's quit
#      request (dev-input "quit") at frame 90; settings are still saved on the way out.
#   2. the editor layout audit as run by hand (no --screenshot): it ends through main()'s return.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j103_clean_exit.cmake
cmake_minimum_required(VERSION 3.16)  # the project's policies, also under CMake 3.x (CMP0054: a quoted "keep" is that string, never a variable)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
configure_file("${DEMO}" "${OUT}/demo copy.gridline.json" COPYONLY)
file(WRITE "${OUT}/quit.txt" "90 quit\n")

function(expectCleanExit tag rc err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL (${tag}): gridline-app exited with ${rc}:\n${err}")
  endif()
  foreach(bad "terminating" "mutex lock failed" "Assertion failed" "libc++abi")
    string(FIND "${err}" "${bad}" at)
    if(NOT at LESS 0)
      message(FATAL_ERROR "FAIL (${tag}): \"${bad}\" on stderr:\n${err}")
    endif()
  endforeach()
  message(STATUS "ok - ${tag}: exit code 0, nothing aborted")
endfunction()

# 1. A session ended by the user's quit.
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_session")
execute_process(COMMAND "${APP}" "${OUT}/demo copy.gridline.json" --hidden --no-ui-sounds --size 1440x900 --play --chan 0
                        --dev-input "${OUT}/quit.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
expectCleanExit(session "${rc}" "${err}")
file(GLOB_RECURSE settings "${OUT}/cfg_session/*settings.json")
if(NOT settings)
  message(FATAL_ERROR "FAIL (session): settings were not saved on quit")
endif()
message(STATUS "ok - session: settings saved on quit")

# 2. The layout audit, ended by main()'s return.
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_audit")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --layout-audit "${OUT}/audit"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 840)
expectCleanExit(layout_audit "${rc}" "${err}")
if(NOT err MATCHES "layout audit: [0-9]+ editors")
  message(FATAL_ERROR "FAIL (layout_audit): no audit summary on stderr:\n${err}")
endif()
