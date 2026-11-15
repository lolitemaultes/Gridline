# Gridline - K-112: a run whose layout never asks for the system menu bar never starts it.
# Studio (its menus stay in its top bar) and a test run of Live (test runs keep the in-window row)
# run a while, choosing menu items and switching layouts; the system menu's status line must say
# the bus worker was never started (Linux: no thread, no libdbus, nothing on the session bus).
# Only Linux has a bus worker: on macOS and Windows "bus" is always 0, so there the test checks
# no more than that the system menu stays uninstalled; it means something on Linux.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_sysmenu_idle_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)

function(run tag settings script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {${settings}}}\n")
  file(WRITE "${cfg}/input.txt" "${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size 1440x900 --fixed-dt 16
                          --dev-input "${cfg}/input.txt" ${ARGN} --frames 70 --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
    math(EXPR failures "${failures} + 1")
  elseif(NOT out MATCHES "frame 60 sysmenu status installed 0 bus 0 ")
    string(REGEX MATCH "frame [0-9]+ sysmenu status [^\n]*" line "${out}")
    message(SEND_ERROR "FAIL: ${tag}: the system menu bus was started (${line})")
    math(EXPR failures "${failures} + 1")
  else()
    message(STATUS "ok - ${tag}: the system menu bus never started")
  endif()
  set(failures ${failures} PARENT_SCOPE)
endfunction()

set(script "5 toasts clear\n20 menutree dump\n30 sysmenu dump\n60 sysmenu status\n")
run(studio "\"ui.layoutChosen\": true" "${script}")
run(live "\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.systemMenuBar\": true" "${script}" --switch-layout live@3
    --switch-layout logic@40)

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - the system menu bus starts only when a layout asks for it")
