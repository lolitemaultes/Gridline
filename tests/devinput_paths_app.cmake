# Gridline - dev-input paths with spaces: a quoted "save" or "shot" path holding spaces is used whole, in the
# real app (hidden, isolated config and HOME), and every test script quotes the paths it hands those commands
# (devinput_paths_lint.py), so a build folder with a space in its path never splits one.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -DTESTS=<tests dir> -DPYTHON=<python3> -P devinput_paths_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
set(dir "${OUT}/with spaces/and more")
file(MAKE_DIRECTORY "${dir}")
file(MAKE_DIRECTORY "${OUT}/cfg/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg/home")

# 1. The scripts all quote their paths.
execute_process(COMMAND "${PYTHON}" -I "${TESTS}/devinput_paths_lint.py" "${TESTS}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE lint ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: test scripts hand dev-input unquoted paths:\n${lint}\n${err}")
endif()
message(STATUS "${lint}")

# 2. The app keeps a quoted path whole.
set(song "${dir}/saved song.gridline.json")
set(pic "${dir}/a shot.ppm")
file(WRITE "${OUT}/input.txt" "4 typing off\n10 save \"${song}\"\n12 shot \"${pic}\"\n20 quit discard\n")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1024x700
                        --dev-input "${OUT}/input.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app failed (${rc}):\n${out}\n${err}")
endif()
if(NOT out MATCHES "save frame 10: saved ${song}\n")
  message(FATAL_ERROR "FAIL: the quoted save path was not used whole:\n${out}")
endif()
foreach(f "${song}" "${pic}")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "FAIL: nothing written at ${f}:\n${out}")
  endif()
endforeach()
# a split path would have cut at the first space: "${OUT}/with"
if(EXISTS "${OUT}/with")
  message(FATAL_ERROR "FAIL: a quoted path was split at its first space (${OUT}/with written)")
endif()
message(STATUS "ok - quoted save and shot paths with spaces are kept whole")
