# Gridline: J-194, the browser search field's clear button (hidden, isolated config and HOME, a fixed
# 16 ms frame time). The app starts with "kick" in the search field; the script then
#   1. clicks the clear button: the field empties (the button is shown only while there is text);
#   2. clicks into the field, types "drum", clicks the clear button while the field is being edited:
#      the field empties and keeps the keyboard focus, so "hat" typed next lands in it.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j194_browser_search_clear.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
file(WRITE "${OUT}/clear.txt" "10 sdlat brclear
12 sdldown 0
13 sdlup 0
18 sdlat brclear
24 sdlat brsearch
26 sdldown 0
27 sdlup 0
30 text drum
34 sdlat brclear
36 sdldown 0
37 sdlup 0
42 sdlat brclear
46 text hat
50 sdlat brclear
55 quit discard
")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --browser-search kick
                        --dev-input "${OUT}/clear.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}\n${out}")
endif()

function(expect frame shown what)
  if(shown)
    set(re "sdlat frame ${frame}: brclear at ")
  else()
    set(re "sdlat frame ${frame}: brclear not shown")
  endif()
  if(NOT out MATCHES "${re}")
    message(FATAL_ERROR "FAIL: ${what}\n${out}")
  endif()
  message(STATUS "ok: ${what}")
endfunction()

expect(10 TRUE "the clear button shows with text in the field")
expect(18 FALSE "clicking it empties the field")
expect(34 TRUE "typed text in the edited field shows the button again")
expect(42 FALSE "clicking it while the field is being edited empties it")
expect(50 TRUE "the field keeps the focus: text typed next lands in it")
