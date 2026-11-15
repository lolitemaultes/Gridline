# Gridline - K-112c: the Delete key in the Live layout's browser, in the real app (hidden, isolated
# config and HOME, a folder of samples added to the browser, a trash folder of the test's own):
#   1. Live: Delete on a file row asks "Move to the trash?" for that file, and the dialog's action
#      moves it there;
#   2. Live: Delete on a folder row asks about that folder;
#   3. Studio: Delete in the browser does nothing (its browser keys are unchanged).
#   cmake -DAPP=<gridline-app> -DDEMO=<demo.gridline.json> -DOUT=<dir> -P k112_browser_delete_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/lib/sub" "${OUT}/hm" "${OUT}/trash")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{HOME} "${OUT}/hm")
set(ENV{GRIDLINE_TRASH_DIR} "${OUT}/trash")  # never the real trash from a test
set(ENV{GRIDLINE_BROWSER_EXPAND} "lib")
get_filename_component(src "${DEMO}" DIRECTORY)
set(wav "${src}/../../assets/sounds/classic/open.wav")
foreach(n A B C sub/D)
  configure_file("${wav}" "${OUT}/lib/${n}.wav" COPYONLY)
endforeach()
configure_file("${DEMO}" "${OUT}/fixture.gridline.json" COPYONLY)  # the demo song is never opened in place
set(L "${OUT}/lib")

function(run tag layout script)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline")
  file(WRITE "${cfg}/gridline/browser-folders.txt" "${OUT}/lib\n")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/fixture.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900
                          --frames 60 --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt"
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
# The trash dialog's subject at a frame, from "browser state".
function(trash_at var frame)
  string(REGEX MATCH "browserstate frame ${frame}: [^\n]* trash \"([^\"]*)\"" _ "${out}")
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

foreach(layout live studio)
  configure_file("${wav}" "${L}/A.wav" COPYONLY)
  if(layout STREQUAL "live")
    set(pick "8 browser cat 12")  # Live: the Folders category
  else()
    set(pick "8 browser tab 3")   # Studio: the Sounds tab
  endif()
  # ---- where the rows are drawn in this layout
  run(geo_${layout} ${layout} "6 browser rows on\n${pick}\n14 browser rows\n")
  row_at(file "${L}/A.wav")
  row_at(folder "${L}/sub")

  # ---- Delete on a file row, then the dialog's action
  set(ok "")
  if(layout STREQUAL "live")
    set(ok "26 browser trashok\n")
  endif()
  run(file_${layout} ${layout} "6 browser rows on\n${pick}\n12 move ${file}\n14 down 0\n15 up 0\n20 key delete\n24 browser state\n${ok}")
  trash_at(asked 24)
  if(layout STREQUAL "live")
    check("live: Delete on a file asks about it (${asked})" asked STREQUAL "${L}/A.wav")
    check("live: the dialog's action moved it" NOT EXISTS "${L}/A.wav")
    file(GLOB_RECURSE trashed "${OUT}/trash/*A.wav")
    list(LENGTH trashed n)
    check("... into the test's trash" n EQUAL 1)
  else()
    check("studio: Delete in the browser asks nothing (${asked})" NOT asked)
    check("studio: the file stays" EXISTS "${L}/A.wav")
  endif()

  # ---- Delete on a folder row
  run(folder_${layout} ${layout} "6 browser rows on\n${pick}\n12 move ${folder}\n14 down 0\n15 up 0\n20 key delete\n24 browser state\n")
  trash_at(asked 24)
  if(layout STREQUAL "live")
    check("live: Delete on a folder asks about it (${asked})" asked STREQUAL "${L}/sub")
  else()
    check("studio: Delete on a folder asks nothing (${asked})" NOT asked)
  endif()
  check("the folder stays until the dialog's action" EXISTS "${L}/sub/D.wav")
endforeach()
