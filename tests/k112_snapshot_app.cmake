# Gridline - K-112: the layout switch snapshot never outlives its switch.
#   stale: the layout is switched while the home screen shows (frame 40) and the project comes back
#          at frame 80: no snapshot is drawn on any frame after that, and none is held (texture freed).
#   fresh: an ordinary switch shows the snapshot at full strength, then fades it and frees it.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_snapshot_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)
macro(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

function(run tag script)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
  file(WRITE "${cfg}/input.txt" "${script}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size 1440x900 --fixed-dt 16
                          --dev-input "${cfg}/input.txt" --frames 130 --switch-layout live@40 --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${tag}: gridline-app failed (${rc})")
  endif()
  set(log "${out}" PARENT_SCOPE)
endfunction()

set(probe "")
foreach(f 41 42 43 44 50 81 82 83 84 85 90 120)
  string(APPEND probe "${f} layout snapshot\n")
endforeach()

# ---- stale: switched behind the home screen ----
run(stale "5 toasts clear\n8 launcher home\n80 launcher back\n${probe}")
foreach(f 81 82 83 84 85 90 120)
  if(NOT log MATCHES "frame ${f} snapshot held ([01]) alpha (-?[0-9.]+)")
    fail("stale: no snapshot line at frame ${f}")
  elseif(NOT CMAKE_MATCH_2 STREQUAL "-1.00")
    fail("stale: a snapshot was drawn at frame ${f} after the home screen (alpha ${CMAKE_MATCH_2})")
  endif()
endforeach()
foreach(f 50 81 120)
  if(log MATCHES "frame ${f} snapshot held 1")
    fail("stale: the snapshot texture is still held at frame ${f}")
  endif()
endforeach()

# ---- fresh: an ordinary switch ----
run(fresh "5 toasts clear\n${probe}")
set(full 0)
foreach(f 41 42 43 44)
  if(log MATCHES "frame ${f} snapshot held [01] alpha 1.00")
    set(full 1)
  endif()
endforeach()
if(NOT full)
  fail("fresh: the snapshot was not shown at full strength right after the switch")
endif()
foreach(f 81 120)
  if(NOT log MATCHES "frame ${f} snapshot held 0 alpha -1.00")
    fail("fresh: the snapshot is still held or drawn at frame ${f}")
  endif()
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
message(STATUS "ok - the switch snapshot is dropped behind the home screen and freed after its fade")
