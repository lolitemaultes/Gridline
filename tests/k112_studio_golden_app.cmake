# Gridline - K-112: Studio stays exactly as it was. The layout style report (every ImGui style size,
# the style colours' hash, the bar heights, the dock tree with every node's rectangle and windows,
# the open windows) of the Studio layout at 1440x900 and 1280x800 must equal the committed report,
# recorded before the layout work began. (The pixel goldens of the same shots are compared on the
# Mac by the review; this text level runs everywhere.)
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DEXPECT=<tests/k112> -DOUT=<dir> -P k112_studio_golden_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(failures 0)
foreach(size 1440x900 1280x800)
  set(cfg "${OUT}/cfg_${size}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json" "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${cfg}/song.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${cfg}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size ${size} --frames 40
                          --screenshot "${OUT}/studio_${size}.ppm" --style-report "${OUT}/studio_${size}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${size}: gridline-app failed (${rc}): ${err}")
    math(EXPR failures "${failures} + 1")
    continue()
  endif()
  file(READ "${OUT}/studio_${size}.txt" got)
  file(READ "${EXPECT}/studio_style_report_${size}.txt" want)
  if(NOT got STREQUAL want)
    message(SEND_ERROR "FAIL: ${size}: the Studio style report changed (got ${OUT}/studio_${size}.txt, expected ${EXPECT}/studio_style_report_${size}.txt)")
    math(EXPR failures "${failures} + 1")
  else()
    message(STATUS "ok - ${size}: the Studio style report is unchanged")
  endif()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failure(s)")
endif()
