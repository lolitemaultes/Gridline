# Gridline - K-173: the slight separator in the channel rack between the channel rows and the
# pattern's automation rows, in the real app (hidden, off-screen, isolated configs, SDL's dummy
# audio driver). The "Bassline" pattern (id 29) gets two automation clips; channel 25 ("Bass")
# is put in a display group of its own ("Low"), and a second group ("Empty") has no channels.
#   1. no automation rows: no separator, and the rows sit where they always did;
#   2. automation rows: one separator in the gap between the last channel row and the first
#      automation row; the gap grows by a small fixed amount only, the rows' pitch stays; the
#      caption sits inside the gap (it overlaps no row) left of the hairline;
#   3. hits stay exact: a click on the separator does nothing, a click on the first automation
#      row opens that clip in the editor;
#   4. a channel dragged onto the separator goes to the end of the channel rows;
#   5. the display group filter: a group with a channel shows the separator, one without any
#      channel rows shows none; with the graph editor open it stays under the automation rows;
#   6. the same invariants in the light theme, at UI scale 200 %, in every look style and in the
#      Live and Logic layouts.
# Screenshots: <OUT>/dark.ppm, <OUT>/light.ppm (1440x900).
#   cmake -DAPP=<gridline-app> -DDEMO=<song.gridline.json> -DOUT=<dir> -P k173_rack_separator_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
file(READ "${DEMO}" demo)  # a copy: the song is never opened (or saved over) in place

# Display groups: "Low" holds the Bass channel, "Empty" none.
set(chanchor "\"id\": 25,\n      \"mixerTrack\": 8,")
string(FIND "${demo}" "${chanchor}" at)
if(at LESS 0)
  message(FATAL_ERROR "FAIL: the test song has no channel 25 on mixer track 8")
endif()
string(REPLACE "${chanchor}" "\"group\": 0,\n      ${chanchor}" demo "${demo}")
string(REGEX REPLACE "^{" "{\n  \"channelGroups\": [\"Low\", \"Empty\"]," demo "${demo}")
file(WRITE "${OUT}/plain.gridline.json" "${demo}")

set(anchor "\"id\": 29,\n      \"name\": \"Bassline\",")
string(FIND "${demo}" "${anchor}" at)
if(at LESS 0)
  message(FATAL_ERROR "FAIL: the test song has no Bassline pattern (id 29)")
endif()
set(auto "\"automation\": [
        {\"owner\": 25, \"index\": 0, \"name\": \"Bass level\", \"color\": 15909198, \"flCurves\": true,
         \"points\": [[0, 0.15, 0, 0], [288, 0.9, 0.45, 0], [576, 0.35, -0.3, 0], [768, 0.8, 0, 0]]},
        {\"owner\": 25, \"index\": 1,
         \"points\": [[0, 0.5, 0, 2], [192, 0.2, 0, 2], [384, 0.8, 0, 2], [576, 0.5, 0, 2]]}
      ],")
string(REPLACE "${anchor}" "${anchor}\n      ${auto}" demo "${demo}")
file(WRITE "${OUT}/auto.gridline.json" "${demo}")

function(check msg)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAIL: ${msg}\nOutput:\n${last_out}")
  endif()
  message(STATUS "ok - ${msg}")
endfunction()

# run(<tag> <song> <rack spec> <frames> <script> [app args]): the dev input script and the output
function(run tag song spec frames script)
  file(REMOVE_RECURSE "${OUT}/cfg_${tag}")
  file(MAKE_DIRECTORY "${OUT}/cfg_${tag}/home")
  set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg_${tag}")
  set(ENV{HOME} "${OUT}/cfg_${tag}/home")
  file(WRITE "${OUT}/${tag}.txt" "${script}")
  execute_process(COMMAND "${APP}" "${OUT}/${song}.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --frames ${frames} --rack ${spec}
                          --screenshot "${OUT}/${tag}.ppm" --dev-input "${OUT}/${tag}.txt" ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 150)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc}): ${err}")
  endif()
  set(${tag}_out "${out}" PARENT_SCOPE)
  set(last_out "${out}" PARENT_SCOPE)
endfunction()

