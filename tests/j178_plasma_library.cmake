# Gridline — J-178: Plasma's preset library in the app (hidden, off-screen, isolated config).
# A synthetic Serum library (GL_PLASMA_SERUM_ROOTS replaces the system's folders) is browsed
# with real SDL clicks:
#  - a bank folder in the tree: the list shows its presets and its sub-folders', the folder
#    is selected and opens (its sub-folders appear);
#  - a sub-folder below it, "All Serum presets", "User presets" (empty) and "Factory";
#  - a preset row: one click selects it, a double click opens it (factory preset).
# The browser reports itself through the dev points "plasma.browser" (pt 0: x = presets
# listed, v = selected row, k = selected folder: -2 factory, -1 user, -3 all, else its tree
# index; pt 1: x = presets opened), "plasma.folders" (one point per folder row, k as above)
# and "plasma.presets" (one point per visible preset row, k = its row).
#   cmake -DAPP=<gridline-app> -DOUT=<dir> -P j178_plasma_library.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg" "${OUT}/home")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GL_PLASMA_VIEW} "browser")

# The library (names with spaces): tree 0 root, 1 Bank A, 2 Bass, 3 Lead, 4 Mono, 5 Bank B, 6 Pads.
set(lib "${OUT}/Synth Library")
set(ENV{GL_PLASMA_SERUM_ROOTS} "${lib}")
foreach(f "Bank A/Bass/One" "Bank A/Bass/Two" "Bank A/Lead/Three" "Bank A/Lead/Four" "Bank A/Lead/Five"
          "Bank A/Lead/Mono/Six" "Bank B/Pads/P1" "Bank B/Pads/P2" "Bank B/Pads/P3" "Bank B/Pads/P4")
  file(WRITE "${lib}/Presets/${f}.SerumPreset" "synthetic")
endforeach()
file(WRITE "${lib}/Presets/Loose.fxp" "synthetic")

# Folder rows (before / after Bank A opens): 0 Factory, 1 User, 2 All, 3 root, 4 Bank A, then
# (open) 5 Bass, 6 Lead, 7 Bank B.
set(script "3 waitfor plasma-library-ready 60
5 toasts clear
6 pts plasma.browser
8 sdlpt plasma.folders pt 4
10 sdldown 0
12 sdlup 0
16 pts plasma.browser
17 pts plasma.folders
18 sdlpt plasma.folders pt 6
20 sdldown 0
22 sdlup 0
26 pts plasma.browser
28 sdlpt plasma.folders pt 2
30 sdldown 0
32 sdlup 0
36 pts plasma.browser
38 sdlpt plasma.folders pt 1
40 sdldown 0
42 sdlup 0
46 pts plasma.browser
48 sdlpt plasma.folders pt 0
50 sdldown 0
52 sdlup 0
56 pts plasma.browser
58 sdlpt plasma.presets pt 2
60 sdldown 0
62 sdlup 0
66 pts plasma.browser
68 sdldown 0
70 sdlup 0
76 pts plasma.browser
")
file(WRITE "${OUT}/script.txt" "${script}")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --plugin gl.plasma --size 1600x1000 --frames 82
                        --screenshot "${OUT}/browser.ppm" --dev-input "${OUT}/script.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gridline-app failed (${rc}): ${err}")
endif()
if(NOT out MATCHES "waitfor plasma-library-ready: reached")
  message(FATAL_ERROR "FAIL: the library was not indexed. Output:\n${out}")
endif()
if(out MATCHES "not shown")
  message(FATAL_ERROR "FAIL: the browser is not on screen. Output:\n${out}")
endif()

# pt 0 (count, selected row, folder) of "plasma.browser" at frame `fr`; pt 1's x (presets opened).
function(state_at fr)
  string(REGEX MATCH "pts frame ${fr}: plasma\\.browser [^\n]*\n  pt 0 x ([-0-9.]+) v ([-0-9.]+) k ([-0-9.]+)\n  pt 1 x ([-0-9.]+)" m "${out}")
  if(NOT m)
    message(FATAL_ERROR "FAIL: no browser report at frame ${fr}. Output:\n${out}")
  endif()
  set(count "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(row "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(folder "${CMAKE_MATCH_3}" PARENT_SCOPE)
  set(opened "${CMAKE_MATCH_4}" PARENT_SCOPE)
endfunction()
function(expect_state fr wantCount wantRow wantFolder wantOpened what)
  state_at(${fr})
  if(NOT count STREQUAL "${wantCount}.0000" OR NOT row STREQUAL "${wantRow}.0000" OR NOT folder STREQUAL "${wantFolder}.0000"
     OR NOT opened STREQUAL "${wantOpened}.0000")
    message(FATAL_ERROR "FAIL (${what}): listed ${count} row ${row} folder ${folder} opened ${opened}, wanted ${wantCount} ${wantRow} "
                        "${wantFolder} ${wantOpened}. Output:\n${out}")
  endif()
  message(STATUS "ok - ${what}")
endfunction()

state_at(6)
set(factoryCount "${count}")
if(NOT folder STREQUAL "-2.0000" OR count LESS 2)
  message(FATAL_ERROR "FAIL: the browser does not start on the factory presets (${count}, ${folder}). Output:\n${out}")
endif()
# Bank A opened: its rows include Bass and Lead below it
string(REGEX MATCH "pts frame 17: plasma\\.folders [^\n]*\n(  pt [^\n]*\n)*  pt 5 x [-0-9.]+ v [-0-9.]+ k 2\\.0000\n  pt 6 x [-0-9.]+ v [-0-9.]+ k 3\\.0000" m "${out}")
expect_state(16 6 -1 1 0 "a bank folder lists its presets and its sub-folders' (6)")
if(NOT m)
  message(FATAL_ERROR "FAIL: the clicked folder did not open (Bass, Lead below it). Output:\n${out}")
endif()
message(STATUS "ok - the clicked folder opens: its sub-folders are listed in the tree")
expect_state(26 4 -1 3 0 "a sub-folder (Lead: 3 + Mono 1)")
expect_state(36 11 -1 -3 0 "All Serum presets (11)")
expect_state(46 0 -1 -1 0 "User presets (none saved)")
string(REGEX REPLACE "\\.0000$" "" factoryCount "${factoryCount}")
expect_state(56 ${factoryCount} -1 -2 0 "back to Factory")
expect_state(66 ${factoryCount} 2 -2 0 "one click selects a preset row")
expect_state(76 ${factoryCount} 2 -2 1 "a double click opens it")
