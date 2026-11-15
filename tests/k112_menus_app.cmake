# Gridline - K-112: the menu bars as data (the menu tree).
#   For each menu style (Studio, Live, Logic, and Tracker as a development layout) the app dumps its
#   menu tree and the command palette. Checks:
#     - the tree equals the committed one (tests/k112/menutree_<style>.txt): Studio's menus keep
#       every item, shortcut, command and state they had. The trees are the same on every system but
#       for two things: the file manager's name ("Show in Finder", "Explorer", "Files": compared as
#       one) and Smooth's (logic) shortcuts, which use Command on macOS only. So Smooth has two trees:
#       menutree_logic.txt (macOS) and menutree_logic_ctrl.txt (Linux, the BSDs, Windows). A macOS
#       run checks both, the second with GRIDLINE_DEV_KEYS_NO_CMD=1 (the set the other systems have);
#       record the goldens on a Mac (-DRECORD=1) and both are written;
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

# Passes: P is the pass (and golden) name, L its layout.
set(passes studio live logic tracker)
if(CMAKE_HOST_APPLE)
  list(APPEND passes logic_ctrl)
endif()
foreach(P IN LISTS passes)
  set(L "${P}")
  set(golden "${P}")
  if(P STREQUAL "logic_ctrl")
    set(L logic)
    set(ENV{GRIDLINE_DEV_KEYS_NO_CMD} "1")
  else()
    unset(ENV{GRIDLINE_DEV_KEYS_NO_CMD})
    if(P STREQUAL "logic" AND NOT CMAKE_HOST_APPLE)
      set(golden logic_ctrl)  # Control everywhere but macOS: the tree a Mac checks with GRIDLINE_DEV_KEYS_NO_CMD
    endif()
  endif()
  set(cfg "${OUT}/cfg_${P}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${L}\", \"keys.profile\": \"${L}\", \"ui.menuStyle\": \"${L}\"}}\n")
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n7 menutree dump\n8 palette dump\n10 quit discard\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${cfg}/song.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${cfg}/song.gridline.json" --hidden --no-ui-sounds --size 1440x900 --dev-input "${cfg}/input.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${P}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "FAIL: ${P}: gridline-app failed (${rc})")
    continue()
  endif()
  string(REGEX MATCH "MENUTREE BEGIN [^\n]*\n(.*)MENUTREE END" m "${out}")
  if(NOT m)
    message(SEND_ERROR "FAIL: ${P}: no menu tree")
    continue()
  endif()
  set(tree "${CMAKE_MATCH_1}")
  file(WRITE "${OUT}/menutree_${P}.txt" "${tree}")
  # Items, commands and order (shortcuts aside: each layout brings its own key set; states aside).
  string(REGEX REPLACE "\t[e-][c-]\n" "\n" plain "\n${tree}")
  string(REGEX REPLACE "\n([^\t\n]*)\t[^\t\n]*\t" "\n\\1\t\t" plain "${plain}")
  set(tree_${P} "${plain}")
  string(REPLACE " > " "/" slashTree "${tree}")
  if(RECORD)
    file(WRITE "${EXPECT}/menutree_${golden}.txt" "${tree}")
  elseif(NOT L STREQUAL "tracker")
    file(READ "${EXPECT}/menutree_${golden}.txt" want)
    # The file manager's name is the system's (GrooveUi revealText): one name for the comparison.
    string(REGEX REPLACE "Show in (Finder|Explorer|Files)\t" "Show in <file manager>\t" want "${want}")
    string(REGEX REPLACE "Show in (Finder|Explorer|Files)\t" "Show in <file manager>\t" got "${tree}")
    if(NOT got STREQUAL want)
      message(SEND_ERROR "FAIL: ${P}: the menu tree changed (diff ${EXPECT}/menutree_${golden}.txt ${OUT}/menutree_${P}.txt)")
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
        message(SEND_ERROR "FAIL: ${P}: palette row not in the menu tree: ${path}")
      endif()
    endif()
  endforeach()
  if(nrows LESS 50)
    message(SEND_ERROR "FAIL: ${P}: only ${nrows} menu rows in the palette")
  endif()
  message(STATUS "${P}: ${nrows} palette menu rows, ${missing} missing from the tree")
endforeach()
unset(ENV{GRIDLINE_DEV_KEYS_NO_CMD})

if(DEFINED tree_tracker AND DEFINED tree_studio AND NOT tree_tracker STREQUAL tree_studio)
  message(SEND_ERROR "FAIL: Tracker's menu bar is not Studio's")
endif()
