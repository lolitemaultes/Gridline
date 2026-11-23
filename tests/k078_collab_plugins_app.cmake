# Gridline: an installed plugin in a collaborative project, in two hidden app windows (each with its
# own config) over the app's real network path, driven by dev-input "collab" commands:
#   1. Alex knows the CLAP test synth, makes a project collaborative, invites Sam (who doesn't have it);
#   2. Alex adds a channel playing it: Sam's project has the channel (no instance: silent), Alex's
#      panel lists Sam as lacking it;
#   3. Alex turns its Gain and works in it (its state is written): Sam keeps the state untouched;
#   4. Sam installs it (a scan): it loads there with Alex's state and parameters.
# No plugin interface is opened (hidden runs, no editor command).
#   cmake -DAPP=<gridline-app> -DCLAP=<test bundle> -DOUT=<dir> -P k078_collab_plugins_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(SYNTH "clap:org.gridline.test.synth")

set(A "5 collab profile Alex
6 plugwin scan clap ${CLAP}
10 collab new \"${OUT}/Alex/Song\"
20 collab invite \"${OUT}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
40 collab plugin-add Lead ${SYNTH}
41 waitfor collab-synced 60
42 waitfor never 5
43 collab report \"${OUT}/a1.json\"
44 waitfor \"file:${OUT}/b1.json\" 120
50 collab plugin-param Lead 0 0.8
51 waitfor never 1
52 collab plugin-touch Lead
53 waitfor collab-synced 60
54 waitfor never 3
55 collab report \"${OUT}/a2.json\"
56 waitfor \"file:${OUT}/b2.json\" 150
60 quit discard
")
set(B "5 collab profile Sam
6 waitfor never 3
10 collab join \"${OUT}/invite.txt\" \"${OUT}/Sam/Song\"
11 waitfor collab-joined 120
40 waitfor collab-channel:Lead 120
41 waitfor never 5
42 collab report \"${OUT}/b1.json\"
43 waitfor \"file:${OUT}/a2.json\" 150
44 plugwin scan clap ${CLAP}
45 waitfor never 6
46 collab report \"${OUT}/b2.json\"
47 waitfor never 2
50 quit discard
")
file(WRITE "${OUT}/a.txt" "${A}")
file(WRITE "${OUT}/b.txt" "${B}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgA HOME=${OUT}/homeA "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/a.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${OUT}/cfgB HOME=${OUT}/homeB "${APP}" --hidden --no-ui-sounds --dev-input "${OUT}/b.txt"
  RESULTS_VARIABLE rcs OUTPUT_FILE "${OUT}/run.out" ERROR_FILE "${OUT}/run.err" TIMEOUT 600)
message(STATUS "exit codes: ${rcs}")

function(load name prefix)
  if(NOT EXISTS "${OUT}/${name}.json")
    message(FATAL_ERROR "FAIL: no report ${name}.json (exit codes ${rcs})")
  endif()
  file(READ "${OUT}/${name}.json" j)
  set(${prefix} "${j}" PARENT_SCOPE)
endfunction()
# must(<what> <condition...>): the condition as separate words, as if() takes them
function(must what)
  if(${ARGN})
    message(STATUS "ok: ${what}")
  else()
    message(FATAL_ERROR "FAIL: ${what} (${ARGN})")
  endif()
endfunction()

load(a1 A1)
load(b1 B1)
load(a2 A2)
load(b2 B2)
string(JSON b1Type GET "${B1}" pluginStates Lead type)
string(JSON b1Run GET "${B1}" pluginStates Lead running)
string(JSON b1Here GET "${B1}" plugins 0 here)
must("the channel with the plugin Sam lacks is in Sam's project" b1Type STREQUAL SYNTH)
must("it is silent there (not installed, no instance)" b1Run EQUAL 0 AND b1Here EQUAL 0)
string(JSON a1Lack GET "${A1}" plugins 0 lacking)
string(FIND "${a1Lack}" "Sam" at)
must("Alex sees that Sam lacks it" at GREATER -1)
string(JSON a2Data GET "${A2}" pluginStates Lead data)
string(JSON b2Data GET "${B2}" pluginStates Lead data)
string(JSON b2Run GET "${B2}" pluginStates Lead running)
string(JSON b2P0 GET "${B2}" pluginStates Lead p0)
must("Alex's touch wrote a state" NOT a2Data STREQUAL "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
must("Sam holds Alex's state exactly" b2Data STREQUAL a2Data)
must("once installed, it runs on Sam's computer" b2Run EQUAL 1)
if(b2P0 LESS 0.79 OR b2P0 GREATER 0.81)
  message(FATAL_ERROR "FAIL: Alex's Gain did not arrive (${b2P0})")
endif()
message(STATUS "ok: Alex's Gain arrived")
if(NOT rcs STREQUAL "0;0")
  message(FATAL_ERROR "FAIL: exit codes ${rcs}")
endif()
