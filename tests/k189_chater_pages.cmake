# Gridline - Chater's editor never traps the user (hidden run, isolated config, real input).
# Every overlay and page opens and closes again, and the main page comes back:
#   Settings: its button toggles it, Escape, the close cross, and a click outside it (which also
#   reaches what it lands on: a tile's chevron opens that page);
#   detail pages: the close cross, Escape, a click on the open tile, the Motion button toggles;
#   the preset browser: its button toggles it, Escape, Close;
#   the dice: Roll toggles it, Escape, a click outside, Settings replaces it;
#   the knob menu: a click outside, Escape.
# Clicks land on points of the editor's 1100 x 680 form ("chater click x y"); "chater state"
# prints the page and which overlays are open.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P k189_chater_pages.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

# steps: <command> then a state report; each step 9 frames apart
set(steps
  "state|main"
  "click 934 23|settings"            # Settings opens
  "click 934 23|main"                # ... and its button closes it again (the reported trap)
  "click 934 23|settings"
  "key escape|main"                  # Escape
  "click 934 23|settings"
  "click 1069 71|main"               # the sheet's close cross
  "click 934 23|settings"
  "click 122 491|glitter"            # a click outside: closes the sheet and opens Sparkle's page
  "click 1069 339|main"              # the page's close cross
  "click 122 491|glitter"
  "key escape|main"                  # Escape on a page
  "click 122 491|glitter"
  "click 40 240|main"                # the open tile again
  "click 842 23|motion"              # Motion toggles
  "click 842 23|main"
  "click 418 23|presets"             # Presets toggles
  "click 418 23|main"
  "click 418 23|presets"
  "key escape|main"
  "click 418 23|presets"
  "click 1040 87|main"               # the browser's Close
  "click 495 23|dice"                # Roll toggles
  "click 495 23|main"
  "click 495 23|dice"
  "key escape|main"
  "click 495 23|dice"
  "click 200 640|main"               # a click outside the popover
  "click 495 23|dice"
  "click 934 23|settings"            # Settings replaces the dice
  "key escape|main"
  "menu|menu"                        # the knob menu
  "click 600 640|main"               # a click outside closes it
  "menu|menu"
  "key escape|main"
  "click 934 23|settings"            # Settings open when the window closes ...
  "plugwin close|-"
  "plugwin fx 1 9|-"
  "state|main"                       # ... reopens on the main page
  "click 495 23|dice"
  "plugwin close|-"
  "plugwin fx 1 9|-"
  "state|main"
  "click 934 23|settings"            # a project saved with Settings open (run 2 loads it)
  "save k189_chater_pages/saved.gridline|-")
set(script "")
set(frame 40)
set(wants "")
foreach(s ${steps})
  string(REPLACE "|" ";" parts "${s}")
  list(GET parts 0 cmd)
  list(GET parts 1 want)
  if(want STREQUAL "-")
    string(APPEND script "${frame} ${cmd}\n")
  elseif(cmd STREQUAL "state")
    string(APPEND script "${frame} chater state\n")
  elseif(cmd MATCHES "^key ")
    string(APPEND script "${frame} ${cmd}\n")
    math(EXPR f2 "${frame} + 6")
    string(APPEND script "${f2} chater state\n")
  else()
    string(APPEND script "${frame} chater ${cmd}\n")
    math(EXPR f2 "${frame} + 6")
    string(APPEND script "${f2} chater state\n")
  endif()
  if(NOT want STREQUAL "-")
    list(APPEND wants "${want}")
  endif()
  math(EXPR frame "${frame} + 9")
endforeach()
math(EXPR frame "${frame} + 4")
string(APPEND script "${frame} quit discard\n")
file(WRITE "${OUT}/a.txt" "${script}")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --size 1600x1000 --ui-scale 1 --plugin gl.chater
                        --dev-input "${OUT}/a.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL: gridline-app exited with ${rc}:\n${err}\n${out}")
endif()
string(REGEX MATCHALL "chater state: [^\n]*" reports "${out}")
list(LENGTH reports n)
list(LENGTH wants nw)
if(NOT n EQUAL nw)
  message(FATAL_ERROR "FAIL: ${n} state reports, expected ${nw}:\n${out}")
endif()
# page numbers: 0 main, 2 glitter, 5 motion (chater_widgets.h Page)
set(pat_main "page=0 settings=0 presets=0 dice=0 menu=0")
set(pat_settings "page=[0-9]+ settings=1 presets=0 dice=0 menu=0")
set(pat_glitter "page=2 settings=0 presets=0 dice=0 menu=0")
set(pat_motion "page=5 settings=0 presets=0 dice=0 menu=0")
set(pat_presets "settings=0 presets=1 dice=0 menu=0")
set(pat_dice "settings=0 presets=0 dice=1 menu=0")
set(pat_menu "settings=0 presets=0 dice=0 menu=1")
math(EXPR last "${n} - 1")
foreach(i RANGE ${last})
  list(GET reports ${i} r)
  list(GET wants ${i} w)
  string(REGEX MATCH "${pat_${w}}" ok "${r}")
  if(NOT ok)
    message(FATAL_ERROR "FAIL: report ${i}: wanted ${w}, got \"${r}\"\nAll:\n${out}")
  endif()
  message(STATUS "ok - report ${i} (${w}): ${r}")
endforeach()
# run 2: the project saved with Settings open loads back to a usable editor on the main page
if(NOT EXISTS "${OUT}/saved.gridline")
  message(FATAL_ERROR "FAIL: the project was not saved:\n${out}")
endif()
file(WRITE "${OUT}/b.txt" "30 plugwin fx 1 9\n40 chater state\n46 chater click 934 23\n52 chater state\n58 chater click 934 23\n64 chater state\n70 quit discard\n")
execute_process(COMMAND "${APP}" "${OUT}/saved.gridline" --hidden --no-ui-sounds --no-update-check --size 1600x1000 --ui-scale 1
                        --dev-input "${OUT}/b.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out2 ERROR_VARIABLE err2 TIMEOUT 300)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (run 2): gridline-app exited with ${rc}:\n${err2}\n${out2}")
endif()
string(REGEX MATCHALL "chater state: [^\n]*" r2 "${out2}")
list(LENGTH r2 n2)
if(NOT n2 EQUAL 3)
  message(FATAL_ERROR "FAIL (run 2): ${n2} state reports, expected 3:\n${out2}")
endif()
list(GET r2 0 a0)
list(GET r2 1 a1)
list(GET r2 2 a2)
foreach(pair "a0;main" "a1;settings" "a2;main")
  list(GET pair 0 v)
  list(GET pair 1 w)
  string(REGEX MATCH "${pat_${w}}" ok "${${v}}")
  if(NOT ok)
    message(FATAL_ERROR "FAIL (run 2): wanted ${w}, got \"${${v}}\"\n${out2}")
  endif()
  message(STATUS "ok - run 2 (${w}): ${${v}}")
endforeach()
message(STATUS "Chater: every page and overlay opens and closes back to the main page, also after reopening and loading")
