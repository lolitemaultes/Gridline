# Gridline - K-112c (round 43): files dropped onto the Clips layout's clip grid.
#   In Clips (as a development layout), on the grid:
#   1. an audio file dropped from another program onto the first column's third slot row lands
#      in that slot: the grid then has at least three scenes (the live area grew) and slot (0, 2)
#      holds a clip;
#   2. an audio file dropped onto the free column makes a new track: one more column, whose slot
#      in the first row holds the clip;
#   3. no window opens for the dropped audio (the floating windows are the same before and after);
#   4. two undos take both drops back (the grid's columns and scenes as before);
#   5. a new project made in Clips (no project file) opens on the grid with one detail band (round 45)
#      and starts with the reference's new set's tracks; the demo opened in Clips has clips in its grid;
#      the grid's titles are 1 MIDI, 2 MIDI, 3 Audio, 4 Audio, and the scene column is numbered.
#   (The live area's growth itself, its undo and a collaborator: test_k112_clips_tracks.cpp.)
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DSRC=<tests dir> -DOUT=<dir> -P k112_clips_drop_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
configure_file("${SRC}/../assets/sounds/classic/open.wav" "${OUT}/Snap.wav" COPYONLY)
configure_file("${SRC}/../assets/sounds/classic/save.wav" "${OUT}/Tick.wav" COPYONLY)

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
                          --dev-input "${cfg}/input.txt" --frames 110 --screenshot "${OUT}/${tag}.ppm"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
  file(WRITE "${OUT}/${tag}.log" "${out}\n---- stderr ----\n${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tag}: gridline-app failed (${rc})")
  endif()
  set(${tag}_log "${out}" PARENT_SCOPE)
endfunction()