# geo(<tag> <frame>): the rack dump of one frame as variables <tag>_<name>, in tenths of a pixel
# (CMake regexes take at most nine groups: one key at a time)
function(geo tag frame)
  set(o "${${tag}_out}")
  string(FIND "${o}" "rack frame ${frame}:" at)
  if(at LESS 0)
    message(FATAL_ERROR "FAIL (${tag}): no rack dump at frame ${frame}\nOutput:\n${o}")
  endif()
  string(SUBSTRING "${o}" ${at} -1 d)
  string(FIND "${d}" "\n  autosep " end)
  if(end LESS 0)
    message(FATAL_ERROR "FAIL (${tag}): the dump at frame ${frame} has no separator line\nOutput:\n${o}")
  endif()
  math(EXPR end "${end} + 400")
  string(SUBSTRING "${d}" 0 ${end} d)
  set(keys "rows= rows" "ox=geom originX" "row0=row0Y" "graph=graphY" "pitch=rowPitch" "lastBottom=lastRowBottom" "sepY=autosep y"
           "sepTop= top" "sepBottom= bottom" "autoRows=autoRows" "autoY0=autoY0" "lastIndex=lastRowIndex")
  set(vals "")
  foreach(k IN LISTS keys)
    string(REGEX MATCH "^([^=]+)=(.*)$" _ "${k}")
    set(n "${CMAKE_MATCH_1}")
    string(REGEX MATCH "${CMAKE_MATCH_2} (-?[0-9.]+)" _ "${d}")
    list(APPEND vals "${n}=${CMAKE_MATCH_1}")
  endforeach()
  string(REGEX MATCH "autosep [^\n]* x (-?[0-9.]+) (-?[0-9.]+) caption (-?[0-9.]+) (-?[0-9.]+) (-?[0-9.]+) (-?[0-9.]+)" _ "${d}")
  list(APPEND vals "sepX0=${CMAKE_MATCH_1}" "sepX1=${CMAKE_MATCH_2}" "capX0=${CMAKE_MATCH_3}" "capY0=${CMAKE_MATCH_4}" "capX1=${CMAKE_MATCH_5}"
       "capY1=${CMAKE_MATCH_6}")
  foreach(kv IN LISTS vals)
    string(REGEX MATCH "^([^=]+)=(.*)$" _ "${kv}")
    set(n "${CMAKE_MATCH_1}")
    set(v "${CMAKE_MATCH_2}")
    if(v STREQUAL "")
      message(FATAL_ERROR "FAIL (${tag}): no ${n} in the dump at frame ${frame}\n${d}")
    endif()
    if(v MATCHES "^-?[0-9]+\\.[0-9]$")  # one decimal: tenths of a pixel as an integer
      string(REPLACE "." "" v "${v}")
      string(REGEX REPLACE "^(-?)0+([0-9])" "\\1\\2" v "${v}")
    else()
      math(EXPR v "${v} * 10")
    endif()
    set(${tag}_${n} "${v}" PARENT_SCOPE)
  endforeach()
endfunction()

# shape(<tag>): the separator's place, for any scale, theme, look and layout (values x10)
function(shape tag)
  check("${tag}: a separator is drawn" ${tag}_sepY GREATER 0)
  check("${tag}: it fills the gap from the last channel row (${${tag}_sepTop}) to the first automation row (${${tag}_autoY0})"
        ${tag}_sepTop EQUAL ${tag}_lastBottom AND ${tag}_sepBottom EQUAL ${tag}_autoY0)
  check("${tag}: the hairline lies inside that gap" ${tag}_sepY GREATER ${tag}_sepTop AND ${tag}_sepY LESS ${tag}_sepBottom)
  check("${tag}: the caption sits inside the gap (it overlaps no row)"
        ${tag}_capY0 GREATER_EQUAL ${tag}_sepTop AND ${tag}_capY1 LESS_EQUAL ${tag}_sepBottom)
  check("${tag}: the caption is left of the hairline, which runs to the row's end"
        ${tag}_capX0 GREATER_EQUAL ${tag}_ox AND ${tag}_capX1 LESS ${tag}_sepX0 AND ${tag}_sepX0 LESS ${tag}_sepX1)
  check("${tag}: two automation rows" ${tag}_autoRows EQUAL 20)
endfunction()

set(dump "4 selpattern 29\n10 rack\n")
# ---- 1. no automation rows
run(plain plain w1300,h560 14 "${dump}" --size 1440x900)
geo(plain 10)
check("no automation: no separator" plain_sepY LESS 0 AND plain_autoRows EQUAL 0)

# ---- 2. automation rows (dark, 1440x900)
run(dark auto w1300,h560 14 "${dump}" --size 1440x900)
geo(dark 10)
shape(dark)
check("the channel rows stay where they were" dark_row0 EQUAL plain_row0 AND dark_pitch EQUAL plain_pitch AND dark_lastBottom EQUAL plain_lastBottom)
math(EXPR gap "${dark_autoY0} - ${dark_lastBottom}")
math(EXPR plainGap "${plain_pitch} - 260")  # the gap between two rows (26 px rows)
math(EXPR extra "${gap} - ${plainGap}")
check("the separator adds a small fixed gap only (12 px, here ${extra}/10)" extra EQUAL 120)
math(EXPR capH "${dark_capY1} - ${dark_capY0}")
check("the caption is small (at most 14 px tall, here ${capH}/10)" capH LESS_EQUAL 140)

# ---- 3. hits: the separator takes no click, the first automation row opens its clip
math(EXPR x "(${dark_ox} + 1200) / 10")
math(EXPR sy "${dark_sepY} / 10")
math(EXPR ay "${dark_autoY0} / 10 + 13")
run(hits auto w1300,h560 34
    "4 selpattern 29\n12 move ${x} ${sy}\n14 move ${x} ${sy}\n15 down 0\n16 up 0\n20 autoedit\n22 move ${x} ${ay}\n24 move ${x} ${ay}\n25 down 0\n26 up 0\n30 autoedit\n"
    --size 1440x900)
