# Gridline - J-146: the welcome screen's Collaborative view in the running app (hidden,
# isolated config, real input).
#   1. --show launcher: the rail's Collaborative row opens the view (nothing known yet: the
#      empty state, the Join / New tiles, the settings inline); the Join tile opens the Join
#      window; Tab cycles the views.
#   2. --show launcher-collab-demo: collaborative projects with simulated collaborators in the
#      same process (in-memory network) who answer presence probes: every project is listed
#      with its members, my role, what needs attention and who is working on it now.
#   3. A project tile's buttons: the hover menu button opens the tile menu; the heart marks the
#      project a favourite (badge), the badge takes the mark off again.
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j146_launcher_collab_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")

function(expectLines tag text pattern)
  string(REGEX MATCH "${pattern}" ok "${text}")
  if(NOT ok)
    message(FATAL_ERROR "FAIL (${tag}): no line matches \"${pattern}\"\nOutput:\n${text}")
  endif()
  message(STATUS "ok - ${tag}: ${pattern}")
endfunction()

# 1280 x 800 at 100 %: the rail's Collaborative row is at (116, 321); the grid starts at
# (268, 92) under the "Start" label (34 px): the Join tile is the first tile.
file(WRITE "${OUT}/a.txt"
  "6 launcher\n"
  "8 move 116 321\n9 down 0\n10 up 0\n"
  "14 launcher\n"
  "16 key tab\n"
  "20 launcher\n"
  "22 key tab\n26 key tab\n"
  "30 launcher\n"
  "32 move 400 200\n33 down 0\n34 up 0\n"
  "38 launcher\n"
  "40 quit discard\n")
execute_process(COMMAND "${APP}" --show launcher --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1
                        --dev-input "${OUT}/a.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (1): gridline-app exited with ${rc}:\n${err}")
endif()
string(REGEX MATCHALL "launcher visible=[^\n]*" reports "${out}")
list(LENGTH reports n)
if(NOT n EQUAL 5)
  message(FATAL_ERROR "FAIL (1): ${n} launcher reports, expected 5:\n${out}")
endif()
set(want "view=0" "view=2" "view=0" "view=2" "view=2")
foreach(i RANGE 4)
  list(GET reports ${i} r)
  list(GET want ${i} w)
  expectLines("view ${i}" "${r}" "${w}")      # the rail opens the view; Tab cycles Recent, Templates, Collaborative
endforeach()
expectLines(empty "${out}" "collab view rows=0 [^\n]*entries=7 join=0")   # Start (2 tiles), Projects (empty), Settings
expectLines(join "${out}" "collab view rows=0 [^\n]*join=1")             # the Join tile opened the Join window

# 2. Simulated collaborators: presence answers come in while the view shows.
file(WRITE "${OUT}/b.txt"
  "900 launcher\n"
  "902 quit discard\n")
execute_process(COMMAND "${APP}" --show launcher-collab-demo --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1
                        --dev-input "${OUT}/b.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (2): gridline-app exited with ${rc}:\n${err}")
endif()
expectLines(demo "${out}" "launcher visible=1 open=0 view=2")
expectLines(demo "${out}" "collab view rows=4 live=2 ")
expectLines(demo "${out}" "collab row name=Night Drive state=0 admin=1 open=0 card=1 [^\n]*working=1 [^\n]*Sam:working\\*")
expectLines(demo "${out}" "collab row name=Glass Choir state=0 admin=0 [^\n]*working=1 [^\n]*Alex:working\\*")
expectLines(hidden "${out}" "collab row name=Glass Choir [^\n]*Lee:(unknown|checking|no-answer)[,\n ]")  # Lee keeps presence private
expectLines(review "${out}" "collab row name=Tape Loops state=0 admin=1 [^\n]*pending=5 review=2 [^\n]*status=2 changes to review")
expectLines(missing "${out}" "collab row name=Old Sketches state=3 [^\n]*status=Folder not found")
expectLines(nofav "${out}" "collab fav name=Night Drive fav=0 tile=")

# 3. Night Drive's tile buttons, placed from its rectangle in the report above: the menu button
# 22 px in from the top right corner, the heart 32 px left of it, the badge 22 px in from the
# top left corner.
string(REGEX MATCH "collab fav name=Night Drive fav=0 tile=([0-9]+),([0-9]+),([0-9]+),([0-9]+)" m "${out}")
if(NOT m)
  message(FATAL_ERROR "FAIL (3): no tile rectangle for Night Drive:\n${out}")
endif()
set(x0 ${CMAKE_MATCH_1})
set(y0 ${CMAKE_MATCH_2})
set(x1 ${CMAKE_MATCH_3})
set(y1 ${CMAKE_MATCH_4})
math(EXPR cx "(${x0} + ${x1}) / 2")
math(EXPR cy "(${y0} + ${y1}) / 2")
math(EXPR mx "${x1} - 22")
math(EXPR fx "${x1} - 54")
math(EXPR by "${y0} + 22")
math(EXPR bx "${x0} + 22")
math(EXPR gy "${y0} - 10")  # above the tile (the section row): a click there closes the menu
file(WRITE "${OUT}/c.txt"
  "880 move ${cx} ${cy}\n884 move ${mx} ${by}\n888 down 0\n890 up 0\n"
  "896 launcher\n"
  "898 move ${bx} ${gy}\n899 down 0\n900 up 0\n"
  "904 move ${cx} ${cy}\n908 move ${fx} ${by}\n912 down 0\n914 up 0\n"
  "920 launcher\n"
  "924 move ${cx} ${cy}\n928 move ${bx} ${by}\n932 down 0\n934 up 0\n"
  "940 launcher\n"
  "942 quit discard\n")
execute_process(COMMAND "${APP}" --show launcher-collab-demo --hidden --no-ui-sounds --no-update-check --size 1280x800 --ui-scale 1
                        --dev-input "${OUT}/c.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "FAIL (3): gridline-app exited with ${rc}:\n${err}")
endif()
string(REGEX MATCHALL "collab menu=[^\n]*" menus "${out}")
string(REGEX MATCHALL "collab fav name=Night Drive fav=[01]" favs "${out}")
if(NOT menus STREQUAL "collab menu=Night Drive;collab menu=-;collab menu=-")
  message(FATAL_ERROR "FAIL (3): the menu button: ${menus}\nOutput:\n${out}")
endif()
if(NOT favs STREQUAL "collab fav name=Night Drive fav=0;collab fav name=Night Drive fav=1;collab fav name=Night Drive fav=0")
  message(FATAL_ERROR "FAIL (3): the heart and the badge: ${favs}\nOutput:\n${out}")
endif()
message(STATUS "ok - 3: menu button, heart, badge")
message(STATUS "PASS")
