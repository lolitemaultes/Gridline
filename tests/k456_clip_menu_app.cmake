# Gridline: K-456, the playlist clip menus are short and hold no value fields or parameter names.
# Runs the app hidden (isolated config and HOME, SDL's dummy audio driver, off-screen 1440x900) with
# a clip's whole menu pinned open (--menu patclip / arrclip / autoclip: the pattern, audio and
# automation clip menus as the playlist opens them, ContextMenus.cpp drawDevMenu), in each layout
# (Studio, Clips, Smooth, Tracker). The menu writes its top level (MENUDUMP: one row per line, a
# value field as "{ value } Label", "--- Name" for a heading, a dashed line for a separator) and its
# whole tree (MENUTREE: "Parent > Item"). For every menu:
#   1. at most kMaxTop rows at the top level (separators not counted);
#   2. no value field at the top level (the audio clip's gain, pitch and fades were inline) and no
#      parameter name ("Drives: <parameter>" on the automation clip, the fields' labels);
#   3. Studio, Smooth and Tracker open with the clip's name as the title (the pattern clip's came
#      after five rows) and end with Delete; no separator first or last and never two in a row;
#   4. every row each menu had before (tests/k456_clip_menu_rows_before.txt) is still in its tree,
#      and the audio clip's fields are in its "Gain, pan, pitch and fades" submenu (--menu cliplevels
#      draws that submenu's body: the four fields are there);
#   5. the layout's own words: the mute row is "Mute clip" in Studio and Tracker, "Mute region" in
#      Smooth and "Deactivate" in Clips, in every clip kind's menu, and no other layout's word.
# Then (6) the new Delete row in a collaboration session (two hidden Gridline windows over real ICE,
# Studio): Alex clicks Delete in an audio clip's menu; the clip goes on both computers; Alex's undo
# brings it back on both.
#   cmake -DAPP=<gridline-app> -DDEMO=<test_song.gridline.json> -DBEFORE=<rows file> -DOUT=<dir> -P k456_clip_menu_app.cmake
cmake_minimum_required(VERSION 3.19)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(kMaxTop 22)  # the longest (an audio clip in each layout) has 21
get_filename_component(src "${DEMO}" DIRECTORY)
configure_file("${src}/../../assets/sounds/classic/startup.wav" "${OUT}/Take.wav" COPYONLY)
file(READ "${DEMO}" demo)  # a copy: never the fixture itself
# A sampler channel playing a WAV file with an audio clip, and an automation clip driving a parameter
# of the instrument with the most parameters (Perc, 83).
string(JSON nch LENGTH "${demo}" channels)
string(JSON demo SET "${demo}" channels ${nch} "{\"color\": 5219296, \"id\": 60, \"mixerTrack\": 0, \"name\": \"Take\", \"plugin\": {\"type\": \"gl.sampler\", \"data\": \"${OUT}/Take.wav\", \"params\": []}}")
string(JSON ncl LENGTH "${demo}" playlist)
string(JSON demo SET "${demo}" playlist ${ncl} "{\"id\": 70, \"kind\": \"audio\", \"source\": 60, \"track\": 7, \"start\": 384, \"length\": 200, \"pattern\": 0, \"gain\": 1.0}")
math(EXPR ncl "${ncl} + 1")
string(JSON demo SET "${demo}" playlist ${ncl} "{\"id\": 81, \"kind\": \"automation\", \"source\": 80, \"track\": 6, \"start\": 0, \"length\": 384, \"pattern\": 0}")
string(JSON demo SET "${demo}" automations "[{\"id\": 80, \"name\": \"Perc cutoff\", \"color\": 16750848, \"owner\": 27, \"index\": 5, \"points\": [[0, 0.2, 0, 0], [384, 0.8, 0, 0]]}]")
string(JSON demo SET "${demo}" tracks 7 mode 1)
string(REGEX REPLACE "\"nextId\": *[0-9]+" "\"nextId\": 100" demo "${demo}")
file(WRITE "${OUT}/song.gridline.json" "${demo}")

set(failures 0)
function(fail msg)
  message(SEND_ERROR "FAIL: ${msg}")
  math(EXPR f "${failures} + 1")
  set(failures ${f} PARENT_SCOPE)
endfunction()

