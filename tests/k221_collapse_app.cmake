# Gridline - Collapse all folders in the browser, in the real app (hidden, off-screen, isolated config):
#   1. nested folders of a browser folder are open (four levels) with a file in the deepest one
#      selected; a click on the Collapse all button (beside the plugin button) folds every folder
#      and section; the selection moves to the file's top folder (its section) and the file is no
#      longer selected;
#   2. with a search typed, the button folds the result sections and the search text stays; with
#      the search cleared the tree is still all folded.
#   cmake -DAPP=<gridline-app> -DSRC=<source dir> -DOUT=<dir> -P k221_collapse_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/lib/Drums/Kicks/Deep")
configure_file("${SRC}/assets/sounds/classic/open.wav" "${OUT}/lib/Drums/Kicks/Deep/kick-deep.wav" COPYONLY)
configure_file("${SRC}/assets/sounds/classic/save.wav" "${OUT}/lib/Drums/snare-tight.wav" COPYONLY)
set(file "${OUT}/lib/Drums/Kicks/Deep/kick-deep.wav")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{SDL_AUDIODRIVER} "dummy")  # never the real speakers
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
set(ENV{GRIDLINE_BROWSER_EXPAND} "lib,Drums,Kicks,Deep")

function(run tag script)
  file(REMOVE_RECURSE "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${OUT}/${tag}/cfg" "${OUT}/${tag}/home")
  file(WRITE "${OUT}/${tag}/cfg/gridline/browser-folders.txt" "${OUT}/lib\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/${tag}/cfg")
  set(ENV{HOME} "${OUT}/${tag}/home")
  file(WRITE "${OUT}/${tag}/in.txt" "${script}")
  execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --no-update-check --fixed-dt 16 --size 1440x900 --frames 90
                          --dev-input "${OUT}/${tag}/in.txt" --screenshot "${OUT}/${tag}/shot.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 180)
  file(WRITE "${OUT}/${tag}/out.txt" "${out}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${tag}: gridline-app failed (${rc}):\n${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(tree var frame)
  string(REGEX MATCH "browsertree frame ${frame}: [^\n]*" t "${out}")
  if(t STREQUAL "")
    message(FATAL_ERROR "FAIL: no browser tree at frame ${frame}:\n${out}")
  endif()
  set(${var} "${t}" PARENT_SCOPE)
endfunction()

# ---- 1. nested folders, a file selected, the button
run(nested "5 browser tab 3\n20 browser select \"${file}\"\n30 browser tree\n40 sdlat brcollapse\n42 sdldown 0\n44 sdlup 0\n50 browser tree\n52 shot \"${OUT}/collapsed.ppm\"\n")
tree(t30 30)
string(REGEX MATCH "folders ([0-9]+) open ([0-9]+)" _ "${t30}")
if(CMAKE_MATCH_2 LESS 4)
  message(FATAL_ERROR "FAIL: the nested folders are not open at first: ${t30}\n${out}")
endif()
if(NOT out MATCHES "treerow indent [0-9]+ folder 0 open 0 path [^\n]*kick-deep.wav")
  message(FATAL_ERROR "FAIL: the file in the deepest folder is not shown at first:\n${out}")
endif()
message(STATUS "ok - four nested folders open: ${t30}")
if(NOT out MATCHES "sdlat frame 40: brcollapse at")
  message(FATAL_ERROR "FAIL: no Collapse all button:\n${out}")
endif()
tree(t50 50)
if(NOT t50 MATCHES "open 0 ")
  message(FATAL_ERROR "FAIL: the button left folders open: ${t50}\n${out}")
endif()
if(out MATCHES "browsertree frame 50:[^\n]*\n(  treerow [^\n]*\n)*  treerow indent [0-9]+ ")
  message(FATAL_ERROR "FAIL: rows inside folders are still shown after Collapse all:\n${out}")
endif()
if(NOT t50 MATCHES "cursor-indent -1 sel \"\"")
  message(FATAL_ERROR "FAIL: the hidden file's selection did not move to its top folder: ${t50}")
endif()
message(STATUS "ok - Collapse all folded every folder and section; the selection moved to the top folder: ${t50}")

# ---- 2. a search stays; the tree stays folded once it is cleared
run(search "5 browser tab 3\n30 browser search kick\n36 browser tree\n40 sdlat brcollapse\n42 sdldown 0\n44 sdlup 0\n50 browser tree\n60 browser search\n70 browser tree\n")
tree(t36 36)
tree(t50 50)
tree(t70 70)
if(NOT t50 MATCHES "search \"kick\"")
  message(FATAL_ERROR "FAIL: the search text was not kept: ${t50}")
endif()
if(NOT t50 MATCHES "open 0 ")
  message(FATAL_ERROR "FAIL: the result sections were not folded: ${t36} -> ${t50}")
endif()
if(NOT t70 MATCHES "open 0 ")
  message(FATAL_ERROR "FAIL: after the search was cleared folders are open: ${t70}")
endif()
message(STATUS "ok - the search text stays and every section is folded: ${t50}; cleared: ${t70}")
