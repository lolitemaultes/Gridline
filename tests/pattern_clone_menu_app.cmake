# Gridline: cloning a pattern from its menu in the playlist's pattern list, many times in a row (a
# crash report: Gridline closed right after a pattern was cloned). The list draws that menu inside its
# loop over the song's patterns, so a clone made there and then would add to the list it is walking.
# Runs the app hidden (isolated config and HOME, fixed frame time, dummy audio) on a copy of the test
# song: right-click the first pattern, click Clone, 40 times; the app stays up (ASan builds catch a
# stale reference), every clone lands, and undo takes them all back.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DOUT=<dir> -P pattern_clone_menu_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)  # a copy: never the fixture itself

function(run tag script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --hide rack
                          --dev-input "${OUT}/${tag}.txt"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}\n${out}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
endfunction()

function(int out v)
  string(REGEX REPLACE "\\..*" "" r "${v}")
  set(${out} ${r} PARENT_SCOPE)
endfunction()

# 1. Where the first pattern of the list is, and how many patterns there are.
run(geo "6 plgeom\n8 quit discard\n")
if(NOT geo_out MATCHES "patterns ([0-9]+) current")
  message(FATAL_ERROR "FAIL: no playlist geometry\n${geo_out}")
endif()
set(n0 ${CMAKE_MATCH_1})
if(NOT geo_out MATCHES "\n  row ([0-9]+) x ([-0-9.]+) y ([-0-9.]+)")
  message(FATAL_ERROR "FAIL: no pattern rows in the list\n${geo_out}")
endif()
int(rx "${CMAKE_MATCH_2}")
int(ry "${CMAKE_MATCH_3}")

# 2. Where its menu draws Clone.
run(menu "6 move ${rx} ${ry}\n8 down 1\n9 up 1\n14 plgeom\n16 quit discard\n")
if(NOT menu_out MATCHES "patmenu clone ([-0-9.]+) ([-0-9.]+)" OR CMAKE_MATCH_1 STREQUAL "0.0")
  message(FATAL_ERROR "FAIL: the pattern menu did not open\n${menu_out}")
endif()
int(cx "${CMAKE_MATCH_1}")
int(cy "${CMAKE_MATCH_2}")

# 3. Forty clones from the menu, then forty undos.
set(clones 40)
set(script "")
set(f 6)
foreach(i RANGE 1 ${clones})
  math(EXPR f1 "${f} + 1")
  math(EXPR f2 "${f} + 2")
  math(EXPR f3 "${f} + 3")
  math(EXPR f4 "${f} + 7")
  math(EXPR f5 "${f} + 8")
  math(EXPR f6 "${f} + 9")
  string(APPEND script "${f} move ${rx} ${ry}\n${f1} down 1\n${f2} up 1\n${f4} move ${cx} ${cy}\n${f5} down 0\n${f6} up 0\n")
  math(EXPR f "${f} + 14")
endforeach()
string(APPEND script "${f} plgeom\n")
foreach(i RANGE 1 ${clones})
  math(EXPR f "${f} + 2")
  string(APPEND script "${f} undo\n")
endforeach()
math(EXPR f "${f} + 4")
math(EXPR fq "${f} + 2")
string(APPEND script "${f} plgeom\n${fq} quit discard\n")
run(clones "${script}")
string(REGEX MATCHALL "patterns [0-9]+ current" counts "${clones_out}")
list(LENGTH counts nc)
if(NOT nc EQUAL 2)
  message(FATAL_ERROR "FAIL: want two pattern counts, got ${nc}\n${clones_out}")
endif()
list(GET counts 0 c1)
list(GET counts 1 c2)
math(EXPR want "${n0} + ${clones}")
if(NOT c1 STREQUAL "patterns ${want} current")
  message(FATAL_ERROR "FAIL: after ${clones} clones from the menu: ${c1}, wanted ${want}\n${clones_out}")
endif()
if(NOT c2 STREQUAL "patterns ${n0} current")
  message(FATAL_ERROR "FAIL: after ${clones} undos: ${c2}, wanted ${n0}\n${clones_out}")
endif()
message(STATUS "ok - ${clones} clones from the pattern list's menu, all undone, no crash")