# run(<tag> <layout id> <menu>): sets <tag>_dump (top level) and <tag>_tree in the parent scope.
function(run tag layout menu)
  set(cfg "${OUT}/cfg_${tag}")
  file(REMOVE_RECURSE "${cfg}")
  file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
  file(WRITE "${cfg}/gridline/settings.json"
       "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layout\": \"${layout}\", \"keys.profile\": \"${layout}\", \"ui.menuStyle\": \"${layout}\"}}\n")
  set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
  set(ENV{HOME} "${cfg}/home")
  set(ENV{SDL_AUDIODRIVER} "dummy")
  set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
  set(ENV{GRIDLINE_HIDE_PLUGINS} "all")
  execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --fixed-dt 16 --size 1440x900 --frames 12 --menu ${menu}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  file(WRITE "${OUT}/${tag}.log" "${out}\n${err}")
  file(REMOVE_RECURSE "${cfg}")
  set(${tag}_dump "" PARENT_SCOPE)
  set(${tag}_tree "" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    fail("${tag}: gridline-app failed (${rc}): ${err}")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  string(FIND "${out}" "MENUDUMP BEGIN ${menu}\n" b)
  string(FIND "${out}" "\nMENUDUMP END" e)
  string(FIND "${out}" "MENUTREE BEGIN ${menu}\n" tb)
  string(FIND "${out}" "MENUTREE END" te)
  if(b LESS 0 OR e LESS b OR tb LESS 0 OR te LESS tb)
    fail("${tag}: no menu dump in the output")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  string(LENGTH "MENUDUMP BEGIN ${menu}\n" hl)
  math(EXPR from "${b} + ${hl}")
  math(EXPR len "${e} - ${from}")
  string(SUBSTRING "${out}" ${from} ${len} dump)
  string(LENGTH "MENUTREE BEGIN ${menu}\n" hl)
  math(EXPR from "${tb} + ${hl}")
  math(EXPR len "${te} - ${from}")
  string(SUBSTRING "${out}" ${from} ${len} tree)
  set(${tag}_dump "${dump}" PARENT_SCOPE)
  set(${tag}_tree "${tree}" PARENT_SCOPE)
endfunction()

# The value fields' labels: never a row of their own at the top level.
set(paramNames "Gain;Pitch;Fade in;Fade out")

set(kLayouts "studio;live;logic;tracker")
set(kNames "Studio;Clips;Smooth;Tracker")
set(kMenus "patclip;arrclip;autoclip")
set(kTitles "Beat;Take;Perc cutoff")
file(STRINGS "${BEFORE}" before)
set(checkedRows 0)
foreach(li RANGE 3)
  list(GET kLayouts ${li} lay)
  list(GET kNames ${li} layName)
  foreach(mi RANGE 2)
    list(GET kMenus ${mi} menu)
    list(GET kTitles ${mi} title)
    set(tag "${lay}_${menu}")
    run(${tag} ${lay} ${menu})
    set(dump "${${tag}_dump}")
    set(tree "${${tag}_tree}")
    if(dump STREQUAL "")
      continue()
    endif()
    string(REPLACE ";" "\\;" dumpEsc "${dump}")
    string(REPLACE "\n" ";" lines "${dumpEsc}")
    set(rows 0)
    set(prev "start")
    set(first "")
    set(last "")
    foreach(l IN LISTS lines)
      if(l STREQUAL "")
        continue()
      endif()
      if(l MATCHES "^-+$")
        if(prev STREQUAL "start")
          fail("${layName} ${menu}: a separator first")
        elseif(prev STREQUAL "sep")
          fail("${layName} ${menu}: two separators in a row")
        endif()
        set(prev "sep")
        continue()
      endif()
      if(first STREQUAL "")
        set(first "${l}")
      endif()
      set(prev "row")
      set(last "${l}")
      if(NOT l MATCHES "^--- ")
        math(EXPR rows "${rows} + 1")
      endif()
      # 2. no value field, no parameter name
      if(l MATCHES "^\\{ .* \\} ")
        fail("${layName} ${menu}: a value field at the top level: \"${l}\"")
      endif()
      if(l MATCHES "^Drives")
        fail("${layName} ${menu}: a parameter name at the top level: \"${l}\"")
      endif()
      string(REGEX REPLACE " \\( .* \\)$" "" bare "${l}")
      list(FIND paramNames "${bare}" pn)
      if(pn GREATER -1)
        fail("${layName} ${menu}: a parameter row at the top level: \"${l}\"")
      endif()
    endforeach()
    if(prev STREQUAL "sep")
      fail("${layName} ${menu}: a separator last")
    endif()
    # 1. short
    if(rows GREATER kMaxTop)
      fail("${layName} ${menu}: ${rows} rows at the top level (at most ${kMaxTop})")
    endif()
    # 3. the title first and Delete last (Studio, Smooth and Tracker; Clips leads with its edit block)
    if(NOT lay STREQUAL "live" AND NOT last MATCHES "^Delete( |$)")
      fail("${layName} ${menu}: the menu ends with \"${last}\", not Delete")
    endif()
    if(NOT lay STREQUAL "live")
      if(NOT first STREQUAL "--- ${title}")
        fail("${layName} ${menu}: the menu opens with \"${first}\", not its title \"${title}\"")
      endif()
    endif()
    # 5. the layout's word for the mute row (the top level), and no other layout's word anywhere
    if(lay STREQUAL "live")
      set(mute "Deactivate")
    elseif(lay STREQUAL "logic")
      set(mute "Mute region")
    else()
      set(mute "Mute clip")
    endif()
    set(hasMute 0)
    foreach(l IN LISTS lines)
      if(l MATCHES "^${mute}( \\(|$)")
        set(hasMute 1)
      endif()
    endforeach()
    if(NOT hasMute)
      fail("${layName} ${menu}: no \"${mute}\" row")
    endif()
    foreach(other "Mute clip" "Mute region" "Deactivate")
      if(NOT other STREQUAL mute AND (dump MATCHES "(^|\n)${other}( \\(|\n|$)" OR tree MATCHES "(^|\n| > )${other}(\n|$)"))
        fail("${layName} ${menu}: \"${other}\" is another layout's word")
      endif()
    endforeach()
    # 4. every row from before is still reachable (in the tree, at any depth)
    set(segs "\n")
    string(REPLACE ";" "\\;" treeEsc "${tree}")
    string(REPLACE "\n" ";" tl "${treeEsc}")
    foreach(l IN LISTS tl)
      string(REGEX REPLACE " >$" "" l "${l}")
      string(REGEX REPLACE "^.* > " "" l "${l}")
      string(REGEX REPLACE "^ +" "" l "${l}")
      string(APPEND segs "${l}\n")
    endforeach()
    set(missing 0)
    foreach(br IN LISTS before)
      if(NOT br MATCHES "^${lay}\\|${menu}\\|(.*)$")
        continue()
      endif()
      set(label "${CMAKE_MATCH_1}")
      math(EXPR checkedRows "${checkedRows} + 1")
      string(FIND "${segs}" "\n${label}\n" at)
      if(at LESS 0)
        fail("${layName} ${menu}: \"${label}\" was in the menu before and is gone")
        math(EXPR missing "${missing} + 1")
      endif()
    endforeach()
    if(menu STREQUAL "arrclip" AND NOT lay STREQUAL "live")
      string(FIND "${segs}" "\nGain, pan, pitch and fades\n" at)
      if(at LESS 0)
        fail("${layName} arrclip: no \"Gain, pan, pitch and fades\" submenu")
      endif()
    endif()
    message(STATUS "ok - ${layName} ${menu}: ${rows} rows at the top level, title \"${first}\", ${missing} rows missing")
  endforeach()
endforeach()

# 4b. the audio clip's fields, in their submenu
run(levels studio cliplevels)
foreach(f "Gain" "Pan" "Pitch" "Fade in" "Fade out")
  if(NOT levels_dump MATCHES "\\{ [^\n]* \\} ${f}(\n|$)")
    fail("Gain, pan, pitch and fades: no \"${f}\" field")
  endif()
endforeach()
message(STATUS "ok - ${checkedRows} rows from before checked")

# 6. Delete in an audio clip's menu, in a collaboration session: synced, and undone on both sides.
if(APPLE)
  set(M super)
else()
  set(M ctrl)
endif()
string(JSON nclips LENGTH "${demo}" playlist)
math(EXPR nless "${nclips} - 1")
set(C "${OUT}/collab")
file(MAKE_DIRECTORY "${C}")
set(A "5 collab profile Alex
10 collab new \"${C}/Alex/Song\"
20 collab invite \"${C}/invite.txt\"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
32 waitfor never 2
33 collab report \"${C}/a-start.json\"
40 cliptools select 70
41 menu open region
46 menu click clipmenu-delete
48 waitfor collab-clips:${nless} 30
49 waitfor collab-synced 60
50 waitfor never 3
51 collab report \"${C}/a-deleted.json\"
55 forget collab-clips:${nclips}
56 sdlkey z down ${M}
57 sdlkey z up ${M}
58 waitfor collab-clips:${nclips} 30
59 waitfor collab-synced 60
60 waitfor never 3
61 collab report \"${C}/a-undone.json\"
70 waitfor file:${C}/b-undone.json 120
71 quit discard
")
set(B "5 collab profile Sam
10 collab join \"${C}/invite.txt\" \"${C}/Sam/Song\"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
13 waitfor collab-clips:${nclips} 60
14 collab report \"${C}/b-start.json\"
20 waitfor collab-clips:${nless} 120
21 waitfor never 1
22 collab report \"${C}/b-deleted.json\"
30 forget collab-clips:${nclips}
31 waitfor collab-clips:${nclips} 120
32 waitfor never 1
33 collab report \"${C}/b-undone.json\"
40 waitfor never 2
41 quit discard
")
file(WRITE "${C}/a.txt" "${A}")
file(WRITE "${C}/b.txt" "${B}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${C}/cfgA HOME=${C}/homeA SDL_AUDIODRIVER=dummy GRIDLINE_NO_PLUGIN_SCAN=1
          "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --layout studio --dev-input "${C}/a.txt"
  COMMAND ${CMAKE_COMMAND} -E env GRIDLINE_CONFIG_HOME=${C}/cfgB HOME=${C}/homeB SDL_AUDIODRIVER=dummy GRIDLINE_NO_PLUGIN_SCAN=1
          "${APP}" --hidden --no-ui-sounds --layout studio --dev-input "${C}/b.txt"
  RESULTS_VARIABLE rcs OUTPUT_FILE "${C}/run.out" ERROR_FILE "${C}/run.err" TIMEOUT 420)
# (Alex's output feeds the pipeline, so only Sam's reaches run.out: the reports show the click. Nothing
# else in Alex's script removes a clip, so clip 70 gone after "menu click clipmenu-delete" is that row.)
# has70(<report> <want 1|0> <what>)
function(has70 file want what)
  if(NOT EXISTS "${file}")
    fail("collab: no report ${file}")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  file(READ "${file}" j)
  string(JSON clips GET "${j}" clips)
  if(clips MATCHES "(^|[^0-9.])70([^0-9.]|$)")
    set(got 1)
  else()
    set(got 0)
  endif()
  if(NOT got EQUAL want)
    fail("collab: ${what}: audio clip 70 present ${got}, wanted ${want}: ${clips}")
    set(failures ${failures} PARENT_SCOPE)
  else()
    message(STATUS "ok - collab: ${what}")
  endif()
endfunction()
has70("${C}/a-start.json" 1 "Alex has the audio clip at the start")
has70("${C}/b-start.json" 1 "Sam has the audio clip at the start")
has70("${C}/a-deleted.json" 0 "Delete in the clip menu removed it for Alex")
has70("${C}/b-deleted.json" 0 "the delete reached Sam")
has70("${C}/a-undone.json" 1 "Alex's undo brought it back")
has70("${C}/b-undone.json" 1 "the undo reached Sam")
if(EXISTS "${C}/a-undone.json" AND EXISTS "${C}/b-undone.json")
  file(READ "${C}/a-undone.json" ja)
  file(READ "${C}/b-undone.json" jb)
  string(JSON sa GET "${ja}" song)
  string(JSON sb GET "${jb}" song)
  if(NOT sa STREQUAL sb)
    fail("collab: the songs differ after the undo")
  endif()
endif()
if(NOT rcs STREQUAL "0;0")
  fail("collab: exit codes ${rcs}")
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} clip menu check(s) failed")
endif()
file(REMOVE_RECURSE "${OUT}")  # passed: nothing kept (disk)
