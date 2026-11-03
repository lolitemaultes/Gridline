# Gridline: K-37: the Sweep Kick channel's envelope page renders in the real app (hidden, isolated
# config, off-screen) at 100 % and 200 %, dark and light. A frame is written for each.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k37_sweepkick_env.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(shot tag size scale)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}/home")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --size ${size} --ui-scale ${scale} ${ARGN}
                          --plugin gl.sweepkick --show chanenv --frames 30 --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  file(SIZE "${OUT}/${tag}.ppm" bytes)
  if(bytes LESS 100000)
    message(FATAL_ERROR "${tag}: no frame written")
  endif()
endfunction()

shot(dark_100 1440x900 1.0)
shot(dark_200 2880x1800 2.0)
shot(light_100 1440x900 1.0 --light)
shot(light_200 2880x1800 2.0 --light)
