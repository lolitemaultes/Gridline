# Gridline — J-216: a run started with options (tests, screenshots, audits, the dev layout options
# such as --show mixer, which rebuilds the layout around the mixer) reads the saved window layout
# but never writes it back, so a test run can't leave its layout in the user's own. The app says so
# on stderr ("layout: <path> is read, not saved"); the saved file stays byte for byte.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j216_layout_untouched.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ini "${OUT}/cfg/layout.ini")
set(saved "[Window][Mixer]\nPos=4,52\nSize=800,600\nCollapsed=0\n")
file(WRITE "${ini}" "${saved}")

foreach(args "--hidden;--fixed-dt;16;--frames;480;--screenshot;${OUT}/a.ppm"
             "--hidden;--show;mixer;--fixed-dt;16;--frames;480;--screenshot;${OUT}/b.ppm"
             "--hidden;--show;playlist;--hide;browser;--fixed-dt;16;--frames;480;--screenshot;${OUT}/c.ppm")
  execute_process(COMMAND "${APP}" --no-ui-sounds --size 1280x800 ${args}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gridline-app ${args} failed (${rc}): ${err}")
  endif()
  if(NOT err MATCHES "layout: [^\n]*layout.ini is read, not saved")
    message(FATAL_ERROR "FAIL: the run '${args}' would save the window layout. stderr:\n${err}")
  endif()
  file(READ "${ini}" now)
  if(NOT now STREQUAL saved)
    message(FATAL_ERROR "FAIL: the run '${args}' rewrote the saved layout:\n${now}")
  endif()
  message(STATUS "ok - '${args}' read layout.ini and left it as it was")
endforeach()