check("a click on the separator opens nothing" hits_out MATCHES "autoedit frame 20: open 0")
check("... and changes nothing" NOT hits_out MATCHES "autoedit frame 20:[^\n]*undo [1-9]")
check("a click on the first automation row opens that clip" hits_out MATCHES "autoedit frame 30: open 1 automation 0 pattern 29 target 25 0")

# ---- 4. a channel dragged onto the separator goes to the end of the channel rows
string(REGEX MATCH "rowgeom 0 lights [0-9]+ selected [0-9]+ pan [-0-9. ]+ vol [-0-9. ]+ mix [-0-9. ]+ name ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)" _ "${dark_out}")
check("channel 0's name button" CMAKE_MATCH_1 MATCHES "[0-9]")
set(nx "${CMAKE_MATCH_1}")  # (a REGEX REPLACE resets CMAKE_MATCH_*: copy both first)
set(ny "${CMAKE_MATCH_2}")
string(REGEX REPLACE "\\..*" "" nx "${nx}")
string(REGEX REPLACE "\\..*" "" ny "${ny}")
math(EXPR nx "${nx} + 20")
math(EXPR ny "${ny} + 10")
math(EXPR ny2 "${ny} + 30")
run(drag auto w1300,h560 40
    "4 selpattern 29\n8 rack\n12 move ${nx} ${ny}\n14 move ${nx} ${ny}\n15 down 0\n17 move ${nx} ${ny2}\n19 move ${x} ${sy}\n21 move ${x} ${sy}\n23 move ${x} ${sy}\n24 up 0\n34 rack\n"
    --size 1440x900)
string(REGEX MATCHALL "  channel [0-9]+ volume [-0-9.]+ pan [-0-9.]+ velsum [0-9]+ pitchsum [0-9]+" chans "${drag_out}")
list(LENGTH chans nchans)
check("the rack lists its channels twice (before and after)" nchans EQUAL 16)
set(b "")
set(a "")
foreach(i RANGE 0 15)
  list(GET chans ${i} c)
  string(REGEX REPLACE ".*volume ([-0-9.]+) pan ([-0-9.]+) velsum ([0-9]+) pitchsum ([0-9]+)" "\\1/\\2/\\3/\\4" c "${c}")
  if(i LESS 8)
    list(APPEND b "${c}")
  else()
    list(APPEND a "${c}")
  endif()
endforeach()
list(GET b 0 first)
list(REMOVE_AT b 0)
list(APPEND b "${first}")
list(JOIN a "," aj)  # (one argument each for check)
list(JOIN b "," bj)
check("the channels' order rotated: the dragged one last, the others up one" aj STREQUAL bj)
# The dragged channel (selected by the click that started the drag) is the only selected row, and it is the last one.
string(FIND "${drag_out}" "rack frame 34:" at34)
string(SUBSTRING "${drag_out}" ${at34} -1 after)
check("the selected (dragged) channel is the last channel row" after MATCHES "rowgeom 7 lights [0-9]+ selected 1")
string(REGEX MATCHALL "rowgeom [0-9]+ lights [0-9]+ selected 1" sel "${after}")
list(LENGTH sel nsel)
check("... and the only selected one (${nsel})" nsel EQUAL 1)

# ---- 5. display groups and the graph editor
run(group0 auto w1300,h560,group0 14 "${dump}" --size 1440x900)
geo(group0 10)
shape(group0)
check("group with one channel: one channel row above the separator" group0_rows EQUAL 10)
run(group1 auto w1300,h560,group1 14 "${dump}" --size 1440x900)
geo(group1 10)
check("group without channel rows: no separator, the automation rows still shown" group1_sepY LESS 0 AND group1_autoRows EQUAL 20)
run(graph auto w1300,h700,graph 14 "${dump}" --size 1440x900)
geo(graph 10)
shape(graph)
if(graph_graph GREATER 0)
  math(EXPR autoEnd "${graph_autoY0} + 2 * ${graph_pitch}")
  check("the graph editor is under the automation rows" graph_graph GREATER_EQUAL autoEnd)
else()
  message(STATUS "note - the graph editor is not shown for the selected channel (piano roll notes)")
endif()

# ---- 6. light theme, 200 %, every look style, the other layouts
run(light auto w1300,h560 14 "${dump}" --size 1440x900 --light)
geo(light 10)
shape(light)
run(scale2 auto w1300,h560 14 "${dump}" --size 2880x1800 --ui-scale 2)
geo(scale2 10)
shape(scale2)
foreach(look classic graphite hairline lumen slate)
  run(look_${look} auto w1300,h560 14 "${dump}" --size 1440x900 --look ${look})
  geo(look_${look} 10)
  shape(look_${look})
endforeach()
foreach(layout live logic)
  run(layout_${layout} auto w1300,h560 24 "4 selpattern 29\n20 rack\n" --size 1440x900 --layout ${layout} --pane rack)
  geo(layout_${layout} 20)
  check("${layout} layout: the rack is showing its rows" layout_${layout}_rows GREATER 0)
  shape(layout_${layout})
endforeach()
foreach(s dark light)
  check("screenshot ${s}.ppm written" EXISTS "${OUT}/${s}.ppm")
endforeach()
