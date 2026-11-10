# Gridline — K-170: the Voltage In and Voltage Out editors and the voltage calibration window render
# cleanly (hidden window, isolated config and HOME):
#  1. the layout audit of both editors at widths x1, x1.5 and x2: no control too close to its panel's
#     edge and no overlapping text or controls (AlignCheck "overlap" rule);
#  2. Voltage In's editor and the calibration window open from the app (--plugin gl.cvin with
#     GRIDLINE_CV_CALIBRATION): the app exits cleanly and writes its frame.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k170_cv_editors_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{SDL_AUDIODRIVER} "dummy")

# ---- 1. layout audit of the two editors
set(ENV{GRIDLINE_AUDIT_ONLY} "gl.cvin,gl.cvctl")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --layout-audit "${OUT}/audit" --size 2400x1600 --screenshot "${OUT}/audit/frame.ppm"
                        --frames 100000
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: layout audit of the voltage editors failed (${rc}):\n${err}")
endif()
if(NOT err MATCHES "layout audit: 2 editors, 0 with issues")
  message(FATAL_ERROR "FAIL: the voltage editors have inset issues:\n${err}")
endif()
file(READ "${OUT}/audit/alignment.tsv" align)
string(REGEX MATCHALL "gl\\.cv[a-z]+\t[0-9.]+\toverlap[^\n]*" overlaps "${align}")
if(overlaps)
  string(REPLACE ";" "\n" overlaps "${overlaps}")
  message(FATAL_ERROR "FAIL: overlapping text or controls in the voltage editors:\n${overlaps}")
endif()
message(STATUS "ok - Voltage In and Voltage Out: no inset issues and no overlaps at three widths")
unset(ENV{GRIDLINE_AUDIT_ONLY})

# ---- 2. the editor and the calibration window in the app
set(ENV{GRIDLINE_CV_CALIBRATION} "1")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --plugin gl.cvin --size 1440x900 --frames 20
                        --screenshot "${OUT}/cvin.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/cvin.ppm")
  message(FATAL_ERROR "FAIL: Voltage In with the calibration window did not render (${rc}):\n${err}")
endif()
if(err MATCHES "Assertion|assert|terminating due to")
  message(FATAL_ERROR "FAIL: an assertion while drawing the voltage tools:\n${err}")
endif()
message(STATUS "ok - Voltage In's editor and the calibration window render")
