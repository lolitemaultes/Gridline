# Gridline - K-112: the menu bars as data (the menu tree).
#   For each menu style (Studio, Live, Logic, and Tracker as a development layout) the app dumps its
#   menu tree and the command palette. Checks:
#     - the tree equals the committed one (tests/k112/menutree_<style>.txt): Studio's menus keep
#       every item, shortcut, command and state they had;
#     - Tracker's menu bar is Studio's, item for item and command for command (shortcuts and states
#       aside: Tracker's key set, and the Layouts menu checks the active one); it has no menus of its
#       own yet, so a Tracker user reaches nothing Studio lacks;
#     - every item of the tree the palette lists is a palette row (the palette reads the tree), and
#       the palette has no menu row the tree lacks.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DEXPECT=<dir> -DOUT=<dir> [-DRECORD=1] -P k112_menus_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

foreach(L studio live logic tracker)
  set(cfg "${OUT}/cfg_${L}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${L}\", \"keys.profile\": \"${L}\", \"ui.menuStyle\": \"${L}\"}}\n")
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n7 menutree dump\n8 palette dump\n10 quit discard\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${cfg}/song.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${cfg}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --dev-input "${cfg}/input.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${L}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${L}: gridline-app failed (${rc})")
    continue()
  endif()
  string(REGEX MATCH "MENUTREE BEGIN [^\n]*\n(.*)MENUTREE END" m "${out}")
  if(NOT m)
    message(SEND_ERROR "FAIL: ${L}: no menu tree")
    continue()
  endif()
  set(tree "${CMAKE_MATCH_1}")
  file(WRITE "${OUT}/menutree_${L}.txt" "${tree}")
  # Items, commands and order (shortcuts aside: each layout brings its own key set; states aside).
  string(REGEX REPLACE "\t[e-][c-]\n" "\n" plain "\n${tree}")
  string(REGEX REPLACE "\n([^\t\n]*)\t[^\t\n]*\t" "\n\\1\t\t" plain "${plain}")
  set(tree_${L} "${plain}")
  string(REPLACE " > " "/" slashTree "${tree}")
  if(RECORD)
    file(WRITE "${EXPECT}/menutree_${L}.txt" "${tree}")
  elseif(NOT L STREQUAL "tracker")
    file(READ "${EXPECT}/menutree_${L}.txt" want)
    if(NOT tree STREQUAL want)
      message(SEND_ERROR "FAIL: ${L}: the menu tree changed (diff ${EXPECT}/menutree_${L}.txt ${OUT}/menutree_${L}.txt)")
    endif()
  endif()
  # The palette's menu rows against the tree: every enabled-or-not item path the palette shows is in the tree.
  string(REGEX MATCHALL "ROW\tcommand\tmenu:[^\t]*" rows "${out}")
  list(LENGTH rows nrows)
  set(missing 0)
  foreach(r IN LISTS rows)
    string(REGEX REPLACE "^ROW\tcommand\tmenu:" "" path "${r}")
    string(REGEX REPLACE "#[0-9]+$" "" path "${path}")
    string(FIND "${slashTree}" "${path}\t" at)
    if(at EQUAL -1)
      math(EXPR missing "${missing} + 1")
      if(missing LESS 6)
        message(SEND_ERROR "FAIL: ${L}: palette row not in the menu tree: ${path}")
      endif()
    endif()
  endforeach()
  if(nrows LESS 50)
    message(SEND_ERROR "FAIL: ${L}: only ${nrows} menu rows in the palette")
  endif()
  message(STATUS "${L}: ${nrows} palette menu rows, ${missing} missing from the tree")
endforeach()

if(DEFINED tree_tracker AND DEFINED tree_studio AND NOT tree_tracker STREQUAL tree_studio)
  message(SEND_ERROR "FAIL: Tracker's menu bar is not Studio's")
endif()
