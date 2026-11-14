# Gridline - GitHub issue #52: the keyboard works from the first frames, in the real app (hidden, an
# isolated config folder and HOME, SDL's dummy audio driver). The keys and text are sent as SDL
# sends them while the system has not told it which window has the keyboard focus (naming no
# window), which the interface used to drop:
#   1. Space pressed in the third frame starts playback;
#   2. text typed into the command palette's search field arrives there.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P key_focus_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/cfg/home")
file(READ "${DEMO}" demo)  # a copy: the demo song is never opened (or saved over) in place
file(WRITE "${OUT}/fixture.gridline.json" "${demo}")
file(WRITE "${OUT}/script.txt" "1 transport
3 sdlkey #44 down orphan
4 sdlkey #44 up orphan
10 transport
12 sdlkey #44 down orphan
13 sdlkey #44 up orphan
18 transport
20 palette open
24 sdltext gri orphan
26 sdltext d orphan
30 palette state
32 palette close
40 quit discard
")
execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --no-update-check --dev-input "${OUT}/script.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
file(WRITE "${OUT}/run.out" "${out}\n${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app ended with ${rc}\n${out}\n${err}")
endif()
function(must regex what)
  if(NOT out MATCHES "${regex}")
    message(FATAL_ERROR "FAIL: ${what}\n${out}\n${err}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()
must("transport frame 1: [^\n]*playing 0" "stopped at the start")
must("transport frame 10: [^\n]*playing 1" "Space in the third frame starts playback")
must("transport frame 18: [^\n]*playing 0" "Space again stops it")
must("palette frame 30: open 1 [^\n]*query \\[grid\\]" "typed text reaches the search field")
if(NOT err MATCHES "\\[keyboard\\] keys arrive while no window has the keyboard focus")
  message(FATAL_ERROR "FAIL: the log does not say keys came without a focused window\n${err}")
endif()
message(STATUS "ok - the log says so once")
