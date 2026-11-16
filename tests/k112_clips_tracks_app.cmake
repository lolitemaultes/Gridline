# Gridline - K-112c: the Clips layout's arrangement and clip grid list the same tracks.
#   The fixture has 32 playlist tracks with three in use. In Clips (as a development layout):
#   1. the arrangement's rows ("plrows") and the clip grid's columns ("livegrid" titles) are the
#      same names in the same order, fewer than the song's 32 tracks;
#   2. dragging the first column's title past the second in the grid moves the track: the
#      arrangement then lists the second track first;
#   3. dragging the first arrangement header below the second moves it: the grid then lists the
#      second track first.
#   (Adds, removals, renames and a collaborator's edits: test_k112_clips_tracks.cpp.)
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DOUT=<dir> -P k112_clips_tracks_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")

# run(<tag> <dev input> args...): one run in its own config folder; sets <tag>_log.
function(run tag input)
  set(cfg "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"clips.arrangementSets\": \"${OUT}/${tag}.gridline.json\"}}\n")
  # (N45-10: the song was last saved in the arrangement, so Clips opens it there, as these runs expect.)
  file(WRITE "${cfg}/input.txt" "5 toasts clear\n${input}")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  configure_file("${DEMO}" "${OUT}/${tag}.gridline.json" COPYONLY)
  execute_process(COMMAND "${APP}" "${OUT}/${tag}.gridline.json" --hidden --no-ui-sounds --size 1440x900 --layout live --layout-dev
                          --dev-input "${cfg}/input.txt" --frames 90 ${ARGN} --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc})")
  endif()
  set(${tag}_log "${out}" PARENT_SCOPE)
endfunction()

# names(<out> <log> <prefix>): the quoted names of the lines "<prefix> <i> "<name>" x y", in order.
function(names var log prefix)
  string(REGEX MATCHALL "${prefix} [0-9]+ \"[^\"]*\"" lines "${log}")
  set(r "")
  foreach(l ${lines})
    string(REGEX REPLACE ".*\"([^\"]*)\"" "\\1" n "${l}")
    list(APPEND r "${n}")
  endforeach()
  set(${var} "${r}" PARENT_SCOPE)
endfunction()

# point(<x> <y> <log> <prefix> <index>): the point printed for that row or column.
function(point xv yv log prefix i)
  string(REGEX MATCH "${prefix} ${i} \"[^\"]*\" ([-0-9]+) ([-0-9]+)" m "${log}")
  if(NOT m)
    message(FATAL_ERROR "no '${prefix} ${i}' in the output")
  endif()
  set(${xv} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${yv} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# 1. Both views, the same tracks. The arrangement first, then Tab to the grid.
run(list "20 plrows\n24 key tab\n40 livegrid\n")
names(rows "${list_log}" "plrow")
names(cols "${list_log}" "livegrid title")
list(LENGTH rows nr)
list(LENGTH cols nc)
message(STATUS "arrangement: ${rows}")
message(STATUS "clip grid:   ${cols}")
if(nr EQUAL 0 OR NOT rows STREQUAL cols)
  message(FATAL_ERROR "FAIL: the arrangement's rows (${rows}) and the grid's columns (${cols}) differ")
endif()
if(NOT nr LESS 32)
  message(FATAL_ERROR "FAIL: the arrangement lists all ${nr} tracks of the song, the empty ones too")
endif()
if(nr LESS 2)
  message(FATAL_ERROR "FAIL: the fixture should list at least two tracks (${nr})")
endif()
list(GET rows 0 n0)
list(GET rows 1 n1)
message(STATUS "ok - both views list ${nr} tracks, the same names in the same order")

# 2. Drag the first column's title past the second (in the grid), then look at the arrangement.
point(gx0 gy0 "${list_log}" "livegrid title" 0)
point(gx1 gy1 "${list_log}" "livegrid title" 1)
math(EXPR gstep "(${gx1} - ${gx0}) / 4")
math(EXPR gm1 "${gx0} + ${gstep}")
math(EXPR gm2 "${gx1}")
math(EXPR gend "${gx1} + 2 * ${gstep}")
run(gridmove "20 key tab\n28 move ${gx0} ${gy0}\n30 down 0\n32 move ${gm1} ${gy0}\n34 move ${gm2} ${gy0}\n36 move ${gend} ${gy0}\n38 move ${gend} ${gy0}\n40 up 0\n48 livegrid\n52 key tab\n66 plrows\n")
names(rows2 "${gridmove_log}" "plrow")
names(cols2 "${gridmove_log}" "livegrid title")
list(GET rows2 0 a0)
list(GET rows2 1 a1)
if(NOT a0 STREQUAL n1 OR NOT a1 STREQUAL n0)
  message(FATAL_ERROR "FAIL: after moving the first column in the grid the arrangement lists ${rows2} (wanted ${n1} then ${n0})")
endif()
if(NOT rows2 STREQUAL cols2)
  message(FATAL_ERROR "FAIL: after the grid move the views differ: ${rows2} / ${cols2}")
endif()
message(STATUS "ok - a column moved in the grid moves the arrangement's row")

# 3. Drag the first arrangement header below the second, then look at the grid.
point(hx0 hy0 "${list_log}" "plrow" 0)
point(hx1 hy1 "${list_log}" "plrow" 1)
math(EXPR hstep "(${hy1} - ${hy0}) / 4")
math(EXPR hm1 "${hy0} + ${hstep}")
math(EXPR hend "${hy1} + 3 * ${hstep}")
run(armove "20 plrows\n28 move ${hx0} ${hy0}\n30 down 0\n32 move ${hx0} ${hm1}\n34 move ${hx0} ${hy1}\n36 move ${hx0} ${hend}\n38 move ${hx0} ${hend}\n40 up 0\n48 plrows\n52 key tab\n66 livegrid\n")
string(FIND "${armove_log}" "plrows n" last REVERSE)
string(SUBSTRING "${armove_log}" ${last} -1 after)
names(rows3 "${after}" "plrow")
names(cols3 "${after}" "livegrid title")
list(GET cols3 0 c0)
list(GET cols3 1 c1)
if(NOT c0 STREQUAL n1 OR NOT c1 STREQUAL n0)
  message(FATAL_ERROR "FAIL: after moving the first arrangement row the grid lists ${cols3} (wanted ${n1} then ${n0})")
endif()
if(NOT rows3 STREQUAL cols3)
  message(FATAL_ERROR "FAIL: after the arrangement move the views differ: ${rows3} / ${cols3}")
endif()
message(STATUS "ok - a row moved in the arrangement moves the grid's column")
