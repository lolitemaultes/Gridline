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
  # Clips shows its Ctrl shortcuts as Cmd on macOS (the key pressed there; Cmd and Ctrl are one
  # modifier in that set): the golden, the same on every system, keeps Ctrl.
  if(CMAKE_HOST_APPLE AND P STREQUAL "live")
    string(REPLACE "\tCmd+" "\tCtrl+" tree "${tree}")
  endif()
  file(WRITE "${OUT}/menutree_${P}.txt" "${tree}")
  # Items, commands and order (shortcuts aside: each layout brings its own key set; states aside).
  string(REGEX REPLACE "\t[e-][c-]\n" "\n" plain "\n${tree}")
  string(REGEX REPLACE "\n([^\t\n]*)\t[^\t\n]*\t" "\n\\1\t\t" plain "${plain}")
  set(tree_${P} "${plain}")
  set(raw_${P} "${tree}")
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

# K-106: Tracker has its own bar (File Edit View Pattern Song Options Tools Help); View starts
# with the pages, named as the header's page strip names them, with the Tracker set's keys.
if(DEFINED tree_tracker)
  set(roots "")
  string(REGEX MATCHALL "\n[^\n>\t]+ >" heads "${tree_tracker}")
  foreach(h IN LISTS heads)
    string(REGEX REPLACE "^\n(.*) >$" "\\1" h "${h}")
    list(FIND roots "${h}" seen)
    if(seen EQUAL -1)
      list(APPEND roots "${h}")
    endif()
  endforeach()
  string(REPLACE ";" " " roots "${roots}")
  if(NOT roots STREQUAL "File Edit View Pattern Song Options Tools Help")
    message(SEND_ERROR "FAIL: Tracker's menu bar is ${roots}")
  endif()
  foreach(p "View > Pattern\t" "View > Song\t" "View > Roll\t" "View > Steps\t" "View > Mix\t" "View > Pads\t" "View > Tracker help\t")
    string(FIND "${tree_tracker}" "${p}" at)
    if(at EQUAL -1)
      message(SEND_ERROR "FAIL: Tracker's View menu lacks ${p}")
    endif()
  endforeach()
  # K-106 checkpoint 04: one name per command. The pages are Song, Roll, Steps, Mix and Pads, so the
  # windows and panels submenu does not offer them again under Studio's names, and Undo history is
  # in Edit only.
  foreach(p "View > Windows and panels > Playlist\t" "View > Windows and panels > Channel rack\t" "View > Windows and panels > Piano roll\t"
            "View > Windows and panels > Mixer\t" "View > Windows and panels > Pads\t" "View > Windows and panels > Undo history\t"
            "View > Windows and panels > Channel settings\t" "View > Windows and panels > Panels > Pattern editor\t"
            "View > Windows and panels > Panels > Order list\t" "View > Windows and panels > Panels > Files\t")
    string(FIND "${tree_tracker}" "${p}" at)
    if(NOT at EQUAL -1)
      message(SEND_ERROR "FAIL: Tracker's View menu shows a page twice: ${p}")
    endif()
  endforeach()
  # Every command id appears once in the whole bar.
  string(REGEX MATCHALL "\t[a-zA-Z]+\\.[a-zA-Z0-9.]+\t" ids "${raw_tracker}")
  set(seenIds "")
  foreach(i IN LISTS ids)
    string(STRIP "${i}" i)
    list(FIND seenIds "${i}" at)
    if(NOT at EQUAL -1 AND NOT i MATCHES "^(edit\\.rename|layout\\.)")
      message(SEND_ERROR "FAIL: Tracker's menus show ${i} twice")
    endif()
    list(APPEND seenIds "${i}")
  endforeach()
  foreach(p "Edit > Insert row\t" "Edit > Delete row\t" "Edit > Insert row in every track\t" "Edit > Interpolate volume\t" "Edit > Cut\tAlt+F3"
            "Edit > Copy\tAlt+F4" "Edit > Paste\tAlt+F5" "Pattern > Edit mode\tEsc" "Pattern > Edit step > 16\t" "Pattern > Follow playback in the pattern\t"
            "Pattern > Columns > One more note column\t" "Pattern > Columns > Effect\t" "Pattern > Row numbers > Decimal\t")
    string(FIND "${raw_tracker}" "${p}" at)
    if(at EQUAL -1)
      message(SEND_ERROR "FAIL: Tracker's menus lack ${p}")
    endif()
  endforeach()
endif()

# K-314 (C-04, C-05): the record options are one submenu per menu bar (no palette row twice), and
# the Tracker's Options > Recording and metronome has the Metronome.
foreach(P studio live logic tracker)
  if(NOT DEFINED raw_${P})
    continue()
  endif()
  string(REGEX MATCHALL "(^|\n)[^\t\n]* > Recording options > " homes "${raw_${P}}")
  set(seen "")
  foreach(h IN LISTS homes)
    string(STRIP "${h}" h)
    list(FIND seen "${h}" at)
    if(at EQUAL -1)
      list(APPEND seen "${h}")
    endif()
  endforeach()
  list(LENGTH seen n)
  if(NOT n EQUAL 1)
    message(SEND_ERROR "FAIL: ${P}: Recording options is in ${n} places (${seen})")
  endif()
endforeach()
if(DEFINED raw_tracker)
  string(FIND "${raw_tracker}" "\nOptions > Recording and metronome > Metronome\t" at)
  if(at EQUAL -1)
    message(SEND_ERROR "FAIL: Tracker's Options > Recording and metronome lacks the Metronome")
  endif()
endif()
