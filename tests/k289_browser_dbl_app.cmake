# Gridline - K-289: a double-click on a folder in the browser, in the real app (hidden, isolated config
# and HOME, SDL's dummy audio driver, a folder of samples added to the browser), in the Studio,
# Clips and Smooth layouts:
#   1. a double-click on a closed folder opens it (the second click does not fold it back) and
#      selects no file;
#   2. a single click opens a folder, a click on a file in it selects the file, a click on the open
#      folder folds it.
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k289_browser_dbl_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/lib/sub" "${OUT}/hm")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/hm")
set(ENV{GRIDLINE_BROWSER_EXPAND} "lib")
get_filename_component(src "${DEMO}" DIRECTORY)
set(wav "${src}/../../assets/sounds/classic/open.wav")
foreach(n A sub/D sub/E)
  configure_file("${wav}" "${OUT}/lib/${n}.wav" COPYONLY)
endforeach()
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)  # the demo song is never opened in place
set(L "${OUT}/lib")

function(run tag layout script)  # ARGN: more options
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline")
  file(WRITE "${cfg}/gridline/browser-folders.txt" "${OUT}/lib\n")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900
                          --frames 60 --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.out" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# Where a row is drawn ("<rx> <ry>": a point inside it), from a "browser rows" listing.
function(row_at var path)
  string(REGEX MATCH "  row \"[^\"]*\" x ([0-9]+)[0-9.]* [0-9.]+ y ([0-9]+)[0-9.]* [0-9.]+ path ${path}\n" m "${out}")
  if(m STREQUAL "")
    message(FATAL_ERROR "FAIL: no row drawn for ${path}\nOutput:\n${out}")
  endif()
  math(EXPR x "${CMAKE_MATCH_1} + 60")
  math(EXPR y "${CMAKE_MATCH_2} + 10")
  set(${var} "${x} ${y}" PARENT_SCOPE)
endfunction()
# The selected file and whether the folder <path> (holding D.wav) is open, from "browser tree" at a frame.
function(tree_at frame path)
  string(REGEX MATCH "browsertree frame ${frame}: [^\n]* sel \"([^\"]*)\"" _ "${out}")
  set(t_sel "${CMAKE_MATCH_1}" PARENT_SCOPE)
  string(REGEX MATCH "browsertree frame ${frame}: [^\n]*\n(  treerow [^\n]*\n)*" block "${out}")
  # (folder rows carry no path: the folder is open when its files are drawn)
  if(block MATCHES "  treerow indent [0-9-]+ folder 0 open 0 path ${path}/D.wav\n")
    set(t_open 1 PARENT_SCOPE)
  else()
    set(t_open 0 PARENT_SCOPE)
  endif()
endfunction()

foreach(layout studio live logic)
  set(pane "")
  if(layout STREQUAL "logic")
    set(pane --pane browser)  # Smooth starts with its browser closed
  endif()
  if(layout STREQUAL "live")
    set(pick "8 browser cat 12")  # Clips: the Folders category
  else()
    set(pick "8 browser tab 3")   # Studio, Smooth: the Sounds tab
  endif()
  run(geo_${layout} ${layout} "6 browser rows on\n${pick}\n14 browser rows\n" ${pane})
  row_at(folder "${L}/sub")
  run(dbl_${layout} ${layout} "6 browser rows on\n${pick}\n12 move ${folder}\n14 down 0\n15 up 0\n16 down 0\n17 up 0\n22 browser tree\n24 browser rows\n" ${pane})
  tree_at(22 "${L}/sub")
  check("${layout}: a double-click opens the folder (open '${t_open}')" t_open STREQUAL "1")
  check("${layout}: ... and selects no file ('${t_sel}')" "x${t_sel}" STREQUAL "x")
  row_at(file "${L}/sub/D.wav")
  run(one_${layout} ${layout} "6 browser rows on\n${pick}\n12 move ${folder}\n14 down 0\n15 up 0\n22 browser tree\n26 move ${file}\n28 down 0\n29 up 0\n34 browser tree\n36 move ${folder}\n38 down 0\n39 up 0\n44 browser tree\n" ${pane})
  tree_at(22 "${L}/sub")
  check("${layout}: a click opens the folder" t_open STREQUAL "1")
  tree_at(34 "${L}/sub")
  check("${layout}: a click on a file in it selects the file ('${t_sel}')" t_sel STREQUAL "${L}/sub/D.wav")
  tree_at(44 "${L}/sub")
  check("${layout}: a click on the open folder folds it" t_open STREQUAL "0")
endforeach()
message(STATUS "k289: a double-click on a browser folder opens it and picks nothing")
