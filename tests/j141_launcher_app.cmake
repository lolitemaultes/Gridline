# Gridline - J-141: the welcome launcher in the running app (hidden, isolated config, real input).
#   1. Started with a project: no launcher. A click on the Gridline logo shows it with that
#      project open (its tile marked open); Esc returns to the project. From the launcher a
#      click on another recent opens it; with unsaved changes the click asks first and the
#      open project stays.
#   2. --show launcher with no project: the launcher with nothing open; Esc cannot leave it;
#      the new-project tile starts a project and the launcher gives way.
#   3. The keyboard. 4. (J-148) A tile's heart and menu buttons.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P j141_launcher_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline")
configure_file("${DEMO}" "${OUT}/Song A.gridline.json" COPYONLY)
configure_file("${DEMO}" "${OUT}/Song B.gridline.json" COPYONLY)
# Recent list: B, then A (A opens from the command line and moves to the front).
file(WRITE "${OUT}/cfg/gridline/recent.txt" "${OUT}/Song B.gridline.json\n${OUT}/Song A.gridline.json\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

# 1280 x 800 at 100 %: the logo is at (12..40, 10..38); the grid starts at (268, 92) with
# 312 px tiles 20 px apart (tile 1 new project, tile 2 the first recent, tile 3 the second).
set(logo "move 26 24")
set(tile3 "move 1088 185")
file(WRITE "${OUT}/a.txt"
  "6 launcher\n"
  "8 ${logo}\n9 down 0\n10 up 0\n"
  "14 launcher\n"
  "16 key escape\n"
  "20 launcher\n"
  "22 ${logo}\n23 down 0\n24 up 0\n"
  "28 ${tile3}\n29 down 0\n30 up 0\n"
  "36 launcher\n"
  "38 mknotes 4 96 48\n"
  "40 ${logo}\n41 down 0\n42 up 0\n"
  "46 ${tile3}\n47 down 0\n48 up 0\n"
  "52 launcher\n"
  "56 quit discard\n")
execute_process(COMMAND "${APP}" "${OUT}/Song A.gridline.json" --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1
                        --dev-input "${OUT}/a.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (1): gridline-app exited with ${rc}:\n${err}")
endif()

function(expectReports tag text)
  string(REGEX MATCHALL "launcher visible=[^\n]*" got "${text}")
  list(LENGTH got n)
  list(LENGTH ARGN want)
  if(NOT n EQUAL want)
    message(FATAL_ERROR "FAIL (${tag}): ${n} launcher reports, expected ${want}:\n${text}")
  endif()
  set(i 0)
  foreach(w IN LISTS ARGN)
    list(GET got ${i} line)
    string(REGEX MATCH "${w}" ok "${line}")
    if(NOT ok)
      message(FATAL_ERROR "FAIL (${tag}): report ${i} \"${line}\" does not match \"${w}\"\nOutput:\n${text}")
    endif()
    message(STATUS "ok - ${tag} report ${i}: ${w}")
    math(EXPR i "${i} + 1")
  endforeach()
endfunction()

expectReports(project "${out}"
  "visible=0 open=1 .* prompt=0 .*project=Song A"                  # started with a project: no launcher
  "visible=1 open=1 .*project=Song A"                              # the logo shows it, the project kept
  "visible=0 open=1 .*project=Song A"                              # Esc: back to the project
  "visible=0 open=1 .* prompt=0 modified=0 project=Song B"         # a recent tile opens that project
  "visible=1 open=1 .* prompt=1 modified=1 project=Song B")        # unsaved changes: asked first, nothing replaced
string(FIND "${out}" "tile kind=2 name=Song A open=1" openTile)
if(openTile LESS 0)
  message(FATAL_ERROR "FAIL (project): the open project's tile is not marked open:\n${out}")
endif()
message(STATUS "ok - the open project's tile is marked open")

# 2. The launcher with nothing open.
file(WRITE "${OUT}/b.txt"
  "6 launcher\n"
  "8 key escape\n"
  "12 launcher\n"
  "14 move 424 185\n15 down 0\n16 up 0\n"
  "20 launcher\n"
  "22 launcher home\n"
  "24 launcher close\n"
  "26 launcher\n"
  "28 quit discard\n")
execute_process(COMMAND "${APP}" --show launcher --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1
                        --dev-input "${OUT}/b.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (2): gridline-app exited with ${rc}:\n${err}")
endif()
expectReports(start "${out}"
  "visible=1 open=0 .*title=Gridline$"        # nothing open: the window is just "Gridline"
  "visible=1 open=0"                          # Esc cannot leave without a project
  "visible=0 open=1 .*modified=0"             # the new-project tile starts one
  "visible=1 open=0")                         # Close project: the launcher, nothing open
# 3. The keyboard: arrows move the focus (the first press shows it), Delete takes a project off
#    the list, Enter opens the focused tile.
file(WRITE "${OUT}/c.txt"
  "6 key right\n10 key right\n"   # (keys a few frames apart: ImGui hands one press per frame)
  "14 key delete\n"
  "18 launcher\n"
  "20 key enter\n"
  "26 launcher\n"
  "28 quit discard\n")
execute_process(COMMAND "${APP}" --show launcher --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1
                        --dev-input "${OUT}/c.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (3): gridline-app exited with ${rc}:\n${err}")
endif()
expectReports(keys "${out}"
  "visible=1 open=0 view=0 tiles=2 "          # Song B left the list (New project + Song A)
  "visible=0 open=1 .*project=Song A")        # Enter opened the focused tile
file(READ "${OUT}/cfg/gridline/recent.txt" recent)
string(FIND "${recent}" "Song B" stillB)
if(NOT stillB LESS 0)
  message(FATAL_ERROR "FAIL (keys): the removed project is still in recent.txt:\n${recent}")
endif()
message(STATUS "ok - removal saved to recent.txt")
# 4. J-148: a tile's hover buttons. The heart marks Song B a favourite (badge, saved to
#    favourite-projects.txt); the menu button opens the tile's menu (it once did nothing: the
#    button vanished on the frame the mouse was released). Tile 2 spans x 600..912 from y 92:
#    the menu button is centred at (890, 114), the heart 32 px to its left.
file(WRITE "${OUT}/cfg/gridline/recent.txt" "${OUT}/Song B.gridline.json\n${OUT}/Song A.gridline.json\n")
file(REMOVE "${OUT}/cfg/gridline/favourite-projects.txt")
file(WRITE "${OUT}/d.txt"
  "6 move 700 200\n8 move 858 114\n12 down 0\n14 up 0\n"
  "18 launcher\n"
  "20 move 700 200\n22 move 890 114\n26 down 0\n28 up 0\n"
  "32 launcher\n"
  "34 quit discard\n")
execute_process(COMMAND "${APP}" --show launcher --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1
                        --dev-input "${OUT}/d.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (4): gridline-app exited with ${rc}:\n${err}")
endif()
expectReports(tilebuttons "${out}" "visible=1 open=0" "visible=1 open=0")
string(REGEX MATCHALL "launcher menu=[^\n]*" menus "${out}")
if(NOT menus STREQUAL "launcher menu=-;launcher menu=Song B")
  message(FATAL_ERROR "FAIL (menu): expected no menu after the heart click, then Song B's menu; got \"${menus}\"")
endif()
if(NOT out MATCHES "name=Song B [^\n]*fav=1" OR out MATCHES "name=Song A [^\n]*fav=1")
  message(FATAL_ERROR "FAIL (favourite): Song B is not the one favourite:\n${out}")
endif()
file(READ "${OUT}/cfg/gridline/favourite-projects.txt" favs)
if(NOT favs STREQUAL "${OUT}/Song B.gridline.json\n")
  message(FATAL_ERROR "FAIL (favourite): favourite-projects.txt is \"${favs}\"")
endif()
message(STATUS "ok - tile heart and menu buttons")
message(STATUS "PASS")