# grid(<prefix> <text>): the columns and scenes of the first "livegrid cols" line in <text>.
function(grid prefix text)
  string(REGEX MATCH "livegrid cols ([0-9]+) scenes ([0-9]+) rows ([0-9]+)" m "${text}")
  if(NOT m)
    message(FATAL_ERROR "no clip grid in the output")
  endif()
  set(${prefix}_cols "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${prefix}_scenes "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${prefix}_rows "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

# The grid's places.
run(probe "20 key tab\n30 livegrid\n")
grid(g0 "${probe_log}")
message(STATUS "grid: ${g0_cols} columns, ${g0_scenes} scenes, ${g0_rows} rows")
string(REGEX MATCH "livegrid title 0 \"[^\"]*\" ([-0-9]+) " m "${probe_log}")
set(x0 "${CMAKE_MATCH_1}")
string(REGEX MATCH "livegrid row 2 y ([-0-9]+)" m "${probe_log}")
set(y2 "${CMAKE_MATCH_1}")
string(REGEX MATCH "livegrid row 0 y ([-0-9]+)" m "${probe_log}")
set(y0 "${CMAKE_MATCH_1}")
string(REGEX MATCH "livegrid free x ([-0-9]+)" m "${probe_log}")
set(fx "${CMAKE_MATCH_1}")
if(x0 STREQUAL "" OR y2 STREQUAL "" OR fx STREQUAL "")
  message(FATAL_ERROR "FAIL: the grid shows no first column, third row or free column")
endif()

set(snap "${OUT}/Snap.wav")
set(tick "${OUT}/Tick.wav")
run(drops "20 key tab
26 windows
30 dragenter ${x0} ${y2} \"${snap}\"
32 dragmove ${x0} ${y2}
34 dragmove ${x0} ${y2}
36 drop ${x0} ${y2}
46 dragenter ${fx} ${y0} \"${tick}\"
48 dragmove ${fx} ${y0}
50 dragmove ${fx} ${y0}
52 drop ${fx} ${y0}
62 livegrid
64 windows
70 undo
74 undo
80 livegrid
")
string(FIND "${drops_log}" "livegrid cols" a1)
string(SUBSTRING "${drops_log}" ${a1} -1 after1)
grid(g1 "${after1}")
string(FIND "${drops_log}" "livegrid cols" a2 REVERSE)
string(SUBSTRING "${drops_log}" ${a2} -1 after2)
grid(g2 "${after2}")
message(STATUS "after the drops: ${g1_cols} columns, ${g1_scenes} scenes; after two undos: ${g2_cols} columns, ${g2_scenes} scenes")

set(fails 0)
macro(check what)
  if(${ARGN})
    message(STATUS "ok - ${what}")
  else()
    message(SEND_ERROR "FAIL: ${what}")
    math(EXPR fails "${fails} + 1")
  endif()
endmacro()

check("the slot drop grows the live area to three scenes at least (${g1_scenes})" g1_scenes GREATER 2)
string(REGEX MATCH "  slot 0 2 x [-0-9.]+ y [-0-9.]+ clip ([0-9]+)" m "${after1}")
check("the dropped file lands in the first column's third slot (clip ${CMAKE_MATCH_1})" m AND NOT CMAKE_MATCH_1 STREQUAL "0")
math(EXPR want "${g0_cols} + 1")
check("the free-column drop makes one new track (${g0_cols} -> ${g1_cols})" g1_cols EQUAL want)
string(REGEX MATCH "  slot ${g0_cols} 0 x [-0-9.]+ y [-0-9.]+ clip ([0-9]+)" m "${after1}")
check("the new track's first slot holds the second file (clip ${CMAKE_MATCH_1})" m AND NOT CMAKE_MATCH_1 STREQUAL "0")
string(REGEX MATCHALL "windows frame [0-9]+:[^\n]*\n(  window [^\n]*\n)*" w "${drops_log}")
list(LENGTH w nw)
if(nw EQUAL 2)
  list(GET w 0 wb)
  list(GET w 1 wa)
  string(REGEX REPLACE "windows frame [0-9]+" "" wb "${wb}")
  string(REGEX REPLACE "windows frame [0-9]+" "" wa "${wa}")
  check("no window opens for the dropped audio" wb STREQUAL wa)
else()
  check("two window lists printed (${nw})" FALSE)
endif()
# 5. A new project in Clips.
set(cfg "${OUT}/cfg_new")
file(MAKE_DIRECTORY "${cfg}/gridline" "${cfg}/home")
file(WRITE "${cfg}/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true}}\n")
file(WRITE "${cfg}/input.txt" "5 toasts clear\n30 livegrid\n32 layout dump\n40 template demo\n60 livegrid\n")
set(ENV{GRIDLINE_CONFIG_HOME} "${cfg}")
set(ENV{HOME} "${cfg}/home")
execute_process(COMMAND "${APP}" --hidden --no-ui-sounds --size 1440x900 --layout live --layout-dev --dev-input "${cfg}/input.txt" --frames 70
                        --screenshot "${OUT}/new.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE newlog ERROR_VARIABLE err TIMEOUT 300)
file(WRITE "${OUT}/new.log" "${newlog}\n---- stderr ----\n${err}")
# Round 45: the new set opens on the clip grid (no Tab pressed) with one detail band (the devices);
# then the demo song, opened in Clips, has clips in its grid.
string(FIND "${newlog}" "livegrid cols" n1)
check("a new Clips project opens on the clip grid" n1 GREATER -1)
string(FIND "${newlog}" "livegrid cols" n2 REVERSE)
set(demolog "")
if(n2 GREATER n1)
  string(SUBSTRING "${newlog}" ${n2} -1 demolog)
  string(SUBSTRING "${newlog}" 0 ${n2} newlog)
endif()
foreach(rv "live.browser 1" "live.device 1" "live.clip 0" "live.mixer 0")
  string(REPLACE " " " visible " rx "${rv}")
  string(FIND "${newlog}" "region ${rx} " hit)
  check("a new Clips set's default: region ${rx}" hit GREATER -1)
endforeach()
string(REGEX MATCHALL "  slot [0-9]+ [0-9]+ x [-0-9.]+ y [-0-9.]+ clip [1-9][0-9]*" demoClips "${demolog}")
list(LENGTH demoClips nDemo)
check("the demo, opened in Clips, has clips in its grid (${nDemo} slots)" nDemo GREATER 3)
string(REGEX MATCHALL "livegrid title [0-9]+ \"[^\"]*\"" nt "${newlog}")
string(REGEX REPLACE "livegrid title [0-9]+ " "" nt "${nt}")
string(REPLACE "\"" "" nt "${nt}")
string(REPLACE ";" "," nt "${nt}")
set(wantNew "1 MIDI,2 MIDI,3 Audio,4 Audio")
check("a new Clips project starts with ${wantNew} (${nt})" nt STREQUAL wantNew)
check("two undos take both drops back (${g2_cols} columns, ${g2_scenes} scenes)" g2_cols EQUAL g0_cols AND g2_scenes EQUAL g0_scenes)
if(fails GREATER 0)
  message(FATAL_ERROR "${fails} check(s) failed")
endif()
